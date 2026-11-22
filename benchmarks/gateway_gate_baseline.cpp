#include "chronos/gateway/tcp.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace chronos;
using namespace std::chrono_literals;
namespace {
void check(bool ok) {
    if (!ok)
        throw std::runtime_error("gateway benchmark verification failed");
}
gateway::Frame order(std::uint64_t id, bool sell, bool market) {
    gateway::Frame f;
    f.type = gateway::MessageType::SubmitOrder;
    f.request_id = id;
    f.payload_size = 30;
    gateway::write_u64(std::span<std::uint8_t, 8>{f.payload.data(), 8}, id);
    f.payload[11] = 1;
    f.payload[12] = sell ? 1 : 0;
    f.payload[13] = market ? 1 : 0;
    gateway::write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 14, 8}, market ? 0 : 100);
    gateway::write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 22, 8}, 1);
    return f;
}
void receive(gateway::Client &client, std::uint8_t kind) {
    gateway::Frame f;
    check(client.receive(f) && f.type == gateway::MessageType::Execution && f.payload[44] == kind);
}
std::int64_t sample(exchange::AllocationMode mode, std::string_view workload) {
    gateway::Gateway server{0, 3, 5s, {}, {}, mode};
    gateway::Client seller{server.port()}, buyer{server.port()};
    gateway::Frame hello, reply;
    hello.type = gateway::MessageType::Hello;
    check(seller.exchange(hello, reply) && reply.type == gateway::MessageType::HelloAck);
    check(buyer.exchange(hello, reply) && reply.type == gateway::MessageType::HelloAck);
    if (workload != "noncrossing") {
        check(seller.send(order(1, true, false)));
        receive(seller, 0);
        receive(seller, 3);
    }
    gateway::Frame request;
    if (workload == "cancel") {
        request.type = gateway::MessageType::CancelOrder;
        request.request_id = 2;
        request.payload_size = 8;
        gateway::write_u64(std::span<std::uint8_t, 8>{request.payload.data(), 8}, 1);
    } else
        request = order(2, false, workload == "crossing");
    auto &client = workload == "cancel" ? seller : buyer;
    const auto begin = std::chrono::steady_clock::now();
    check(client.send(request));
    if (workload == "cancel")
        receive(seller, 4);
    else {
        receive(buyer, 0);
        receive(buyer, workload == "crossing" ? 2 : 3);
        if (workload == "crossing")
            receive(seller, 2);
    }
    const auto end = std::chrono::steady_clock::now();
    server.stop();
    check(server.trade_count() == (workload == "crossing" ? 1U : 0U));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}
} // namespace

int main() {
    try {
        std::cout << "mode,workload,sample,latency_ns\n";
        for (const auto mode :
             {exchange::AllocationMode::Shared, exchange::AllocationMode::Exclusive})
            for (const auto workload : {"noncrossing", "cancel", "crossing"})
                for (int i = -2; i < 21; ++i) {
                    const auto ns = sample(mode, workload);
                    if (i >= 0)
                        std::cout << (mode == exchange::AllocationMode::Shared ? "shared"
                                                                               : "exclusive")
                                  << ',' << workload << ',' << i << ',' << ns << '\n';
                }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
