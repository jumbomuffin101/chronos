#include "chronos/snapshot/restorable.hpp"
#include "chronos/eventlog/event_log.hpp"
#include "chronos/snapshot/checkpoint.hpp"
#include <algorithm>
#include <array>
#include <istream>
#include <new>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace chronos::snapshot {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> magic{'C', 'H', 'R', 'S', 'T', 'A', 'T', 'E'};
constexpr std::size_t prefix_size = 48;
constexpr std::size_t word_limit = max_checkpoint_bytes / 8;
void put(Bytes &b, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}
std::uint64_t get(const std::uint8_t *p) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (8U * i);
    return v;
}
void put32(Bytes &b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}
std::uint32_t get32(const std::uint8_t *p) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(p[i]) << (8U * i);
    return v;
}
ImageResult fail(std::string_view error) noexcept {
    ImageResult r;
    try {
        r.error.assign(error);
    } catch (...) {
        r.allocation_failure = true;
    }
    return r;
}
ImageResult compare_exact(const std::vector<std::uint64_t> &expected,
                          const exchange::Exchange &book) {
    const auto actual = book.isolated_continuation_state(word_limit);
    if (actual != expected) {
        const auto diff =
            std::mismatch(expected.begin(), expected.end(), actual.begin(), actual.end());
        auto r = fail("exact continuation state mismatch");
        r.mismatch_word = static_cast<std::size_t>(diff.first - expected.begin());
        r.error +=
            " at word " + std::to_string(r.mismatch_word) + ": expected " +
            (diff.first == expected.end() ? std::string("end") : std::to_string(*diff.first)) +
            ", actual " +
            (diff.second == actual.end() ? std::string("end") : std::to_string(*diff.second));
        return r;
    }
    ImageResult r;
    r.ok = true;
    return r;
}
} // namespace
ImageResult write_image(std::ostream &out, const std::string &log, const exchange::Exchange &source,
                        std::uint64_t generation) {
    try {
        if (!source.is_isolated())
            return fail("source must be isolated");
        if (log.size() > eventlog::max_log_size)
            return fail("log exceeds limit");
        const auto exact = source.isolated_continuation_state(word_limit);
        std::istringstream input(log, std::ios::binary);
        std::unique_ptr<exchange::Exchange> candidate;
        const auto replay = eventlog::reconstruct_isolated(input, candidate);
        if (!replay.verified || !candidate)
            return fail("log verification failed: " + replay.error);
        auto equality = compare_exact(exact, *candidate);
        if (!equality.ok)
            return equality;
        Bytes bytes;
        bytes.reserve(prefix_size + log.size() + exact.size() * 8 + 4);
        bytes.insert(bytes.end(), magic.begin(), magic.end());
        put(bytes, 1);
        put(bytes, generation);
        put(bytes, replay.commands);
        put(bytes, log.size());
        put(bytes, exact.size());
        bytes.insert(bytes.end(), log.begin(), log.end());
        for (const auto v : exact)
            put(bytes, v);
        put32(bytes, eventlog::crc32(bytes.data(), bytes.size()));
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out)
            return fail("image write or flush failed");
        ImageResult result;
        result.ok = true;
        result.generation = generation;
        result.ingress = replay.commands;
        return result;
    } catch (const std::bad_alloc &) {
        ImageResult result;
        result.allocation_failure = true;
        return result;
    } catch (const std::exception &e) {
        return fail(e.what());
    } catch (...) {
        return fail("image write failed");
    }
}
ImageResult restore_image(std::istream &in, std::unique_ptr<exchange::Exchange> &destination) {
    try {
        if (destination)
            return fail("destination must be empty");
        std::array<std::uint8_t, prefix_size> prefix{};
        in.read(reinterpret_cast<char *>(prefix.data()),
                static_cast<std::streamsize>(prefix.size()));
        if (static_cast<std::size_t>(in.gcount()) != prefix.size())
            return fail("truncated image header");
        if (!std::equal(magic.begin(), magic.end(), prefix.begin()))
            return fail("invalid image magic");
        if (get(prefix.data() + 8) != 1)
            return fail("unsupported image version");
        const auto generation = get(prefix.data() + 16), ingress = get(prefix.data() + 24);
        const auto log_size = get(prefix.data() + 32), words = get(prefix.data() + 40);
        if (log_size > eventlog::max_log_size || words > word_limit || words < 2)
            return fail("image size limit");
        const auto total = prefix_size + static_cast<std::size_t>(log_size) +
                           static_cast<std::size_t>(words) * 8 + 4;
        Bytes bytes(prefix.begin(), prefix.end());
        bytes.resize(total);
        const auto rest = total - prefix_size;
        in.read(reinterpret_cast<char *>(bytes.data() + prefix_size),
                static_cast<std::streamsize>(rest));
        if (static_cast<std::size_t>(in.gcount()) != rest)
            return fail("truncated image payload");
        if (in.peek() != std::char_traits<char>::eof() || in.bad() || !in.eof())
            return fail("image trailing data or read error");
        if (get32(bytes.data() + total - 4) != eventlog::crc32(bytes.data(), total - 4))
            return fail("image checksum mismatch");
        std::string log(reinterpret_cast<const char *>(bytes.data() + prefix_size),
                        static_cast<std::size_t>(log_size));
        std::vector<std::uint64_t> exact;
        exact.reserve(static_cast<std::size_t>(words));
        const auto *p = bytes.data() + prefix_size + static_cast<std::size_t>(log_size);
        for (std::size_t i = 0; i < words; ++i)
            exact.push_back(get(p + 8 * i));
        std::istringstream input(log, std::ios::binary);
        std::unique_ptr<exchange::Exchange> candidate;
        const auto replay = eventlog::reconstruct_isolated(input, candidate);
        if (!replay.verified || !candidate)
            return fail("log verification failed: " + replay.error);
        if (replay.commands != ingress)
            return fail("ingress boundary mismatch");
        auto equality = compare_exact(exact, *candidate);
        if (!equality.ok)
            return equality;
        destination = std::move(candidate);
        ImageResult result;
        result.ok = true;
        result.generation = generation;
        result.ingress = ingress;
        return result;
    } catch (const std::bad_alloc &) {
        ImageResult result;
        result.allocation_failure = true;
        return result;
    } catch (const std::exception &e) {
        return fail(e.what());
    } catch (...) {
        return fail("image restore failed");
    }
}
} // namespace chronos::snapshot
