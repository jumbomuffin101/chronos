#include "chronos/exchange/canonical_command.hpp"

#include <algorithm>
#include <bit>

namespace chronos::exchange {
namespace {
void put(CanonicalCommand::Bytes &bytes, std::size_t offset, std::uint64_t value,
         std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + width - 1 - i] = static_cast<std::uint8_t>(value >> (8 * i));
}
} // namespace

CanonicalCommand CanonicalCommand::submit(std::uint64_t session, std::uint64_t request,
                                          std::uint64_t client, std::uint32_t symbol,
                                          std::uint8_t side, std::uint8_t type, std::int64_t price,
                                          std::uint64_t quantity) noexcept {
    Bytes bytes{};
    bytes[1] = 1;
    bytes[2] = static_cast<std::uint8_t>(Kind::Submit);
    put(bytes, 4, session, 8);
    put(bytes, 12, request, 8);
    put(bytes, 20, client, 8);
    put(bytes, 28, symbol, 4);
    bytes[32] = side;
    bytes[33] = type;
    put(bytes, 36, std::bit_cast<std::uint64_t>(price), 8);
    put(bytes, 44, quantity, 8);
    return CanonicalCommand(bytes);
}
CanonicalCommand CanonicalCommand::cancel(std::uint64_t session, std::uint64_t request,
                                          std::uint64_t client) noexcept {
    auto command = submit(session, request, client, 0, 0, 0, 0, 0);
    command.bytes_[2] = static_cast<std::uint8_t>(Kind::Cancel);
    return command;
}
std::optional<CanonicalCommand>
CanonicalCommand::decode(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != encoded_size || bytes[0] != 0 || bytes[1] != 1 || bytes[3] != 0 ||
        bytes[34] != 0 || bytes[35] != 0 ||
        (bytes[2] != static_cast<std::uint8_t>(Kind::Submit) &&
         bytes[2] != static_cast<std::uint8_t>(Kind::Cancel)))
        return std::nullopt;
    if (bytes[2] == static_cast<std::uint8_t>(Kind::Cancel) &&
        std::any_of(bytes.begin() + 28, bytes.end(), [](std::uint8_t b) { return b != 0; }))
        return std::nullopt;
    Bytes copy{};
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    return CanonicalCommand(copy);
}
} // namespace chronos::exchange
