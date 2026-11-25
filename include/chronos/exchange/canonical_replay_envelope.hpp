#pragma once

#include "chronos/exchange/restricted_replay.hpp"
#include "chronos/journal/inspection.hpp"

#include <array>
#include <string_view>

namespace chronos::exchange {

// In-memory experimental envelope, distinct from the journal inspector format.
class CanonicalReplayEnvelope final {
  public:
    using Bytes = std::vector<std::uint8_t>;
    using Identity = std::array<std::uint8_t, 16>;
    using Digest = std::array<std::uint8_t, 32>;
    static constexpr std::size_t header_size = 168;
    static constexpr std::size_t operation_size = 120;
    static constexpr std::size_t max_stream_bytes = 32 * 1024 * 1024;
    struct Anchor {
        Identity identity;
        std::uint64_t terminal;
        Digest digest;
    };
    struct Sealed {
        Bytes bytes;
        Anchor anchor;
    };

    [[nodiscard]] static Sealed seal(const RestrictedReplay &owner, Identity identity) {
        if (std::all_of(identity.begin(), identity.end(), [](auto x) { return x == 0; }))
            throw std::invalid_argument("zero isolated stream identity");
        const auto records = owner.records();
        auto candidate = RestrictedReplay::fresh();
        const auto initial = candidate->canonical_isolated_state();
        const auto initial_hash = journal::sha256(initial);
        const auto config = config_hash();
        Bytes out(header_size, 0);
        constexpr std::array<std::uint8_t, 8> magic{'C', 'H', 'R', 'C', 'A', 'N', '0', '1'};
        std::copy(magic.begin(), magic.end(), out.begin());
        write(out, 8, 1); // schema
        std::copy(identity.begin(), identity.end(), out.begin() + 16);
        std::copy(config.begin(), config.end(), out.begin() + 32);
        std::copy(initial_hash.begin(), initial_hash.end(), out.begin() + 64);
        write(out, 96, 1);               // isolated private allocator
        write(out, 104, 1);              // initial operation sequence
        write(out, 112, records.size()); // terminal sequence, zero for empty
        write(out, 120, records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto record = RestrictedReplay::decode(records[i]);
            if (record.sequence != i + 1 ||
                RestrictedReplay::execute(*candidate, record.op) != record.outcome ||
                candidate->isolated_continuation_state(RestrictedReplay::max_state_words) !=
                    record.state)
                throw std::logic_error("source replay divergence");
            const auto state = candidate->canonical_isolated_state();
            if (out.size() > max_stream_bytes - 16 ||
                operation_size > max_stream_bytes - out.size() - 16 ||
                state.size() > max_stream_bytes - out.size() - 16 - operation_size)
                throw std::length_error("canonical stream limit");
            append(out, operation_size);
            append(out, state.size());
            append(out, record.sequence);
            append(out, static_cast<std::uint64_t>(record.op.kind));
            append(out, record.outcome);
            for (const auto arg : record.op.args)
                append(out, arg);
            out.insert(out.end(), state.begin(), state.end());
        }
        if (candidate->canonical_isolated_state() != owner.state_->canonical_isolated_state())
            throw std::logic_error("source terminal canonical state mismatch");
        write(out, 128, out.size());
        const auto digest = journal::sha256(out); // digest field is zero
        std::copy(digest.begin(), digest.end(), out.begin() + 136);
        return {std::move(out), Anchor{identity, records.size(), digest}};
    }

    // Anchor must be obtained from an independent trusted source if identity
    // and terminal completeness are to be trusted; a self-supplied one is not authentication.
    [[nodiscard]] static bool verify(std::span<const std::uint8_t> bytes, const Anchor &anchor) {
        if (bytes.size() < header_size || bytes.size() > max_stream_bytes)
            return false;
        constexpr std::array<std::uint8_t, 8> magic{'C', 'H', 'R', 'C', 'A', 'N', '0', '1'};
        if (!std::equal(magic.begin(), magic.end(), bytes.begin()) || read(bytes, 8) != 1 ||
            !std::equal(anchor.identity.begin(), anchor.identity.end(), bytes.begin() + 16) ||
            std::all_of(anchor.identity.begin(), anchor.identity.end(),
                        [](auto x) { return x == 0; }) ||
            read(bytes, 96) != 1 || read(bytes, 104) != 1 || read(bytes, 112) != anchor.terminal ||
            read(bytes, 120) != anchor.terminal ||
            anchor.terminal > RestrictedReplay::max_operations || read(bytes, 128) != bytes.size())
            return false;
        const auto config = config_hash();
        if (!std::equal(config.begin(), config.end(), bytes.begin() + 32) ||
            !std::equal(anchor.digest.begin(), anchor.digest.end(), bytes.begin() + 136))
            return false;
        Bytes digest_input(bytes.begin(), bytes.end());
        std::fill(digest_input.begin() + 136, digest_input.begin() + 168, 0);
        if (journal::sha256(digest_input) != anchor.digest)
            return false;
        try {
            auto candidate = RestrictedReplay::fresh();
            const auto initial_hash = journal::sha256(candidate->canonical_isolated_state());
            if (!std::equal(initial_hash.begin(), initial_hash.end(), bytes.begin() + 64))
                return false;
            std::size_t pos = header_size;
            for (std::uint64_t i = 0; i < anchor.terminal; ++i) {
                if (bytes.size() - pos < 16)
                    return false;
                const auto record_len = read(bytes, pos);
                const auto state_len = read(bytes, pos + 8);
                pos += 16;
                if (record_len != operation_size || state_len > 65536 ||
                    record_len > bytes.size() - pos || state_len > bytes.size() - pos - record_len)
                    return false;
                const auto sequence = read(bytes, pos);
                RestrictedReplay::Operation operation{
                    static_cast<RestrictedReplay::Kind>(read(bytes, pos + 8))};
                const auto outcome = read(bytes, pos + 16);
                for (std::size_t j = 0; j < operation.args.size(); ++j)
                    operation.args[j] = read(bytes, pos + 24 + j * 8);
                pos += static_cast<std::size_t>(record_len);
                if (sequence != i + 1 || !RestrictedReplay::valid(operation) ||
                    RestrictedReplay::execute(*candidate, operation) != outcome)
                    return false;
                const auto state = candidate->canonical_isolated_state();
                if (state.size() != state_len ||
                    !std::equal(state.begin(), state.end(),
                                bytes.begin() + static_cast<std::ptrdiff_t>(pos)))
                    return false;
                pos += static_cast<std::size_t>(state_len);
            }
            return pos == bytes.size();
        } catch (const std::logic_error &) {
            return false;
        }
    }

  private:
    static Digest config_hash() noexcept {
        constexpr std::string_view tag =
            "chronos-isolated-v1;sessions=16;orders=128;reports=128;requests=128;feed=0;allocator="
            "private-zero;subscriptions=excluded";
        return journal::sha256({reinterpret_cast<const std::uint8_t *>(tag.data()), tag.size()});
    }
    static void write(Bytes &b, std::size_t offset, std::uint64_t value) noexcept {
        for (std::size_t i = 0; i < 8; ++i)
            b[offset + 7 - i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    static void append(Bytes &b, std::uint64_t value) {
        for (int i = 7; i >= 0; --i)
            b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    static std::uint64_t read(std::span<const std::uint8_t> b, std::size_t pos) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
            value = (value << 8) | b[pos + i];
        return value;
    }
};
} // namespace chronos::exchange
