#include "chronos/snapshot/checkpoint.hpp"
#include "chronos/eventlog/event_log.hpp"
#include <algorithm>
#include <array>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace chronos::snapshot {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> magic{'C', 'H', 'R', 'O', 'V', 'I', 'E', 'W'};
constexpr std::size_t header = 40;
void put(Bytes &b, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
}
std::uint64_t get(const std::uint8_t *p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(p[i]) << (8U * i);
    return value;
}
void put32(Bytes &b, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
}
std::uint32_t get32(const std::uint8_t *p) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(p[i]) << (8U * i);
    return value;
}
Result failure(const char *message) { return {false, message, 0}; }
} // namespace

Checkpoint capture(const exchange::Exchange &source, std::uint64_t generation) {
    auto state = source.verification_state_bounded((max_checkpoint_bytes - header - 4) / 8);
    if (state.size() < 2 || state[0] != 1 || state.size() > (max_checkpoint_bytes - header - 4) / 8)
        throw std::length_error("checkpoint state exceeds supported format");
    const auto command = state[1];
    return {generation, command, std::move(state)};
}
Result write(std::ostream &output, const Checkpoint &checkpoint) {
    try {
        if (checkpoint.state.size() < 2 || checkpoint.state[0] != 1 ||
            checkpoint.state[1] != checkpoint.exchange_command ||
            checkpoint.state.size() > (max_checkpoint_bytes - header - 4) / 8)
            return failure("invalid checkpoint");
        Bytes bytes(magic.begin(), magic.end());
        put(bytes, 1);
        put(bytes, checkpoint.generation);
        put(bytes, checkpoint.exchange_command);
        put(bytes, checkpoint.state.size());
        for (const auto word : checkpoint.state)
            put(bytes, word);
        put32(bytes, eventlog::crc32(bytes.data(), bytes.size()));
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
            return failure("checkpoint write or flush failed");
        return {true, {}, 0};
    } catch (...) {
        return failure("checkpoint write failed");
    }
}
Result read(std::istream &input, Checkpoint &destination) {
    try {
        std::array<std::uint8_t, header> prefix{};
        input.read(reinterpret_cast<char *>(prefix.data()),
                   static_cast<std::streamsize>(prefix.size()));
        if (static_cast<std::size_t>(input.gcount()) != prefix.size())
            return failure("truncated header");
        if (!std::equal(magic.begin(), magic.end(), prefix.begin()))
            return failure("invalid magic");
        if (get(prefix.data() + 8) != 1)
            return failure("unsupported version");
        const auto count = get(prefix.data() + 32);
        if (count < 2 || count > (max_checkpoint_bytes - header - 4) / 8)
            return failure("invalid length");
        Bytes bytes(prefix.begin(), prefix.end());
        bytes.resize(header + static_cast<std::size_t>(count) * 8 + 4);
        const auto remaining = bytes.size() - header;
        input.read(reinterpret_cast<char *>(bytes.data() + header),
                   static_cast<std::streamsize>(remaining));
        if (static_cast<std::size_t>(input.gcount()) != remaining)
            return failure("truncated payload");
        if (input.peek() != std::char_traits<char>::eof() || input.bad() || !input.eof())
            return failure("trailing data or read error");
        if (get32(bytes.data() + bytes.size() - 4) !=
            eventlog::crc32(bytes.data(), bytes.size() - 4))
            return failure("checksum mismatch");
        Checkpoint candidate;
        candidate.generation = get(prefix.data() + 16);
        candidate.exchange_command = get(prefix.data() + 24);
        candidate.state.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < count; ++i)
            candidate.state.push_back(get(bytes.data() + header + 8 * i));
        if (candidate.state[0] != 1 || candidate.state[1] != candidate.exchange_command)
            return failure("invalid state metadata");
        destination = std::move(candidate);
        return {true, {}, 0};
    } catch (...) {
        return failure("checkpoint read failed");
    }
}
Result compare(const Checkpoint &checkpoint, const exchange::Exchange &source) {
    try {
        if (checkpoint.state.size() < 2 || checkpoint.state[0] != 1 ||
            checkpoint.state[1] != checkpoint.exchange_command)
            return failure("invalid checkpoint metadata");
        const auto state =
            source.verification_state_bounded((max_checkpoint_bytes - header - 4) / 8);
        const auto mismatch = std::mismatch(checkpoint.state.begin(), checkpoint.state.end(),
                                            state.begin(), state.end());
        if (mismatch.first != checkpoint.state.end() || mismatch.second != state.end())
            return {false, "verification state differs",
                    static_cast<std::size_t>(mismatch.first - checkpoint.state.begin())};
        return {true, {}, 0};
    } catch (...) {
        return failure("state comparison failed");
    }
}
} // namespace chronos::snapshot
