#include "chronos/gateway/tcp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace chronos::gateway;
using namespace std::chrono_literals;

namespace {
Frame message(MessageType type, std::uint64_t id = 1) {
    Frame f;
    f.type = type;
    f.request_id = id;
    return f;
}
std::vector<std::uint8_t> bytes(const Frame &frame) {
    std::vector<std::uint8_t> out;
    if (!encode_frame(frame, out))
        throw std::runtime_error("encode");
    return out;
}
void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
}
} // namespace

TEST(Protocol, DeterministicEndianRoundTripAndFragmentation) {
    Frame original = message(MessageType::Echo, 0x0102030405060708ULL);
    original.payload_size = 3;
    original.payload[0] = 0;
    original.payload[1] = 127;
    original.payload[2] = 255;
    const auto encoded = bytes(original);
    EXPECT_EQ(encoded[3], 19);
    EXPECT_EQ(encoded[4], 1);
    EXPECT_EQ(encoded[8], 1);
    EXPECT_EQ(encoded[15], 8);
    for (std::size_t split = 0; split <= encoded.size(); ++split) {
        FrameDecoder decoder;
        int count = 0;
        Frame decoded;
        auto handler = [&](const Frame &frame) {
            decoded = frame;
            ++count;
            return true;
        };
        ASSERT_EQ(decoder.feed(std::span{encoded}.first(split), handler), ProtocolError::None);
        ASSERT_EQ(decoder.feed(std::span{encoded}.subspan(split), handler), ProtocolError::None);
        EXPECT_EQ(decoder.finish(), ProtocolError::None);
        EXPECT_EQ(count, 1);
        EXPECT_EQ(decoded.type, original.type);
        EXPECT_EQ(decoded.request_id, original.request_id);
        EXPECT_EQ(decoded.payload_size, 3U);
        EXPECT_TRUE(std::equal(original.payload.begin(), original.payload.begin() + 3,
                               decoded.payload.begin()));
        EXPECT_EQ(bytes(decoded), encoded);
    }
}

TEST(Protocol, ConcatenatedFramesAndBytewiseInput) {
    auto input = bytes(message(MessageType::Hello, 1));
    const auto next = bytes(message(MessageType::Health, 2));
    input.insert(input.end(), next.begin(), next.end());
    for (bool bytewise : {false, true}) {
        FrameDecoder decoder;
        std::vector<std::uint64_t> ids;
        auto handler = [&](const Frame &f) {
            ids.push_back(f.request_id);
            return true;
        };
        if (bytewise)
            for (const auto &byte : input)
                ASSERT_EQ(decoder.feed(std::span{&byte, 1}, handler), ProtocolError::None);
        else
            ASSERT_EQ(decoder.feed(input, handler), ProtocolError::None);
        EXPECT_EQ(ids, (std::vector<std::uint64_t>{1, 2}));
        EXPECT_EQ(decoder.finish(), ProtocolError::None);
    }
}

TEST(Protocol, RejectsInvalidHeadersAndBoundedLengths) {
    auto input = bytes(message(MessageType::Hello));
    auto check = [&](std::vector<std::uint8_t> data, ProtocolError expected) {
        FrameDecoder decoder;
        EXPECT_EQ(decoder.feed(data, [](const Frame &) { return true; }), expected);
        EXPECT_EQ(decoder.finish(), expected);
        EXPECT_LE(decoder.buffered(), max_frame_size);
    };
    auto v = input;
    v[4] = 2;
    check(v, ProtocolError::UnsupportedVersion);
    v = input;
    v[5] = 255;
    check(v, ProtocolError::UnknownType);
    v = input;
    v[7] = 1;
    check(v, ProtocolError::InvalidFlags);
    v = input;
    v[3] = 15;
    check(v, ProtocolError::InvalidLength);
    v = input;
    v[2] = 0x10;
    v[3] = 1;
    check(v, ProtocolError::Oversized);
    v = input;
    v[3] = 17;
    check(v, ProtocolError::InvalidPayload);
    Frame invalid = message(MessageType::Hello);
    invalid.payload_size = 1;
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(encode_frame(invalid, out));
}

TEST(Protocol, TruncatedAtEveryByteAndMaximumEcho) {
    Frame frame = message(MessageType::Echo);
    frame.payload_size = frame.payload.size();
    std::fill(frame.payload.begin(), frame.payload.end(), 42);
    const auto input = bytes(frame);
    for (std::size_t i = 1; i < input.size(); ++i) {
        FrameDecoder decoder;
        ASSERT_EQ(decoder.feed(std::span{input}.first(i), [](const Frame &) { return true; }),
                  ProtocolError::None);
        EXPECT_EQ(decoder.finish(), ProtocolError::Truncated);
        EXPECT_LE(decoder.buffered(), max_frame_size);
    }
    FrameDecoder decoder;
    int count = 0;
    EXPECT_EQ(decoder.feed(input,
                           [&](const Frame &f) {
                               ++count;
                               return f.payload_size == frame.payload_size;
                           }),
              ProtocolError::None);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(decoder.finish(), ProtocolError::None);
}

TEST(Tcp, HandlesPartialWritesAndPeerDisconnect) {
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    Socket writer{pair[0]}, reader{pair[1]};
    set_nonblocking(writer.get());
    int size = 1024;
    ASSERT_EQ(setsockopt(writer.get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    std::vector<std::uint8_t> payload(256 * 1024, 0x5a);
    std::atomic<std::size_t> received{0};
    std::thread consumer([&] {
        std::array<std::uint8_t, 4096> chunk{};
        std::this_thread::sleep_for(10ms);
        while (received < payload.size()) {
            auto n = recv(reader.get(), chunk.data(), chunk.size(), 0);
            if (n <= 0)
                break;
            received += static_cast<std::size_t>(n);
        }
    });
    const bool ok = write_all(writer.get(), payload, 2s);
    shutdown(writer.get(), SHUT_WR);
    consumer.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(received, payload.size());
    reader = Socket{};
    EXPECT_FALSE(write_all(writer.get(), payload, 100ms));
}

TEST(Tcp, WriteTimeoutWhenPeerDoesNotRead) {
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    Socket writer{pair[0]}, reader{pair[1]};
    set_nonblocking(writer.get());
    int size = 1024;
    ASSERT_EQ(setsockopt(writer.get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    std::vector<std::uint8_t> payload(256 * 1024, 0x5a);
    EXPECT_FALSE(write_all(writer.get(), payload, 100ms));
}

TEST(Gateway, HandshakeHealthEchoAndUnsupportedOrder) {
    Gateway server{0};
    Client client{server.port()};
    Frame response;
    ASSERT_TRUE(client.exchange(message(MessageType::Hello, 11), response));
    EXPECT_EQ(response.type, MessageType::HelloAck);
    EXPECT_EQ(response.payload_size, 8U);
    EXPECT_GT(read_u64(std::span<const std::uint8_t, 8>{response.payload.data(), 8}), 0U);
    ASSERT_TRUE(client.exchange(message(MessageType::Health, 12), response));
    EXPECT_EQ(response.type, MessageType::HealthAck);
    Frame echo = message(MessageType::Echo, 13);
    echo.payload_size = 4;
    echo.payload[0] = 0;
    echo.payload[1] = 1;
    echo.payload[2] = 2;
    echo.payload[3] = 255;
    ASSERT_TRUE(client.exchange(echo, response));
    EXPECT_EQ(response.type, MessageType::EchoReply);
    EXPECT_EQ(response.payload_size, 4U);
    EXPECT_TRUE(
        std::equal(echo.payload.begin(), echo.payload.begin() + 4, response.payload.begin()));
    echo.request_id = 15;
    echo.payload_size = echo.payload.size();
    std::fill(echo.payload.begin(), echo.payload.end(), 0xa5);
    ASSERT_TRUE(client.exchange(echo, response));
    EXPECT_EQ(response.type, MessageType::EchoReply);
    EXPECT_EQ(response.payload_size, echo.payload_size);
    EXPECT_EQ(response.payload, echo.payload);
    Frame order = message(MessageType::SubmitOrder, 14);
    order.payload_size = 30;
    ASSERT_TRUE(client.exchange(order, response));
    EXPECT_EQ(response.type, MessageType::Error);
    EXPECT_EQ(response.payload[1], static_cast<std::uint8_t>(ProtocolError::UnsupportedOperation));
    server.stop();
}

TEST(Gateway, RequiresHandshakeAndEnforcesSessionLimit) {
    Gateway server{0, 1};
    Client first{server.port()};
    Frame response;
    ASSERT_TRUE(first.exchange(message(MessageType::Hello), response));
    Client second{server.port()};
    EXPECT_FALSE(second.exchange(message(MessageType::Hello), response));
    server.stop();
    Gateway another{0};
    Client uninitialized{another.port()};
    ASSERT_TRUE(uninitialized.exchange(message(MessageType::Health), response));
    EXPECT_EQ(response.type, MessageType::Error);
    EXPECT_EQ(response.payload[1], static_cast<std::uint8_t>(ProtocolError::HandshakeRequired));
}

TEST(Gateway, HandshakeViolationStopsPipelinedFrames) {
    Gateway server{0};
    Socket raw{socket(AF_INET, SOCK_STREAM, 0)};
    ASSERT_GE(raw.get(), 0);
    timeval timeout{2, 0};
    ASSERT_EQ(setsockopt(raw.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(raw.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    auto input = bytes(message(MessageType::Health, 1));
    const auto hello = bytes(message(MessageType::Hello, 2));
    input.insert(input.end(), hello.begin(), hello.end());
    ASSERT_TRUE(write_all(raw.get(), input, 1s));
    FrameDecoder decoder;
    std::vector<MessageType> replies;
    std::array<std::uint8_t, 128> buffer{};
    for (;;) {
        const auto n = recv(raw.get(), buffer.data(), buffer.size(), 0);
        ASSERT_GE(n, 0);
        if (n == 0)
            break;
        ASSERT_EQ(
            decoder.feed(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(n)},
                         [&](const Frame &frame) {
                             replies.push_back(frame.type);
                             return true;
                         }),
            ProtocolError::None);
    }
    EXPECT_EQ(replies, (std::vector<MessageType>{MessageType::Error}));
    EXPECT_EQ(decoder.finish(), ProtocolError::None);
}

TEST(Gateway, SupportsMultipleClientsAndStopDisconnectsThem) {
    Gateway server{0, 4};
    Client a{server.port()}, b{server.port()};
    Frame one, two;
    ASSERT_TRUE(a.exchange(message(MessageType::Hello), one));
    ASSERT_TRUE(b.exchange(message(MessageType::Hello), two));
    EXPECT_NE(read_u64(std::span<const std::uint8_t, 8>{one.payload.data(), 8}),
              read_u64(std::span<const std::uint8_t, 8>{two.payload.data(), 8}));
    server.stop();
    EXPECT_FALSE(a.exchange(message(MessageType::Health), one));
    EXPECT_FALSE(b.exchange(message(MessageType::Health), two));
    server.stop();
    EXPECT_THROW((Client{server.port(), 200ms}), std::runtime_error);
}

TEST(Gateway, TruncatedPeerDoesNotPreventNewSession) {
    Gateway server{0, 2};
    {
        Socket raw{socket(AF_INET, SOCK_STREAM, 0)};
        ASSERT_GE(raw.get(), 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server.port());
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(connect(raw.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
        auto frame = bytes(message(MessageType::Hello));
        ASSERT_EQ(send(raw.get(), frame.data(), 5, MSG_NOSIGNAL), 5);
    }
    Client good{server.port()};
    Frame response;
    ASSERT_TRUE(good.exchange(message(MessageType::Hello), response));
    EXPECT_EQ(response.type, MessageType::HelloAck);
}

TEST(Gateway, FragmentedAndConcatenatedFramesOverTcp) {
    Gateway server{0};
    Socket raw{socket(AF_INET, SOCK_STREAM, 0)};
    ASSERT_GE(raw.get(), 0);
    timeval timeout{2, 0};
    ASSERT_EQ(setsockopt(raw.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(raw.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    auto input = bytes(message(MessageType::Hello, 21));
    const auto health = bytes(message(MessageType::Health, 22));
    input.insert(input.end(), health.begin(), health.end());
    for (const auto &byte : input)
        ASSERT_EQ(send(raw.get(), &byte, 1, MSG_NOSIGNAL), 1);
    FrameDecoder decoder;
    std::vector<MessageType> replies;
    std::vector<std::uint64_t> ids;
    while (replies.size() < 2) {
        std::array<std::uint8_t, 128> buffer{};
        const auto n = recv(raw.get(), buffer.data(), buffer.size(), 0);
        ASSERT_GT(n, 0);
        ASSERT_EQ(
            decoder.feed(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(n)},
                         [&](const Frame &frame) {
                             replies.push_back(frame.type);
                             ids.push_back(frame.request_id);
                             return true;
                         }),
            ProtocolError::None);
    }
    EXPECT_EQ(replies, (std::vector<MessageType>{MessageType::HelloAck, MessageType::HealthAck}));
    EXPECT_EQ(ids, (std::vector<std::uint64_t>{21, 22}));
}

TEST(Gateway, IdleTimeoutReleasesSessionSlot) {
    Gateway server{0, 1, 100ms};
    Client idle{server.port()};
    Frame response;
    ASSERT_TRUE(idle.exchange(message(MessageType::Hello), response));
    std::this_thread::sleep_for(250ms);
    EXPECT_FALSE(idle.exchange(message(MessageType::Health), response));
    Client next{server.port()};
    ASSERT_TRUE(next.exchange(message(MessageType::Hello), response));
    EXPECT_EQ(response.type, MessageType::HelloAck);
}

TEST(Gateway, MalformedVersionReceivesErrorAndCloses) {
    Gateway server{0};
    Socket raw{socket(AF_INET, SOCK_STREAM, 0)};
    ASSERT_GE(raw.get(), 0);
    timeval timeout{2, 0};
    ASSERT_EQ(setsockopt(raw.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(raw.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    auto input = bytes(message(MessageType::Hello));
    input[4] = 99;
    ASSERT_EQ(send(raw.get(), input.data(), input.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(input.size()));
    FrameDecoder decoder;
    bool received = false;
    Frame response;
    while (!received) {
        std::array<std::uint8_t, 128> buffer{};
        const auto n = recv(raw.get(), buffer.data(), buffer.size(), 0);
        ASSERT_GT(n, 0);
        ASSERT_EQ(
            decoder.feed(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(n)},
                         [&](const Frame &frame) {
                             response = frame;
                             received = true;
                             return true;
                         }),
            ProtocolError::None);
    }
    EXPECT_EQ(response.type, MessageType::Error);
    EXPECT_EQ(response.request_id, 0U);
    EXPECT_EQ(response.payload[1], static_cast<std::uint8_t>(ProtocolError::UnsupportedVersion));
    std::uint8_t byte = 0;
    EXPECT_EQ(recv(raw.get(), &byte, 1, 0), 0);
}
