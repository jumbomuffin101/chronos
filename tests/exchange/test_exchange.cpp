#include "chronos/exchange/exchange.hpp"
#include <gtest/gtest.h>
#include <limits>

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

TEST(ExchangeRisk, RejectsLimitsAndKillSwitchWithoutChangingBook) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    RiskLimits risk;
    risk.allowed_symbols = {1};
    risk.max_order_quantity = 5;
    risk.max_order_notional = 500;
    risk.max_open_notional = 500;
    risk.max_open_orders = 1;
    risk.max_abs_position = 5;
    risk.max_market_price = 100;
    ASSERT_TRUE(e.configure_risk(1, risk));
    EXPECT_EQ(e.submit(1, 1, 1, core::SymbolId{2}, core::Side::Buy, core::OrderType::Limit, 100, 1),
              Result::RiskSymbol);
    EXPECT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 6),
              Result::RiskQuantity);
    EXPECT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 101, 5),
              Result::RiskNotional);
    ASSERT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 5),
              Result::Accepted);
    EXPECT_EQ(e.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 1, 1),
              Result::RiskOpenOrders);
    ASSERT_TRUE(e.set_killed(1, true));
    EXPECT_EQ(e.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 1, 1),
              Result::RiskKilled);
    EXPECT_EQ(e.cancel(1, 2, 1), Result::Accepted);
    EXPECT_FALSE(e.find(1, 1));
    ASSERT_TRUE(e.set_killed(1, false));
}

TEST(ExchangeRisk, PositionsAndRestingReservationsTrackPartialAndFullFills) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    RiskLimits risk;
    risk.max_abs_position = 5;
    risk.max_market_price = 100;
    ASSERT_TRUE(e.configure_risk(1, risk));
    ASSERT_TRUE(e.configure_risk(2, risk));
    ASSERT_EQ(
        e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 5),
        Result::Accepted);
    EXPECT_EQ(
        e.submit(1, 2, 2, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 1),
        Result::RiskPosition);
    ASSERT_EQ(e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 3),
              Result::Accepted);
    EXPECT_EQ(e.position(1, core::SymbolId{1}), -3);
    EXPECT_EQ(e.position(2, core::SymbolId{1}), 3);
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 2U);
    EXPECT_EQ(
        e.submit(1, 2, 2, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 1),
        Result::RiskPosition);
    ASSERT_EQ(e.cancel(1, 2, 1), Result::Accepted);
    ASSERT_EQ(
        e.submit(1, 3, 2, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        Result::Accepted);
    ASSERT_EQ(e.submit(2, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 2),
              Result::Accepted);
    EXPECT_EQ(e.position(1, core::SymbolId{1}), -5);
    EXPECT_EQ(e.position(2, core::SymbolId{1}), 5);
    EXPECT_FALSE(e.find(1, 2));
}

TEST(ExchangeRisk, ChecksMarketBoundAndIntegerOverflowBeforeMatching) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    RiskLimits risk;
    risk.max_market_price = 100;
    ASSERT_TRUE(e.configure_risk(2, risk));
    ASSERT_EQ(
        e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 101, 1),
        Result::Accepted);
    EXPECT_EQ(e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 1),
              Result::RiskMarketBound);
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 1U);
    EXPECT_EQ(e.trade_count(), 0U);
    EXPECT_EQ(e.position(1, core::SymbolId{1}), 0);
    EXPECT_EQ(e.position(2, core::SymbolId{1}), 0);
    EXPECT_EQ(e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                       std::numeric_limits<core::Price>::max(), 3),
              Result::RiskOverflow);
}

TEST(ExchangeRisk, OpenNotionalReservationIsReleasedOnCancel) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    RiskLimits risk;
    risk.max_open_notional = 200;
    ASSERT_TRUE(e.configure_risk(1, risk));
    ASSERT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2),
              Result::Accepted);
    EXPECT_EQ(e.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
              Result::RiskNotional);
    ASSERT_EQ(e.cancel(1, 2, 1), Result::Accepted);
    EXPECT_EQ(e.submit(1, 3, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
              Result::Accepted);
}

TEST(ExchangeRisk, CrossingSellUsesRestingBidForNotionalLimit) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_TRUE(e.open_session(3));
    RiskLimits seller;
    seller.max_order_notional = 100;
    ASSERT_TRUE(e.configure_risk(2, seller));
    ASSERT_EQ(
        e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 1000, 1),
        Result::Accepted);
    ASSERT_TRUE(e.subscribe(3, core::SymbolId{1}));
    const auto snapshot = e.market_front(3);
    ASSERT_TRUE(snapshot);
    e.market_pop(3, *snapshot);
    EXPECT_EQ(
        e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 1),
        Result::RiskNotional);
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 1U);
    EXPECT_EQ(e.position(1, core::SymbolId{1}), 0);
    EXPECT_EQ(e.position(2, core::SymbolId{1}), 0);
    EXPECT_EQ(e.trade_count(), 0U);
    EXPECT_FALSE(e.front(2));
    EXPECT_FALSE(e.market_front(3));
    seller.max_order_notional = 1000;
    seller.max_open_notional = 100;
    ASSERT_TRUE(e.configure_risk(2, seller));
    EXPECT_EQ(
        e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 1),
        Result::RiskNotional);
    EXPECT_FALSE(e.front(2));
    EXPECT_FALSE(e.market_front(3));
}

TEST(ExchangeRisk, MultiLevelFillUpdatesEachAccountAndPreservesDirection) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_TRUE(e.open_session(3));
    ASSERT_EQ(
        e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        Result::Accepted);
    ASSERT_EQ(
        e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 101, 3),
        Result::Accepted);
    ASSERT_EQ(e.submit(3, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 4),
              Result::Accepted);
    EXPECT_EQ(e.position(1, core::SymbolId{1}), -2);
    EXPECT_EQ(e.position(2, core::SymbolId{1}), -2);
    EXPECT_EQ(e.position(3, core::SymbolId{1}), 4);
    EXPECT_FALSE(e.find(1, 1));
    ASSERT_TRUE(e.find(2, 1));
    EXPECT_EQ(e.find(2, 1)->quantity, 1U);
    ASSERT_EQ(e.trade_count(), 2U);
    EXPECT_EQ(e.trade(0)->price, 100);
    EXPECT_EQ(e.trade(1)->price, 101);
}

TEST(ExchangeMarketData, SnapshotTradeAndBookEventsAreSequenced) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_TRUE(e.open_session(3));
    ASSERT_TRUE(e.subscribe(3, core::SymbolId{1}));
    auto event = e.market_front(3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Snapshot);
    EXPECT_EQ(event->sequence, 0U);
    e.market_pop(3, *event);
    ASSERT_EQ(
        e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 5),
        Result::Accepted);
    event = e.market_front(3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Book);
    EXPECT_EQ(event->sequence, 1U);
    EXPECT_EQ(event->ask_price, 100);
    EXPECT_EQ(event->ask_quantity, 5U);
    e.market_pop(3, *event);
    ASSERT_EQ(e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 3),
              Result::Accepted);
    event = e.market_front(3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Trade);
    EXPECT_EQ(event->sequence, 2U);
    EXPECT_EQ(event->trade_price, 100);
    EXPECT_EQ(event->trade_quantity, 3U);
    e.market_pop(3, *event);
    event = e.market_front(3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Book);
    EXPECT_EQ(event->sequence, 3U);
    EXPECT_EQ(event->ask_quantity, 2U);
    e.market_pop(3, *event);
    ASSERT_TRUE(e.unsubscribe(3, core::SymbolId{1}));
    ASSERT_EQ(e.cancel(1, 2, 1), Result::Accepted);
    EXPECT_FALSE(e.market_front(3));
}

TEST(ExchangeMarketData, OverflowSignalsGapAndResnapshotRestoresContinuity) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_TRUE(e.subscribe(2, core::SymbolId{1}));
    auto event = e.market_front(2);
    ASSERT_TRUE(event);
    e.market_pop(2, *event);
    for (std::uint64_t i = 1; i <= 33; ++i)
        ASSERT_EQ(e.submit(1, i, i, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                           static_cast<core::Price>(100 + i), 1),
                  Result::Accepted);
    for (std::uint64_t i = 1; i <= 32; ++i) {
        event = e.market_front(2);
        ASSERT_TRUE(event);
        EXPECT_EQ(event->kind, MarketKind::Book);
        EXPECT_EQ(event->sequence, i);
        e.market_pop(2, *event);
    }
    event = e.market_front(2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Gap);
    EXPECT_EQ(event->sequence, 33U);
    e.market_pop(2, *event);
    EXPECT_FALSE(e.market_front(2));
    ASSERT_TRUE(e.subscribe(2, core::SymbolId{1}));
    event = e.market_front(2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Snapshot);
    EXPECT_EQ(event->sequence, 33U);
    EXPECT_EQ(event->bid_price, 133);
    e.market_pop(2, *event);
    ASSERT_EQ(
        e.submit(1, 34, 34, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 134, 1),
        Result::Accepted);
    event = e.market_front(2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, MarketKind::Book);
    EXPECT_EQ(event->sequence, 34U);
}

TEST(ExchangeMarketData, OldInflightSnapshotCannotConsumeNewSubscription) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.subscribe(1, core::SymbolId{1}));
    const auto old = e.market_front(1);
    ASSERT_TRUE(old);
    ASSERT_TRUE(e.subscribe(1, core::SymbolId{1}));
    e.market_pop(1, *old);
    const auto current = e.market_front(1);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->kind, MarketKind::Snapshot);
    EXPECT_NE(current->generation, old->generation);
}

TEST(ExchangeMarketData, DisconnectRemovesPublicSubscription) {
    Exchange e;
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
              Result::Accepted);
    ASSERT_TRUE(e.subscribe(1, core::SymbolId{1}));
    e.close_session(1);
    EXPECT_FALSE(e.market_front(1));
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 1U);
}

TEST(ExchangeMarketData, SequenceExhaustionRollsBackBookAndReports) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    Exchange e{2, 10, 10, 10, max - 1};
    ASSERT_TRUE(e.open_session(1));
    ASSERT_TRUE(e.open_session(2));
    ASSERT_TRUE(e.subscribe(2, core::SymbolId{1}));
    auto snapshot = e.market_front(2);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->sequence, max - 1);
    e.market_pop(2, *snapshot);
    ASSERT_EQ(e.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
              Result::Accepted);
    auto update = e.market_front(2);
    ASSERT_TRUE(update);
    EXPECT_EQ(update->sequence, max);
    e.market_pop(2, *update);
    e.pop(1);
    e.pop(1);
    EXPECT_EQ(e.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 101, 1),
              Result::CounterExhausted);
    EXPECT_FALSE(e.find(1, 2));
    ASSERT_TRUE(e.find(1, 1));
    EXPECT_EQ(e.find(1, 1)->quantity, 1U);
    EXPECT_FALSE(e.front(1));
    EXPECT_FALSE(e.market_front(2));
}
