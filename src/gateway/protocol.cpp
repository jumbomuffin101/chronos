#include "chronos/gateway/protocol.hpp"

#include <algorithm>

namespace chronos::gateway {
namespace {
std::uint32_t get32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
void put32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
bool known(MessageType type) {
    switch (type) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::Health:
    case MessageType::HealthAck:
    case MessageType::Echo:
    case MessageType::EchoReply:
    case MessageType::Error:
    case MessageType::SubmitOrder:
    case MessageType::CancelOrder:
    case MessageType::OrderAck:
    case MessageType::Execution:
    case MessageType::Subscribe:
    case MessageType::Unsubscribe:
    case MessageType::MarketAck:
    case MessageType::MarketData:
        return true;
    }
    return false;
}
bool valid_size(MessageType type, std::size_t size) {
    switch (type) {
    case MessageType::Hello:
    case MessageType::Health:
    case MessageType::HealthAck:
        return size == 0;
    case MessageType::HelloAck:
    case MessageType::CancelOrder:
        return size == 8;
    case MessageType::Error:
        return size == 2;
    case MessageType::SubmitOrder:
        return size == 30;
    case MessageType::OrderAck:
        return size == 17;
    case MessageType::Execution:
        return size == 69;
    case MessageType::Subscribe:
    case MessageType::Unsubscribe:
        return size == 4;
    case MessageType::MarketAck:
        return size == 5;
    case MessageType::MarketData:
        return size == 61;
    case MessageType::Echo:
    case MessageType::EchoReply:
        return true;
    }
    return false;
}
} // namespace

void write_u64(std::span<std::uint8_t, 8> out, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i)
        out[i] = static_cast<std::uint8_t>(value >> ((7 - i) * 8));
}
std::uint64_t read_u64(std::span<const std::uint8_t, 8> in) noexcept {
    std::uint64_t value = 0;
    for (auto byte : in)
        value = (value << 8) | byte;
    return value;
}
bool encode_frame(const Frame &frame, std::vector<std::uint8_t> &output) {
    if (frame.payload_size > frame.payload.size() || !known(frame.type) ||
        !valid_size(frame.type, frame.payload_size))
        return false;
    output.resize(header_size + frame.payload_size);
    put32(output.data(), static_cast<std::uint32_t>(output.size()));
    output[4] = protocol_version;
    output[5] = static_cast<std::uint8_t>(frame.type);
    output[6] = 0;
    output[7] = 0;
    write_u64(std::span<std::uint8_t, 8>{output.data() + 8, 8}, frame.request_id);
    std::copy_n(frame.payload.begin(), frame.payload_size, output.begin() + header_size);
    return true;
}
ProtocolError FrameDecoder::feed(std::span<const std::uint8_t> input, const Handler &handler) {
    if (error_ != ProtocolError::None)
        return error_;
    while (!input.empty()) {
        const auto count = std::min(input.size(), expected_ - used_);
        std::copy_n(input.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += count;
        input = input.subspan(count);
        if (used_ != expected_)
            continue;
        if (expected_ == header_size) {
            const auto length = get32(buffer_.data());
            if (length < header_size)
                return error_ = ProtocolError::InvalidLength;
            if (length > max_frame_size)
                return error_ = ProtocolError::Oversized;
            if (buffer_[4] != protocol_version)
                return error_ = ProtocolError::UnsupportedVersion;
            const auto type = static_cast<MessageType>(buffer_[5]);
            if (!known(type))
                return error_ = ProtocolError::UnknownType;
            if (buffer_[6] != 0 || buffer_[7] != 0)
                return error_ = ProtocolError::InvalidFlags;
            if (!valid_size(type, length - header_size))
                return error_ = ProtocolError::InvalidPayload;
            expected_ = length;
            if (used_ != expected_)
                continue;
        }
        Frame frame;
        frame.type = static_cast<MessageType>(buffer_[5]);
        frame.request_id = read_u64(std::span<const std::uint8_t, 8>{buffer_.data() + 8, 8});
        frame.payload_size = expected_ - header_size;
        std::copy_n(buffer_.begin() + header_size, frame.payload_size, frame.payload.begin());
        used_ = 0;
        expected_ = header_size;
        if (!handler(frame))
            return error_ = ProtocolError::UnexpectedMessage;
    }
    return error_;
}
ProtocolError FrameDecoder::finish() const noexcept {
    return error_ != ProtocolError::None
               ? error_
               : (used_ == 0 ? ProtocolError::None : ProtocolError::Truncated);
}
} // namespace chronos::gateway
