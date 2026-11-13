#include "chronos/exchange/exchange.hpp"
#include "chronos/matching/matching_engine.hpp"
#include "chronos/orderbook/order_book.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <new>

// Confined to this test executable. The counter is armed only during the operation
// under test, so GoogleTest's own allocations do not consume the failure point.
namespace {
thread_local int allocations_until_failure = -1;
}

void *operator new(std::size_t size) {
    if (allocations_until_failure == 0) {
        throw std::bad_alloc{};
    }
    if (allocations_until_failure > 0) {
        --allocations_until_failure;
    }
    if (void *p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }

using namespace chronos::core;
using namespace chronos::orderbook;

namespace {
Order make_order(std::uint64_t id, Price price, Quantity quantity) {
    return Order{OrderId{id}, SymbolId{1}, Side::Buy, OrderType::Limit, price, quantity, id};
}

void verify_original(const OrderBook &book, bool destination_exists) {
    ASSERT_EQ(book.order_count(), destination_exists ? 2U : 1U);
    const auto source = book.find(OrderId{1});
    ASSERT_TRUE(source);
    EXPECT_EQ(source->price, 10);
    EXPECT_EQ(source->quantity, 5U);
    const auto source_level = book.level(Side::Buy, 10);
    ASSERT_TRUE(source_level);
    EXPECT_EQ(source_level->quantity, 5U);
    EXPECT_EQ(source_level->order_count, 1U);
    const auto destination = book.level(Side::Buy, 20);
    ASSERT_EQ(destination.has_value(), destination_exists);
    if (destination_exists) {
        EXPECT_EQ(destination->quantity, 3U);
        EXPECT_EQ(book.find(OrderId{2})->quantity, 3U);
    }
}

void exercise_failures(bool modifying, bool destination_exists) {
    int failures = 0;
    bool completed = false;
    for (int fail_at = 0; fail_at < 16 && !completed; ++fail_at) {
        OrderBook book{SymbolId{1}};
        ASSERT_EQ(book.add(make_order(1, 10, 5)), Result::Success);
        if (destination_exists) {
            ASSERT_EQ(book.add(make_order(2, 20, 3)), Result::Success);
        }
        const Order candidate = make_order(3, 20, 7);
        Result result = Result::NotFound;
        bool threw = false;
        allocations_until_failure = fail_at;
        try {
            result = modifying ? book.modify(OrderId{1}, 20, 7) : book.add(candidate);
        } catch (const std::bad_alloc &) {
            threw = true;
        } catch (...) {
            allocations_until_failure = -1;
            throw;
        }
        allocations_until_failure = -1;
        if (threw) {
            ++failures;
            verify_original(book, destination_exists);
            EXPECT_FALSE(book.find(OrderId{3}));
            // Existing nodes and indexes remain usable after a rollback.
            EXPECT_EQ(book.cancel(OrderId{1}), Result::Success);
        } else {
            ASSERT_EQ(result, Result::Success);
            EXPECT_EQ(book.find(OrderId{modifying ? 1U : 3U})->quantity, 7U);
            EXPECT_EQ(book.level(Side::Buy, 20)->quantity, destination_exists ? 10U : 7U);
            completed = true;
        }
    }
    EXPECT_TRUE(completed);
    EXPECT_GE(failures, destination_exists ? 1 : 2);
}
} // namespace

TEST(OrderBookAllocation, InsertNewLevelRollsBack) { exercise_failures(false, false); }
TEST(OrderBookAllocation, InsertExistingLevelRollsBack) { exercise_failures(false, true); }
TEST(OrderBookAllocation, MoveToNewLevelRollsBack) { exercise_failures(true, false); }
TEST(OrderBookAllocation, MoveToExistingLevelRollsBack) { exercise_failures(true, true); }

TEST(ExchangeAllocation, StagedFillLeavesLiveStateUntouchedOnAllocationFailure) {
    using chronos::exchange::Exchange;
    using ExchangeResult = chronos::exchange::Result;
    int failures = 0;
    bool completed = false;
    for (int fail_at = 0; fail_at < 256 && !completed; ++fail_at) {
        Exchange exchange;
        ASSERT_TRUE(exchange.open_session(1));
        ASSERT_TRUE(exchange.open_session(2));
        ASSERT_EQ(exchange.submit(1, 1, 1, SymbolId{1}, Side::Sell, OrderType::Limit, 10, 5),
                  ExchangeResult::Accepted);
        exchange.pop(1);
        exchange.pop(1);
        bool threw = false;
        allocations_until_failure = fail_at;
        try {
            const auto result =
                exchange.submit(2, 2, 2, SymbolId{1}, Side::Buy, OrderType::Market, 0, 3);
            allocations_until_failure = -1;
            ASSERT_EQ(result, ExchangeResult::Accepted);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            threw = true;
        } catch (...) {
            allocations_until_failure = -1;
            throw;
        }
        if (threw) {
            ++failures;
            ASSERT_TRUE(exchange.find(1, 1));
            EXPECT_EQ(exchange.find(1, 1)->quantity, 5U);
            EXPECT_FALSE(exchange.find(2, 2));
            EXPECT_FALSE(exchange.front(1));
            EXPECT_FALSE(exchange.front(2));
            EXPECT_EQ(exchange.trade_count(), 0U);
        }
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 5);
}

TEST(ExchangeAllocation, CancellationStagingFailurePreservesOrder) {
    using chronos::exchange::Exchange;
    using ExchangeResult = chronos::exchange::Result;
    int failures = 0;
    bool completed = false;
    for (int fail_at = 0; fail_at < 128 && !completed; ++fail_at) {
        Exchange exchange;
        ASSERT_TRUE(exchange.open_session(1));
        ASSERT_EQ(exchange.submit(1, 1, 1, SymbolId{1}, Side::Buy, OrderType::Limit, 10, 5),
                  ExchangeResult::Accepted);
        exchange.pop(1);
        exchange.pop(1);
        allocations_until_failure = fail_at;
        try {
            const auto result = exchange.cancel(1, 2, 1);
            allocations_until_failure = -1;
            ASSERT_EQ(result, ExchangeResult::Accepted);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            ++failures;
            ASSERT_TRUE(exchange.find(1, 1));
            EXPECT_EQ(exchange.find(1, 1)->quantity, 5U);
            EXPECT_FALSE(exchange.front(1));
        } catch (...) {
            allocations_until_failure = -1;
            throw;
        }
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 3);
}

TEST(MatchingAllocation, MultiFillCommandIsAtomic) {
    using namespace chronos::matching;
    int failures = 0;
    bool completed = false;
    for (int fail_at = 0; fail_at < 128 && !completed; ++fail_at) {
        MatchingEngine engine{SymbolId{1}};
        ASSERT_EQ(
            engine.submit({OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 10, 2}).error,
            Error::None);
        ASSERT_EQ(
            engine.submit({OrderId{2}, SymbolId{1}, Side::Sell, OrderType::Limit, 11, 2}).error,
            Error::None);
        const auto before = engine.counters();
        bool threw = false;
        allocations_until_failure = fail_at;
        try {
            auto out = engine.submit({OrderId{3}, SymbolId{1}, Side::Buy, OrderType::Limit, 11, 5});
            allocations_until_failure = -1;
            EXPECT_EQ(out.error, Error::None);
            EXPECT_EQ(out.trades.size(), 2U);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            threw = true;
        } catch (...) {
            allocations_until_failure = -1;
            throw;
        }
        if (threw) {
            ++failures;
            ASSERT_TRUE(engine.find(OrderId{1}));
            ASSERT_TRUE(engine.find(OrderId{2}));
            EXPECT_EQ(engine.find(OrderId{1})->quantity, 2U);
            EXPECT_EQ(engine.find(OrderId{2})->quantity, 2U);
            EXPECT_FALSE(engine.find(OrderId{3}));
            EXPECT_EQ(engine.counters().command, before.command);
            EXPECT_EQ(engine.counters().event, before.event);
            EXPECT_EQ(engine.counters().trade, before.trade);
        }
    }
    EXPECT_TRUE(completed);
    EXPECT_GE(failures, 5);
}

TEST(MatchingAllocation, PlannedRemainderIntoExistingLevelIsAtomic) {
    using namespace chronos::matching;
    int failures = 0;
    bool completed = false;
    bool completed_with_next_allocation_failing = false;
    for (int fail_at = 0; fail_at < 128 && !completed; ++fail_at) {
        MatchingEngine engine{SymbolId{1}};
        ASSERT_EQ(
            engine.submit({OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Limit, 10, 3}).error,
            Error::None);
        ASSERT_EQ(
            engine.submit({OrderId{2}, SymbolId{1}, Side::Sell, OrderType::Limit, 20, 1}).error,
            Error::None);
        ASSERT_EQ(engine.modify_resting(OrderId{2}, 10, 1), Result::Success);
        const auto before = engine.counters();
        bool threw = false;
        allocations_until_failure = fail_at;
        try {
            auto result =
                engine.submit({OrderId{3}, SymbolId{1}, Side::Buy, OrderType::Limit, 10, 2});
            completed_with_next_allocation_failing = allocations_until_failure == 0;
            allocations_until_failure = -1;
            EXPECT_EQ(result.error, Error::None);
            ASSERT_EQ(result.trades.size(), 1U);
            EXPECT_EQ(result.remaining, 1U);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            threw = true;
        } catch (...) {
            allocations_until_failure = -1;
            throw;
        }
        if (threw) {
            ++failures;
            ASSERT_TRUE(engine.find(OrderId{1}));
            ASSERT_TRUE(engine.find(OrderId{2}));
            EXPECT_EQ(engine.find(OrderId{1})->quantity, 3U);
            EXPECT_EQ(engine.find(OrderId{2})->quantity, 1U);
            EXPECT_FALSE(engine.find(OrderId{3}));
            EXPECT_EQ(engine.counters().command, before.command);
            EXPECT_EQ(engine.counters().trade, before.trade);
            EXPECT_EQ(engine.counters().event, before.event);
        }
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(completed_with_next_allocation_failing);
    EXPECT_GE(failures, 5);
}

TEST(MatchingAllocation, FullMultiFillCompletesWhenNextAllocationWouldFail) {
    using namespace chronos::matching;
    bool complete = false;
    bool next_allocation_would_fail = false;
    int failures = 0;
    for (int fail_at = 0; fail_at < 128 && !complete; ++fail_at) {
        MatchingEngine engine{SymbolId{1}};
        ASSERT_EQ(
            engine.submit({OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 10, 1}).error,
            Error::None);
        ASSERT_EQ(
            engine.submit({OrderId{2}, SymbolId{1}, Side::Sell, OrderType::Limit, 11, 1}).error,
            Error::None);
        const auto before = engine.counters();
        allocations_until_failure = fail_at;
        try {
            auto result =
                engine.submit({OrderId{3}, SymbolId{1}, Side::Buy, OrderType::Limit, 11, 2});
            next_allocation_would_fail = allocations_until_failure == 0;
            allocations_until_failure = -1;
            ASSERT_EQ(result.error, Error::None);
            ASSERT_EQ(result.trades.size(), 2U);
            EXPECT_FALSE(engine.find(OrderId{1}));
            EXPECT_FALSE(engine.find(OrderId{2}));
            complete = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            ++failures;
            ASSERT_TRUE(engine.find(OrderId{1}));
            ASSERT_TRUE(engine.find(OrderId{2}));
            EXPECT_EQ(engine.find(OrderId{1})->quantity, 1U);
            EXPECT_EQ(engine.find(OrderId{2})->quantity, 1U);
            EXPECT_EQ(engine.counters().command, before.command);
            EXPECT_EQ(engine.counters().event, before.event);
            EXPECT_EQ(engine.counters().trade, before.trade);
        } catch (...) {
            allocations_until_failure = -1;
            throw;
        }
    }
    EXPECT_TRUE(complete);
    EXPECT_TRUE(next_allocation_would_fail);
    EXPECT_GE(failures, 3);
}
