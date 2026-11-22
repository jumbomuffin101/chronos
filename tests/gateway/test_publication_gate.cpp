#include "chronos/exchange/live_owner.hpp"
#include "chronos/gateway/tcp.hpp"

#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>

using namespace chronos;
using namespace std::chrono_literals;

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    bool timed_out = false;
    bool abort = false;
    static bool wait(void *context) {
        auto &gate = *static_cast<Gate *>(context);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        if (!gate.changed.wait_for(lock, 5s, [&] { return gate.release; }))
            gate.timed_out = true;
        return !gate.abort;
    }
    void open() {
        std::lock_guard lock(mutex);
        release = true;
        changed.notify_all();
    }
};
gateway::Frame message(gateway::MessageType type, std::uint64_t request) {
    gateway::Frame frame;
    frame.type = type;
    frame.request_id = request;
    return frame;
}
gateway::Frame limit(std::uint64_t request, std::uint64_t client, bool sell = false) {
    auto frame = message(gateway::MessageType::SubmitOrder, request);
    frame.payload_size = 30;
    gateway::write_u64(std::span<std::uint8_t, 8>{frame.payload.data(), 8}, client);
    frame.payload[11] = 1;
    frame.payload[12] = sell ? 1 : 0;
    gateway::write_u64(std::span<std::uint8_t, 8>{frame.payload.data() + 14, 8}, 100);
    gateway::write_u64(std::span<std::uint8_t, 8>{frame.payload.data() + 22, 8}, 1);
    return frame;
}
struct Fault {
    int calls = 0;
    bool abort = false;
    static void before(void *context) {
        auto &fault = *static_cast<Fault *>(context);
        if (++fault.calls == 2)
            throw std::bad_alloc();
    }
    static bool after(void *context) {
        auto &fault = *static_cast<Fault *>(context);
        if (++fault.calls != 2)
            return true;
        if (fault.abort)
            return false;
        throw std::bad_alloc();
    }
};
struct ResetHooks {
    ~ResetHooks() {
        exchange::LiveExchangeOwner::test_before_prepare = nullptr;
        exchange::LiveExchangeOwner::test_before_install = nullptr;
        exchange::LiveExchangeOwner::test_before_install_context = nullptr;
        gateway::detail::test_before_output_reservation = nullptr;
        gateway::detail::test_output_reservation_context = nullptr;
        gateway::detail::test_before_output_capacity_check = nullptr;
        gateway::detail::test_output_capacity_context = nullptr;
    }
};
struct StopTrace {
    std::mutex mutex;
    std::condition_variable changed;
    bool requested = false;
    static void record(void *context, const gateway::TraceEvent &event) noexcept {
        if (event.point != gateway::TracePoint::StopRequested)
            return;
        auto &trace = *static_cast<StopTrace *>(context);
        std::lock_guard lock(trace.mutex);
        trace.requested = true;
        trace.changed.notify_all();
    }
};
struct FillOutput {
    std::size_t size;
    bool injected = false;
    static void fill(void *context, std::vector<std::uint8_t> &output) {
        auto &self = *static_cast<FillOutput *>(context);
        if (!self.injected) {
            self.injected = true;
            output.assign(self.size, 0);
        }
    }
};
} // namespace

TEST(GatewayPublicationGate, PreparedOrderIsInvisibleUntilInstallation) {
    Gate gate;
    exchange::LiveExchangeOwner::test_before_install_context = &gate;
    exchange::LiveExchangeOwner::test_before_install = &Gate::wait;
    gateway::Gateway server{0, 3, 5s, {}, {}, exchange::AllocationMode::Exclusive};
    gateway::Client trader{server.port(), 100ms}, subscriber{server.port(), 100ms};
    gateway::Frame frame;
    ASSERT_TRUE(trader.exchange(message(gateway::MessageType::Hello, 1), frame));
    ASSERT_TRUE(subscriber.exchange(message(gateway::MessageType::Hello, 2), frame));
    auto subscribe = message(gateway::MessageType::Subscribe, 3);
    subscribe.payload_size = 4;
    subscribe.payload[3] = 1;
    ASSERT_TRUE(subscriber.exchange(subscribe, frame));
    ASSERT_EQ(frame.type, gateway::MessageType::MarketAck);
    ASSERT_TRUE(subscriber.receive(frame));
    ASSERT_EQ(frame.type, gateway::MessageType::MarketData);
    auto order = message(gateway::MessageType::SubmitOrder, 4);
    order.payload_size = 30;
    gateway::write_u64(std::span<std::uint8_t, 8>{order.payload.data(), 8}, 9);
    order.payload[11] = 1;
    order.payload[12] = 0;
    order.payload[13] = 0;
    gateway::write_u64(std::span<std::uint8_t, 8>{order.payload.data() + 14, 8}, 100);
    gateway::write_u64(std::span<std::uint8_t, 8>{order.payload.data() + 22, 8}, 1);
    ASSERT_TRUE(trader.send(order));
    {
        std::unique_lock lock(gate.mutex);
        ASSERT_TRUE(gate.changed.wait_for(lock, 2s, [&] { return gate.entered; }));
    }
    // The owner has completed preparation and is blocked immediately before
    // installation. These bounded receives check the actual TCP endpoints.
    EXPECT_FALSE(trader.receive(frame));
    EXPECT_FALSE(subscriber.receive(frame));
    gate.open();
    ASSERT_TRUE(trader.receive(frame));
    EXPECT_EQ(frame.type, gateway::MessageType::Execution);
    EXPECT_EQ(frame.payload[44], 0);
    ASSERT_TRUE(trader.receive(frame));
    EXPECT_EQ(frame.payload[44], 3);
    ASSERT_TRUE(subscriber.receive(frame));
    EXPECT_EQ(frame.type, gateway::MessageType::MarketData);
    EXPECT_EQ(frame.payload[12], 1);
    server.stop();
    EXPECT_FALSE(gate.timed_out);
    exchange::LiveExchangeOwner::test_before_install = nullptr;
    exchange::LiveExchangeOwner::test_before_install_context = nullptr;
}

TEST(GatewayPublicationGate, AbortAndAllocationFailuresDoNotConsumeIds) {
    for (int kind = 0; kind < 4; ++kind) {
        Fault fault;
        fault.abort = kind == 2;
        ResetHooks reset;
        exchange::LiveExchangeOwner::test_before_install_context = &fault;
        if (kind == 0)
            exchange::LiveExchangeOwner::test_before_prepare = &Fault::before;
        else if (kind < 3)
            exchange::LiveExchangeOwner::test_before_install = &Fault::after;
        else {
            gateway::detail::test_before_output_reservation = &Fault::before;
            gateway::detail::test_output_reservation_context = &fault;
        }
        gateway::Gateway server{0, 4, 5s, {}, {}, exchange::AllocationMode::Exclusive};
        gateway::Client first{server.port()}, failed{server.port()}, retry{server.port()};
        gateway::Frame frame;
        ASSERT_TRUE(first.exchange(message(gateway::MessageType::Hello, 1), frame));
        ASSERT_TRUE(failed.exchange(message(gateway::MessageType::Hello, 1), frame));
        ASSERT_TRUE(retry.exchange(message(gateway::MessageType::Hello, 1), frame));
        ASSERT_TRUE(first.send(limit(10, 10)));
        ASSERT_TRUE(first.receive(frame));
        ASSERT_EQ(frame.type, gateway::MessageType::Execution);
        const auto first_id =
            gateway::read_u64(std::span<const std::uint8_t, 8>{frame.payload.data() + 24, 8});
        ASSERT_TRUE(first.receive(frame));
        ASSERT_TRUE(failed.send(limit(20, 20)));
        EXPECT_FALSE(failed.receive(frame));
        ASSERT_TRUE(server.running());
        ASSERT_TRUE(retry.send(limit(20, 20)));
        ASSERT_TRUE(retry.receive(frame));
        ASSERT_EQ(frame.type, gateway::MessageType::Execution);
        EXPECT_EQ(gateway::read_u64(std::span<const std::uint8_t, 8>{frame.payload.data() + 24, 8}),
                  first_id + 1);
        ASSERT_TRUE(retry.receive(frame));
        server.stop();
        EXPECT_EQ(server.trade_count(), 0U);
    }
}

TEST(GatewayPublicationGate, AbortedCancellationLeavesRestingOrderExecutable) {
    Fault fault;
    fault.abort = true;
    ResetHooks reset;
    exchange::LiveExchangeOwner::test_before_install_context = &fault;
    exchange::LiveExchangeOwner::test_before_install = &Fault::after;
    gateway::Gateway server{0, 3, 5s, {}, {}, exchange::AllocationMode::Exclusive};
    gateway::Client seller{server.port()}, buyer{server.port()};
    gateway::Frame frame;
    ASSERT_TRUE(seller.exchange(message(gateway::MessageType::Hello, 1), frame));
    ASSERT_TRUE(buyer.exchange(message(gateway::MessageType::Hello, 1), frame));
    ASSERT_TRUE(seller.send(limit(1, 1, true)));
    ASSERT_TRUE(seller.receive(frame));
    ASSERT_TRUE(seller.receive(frame));
    auto cancel = message(gateway::MessageType::CancelOrder, 2);
    cancel.payload_size = 8;
    gateway::write_u64(std::span<std::uint8_t, 8>{cancel.payload.data(), 8}, 1);
    ASSERT_TRUE(seller.send(cancel));
    EXPECT_FALSE(seller.receive(frame));
    ASSERT_TRUE(buyer.send(limit(2, 2)));
    ASSERT_TRUE(buyer.receive(frame));
    ASSERT_TRUE(buyer.receive(frame));
    EXPECT_EQ(frame.payload[44], 2);
    server.stop();
    ASSERT_EQ(server.trade_count(), 1U);
}

TEST(GatewayPublicationGate, DisconnectDuringPreparationRetainsInstalledOrder) {
    Gate gate;
    ResetHooks reset;
    exchange::LiveExchangeOwner::test_before_install_context = &gate;
    exchange::LiveExchangeOwner::test_before_install = &Gate::wait;
    gateway::Gateway server{0, 3, 5s, {}, {}, exchange::AllocationMode::Exclusive};
    auto seller = std::make_unique<gateway::Client>(server.port());
    gateway::Client buyer{server.port()};
    gateway::Frame frame;
    ASSERT_TRUE(seller->exchange(message(gateway::MessageType::Hello, 1), frame));
    ASSERT_TRUE(buyer.exchange(message(gateway::MessageType::Hello, 1), frame));
    ASSERT_TRUE(seller->send(limit(1, 1, true)));
    {
        std::unique_lock lock(gate.mutex);
        ASSERT_TRUE(gate.changed.wait_for(lock, 2s, [&] { return gate.entered; }));
    }
    seller.reset();
    gate.open();
    ASSERT_TRUE(buyer.send(limit(2, 2)));
    ASSERT_TRUE(buyer.receive(frame));
    ASSERT_TRUE(buyer.receive(frame));
    EXPECT_EQ(frame.payload[44], 2);
    server.stop();
    EXPECT_EQ(server.trade_count(), 1U);
    EXPECT_FALSE(gate.timed_out);
}

TEST(GatewayPublicationGate, DisconnectAfterInstallationDoesNotUndoOrder) {
    gateway::Gateway server{0, 3, 5s, {}, {}, exchange::AllocationMode::Exclusive};
    gateway::Client buyer{server.port()};
    gateway::Frame frame;
    ASSERT_TRUE(buyer.exchange(message(gateway::MessageType::Hello, 1), frame));
    {
        gateway::Client seller{server.port()};
        ASSERT_TRUE(seller.exchange(message(gateway::MessageType::Hello, 1), frame));
        ASSERT_TRUE(seller.send(limit(1, 1, true)));
        ASSERT_TRUE(seller.receive(frame));
        EXPECT_EQ(frame.payload[44], 0);
        ASSERT_TRUE(seller.receive(frame));
        EXPECT_EQ(frame.payload[44], 3);
    }
    ASSERT_TRUE(buyer.send(limit(2, 2)));
    ASSERT_TRUE(buyer.receive(frame));
    ASSERT_TRUE(buyer.receive(frame));
    EXPECT_EQ(frame.payload[44], 2);
    server.stop();
    EXPECT_EQ(server.trade_count(), 1U);
}

TEST(GatewayPublicationGate, ConcurrentStopWaitsForPreparedCommand) {
    Gate gate;
    StopTrace trace;
    ResetHooks reset;
    exchange::LiveExchangeOwner::test_before_install_context = &gate;
    exchange::LiveExchangeOwner::test_before_install = &Gate::wait;
    gateway::Gateway server{
        0, 2, 5s, {.trace = {StopTrace::record, &trace}}, {}, exchange::AllocationMode::Exclusive};
    gateway::Client client{server.port()};
    gateway::Frame frame;
    ASSERT_TRUE(client.exchange(message(gateway::MessageType::Hello, 1), frame));
    const auto session =
        gateway::read_u64(std::span<const std::uint8_t, 8>{frame.payload.data(), 8});
    ASSERT_TRUE(client.send(limit(1, 1)));
    {
        std::unique_lock lock(gate.mutex);
        ASSERT_TRUE(gate.changed.wait_for(lock, 2s, [&] { return gate.entered; }));
    }
    std::jthread first([&] { server.stop(); });
    std::jthread second([&] { server.stop(); });
    bool requested = false;
    {
        std::unique_lock lock(trace.mutex);
        requested = trace.changed.wait_for(lock, 2s, [&] { return trace.requested; });
    }
    gate.open();
    first.join();
    second.join();
    EXPECT_TRUE(requested);
    EXPECT_FALSE(gate.timed_out);
    EXPECT_FALSE(server.running());
    const auto report = server.pending_report(session);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->kind, exchange::ReportKind::Accepted);
    exchange::Exchange shared;
    EXPECT_TRUE(shared.open_session(1));
}

TEST(GatewayPublicationGate, FullAndNearlyFullOutputRejectOrReachPreparation) {
    for (const bool full : {true, false}) {
        Gate gate;
        gate.abort = true;
        FillOutput fill{full ? 8192U - 32U : 8192U - 33U};
        ResetHooks reset;
        gateway::detail::test_output_capacity_context = &fill;
        gateway::detail::test_before_output_capacity_check = &FillOutput::fill;
        exchange::LiveExchangeOwner::test_before_install_context = &gate;
        exchange::LiveExchangeOwner::test_before_install = &Gate::wait;
        gateway::Gateway server{0, 2, 5s, {}, {}, exchange::AllocationMode::Exclusive};
        gateway::Client client{server.port()};
        gateway::Frame frame;
        ASSERT_TRUE(client.exchange(message(gateway::MessageType::Hello, 1), frame));
        const auto session =
            gateway::read_u64(std::span<const std::uint8_t, 8>{frame.payload.data(), 8});
        ASSERT_TRUE(client.send(limit(1, 1)));
        if (full) {
            EXPECT_FALSE(client.receive(frame));
        } else {
            std::unique_lock lock(gate.mutex);
            ASSERT_TRUE(gate.changed.wait_for(lock, 2s, [&] { return gate.entered; }));
            lock.unlock();
            gate.open();
        }
        server.stop();
        EXPECT_TRUE(fill.injected);
        EXPECT_FALSE(server.pending_report(session));
        EXPECT_FALSE(gate.timed_out);
    }
}
