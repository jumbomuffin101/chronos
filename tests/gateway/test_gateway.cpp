#include "chronos/gateway/ingress.hpp"
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
struct TraceCapture {
    std::array<TraceEvent, 128> events{};
    std::atomic<std::size_t> count{0};
    static void record(void *context, const TraceEvent &event) noexcept {
        auto &capture = *static_cast<TraceCapture *>(context);
        const auto index = capture.count.fetch_add(1, std::memory_order_relaxed);
        if (index < capture.events.size())
            capture.events[index] = event;
    }
    bool contains(TracePoint point, std::uint64_t request, int value = -1) const {
        const auto end = std::min(count.load(), events.size());
        return std::any_of(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(end),
                           [&](const TraceEvent &event) {
                               return event.point == point && event.request_id == request &&
                                      (value < 0 || event.value == value);
                           });
    }
};
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
Frame order(std::uint64_t request, std::uint64_t client, std::uint32_t symbol, std::uint8_t side,
            std::uint8_t type, std::uint64_t price, std::uint64_t qty) {
    Frame f = message(MessageType::SubmitOrder, request);
    f.payload_size = 30;
    write_u64(std::span<std::uint8_t, 8>{f.payload.data(), 8}, client);
    f.payload[8] = static_cast<std::uint8_t>(symbol >> 24);
    f.payload[9] = static_cast<std::uint8_t>(symbol >> 16);
    f.payload[10] = static_cast<std::uint8_t>(symbol >> 8);
    f.payload[11] = static_cast<std::uint8_t>(symbol);
    f.payload[12] = side;
    f.payload[13] = type;
    write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 14, 8}, price);
    write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 22, 8}, qty);
    return f;
}
std::uint64_t field(const Frame &f, std::size_t offset) {
    return read_u64(std::span<const std::uint8_t, 8>{f.payload.data() + offset, 8});
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
    EXPECT_EQ(encoded[4], protocol_version);
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

TEST(Ingress, SaturationRejectsWithoutDiscardingAdmittedCommands) {
    chronos::gateway::detail::Ingress ingress;
    for (std::size_t i = 0; i < ingress.capacity; ++i)
        ASSERT_TRUE(ingress.push(7, message(MessageType::Health, i)));
    EXPECT_FALSE(ingress.push(8, message(MessageType::Health, 999)));
    ASSERT_EQ(ingress.commands().size(), ingress.capacity);
    for (std::size_t i = 0; i < ingress.capacity; ++i) {
        EXPECT_EQ(ingress.commands()[i].session, 7U);
        EXPECT_EQ(ingress.commands()[i].frame.request_id, i);
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
    v[4] = 1;
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

TEST(Gateway, HandshakeHealthEchoAndInvalidOrder) {
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
    EXPECT_EQ(response.type, MessageType::OrderAck);
    EXPECT_NE(response.payload[8], 0);
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

TEST(GatewayExchange, RoutesPartialAndCompleteFillsToBothClients) {
    Gateway server{0};
    Client seller{server.port()}, buyer{server.port()};
    Frame f;
    ASSERT_TRUE(seller.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(buyer.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(seller.send(order(11, 100, 1, 1, 0, 100, 5)));
    ASSERT_TRUE(seller.receive(f));
    ASSERT_EQ(f.type, MessageType::Execution);
    EXPECT_EQ(f.payload[44], 0);
    const auto sell_id = field(f, 24);
    ASSERT_TRUE(seller.receive(f));
    EXPECT_EQ(f.payload[44], 3);
    ASSERT_TRUE(buyer.send(order(22, 200, 1, 0, 1, 0, 3)));
    ASSERT_TRUE(buyer.receive(f));
    EXPECT_EQ(f.payload[44], 0);
    EXPECT_NE(field(f, 24), sell_id);
    ASSERT_TRUE(buyer.receive(f));
    EXPECT_EQ(f.payload[44], 2);
    EXPECT_EQ(field(f, 45), 100U);
    EXPECT_EQ(field(f, 53), 3U);
    ASSERT_TRUE(seller.receive(f));
    EXPECT_EQ(f.payload[44], 1);
    EXPECT_EQ(field(f, 61), 2U);
    Frame cancel = message(MessageType::CancelOrder, 33);
    cancel.payload_size = 8;
    write_u64(std::span<std::uint8_t, 8>{cancel.payload.data(), 8}, 100);
    ASSERT_TRUE(seller.send(cancel));
    ASSERT_TRUE(seller.receive(f));
    EXPECT_EQ(f.payload[44], 4);
    EXPECT_EQ(f.request_id, 33U);
    server.stop();
    ASSERT_EQ(server.trade_count(), 1U);
    ASSERT_TRUE(server.trade(0));
    EXPECT_EQ(server.trade(0)->price, 100);
    EXPECT_EQ(server.trade(0)->quantity, 3U);
}

TEST(GatewayExchange, RejectsUnauthorizedCancellationAndInvalidFields) {
    Gateway server{0};
    Client owner{server.port()}, other{server.port()};
    Frame f;
    ASSERT_TRUE(owner.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(other.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(owner.send(order(10, 123, 9, 1, 0, 100, 4)));
    ASSERT_TRUE(owner.receive(f));
    ASSERT_TRUE(owner.receive(f));
    Frame cancel = message(MessageType::CancelOrder, 11);
    cancel.payload_size = 8;
    write_u64(std::span<std::uint8_t, 8>{cancel.payload.data(), 8}, 123);
    ASSERT_TRUE(other.exchange(cancel, f));
    EXPECT_EQ(f.type, MessageType::OrderAck);
    EXPECT_EQ(f.payload[8], 5);
    ASSERT_TRUE(owner.exchange(order(12, 123, 9, 1, 0, 100, 4), f));
    EXPECT_EQ(f.payload[8], 3);
    ASSERT_TRUE(other.exchange(order(13, 456, 9, 99, 0, 100, 1), f));
    EXPECT_EQ(f.payload[8], 6);
    ASSERT_TRUE(owner.send(cancel));
    ASSERT_TRUE(owner.receive(f));
    EXPECT_EQ(f.type, MessageType::Execution);
    EXPECT_EQ(f.payload[44], 4);
    EXPECT_EQ(field(f, 53), 4U);
    EXPECT_EQ(field(f, 61), 0U);
}

TEST(GatewayExchange, ReportsFullRestingFillAndMarketRemainder) {
    Gateway server{0};
    Client seller{server.port()}, buyer{server.port()};
    Frame f;
    ASSERT_TRUE(seller.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(buyer.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(seller.send(order(1, 1, 1, 1, 0, 77, 2)));
    ASSERT_TRUE(seller.receive(f));
    ASSERT_TRUE(seller.receive(f));
    ASSERT_TRUE(buyer.send(order(2, 2, 1, 0, 1, 0, 5)));
    ASSERT_TRUE(buyer.receive(f));
    EXPECT_EQ(f.payload[44], 0);
    ASSERT_TRUE(buyer.receive(f));
    EXPECT_EQ(f.payload[44], 1);
    EXPECT_EQ(field(f, 61), 3U);
    ASSERT_TRUE(buyer.receive(f));
    EXPECT_EQ(f.payload[44], 5);
    EXPECT_EQ(field(f, 61), 3U);
    ASSERT_TRUE(seller.receive(f));
    EXPECT_EQ(f.payload[44], 2);
    EXPECT_EQ(field(f, 53), 2U);
    EXPECT_EQ(field(f, 45), 77U);
}

TEST(GatewayExchange, DisconnectRetainsOwnerReportAndTradeAudit) {
    Gateway server{0};
    std::uint64_t seller_session = 0;
    {
        Client seller{server.port()};
        Frame f;
        ASSERT_TRUE(seller.exchange(message(MessageType::Hello), f));
        seller_session = field(f, 0);
        ASSERT_TRUE(seller.send(order(1, 9, 1, 1, 0, 50, 3)));
        ASSERT_TRUE(seller.receive(f));
        ASSERT_TRUE(seller.receive(f));
    }
    std::this_thread::sleep_for(100ms);
    Client buyer{server.port()};
    Frame f;
    ASSERT_TRUE(buyer.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(buyer.send(order(2, 10, 1, 0, 1, 0, 3)));
    ASSERT_TRUE(buyer.receive(f));
    ASSERT_TRUE(buyer.receive(f));
    server.stop();
    ASSERT_EQ(server.trade_count(), 1U);
    const auto report = server.pending_report(seller_session);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->kind, chronos::exchange::ReportKind::Filled);
    EXPECT_EQ(report->quantity, 3U);
}

TEST(GatewayExchange, TracesOrderAndReportsWithConfiguredSocketOptions) {
    TraceCapture server_trace, client_trace;
    Gateway server{0, 2, 5s, {.trace = {TraceCapture::record, &server_trace}}};
    Client client{server.port(), 2s, {.trace = {TraceCapture::record, &client_trace}}};
    Frame f;
    ASSERT_TRUE(client.exchange(message(MessageType::Hello), f));
    ASSERT_TRUE(client.send(order(27, 20, 1, 0, 0, 100, 1)));
    ASSERT_TRUE(client.receive(f));
    ASSERT_TRUE(client.receive(f));
    EXPECT_EQ(f.payload[44], 3);
    server.stop();
    EXPECT_TRUE(server_trace.contains(TracePoint::SocketConfigured, 0, 1));
    EXPECT_TRUE(client_trace.contains(TracePoint::SocketConfigured, 0, 0));
    for (const auto point :
         {TracePoint::FrameReceived, TracePoint::CommandEnqueued, TracePoint::Dispatch,
          TracePoint::TransactionStart, TracePoint::TransactionEnd, TracePoint::ReportQueued,
          TracePoint::SocketWrite})
        EXPECT_TRUE(server_trace.contains(point, 27));
    EXPECT_TRUE(client_trace.contains(TracePoint::ClientSendStart, 27));
    EXPECT_TRUE(client_trace.contains(TracePoint::ClientSendEnd, 27, 1));
    EXPECT_TRUE(client_trace.contains(TracePoint::ClientReceived, 27, 3));
}

TEST(Gateway, CanDisableNoDelayForControlledComparison) {
    TraceCapture capture;
    Gateway server{0, 1, 5s, {false, {TraceCapture::record, &capture}}};
    Client client{server.port()};
    Frame reply;
    ASSERT_TRUE(client.exchange(message(MessageType::Hello), reply));
    server.stop();
    EXPECT_TRUE(capture.contains(TracePoint::SocketConfigured, 0, 0));
}
