#include "chronos/exchange/exchange.hpp"
#include "chronos/gateway/tcp.hpp"
#include "chronos/matching/matching_engine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string_view>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0;
thread_local std::size_t allocated_bytes = 0;
} // namespace
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) {
        if (counting) {
            ++allocations;
            allocated_bytes += size;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace chronos;
using Clock = std::chrono::steady_clock;
using gateway::Frame;
void check(bool value) {
    if (!value)
        throw std::runtime_error("benchmark verification failed");
}
Frame make_order(std::uint64_t id, bool buy, bool market) {
    Frame f;
    f.type = gateway::MessageType::SubmitOrder;
    f.request_id = id;
    f.payload_size = 30;
    gateway::write_u64(std::span<std::uint8_t, 8>{f.payload.data(), 8}, id);
    f.payload[11] = 1;
    f.payload[12] = buy ? 0 : 1;
    f.payload[13] = market ? 1 : 0;
    gateway::write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 14, 8},
                       market ? 0 : (buy ? 100 : 110));
    gateway::write_u64(std::span<std::uint8_t, 8>{f.payload.data() + 22, 8}, 1);
    return f;
}
void drain(exchange::Exchange &ex) {
    while (ex.front(1))
        ex.pop(1);
}
struct Measurement {
    std::int64_t ns;
    std::size_t count, bytes;
};
Measurement run(std::string_view mode, bool fill, std::size_t n) {
    const core::SymbolId symbol{1};
    if (mode == "engine") {
        matching::MatchingEngine engine{symbol};
        for (std::size_t i = 1; i <= n; ++i)
            check(engine
                      .submit({core::OrderId{i}, symbol, core::Side::Sell, core::OrderType::Limit,
                               110, 1})
                      .error == matching::Error::None);
        allocations = allocated_bytes = 0;
        counting = true;
        const auto start = Clock::now();
        auto out = engine.submit({core::OrderId{n + 1}, symbol, core::Side::Buy,
                                  fill ? core::OrderType::Market : core::OrderType::Limit,
                                  fill ? 0 : 100, 1});
        const auto end = Clock::now();
        counting = false;
        check(out.error == matching::Error::None && out.trades.size() == (fill ? 1U : 0U));
        if (fill)
            check(out.trades[0].price == 110 && out.trades[0].quantity == 1);
        else
            check(engine.find(core::OrderId{n + 1}).has_value());
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
                allocations, allocated_bytes};
    }
    if (mode == "exchange") {
        exchange::Exchange ex{1, n + 2, 128, n + 2};
        check(ex.open_session(1));
        for (std::size_t i = 1; i <= n; ++i) {
            check(ex.submit(1, i, i, symbol, core::Side::Sell, core::OrderType::Limit, 110, 1) ==
                  exchange::Result::Accepted);
            drain(ex);
        }
        allocations = allocated_bytes = 0;
        counting = true;
        const auto start = Clock::now();
        auto result =
            ex.submit(1, n + 1, n + 1, symbol, core::Side::Buy,
                      fill ? core::OrderType::Market : core::OrderType::Limit, fill ? 0 : 100, 1);
        const auto end = Clock::now();
        counting = false;
        check(result == exchange::Result::Accepted);
        check(ex.trade_count() == (fill ? 1U : 0U));
        if (fill)
            check(ex.trade(0)->price == 110 && ex.trade(0)->quantity == 1);
        else
            check(ex.find(1, n + 1).has_value());
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
                allocations, allocated_bytes};
    }
    const auto allocation = mode == "tcp_exclusive" ? exchange::AllocationMode::Exclusive
                                                    : exchange::AllocationMode::Shared;
    gateway::Gateway server{0, 1, std::chrono::seconds(30), {}, {}, allocation};
    gateway::Client client{server.port(), std::chrono::seconds(10)};
    Frame hello, reply;
    hello.type = gateway::MessageType::Hello;
    check(client.exchange(hello, reply) && reply.type == gateway::MessageType::HelloAck);
    for (std::size_t i = 1; i <= n; ++i) {
        check(client.send(make_order(i, false, false)));
        check(client.receive(reply) && reply.type == gateway::MessageType::Execution &&
              reply.payload[44] == 0);
        check(client.receive(reply) && reply.type == gateway::MessageType::Execution &&
              reply.payload[44] == 3);
    }
    const auto request = make_order(n + 1, true, fill);
    const auto start = Clock::now();
    check(client.send(request));
    bool accepted = false, resting_fill = false;
    for (;;) {
        check(client.receive(reply) && reply.type == gateway::MessageType::Execution);
        const auto kind = reply.payload[44];
        if (reply.request_id == request.request_id && kind == 0)
            accepted = true;
        if (fill && reply.request_id != request.request_id && kind == 2)
            resting_fill = true;
        if (reply.request_id == request.request_id && kind == (fill ? 2 : 3))
            break;
    }
    const auto end = Clock::now();
    check(accepted && (!fill || resting_fill));
    if (fill)
        check(gateway::read_u64(std::span<const std::uint8_t, 8>{reply.payload.data() + 45, 8}) ==
              110);
    server.stop();
    check(server.trade_count() == (fill ? 1U : 0U));
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(), 0, 0};
}
} // namespace
int main() {
    try {
        std::cout << "mode,workload,background,sample,latency_ns,allocations,bytes\n";
        for (const auto n : {10U, 30U, 100U})
            for (const bool fill : {false, true})
                for (const auto mode : {"engine", "exchange", "tcp", "tcp_exclusive"})
                    for (int sample = -1; sample < 9; ++sample) {
                        const auto m = run(mode, fill, n);
                        if (sample >= 0)
                            std::cout << mode << ',' << (fill ? "single_fill" : "noncrossing")
                                      << ',' << n << ',' << sample << ',' << m.ns << ',' << m.count
                                      << ',' << m.bytes << '\n';
                    }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
