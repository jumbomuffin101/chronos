#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace chronos::gateway {

inline constexpr std::size_t header_size = 16;
inline constexpr std::size_t max_frame_size = 4096;
inline constexpr std::uint8_t protocol_version = 2;

enum class MessageType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    Health = 3,
    HealthAck = 4,
    Echo = 5,
    EchoReply = 6,
    Error = 7,
    SubmitOrder = 16,
    CancelOrder = 17,
    OrderAck = 18,
    Execution = 19
};
enum class ProtocolError : std::uint16_t {
    None = 0,
    InvalidLength = 1,
    Oversized = 2,
    UnsupportedVersion = 3,
    UnknownType = 4,
    InvalidFlags = 5,
    InvalidPayload = 6,
    Truncated = 7,
    HandshakeRequired = 8,
    UnsupportedOperation = 9,
    UnexpectedMessage = 10,
    ExchangeRejected = 11,
    IngressFull = 12
};

// Logical fields only; native struct layout is never serialized.
struct FutureOrder {
    std::uint64_t client_order_id;
    std::uint32_t symbol;
    std::uint8_t side;
    std::uint8_t order_type;
    std::int64_t price_ticks;
    std::uint64_t quantity;
};
struct FutureCancel {
    std::uint64_t client_order_id;
};
struct FutureAck {
    std::uint64_t client_order_id;
    std::uint8_t status;
    std::uint64_t sequence;
};
struct FutureExecution {
    std::uint64_t sequence;
    std::uint64_t command_sequence;
    std::uint64_t client_order_id;
    std::uint64_t internal_order_id;
    std::uint64_t trade_id;
    std::uint32_t symbol;
    std::uint8_t kind;
    std::int64_t price_ticks;
    std::uint64_t quantity;
    std::uint64_t remaining;
};

struct Frame {
    MessageType type = MessageType::Hello;
    std::uint64_t request_id = 0;
    std::array<std::uint8_t, max_frame_size - header_size> payload{};
    std::size_t payload_size = 0;
};

[[nodiscard]] bool encode_frame(const Frame &frame, std::vector<std::uint8_t> &output);
void write_u64(std::span<std::uint8_t, 8> out, std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t read_u64(std::span<const std::uint8_t, 8> in) noexcept;

// Fixed storage regardless of input size. The callback runs synchronously once per
// complete frame; an error is terminal and finish() diagnoses incomplete EOF.
class FrameDecoder {
  public:
    using Handler = std::function<bool(const Frame &)>;
    [[nodiscard]] ProtocolError feed(std::span<const std::uint8_t> input, const Handler &handler);
    [[nodiscard]] ProtocolError finish() const noexcept;
    [[nodiscard]] std::size_t buffered() const noexcept { return used_; }

  private:
    std::array<std::uint8_t, max_frame_size> buffer_{};
    std::size_t used_ = 0;
    std::size_t expected_ = header_size;
    ProtocolError error_ = ProtocolError::None;
};

} // namespace chronos::gateway
