#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace chronos::exchange {

// A bounded immutable copy of command input. This is not an authoritative
// prepared decision, install capability, admission check or journal format.
class CanonicalCommand final {
  public:
    enum class Kind : std::uint8_t { Submit = 1, Cancel = 2 };
    static constexpr std::size_t encoded_size = 52;
    using Bytes = std::array<std::uint8_t, encoded_size>;

    [[nodiscard]] static CanonicalCommand submit(std::uint64_t session, std::uint64_t request,
                                                 std::uint64_t client, std::uint32_t symbol,
                                                 std::uint8_t side, std::uint8_t type,
                                                 std::int64_t price,
                                                 std::uint64_t quantity) noexcept;
    [[nodiscard]] static CanonicalCommand cancel(std::uint64_t session, std::uint64_t request,
                                                 std::uint64_t client) noexcept;
    [[nodiscard]] static std::optional<CanonicalCommand>
    decode(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] Bytes encode() const noexcept { return bytes_; }
    [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(bytes_[2]); }

  private:
    explicit CanonicalCommand(Bytes bytes) noexcept : bytes_(bytes) {}
    Bytes bytes_{};
};
} // namespace chronos::exchange
