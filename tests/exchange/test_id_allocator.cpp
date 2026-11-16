#include "chronos/exchange/exchange.hpp"
#ifdef __linux__
#include "chronos/gateway/tcp.hpp"
#endif
#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

using namespace chronos;
namespace {
std::uint64_t live_order() {
    exchange::Exchange book;
    EXPECT_FALSE(book.is_isolated());
    EXPECT_TRUE(book.open_session(1));
    EXPECT_EQ(
        book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    const auto report = book.front(1);
    EXPECT_TRUE(report.has_value());
    return report ? report->internal_order_id : 0;
}
static_assert(!std::is_move_constructible_v<exchange::Exchange>);
#ifdef __linux__
static_assert(!std::is_constructible_v<gateway::Gateway, std::unique_ptr<exchange::Exchange>>);
#endif
} // namespace

TEST(OrderIdAllocator, IsolatedLifetimeAndOperationsNeverConsumeLiveIds) {
    const auto before = live_order();
    {
        auto book = exchange::Exchange::isolated();
        ASSERT_TRUE(book->is_isolated());
        ASSERT_TRUE(book->open_session(1));
        ASSERT_TRUE(book->open_session(2));
        ASSERT_EQ(book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit,
                               100, 5),
                  exchange::Result::Accepted);
        ASSERT_EQ(book->front(1)->internal_order_id, 1U);
        ASSERT_EQ(book->submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                               100, 2),
                  exchange::Result::Accepted);
        ASSERT_EQ(book->front(2)->internal_order_id, 2U);
        ASSERT_EQ(book->trade_count(), 1U);
        EXPECT_EQ(book->trade(0)->sell_internal_id, 1U);
        EXPECT_EQ(book->trade(0)->buy_internal_id, 2U);
        EXPECT_EQ(book->cancel(2, 2, 1), exchange::Result::NotFound);
        EXPECT_EQ(book->cancel(1, 2, 1), exchange::Result::Accepted);
        EXPECT_EQ(book->submit(2, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                               100, 0),
                  exchange::Result::RiskQuantity);
    }
    EXPECT_EQ(live_order(), before + 1);
    auto a = exchange::Exchange::isolated();
    auto b = exchange::Exchange::isolated();
    ASSERT_TRUE(a->open_session(1));
    ASSERT_TRUE(b->open_session(1));
    for (auto *book : {a.get(), b.get()}) {
        ASSERT_EQ(book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                               100, 1),
                  exchange::Result::Accepted);
        EXPECT_EQ(book->front(1)->internal_order_id, 1U);
    }
}
TEST(OrderIdAllocator, PrivateExhaustionIsCheckedWithoutWrapping) {
    auto book = exchange::Exchange::isolated(2, 10, 16, 10, 0,
                                             std::numeric_limits<std::uint64_t>::max() - 1);
    ASSERT_TRUE(book->open_session(1));
    ASSERT_EQ(
        book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    EXPECT_EQ(book->front(1)->internal_order_id, std::numeric_limits<std::uint64_t>::max());
    const auto state = book->verification_state();
    EXPECT_EQ(
        book->submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::CounterExhausted);
    EXPECT_EQ(book->verification_state(), state);
}
TEST(OrderIdAllocator, StagingFailureDoesNotConsumePrivateId) {
    auto book = exchange::Exchange::isolated(1, 10, 2, 10);
    ASSERT_TRUE(book->open_session(1));
    ASSERT_EQ(
        book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    const auto before = book->verification_state();
    EXPECT_EQ(
        book->submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Capacity);
    EXPECT_EQ(book->verification_state(), before);
    book->pop(1);
    book->pop(1);
    ASSERT_EQ(
        book->submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    EXPECT_EQ(book->front(1)->internal_order_id, 2U);
}
TEST(OrderIdAllocator, SynchronizedServiceExhaustionAndRollback) {
    exchange::detail::OrderIdAllocator allocator(true,
                                                 std::numeric_limits<std::uint64_t>::max() - 1);
    {
        exchange::detail::OrderIdAllocator::Reservation attempt(allocator);
        EXPECT_EQ(attempt.id(), std::numeric_limits<std::uint64_t>::max());
    }
    {
        exchange::detail::OrderIdAllocator::Reservation attempt(allocator);
        EXPECT_EQ(attempt.id(), std::numeric_limits<std::uint64_t>::max());
        attempt.commit();
    }
    {
        exchange::detail::OrderIdAllocator::Reservation attempt(allocator);
        EXPECT_EQ(attempt.id(), 0U);
        attempt.commit();
    }
    {
        exchange::detail::OrderIdAllocator::Reservation attempt(allocator);
        EXPECT_EQ(attempt.id(), 0U);
    }
}
TEST(OrderIdAllocator, ConcurrentSynchronizedReservationsAreUnique) {
    exchange::detail::OrderIdAllocator allocator(true);
    std::mutex guard;
    std::vector<std::uint64_t> ids;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                exchange::detail::OrderIdAllocator::Reservation reservation(allocator);
                const auto id = reservation.id();
                reservation.commit();
                std::lock_guard lock(guard);
                ids.push_back(id);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 400U);
    for (std::size_t i = 0; i < ids.size(); ++i)
        EXPECT_EQ(ids[i], i + 1);
}
TEST(OrderIdAllocator, ConcurrentDefaultExchangesProduceUniqueProcessIds) {
    std::mutex guard;
    std::vector<std::uint64_t> ids;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&] {
            exchange::Exchange book;
            if (!book.open_session(1))
                return;
            for (std::uint64_t i = 1; i <= 20; ++i) {
                if (book.submit(1, i, i, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                                100, 1) != exchange::Result::Accepted)
                    return;
                const auto report = book.front(1);
                if (!report)
                    return;
                {
                    std::lock_guard lock(guard);
                    ids.push_back(report->internal_order_id);
                }
                while (book.front(1))
                    book.pop(1);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 80U);
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}
