#include "chronos/exchange/exchange.hpp"
#include <gtest/gtest.h>

using namespace chronos;

TEST(ExchangeTransactionBoundary, RejectedCommandsPreserveExactStateAndRequestIds) {
    auto book = exchange::Exchange::isolated(2, 16, 16, 16);
    ASSERT_TRUE(book->open_session(1));
    const auto empty = book->isolated_continuation_state(4096);
    EXPECT_EQ(
        book->submit(1, 11, 21, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 0),
        exchange::Result::RiskQuantity);
    EXPECT_EQ(book->isolated_continuation_state(4096), empty);
    EXPECT_FALSE(book->front(1));
    EXPECT_EQ(book->cancel(1, 11, 21), exchange::Result::NotFound);
    EXPECT_EQ(book->isolated_continuation_state(4096), empty);

    // Neither rejection consumed the request or private order ID.
    ASSERT_EQ(
        book->submit(1, 11, 21, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    ASSERT_TRUE(book->front(1));
    EXPECT_EQ(book->front(1)->internal_order_id, 1U);
    const auto resting = book->isolated_continuation_state(4096);
    EXPECT_EQ(book->cancel(1, 12, 22), exchange::Result::NotFound);
    EXPECT_EQ(book->isolated_continuation_state(4096), resting);
    EXPECT_EQ(book->cancel(1, 12, 21), exchange::Result::Accepted);
}

TEST(ExchangeTransactionBoundary, OwnerMutationsCanChangeStateWithoutAnOrderCommand) {
    auto book = exchange::Exchange::isolated(2, 16, 16, 16);
    ASSERT_TRUE(book->open_session(1));
    ASSERT_EQ(
        book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    const auto before_kill = book->isolated_continuation_state(4096);
    ASSERT_TRUE(book->set_killed(1, true));
    const auto after_kill = book->isolated_continuation_state(4096);
    ASSERT_NE(before_kill, after_kill);
    ASSERT_TRUE(book->front(1));
    book->pop(1);
    EXPECT_NE(after_kill, book->isolated_continuation_state(4096));
    EXPECT_TRUE(book->find(1, 1));
}

TEST(ExchangeTransactionBoundary, FullReportQueueRejectsCandidateWithoutConsumingId) {
    auto book = exchange::Exchange::isolated(2, 16, 2, 16);
    ASSERT_TRUE(book->open_session(1));
    ASSERT_TRUE(book->open_session(2));
    ASSERT_EQ(
        book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    const auto before = book->isolated_continuation_state(4096);
    EXPECT_EQ(
        book->submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Capacity);
    EXPECT_EQ(book->isolated_continuation_state(4096), before);
    EXPECT_FALSE(book->front(2));
    ASSERT_TRUE(book->find(1, 1));
    EXPECT_EQ(book->find(1, 1)->quantity, 2U);
    book->pop(1);
    book->pop(1);
    ASSERT_EQ(
        book->submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(book->front(2));
    EXPECT_EQ(book->front(2)->internal_order_id, 2U);
}
