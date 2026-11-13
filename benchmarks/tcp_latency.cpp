#include "chronos/gateway/tcp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace chronos::gateway;
using namespace std::chrono_literals;
void check(bool value) {
    if (!value)
        throw std::runtime_error("latency verification failed");
}
struct Trace {
    std::vector<TraceEvent> events = std::vector<TraceEvent>(100000);
    std::atomic<std::size_t> count{0};
    static void record(void *context, const TraceEvent &event) noexcept {
        auto &self = *static_cast<Trace *>(context);
        const auto index = self.count.fetch_add(1, std::memory_order_relaxed);
        if (index < self.events.size())
            self.events[index] = event;
    }
};
Frame order(std::uint64_t id, bool buy, bool market, std::uint64_t qty) {
    Frame f;
    f.type = MessageType::SubmitOrder;
    f.request_id = id;
    f.payload_size = 30;
    write_u64(std::span<std::uint8_t, 8>{f.payload.data(), 8}, id);
    f.payload[11] = 1;
    f.payload[12] = buy ? 0 : 1;
    f.payload[13] = market ? 1 : 0;
    write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 14, 8}, market ? 0 : (buy ? 100 : 110));
    write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 22, 8}, qty);
    return f;
}
Frame cancel(std::uint64_t request, std::uint64_t client) {
    Frame f;
    f.type = MessageType::CancelOrder;
    f.request_id = request;
    f.payload_size = 8;
    write_u64(std::span<std::uint8_t, 8>{f.payload.data(), 8}, client);
    return f;
}
std::int64_t find(const Trace &t, TracePoint p, std::uint64_t id, int value = -1) {
    const auto count = std::min(t.count.load(std::memory_order_relaxed), t.events.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto &e = t.events[i];
        if (e.point == p && e.request_id == id && (value < 0 || e.value == value))
            return e.monotonic_ns;
    }
    throw std::runtime_error("missing timestamp");
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 6)
            throw std::runtime_error(
                "usage: tcp_latency SERVER_NODELAY CLIENT_NODELAY N FILL SAMPLES");
        const bool server_nd = std::atoi(argv[1]) != 0;
        const bool client_nd = std::atoi(argv[2]) != 0;
        const auto background = std::atoi(argv[3]);
        const bool fill = std::atoi(argv[4]) != 0;
        const auto samples = std::atoi(argv[5]);
        check(background > 0 && background <= 100 && samples > 0 && samples <= 150);
        Trace t;
        const TraceSink sink{Trace::record, &t};
        Gateway server{0, 1, 30s, {server_nd, sink}};
        Client client{server.port(), 10s, {client_nd, sink}};
        Frame f, reply;
        f.type = MessageType::Hello;
        check(client.exchange(f, reply) && reply.type == MessageType::HelloAck);
        std::uint64_t id = 1;
        for (int i = 0; i < background; ++i, ++id) {
            check(client.send(order(id, false, false, i == 0 ? 1000 : 1)));
            check(client.receive(reply) && reply.payload[44] == 0);
            check(client.receive(reply) && reply.payload[44] == 3);
        }
        std::vector<std::uint64_t> requests;
        for (int i = -5; i < samples; ++i) {
            const auto request = id++;
            check(client.send(order(request, true, fill, 1)));
            bool accepted = false, resting = false;
            for (;;) {
                check(client.receive(reply) && reply.type == MessageType::Execution);
                if (reply.request_id == request && reply.payload[44] == 0)
                    accepted = true;
                if (fill && reply.request_id != request && reply.payload[44] == 1)
                    resting = true;
                if (reply.request_id == request && reply.payload[44] == (fill ? 2 : 3))
                    break;
            }
            check(accepted && (!fill || resting));
            if (fill)
                check(read_u64(std::span<const std::uint8_t, 8>{reply.payload.data() + 45, 8}) ==
                      110);
            if (i >= 0)
                requests.push_back(request);
            if (!fill) {
                const auto cancel_id = id++;
                check(client.send(cancel(cancel_id, request)));
                check(client.receive(reply) && reply.request_id == cancel_id &&
                      reply.payload[44] == 4);
            }
        }
        server.stop();
        check(t.count.load() < t.events.size());
        std::cout << "server_nd,client_nd,background,fill,sample,send_ns,network_in_ns,queue_ns,"
                     "transaction_ns,report_queue_ns,write_wait_ns,delivery_ns,total_ns\n";
        int sample = 0;
        for (const auto request : requests) {
            const auto a = find(t, TracePoint::ClientSendStart, request);
            const auto b = find(t, TracePoint::ClientSendEnd, request);
            const auto c = find(t, TracePoint::FrameReceived, request);
            const auto d = find(t, TracePoint::CommandEnqueued, request);
            const auto e = find(t, TracePoint::Dispatch, request);
            const auto x = find(t, TracePoint::TransactionStart, request);
            const auto y = find(t, TracePoint::TransactionEnd, request);
            const auto q = find(t, TracePoint::ReportQueued, request, fill ? 2 : 3);
            const auto w = find(t, TracePoint::SocketWrite, request, fill ? 2 : 3);
            const auto z = find(t, TracePoint::ClientReceived, request, fill ? 2 : 3);
            if (!(a <= b && c <= d && d <= e && e <= x && x <= y && y <= q && q <= w && w <= z)) {
                std::cerr << "timestamp ordering for " << request << ": " << a << ' ' << b << ' '
                          << c << ' ' << d << ' ' << e << ' ' << x << ' ' << y << ' ' << q << ' '
                          << w << ' ' << z << '\n';
                throw std::runtime_error("timestamp ordering failed");
            }
            std::cout << server_nd << ',' << client_nd << ',' << background << ',' << fill << ','
                      << sample++ << ',' << b - a << ',' << c - b << ',' << e - d << ',' << y - x
                      << ',' << q - y << ',' << w - q << ',' << z - w << ',' << z - a << '\n';
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
