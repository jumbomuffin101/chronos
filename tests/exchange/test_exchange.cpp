#include "chronos/exchange/exchange.hpp"
#include <gtest/gtest.h>

using namespace chronos;
using namespace chronos::exchange;

TEST(ExchangePrototype, StagesCrossSessionFillsAndPreservesOwnership) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_EQ(
        e.submit(1, 1, 10, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 5),
        Result::Accepted);
    e.pop(1);
    e.pop(1);
    EXPECT_EQ(e.cancel(2, 1, 10), Result::NotFound);
    ASSERT_EQ(e.submit(2, 2, 10, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 3),
              Result::Accepted);
    ASSERT_TRUE(e.front(1));
    EXPECT_EQ(e.front(1)->kind, ReportKind::PartialFill);
    EXPECT_EQ(e.front(1)->remaining, 2U);
    EXPECT_EQ(e.front(1)->price, 100);
    ASSERT_TRUE(e.find(1, 10));
    EXPECT_EQ(e.find(1, 10)->quantity, 2U);
    EXPECT_EQ(e.front(2)->kind, ReportKind::Accepted);
    e.pop(2);
    EXPECT_EQ(e.front(2)->kind, ReportKind::Filled);
    EXPECT_EQ(e.front(2)->price, 100);
    EXPECT_EQ(e.submit(1, 3, 10, core::SymbolId{2}, core::Side::Buy, core::OrderType::Limit, 99, 1),
              Result::DuplicateOrder);
    EXPECT_EQ(e.cancel(1, 1, 10), Result::DuplicateRequest);
    EXPECT_EQ(e.cancel(1, 4, 10), Result::Accepted);
    EXPECT_FALSE(e.find(1, 10));
}

TEST(ExchangePrototype, FullRecipientQueueRejectsWithoutBookMutation) {
    Exchange e{2, 10, 2, 10};
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_EQ(
        e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 5),
        Result::Accepted);
    ASSERT_EQ(e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 5),
              Result::Capacity);
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 5U);
    EXPECT_FALSE(e.front(2));
    e.pop(1);
    ASSERT_EQ(e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 5),
              Result::Accepted);
    EXPECT_FALSE(e.find(1, 1));
}

TEST(ExchangePrototype, FullReportQueueLeavesCancellationRetryable) {
    Exchange e{1, 10, 2, 10};
    ASSERT_TRUE(e.open_session(1));
    ASSERT_EQ(e.submit(1, 1, 8, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 4),
              Result::Accepted);
    EXPECT_EQ(e.cancel(1, 2, 8), Result::Capacity);
    ASSERT_TRUE(e.find(1, 8));
    EXPECT_EQ(e.find(1, 8)->quantity, 4U);
    e.pop(1);
    EXPECT_EQ(e.cancel(1, 2, 8), Result::Accepted);
    EXPECT_FALSE(e.find(1, 8));
    e.pop(1);
    ASSERT_TRUE(e.front(1));
    EXPECT_EQ(e.front(1)->kind, ReportKind::Cancelled);
    EXPECT_EQ(e.front(1)->quantity, 4U);
    EXPECT_EQ(e.front(1)->remaining, 0U);
}

TEST(ExchangePrototype, IdentifiersAndSequencesSpanSymbolsAndSessions) {
    Exchange e;
    ASSERT_TRUE(e.open_session(7));
    ASSERT_TRUE(e.open_session(8));
    ASSERT_EQ(e.submit(7, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 10, 1),
              Result::Accepted);
    const auto first = *e.front(7);
    ASSERT_EQ(e.submit(8, 1, 1, core::SymbolId{2}, core::Side::Buy, core::OrderType::Limit, 10, 1),
              Result::Accepted);
    const auto second = *e.front(8);
    EXPECT_NE(first.internal_order_id, second.internal_order_id);
    EXPECT_LT(first.sequence, second.sequence);
    EXPECT_LT(first.command_sequence, second.command_sequence);
    EXPECT_EQ(e.submit(7, 1, 2, core::SymbolId{3}, core::Side::Buy, core::OrderType::Limit, 10, 1),
              Result::DuplicateRequest);
    EXPECT_FALSE(e.find(7, 2));
}

TEST(ExchangePrototype, DisconnectedOwnerRetainsFillsAndSaturationBlocksMatches) {
    Exchange e{2, 10, 2, 10};
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 10, 4),
              Result::Accepted);
    e.pop(1);
    e.pop(1);
    e.close_session(1);
    ASSERT_EQ(e.submit(2, 1, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 2),
              Result::Accepted);
    EXPECT_EQ(e.front(1)->kind, ReportKind::PartialFill);
    EXPECT_EQ(e.trade_count(), 1U);
    const auto t = e.trade(0);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->buyer_session, 2U);
    EXPECT_EQ(t->seller_session, 1U);
    e.pop(2);
    e.pop(2);
    ASSERT_EQ(e.submit(2, 2, 3, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 1),
              Result::Accepted);
    e.pop(2);
    e.pop(2);
    EXPECT_EQ(e.submit(2, 3, 4, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 1),
              Result::Capacity);
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 1U);
    EXPECT_EQ(e.trade_count(), 2U);
}

TEST(ExchangePrototype, InternalIdsAreUniqueAcrossInstances) {
    Exchange a, b;
    ASSERT_TRUE(a.open_session(1));
    ASSERT_TRUE(b.open_session(1));
    ASSERT_EQ(a.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 10, 1),
              Result::Accepted);
    ASSERT_EQ(b.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 10, 1),
              Result::Accepted);
    EXPECT_NE(a.front(1)->internal_order_id, b.front(1)->internal_order_id);
}
