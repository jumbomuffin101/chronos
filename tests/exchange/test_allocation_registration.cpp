#include "chronos/exchange/live_owner.hpp"
#include "chronos/simulation/simulation.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace chronos;
namespace {
std::unique_ptr<exchange::LiveExchangeOwner> exclusive() {
    return std::make_unique<exchange::LiveExchangeOwner>(16, 1024, 128, 1024,
                                                         exchange::AllocationMode::Exclusive);
}
} // namespace

TEST(AllocationRegistration, SharedLifetimeRejectsExclusiveWithoutConsumingId) {
    exchange::Exchange shared;
    ASSERT_TRUE(shared.open_session(1));
    ASSERT_EQ(
        shared.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(shared.front(1));
    const auto first = shared.front(1)->internal_order_id;
    EXPECT_THROW(static_cast<void>(exclusive()), std::runtime_error);
    ASSERT_EQ(
        shared.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 99, 1),
        exchange::Result::Accepted);
    shared.pop(1);
    shared.pop(1);
    ASSERT_TRUE(shared.front(1));
    EXPECT_EQ(shared.front(1)->internal_order_id, first + 1);
}

TEST(AllocationRegistration, ExclusiveRejectsOthersAndPrivateIdsRemainIsolated) {
    auto owner = exclusive();
    EXPECT_THROW(static_cast<void>(exclusive()), std::runtime_error);
    EXPECT_THROW(static_cast<void>(exchange::Exchange{}), std::runtime_error);
    auto private_book = exchange::Exchange::isolated();
    ASSERT_TRUE(private_book->open_session(1));
    ASSERT_EQ(private_book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                   core::OrderType::Limit, 100, 1),
              exchange::Result::Accepted);
    ASSERT_TRUE(private_book->front(1));
    EXPECT_EQ(private_book->front(1)->internal_order_id, 1U);
    owner->bind();
    ASSERT_TRUE(owner->open_session(1));
    EXPECT_EQ(
        owner->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 0),
        exchange::Result::RiskQuantity);
    ASSERT_EQ(
        owner->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(owner->front(1));
    const auto first = owner->front(1)->internal_order_id;
    ASSERT_EQ(owner->cancel(1, 2, 1), exchange::Result::Accepted);
    owner->pop(1);
    owner->pop(1);
    ASSERT_TRUE(owner->front(1));
    EXPECT_EQ(owner->front(1)->internal_order_id, first);
    EXPECT_TRUE(owner->finish());
    EXPECT_FALSE(owner->finish());
    EXPECT_THROW(static_cast<void>(owner->open_session(2)), std::logic_error);
    exchange::Exchange next;
    ASSERT_TRUE(next.open_session(1));
    ASSERT_EQ(
        next.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(next.front(1));
    EXPECT_EQ(next.front(1)->internal_order_id, first + 1);
}

TEST(AllocationRegistration, DestructionAndExceptionReleaseRegistration) {
    for (int i = 0; i < 3; ++i) {
        bool acquired = false;
        try {
            auto owner = exclusive();
            acquired = true;
            throw std::runtime_error("abort");
        } catch (const std::runtime_error &) {
        }
        EXPECT_TRUE(acquired);
        exchange::Exchange shared;
        EXPECT_TRUE(shared.open_session(1));
    }
    auto owner = exclusive();
    EXPECT_THROW(static_cast<void>(exchange::Exchange{}), std::runtime_error);
}

TEST(AllocationRegistration, ConcurrentExclusiveAttemptsHaveExactlyOneWinner) {
    constexpr int workers = 8;
    std::latch ready(workers), go(1), attempted(workers), release(1);
    std::atomic<int> winners{0}, failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            ready.count_down();
            go.wait();
            try {
                auto owner = exclusive();
                ++winners;
                attempted.count_down();
                release.wait();
            } catch (const std::runtime_error &) {
                ++failures;
                attempted.count_down();
            }
        });
    }
    ready.wait();
    go.count_down();
    attempted.wait();
    EXPECT_EQ(winners.load(), 1);
    EXPECT_EQ(failures.load(), workers - 1);
    release.count_down();
    for (auto &thread : threads)
        thread.join();
    EXPECT_NO_THROW(static_cast<void>(exchange::Exchange{}));
}

TEST(AllocationRegistration, SharedConstructionAndExclusiveRegistrationCannotOverlap) {
    std::latch ready(2), go(1), attempted(2), release(1);
    std::atomic<int> winners{0}, failures{0};
    std::thread shared([&] {
        ready.count_down();
        go.wait();
        try {
            exchange::Exchange book;
            ++winners;
            attempted.count_down();
            release.wait();
        } catch (const std::runtime_error &) {
            ++failures;
            attempted.count_down();
        }
    });
    std::thread durable([&] {
        ready.count_down();
        go.wait();
        try {
            auto owner = exclusive();
            ++winners;
            attempted.count_down();
            release.wait();
        } catch (const std::runtime_error &) {
            ++failures;
            attempted.count_down();
        }
    });
    ready.wait();
    go.count_down();
    attempted.wait();
    EXPECT_EQ(winners.load(), 1);
    EXPECT_EQ(failures.load(), 1);
    release.count_down();
    shared.join();
    durable.join();
}

TEST(AllocationRegistration, SharedAcquisitionWaitsForTerminalOwnerHandoff) {
    auto owner = exclusive();
    std::latch active(1), stop(1), finished(1);
    bool opened = false, terminated = false;
    std::thread worker([&] {
        owner->bind();
        opened = owner->open_session(1);
        active.count_down();
        stop.wait();
        terminated = owner->finish();
        finished.count_down();
    });
    active.wait();
    EXPECT_THROW(static_cast<void>(exchange::Exchange{}), std::runtime_error);
    stop.count_down();
    finished.wait();
    EXPECT_NO_THROW(static_cast<void>(exchange::Exchange{}));
    worker.join();
    EXPECT_TRUE(opened);
    EXPECT_TRUE(terminated);
    EXPECT_THROW(static_cast<void>(owner->open_session(2)), std::logic_error);
}

TEST(AllocationRegistration, IsolatedSimulationRunsWhileDefaultSimulationFails) {
    auto owner = exclusive();
    simulation::ScenarioConfig config;
    config.max_commands = 2;
    simulation::SimulationRunner ordinary(config);
    ordinary.start();
    EXPECT_EQ(ordinary.result().status.state, simulation::Lifecycle::Failed);
    simulation::SimulationRunner isolated(config, nullptr, true);
    isolated.start();
    isolated.run_batch(config.max_commands);
    EXPECT_EQ(isolated.result().status.state, simulation::Lifecycle::Completed);
}
