#include "chronos/matching/matching_engine.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <type_traits>
#include <utility>

using namespace chronos::core;
using namespace chronos::matching;
using chronos::orderbook::OrderBook;
using chronos::orderbook::Result;

static_assert(
    std::is_same_v<decltype(std::declval<const chronos::orderbook::Selection &>().order()),
                   chronos::orderbook::OrderSnapshot>);

namespace {
Request req(std::uint64_t id, Side side, Price price, Quantity qty,
            OrderType type = OrderType::Limit, SymbolId symbol = SymbolId{1}) {
    return {OrderId{id}, symbol, side, type, price, qty};
}
} // namespace

TEST(Matching, CrossesAtRestingPriceBothSides) {
    MatchingEngine e{SymbolId{1}};
    ASSERT_EQ(e.submit(req(1, Side::Sell, 100, 5)).error, Error::None);
    const auto buy = e.submit(req(2, Side::Buy, 110, 5));
    ASSERT_EQ(buy.error, Error::None);
    ASSERT_EQ(buy.trades.size(), 1U);
    EXPECT_EQ(buy.trades[0].price, 100);
    EXPECT_EQ(buy.trades[0].buy_order_id, OrderId{2});
    EXPECT_EQ(buy.trades[0].sell_order_id, OrderId{1});
    EXPECT_FALSE(e.find(OrderId{1}));
    ASSERT_EQ(e.submit(req(3, Side::Buy, 120, 4)).error, Error::None);
    const auto sell = e.submit(req(4, Side::Sell, 110, 4));
    ASSERT_EQ(sell.trades.size(), 1U);
    EXPECT_EQ(sell.trades[0].price, 120);
    EXPECT_EQ(sell.trades[0].quantity, 4U);
    EXPECT_EQ(sell.reports.back().kind, ReportKind::Filled);
}

TEST(Matching, PriceTimePartialAndRemainder) {
    MatchingEngine e{SymbolId{1}};
    ASSERT_EQ(e.submit(req(1, Side::Sell, 102, 3)).error, Error::None);
    ASSERT_EQ(e.submit(req(2, Side::Sell, 101, 2)).error, Error::None);
    ASSERT_EQ(e.submit(req(3, Side::Sell, 101, 4)).error, Error::None);
    const auto out = e.submit(req(4, Side::Buy, 101, 5));
    ASSERT_EQ(out.trades.size(), 2U);
    EXPECT_EQ(out.trades[0].sell_order_id, OrderId{2});
    EXPECT_EQ(out.trades[1].sell_order_id, OrderId{3});
    EXPECT_EQ(out.trades[1].quantity, 3U);
    ASSERT_TRUE(e.find(OrderId{3}));
    EXPECT_EQ(e.find(OrderId{3})->quantity, 1U);
    EXPECT_EQ(e.orders_at(Side::Sell, 101).front().id, OrderId{3});
    EXPECT_EQ(out.remaining, 0U);
    const auto second = e.submit(req(5, Side::Buy, 102, 10));
    ASSERT_EQ(second.trades.size(), 2U);
    EXPECT_EQ(second.trades[0].sell_order_id, OrderId{3});
    EXPECT_EQ(second.trades[1].sell_order_id, OrderId{1});
    EXPECT_EQ(second.remaining, 6U);
    EXPECT_EQ(e.find(OrderId{5})->quantity, 6U);
    EXPECT_EQ(second.reports.back().kind, ReportKind::Resting);
}

TEST(Matching, MarketRemaindersNeverRest) {
    MatchingEngine e{SymbolId{1}};
    const auto empty = e.submit(req(1, Side::Buy, 0, 5, OrderType::Market));
    EXPECT_EQ(empty.remaining, 5U);
    EXPECT_EQ(empty.reports.back().kind, ReportKind::MarketRemainderCancelled);
    EXPECT_FALSE(e.find(OrderId{1}));
    ASSERT_EQ(e.submit(req(2, Side::Buy, 5, 3)).error, Error::None);
    const auto out = e.submit(req(3, Side::Sell, 0, 5, OrderType::Market));
    ASSERT_EQ(out.trades.size(), 1U);
    EXPECT_EQ(out.remaining, 2U);
    EXPECT_EQ(out.reports.back().kind, ReportKind::MarketRemainderCancelled);
    EXPECT_FALSE(e.find(OrderId{3}));
    EXPECT_FALSE(e.find(OrderId{2}));
}

TEST(Matching, MaintenanceRespectsPriority) {
    MatchingEngine e{SymbolId{1}};
    ASSERT_EQ(e.submit(req(1, Side::Sell, 10, 3)).error, Error::None);
    ASSERT_EQ(e.submit(req(2, Side::Sell, 10, 3)).error, Error::None);
    ASSERT_EQ(e.modify_resting(OrderId{1}, 10, 2), Result::Success);
    ASSERT_EQ(e.modify_resting(OrderId{2}, 11, 3), Result::Success);
    ASSERT_EQ(e.modify_resting(OrderId{2}, 10, 3), Result::Success);
    const auto first = e.submit(req(3, Side::Buy, 10, 1));
    ASSERT_EQ(first.trades.size(), 1U);
    EXPECT_EQ(first.trades[0].sell_order_id, OrderId{1});
    ASSERT_EQ(e.modify_resting(OrderId{1}, 10, 2), Result::Success);
    const auto second = e.submit(req(4, Side::Buy, 10, 1));
    ASSERT_EQ(second.trades.size(), 1U);
    EXPECT_EQ(second.trades[0].sell_order_id, OrderId{2});
    EXPECT_EQ(e.cancel_resting(OrderId{1}), Result::Success);
}

TEST(Matching, InvalidRequestsDoNotAdvanceOrMutate) {
    MatchingEngine e{SymbolId{1}};
    ASSERT_EQ(e.submit(req(1, Side::Buy, 1, 2)).error, Error::None);
    const auto before = e.counters();
    EXPECT_EQ(e.submit(req(2, Side::Buy, 1, 0)).error, Error::InvalidQuantity);
    EXPECT_EQ(e.submit(req(2, Side::Buy, -1, 1)).error, Error::InvalidPrice);
    EXPECT_EQ(e.submit(req(2, Side::Buy, 1, 1, OrderType::Market)).error, Error::InvalidPrice);
    EXPECT_EQ(e.submit(req(2, Side::Buy, 1, 1, OrderType::Limit, SymbolId{2})).error,
              Error::WrongSymbol);
    EXPECT_EQ(e.submit(req(1, Side::Sell, 1, 1)).error, Error::DuplicateId);
    EXPECT_EQ(e.submit(req(2, static_cast<Side>(99), 1, 1)).error, Error::InvalidSide);
    EXPECT_EQ(e.counters().command, before.command);
    EXPECT_EQ(e.counters().event, before.event);
    EXPECT_EQ(e.find(OrderId{1})->quantity, 2U);
}

TEST(Matching, RejectsOverflowWithoutPartialFillsOrCounters) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    MatchingEngine e{SymbolId{1}, Counters{0, 0, max - 1}};
    ASSERT_EQ(e.submit(req(1, Side::Sell, 1, 1)).error, Error::None);
    ASSERT_EQ(e.submit(req(2, Side::Sell, 2, 1)).error, Error::None);
    const auto before = e.counters();
    const auto out = e.submit(req(3, Side::Buy, 2, 2));
    EXPECT_EQ(out.error, Error::CounterExhausted);
    EXPECT_TRUE(out.trades.empty());
    ASSERT_EQ(out.reports.size(), 1U);
    EXPECT_EQ(out.reports[0].kind, ReportKind::Rejected);
    EXPECT_EQ(out.reports[0].sequence, 0U);
    EXPECT_EQ(e.find(OrderId{1})->quantity, 1U);
    EXPECT_EQ(e.find(OrderId{2})->quantity, 1U);
    EXPECT_EQ(e.counters().event, before.event);
    EXPECT_EQ(e.counters().trade, before.trade);
    MatchingEngine full{SymbolId{1}, Counters{max, 0, 0}};
    EXPECT_EQ(full.submit(req(1, Side::Buy, 1, 1)).error, Error::CounterExhausted);
    MatchingEngine events{SymbolId{1}, Counters{0, max, 0}};
    EXPECT_EQ(events.submit(req(1, Side::Buy, 1, 1)).error, Error::CounterExhausted);
}

TEST(Matching, EventExhaustionAfterFirstFillRollsBackEntireCommand) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    MatchingEngine engine{SymbolId{1}, Counters{0, max - 8, 0}};
    ASSERT_EQ(engine.submit(req(1, Side::Sell, 10, 1)).error, Error::None);
    ASSERT_EQ(engine.submit(req(2, Side::Sell, 11, 1)).error, Error::None);
    const auto before = engine.counters();
    const auto result = engine.submit(req(3, Side::Buy, 11, 2));
    EXPECT_EQ(result.error, Error::CounterExhausted);
    EXPECT_TRUE(result.trades.empty());
    ASSERT_EQ(result.reports.size(), 1U);
    EXPECT_EQ(result.reports.front().kind, ReportKind::Rejected);
    ASSERT_TRUE(engine.find(OrderId{1}));
    ASSERT_TRUE(engine.find(OrderId{2}));
    EXPECT_EQ(engine.find(OrderId{1})->quantity, 1U);
    EXPECT_EQ(engine.find(OrderId{2})->quantity, 1U);
    EXPECT_EQ(engine.counters().command, before.command);
    EXPECT_EQ(engine.counters().trade, before.trade);
    EXPECT_EQ(engine.counters().event, before.event);
}

TEST(Matching, SellConsumesHighestBidsFirstAndLeavesPartialAtHead) {
    MatchingEngine engine{SymbolId{1}};
    ASSERT_EQ(engine.submit(req(1, Side::Buy, 9, 2)).error, Error::None);
    ASSERT_EQ(engine.submit(req(2, Side::Buy, 10, 2)).error, Error::None);
    ASSERT_EQ(engine.submit(req(3, Side::Buy, 10, 3)).error, Error::None);
    auto result = engine.submit(req(4, Side::Sell, 9, 6));
    ASSERT_EQ(result.trades.size(), 3U);
    EXPECT_EQ(result.trades[0].buy_order_id, OrderId{2});
    EXPECT_EQ(result.trades[1].buy_order_id, OrderId{3});
    EXPECT_EQ(result.trades[2].buy_order_id, OrderId{1});
    EXPECT_EQ(result.trades[0].price, 10);
    EXPECT_EQ(result.trades[2].price, 9);
    ASSERT_TRUE(engine.find(OrderId{1}));
    EXPECT_EQ(engine.find(OrderId{1})->quantity, 1U);
}

TEST(Matching, AggregateOverflowRejectsWithoutMutation) {
    constexpr auto max = std::numeric_limits<Quantity>::max();
    MatchingEngine e{SymbolId{1}};
    ASSERT_EQ(e.submit(req(1, Side::Buy, 1, max)).error, Error::None);
    auto before = e.counters();
    EXPECT_EQ(e.submit(req(2, Side::Buy, 1, 1)).error, Error::BookOverflow);
    EXPECT_FALSE(e.find(OrderId{2}));
    EXPECT_EQ(e.find(OrderId{1})->quantity, max);
    EXPECT_EQ(e.counters().command, before.command);
}

TEST(Matching, OverflowWhileRestingRemainderRollsBackEarlierFill) {
    constexpr auto max = std::numeric_limits<Quantity>::max();
    MatchingEngine e{SymbolId{1}};
    ASSERT_EQ(e.submit(req(1, Side::Buy, 10, max)).error, Error::None);
    ASSERT_EQ(e.submit(req(2, Side::Sell, 20, 1)).error, Error::None);
    ASSERT_EQ(e.modify_resting(OrderId{2}, 10, 1), Result::Success);
    const auto before = e.counters();
    const auto out = e.submit(req(3, Side::Buy, 10, 2));
    EXPECT_EQ(out.error, Error::BookOverflow);
    EXPECT_TRUE(out.trades.empty());
    ASSERT_TRUE(e.find(OrderId{2}));
    EXPECT_EQ(e.find(OrderId{2})->quantity, 1U);
    EXPECT_EQ(e.find(OrderId{1})->quantity, max);
    EXPECT_EQ(e.counters().command, before.command);
    EXPECT_EQ(e.counters().event, before.event);
    EXPECT_EQ(e.counters().trade, before.trade);
}

TEST(Matching, MaximumQuantityMarketFillAndEventOrder) {
    constexpr auto max = std::numeric_limits<Quantity>::max();
    constexpr auto max_price = std::numeric_limits<Price>::max();
    MatchingEngine e{SymbolId{1}};
    const auto resting = e.submit(req(1, Side::Sell, max_price, max));
    ASSERT_EQ(resting.error, Error::None);
    ASSERT_EQ(resting.reports.size(), 2U);
    EXPECT_EQ(resting.command_sequence, 1U);
    EXPECT_EQ(resting.reports[0].sequence, 1U);
    EXPECT_EQ(resting.reports[1].sequence, 2U);
    const auto out = e.submit(req(2, Side::Buy, 0, max, OrderType::Market));
    ASSERT_EQ(out.error, Error::None);
    ASSERT_EQ(out.trades.size(), 1U);
    EXPECT_EQ(out.trades[0].quantity, max);
    EXPECT_EQ(out.trades[0].price, max_price);
    EXPECT_EQ(out.trades[0].id, TradeId{1});
    EXPECT_EQ(out.trades[0].sequence, 4U);
    ASSERT_EQ(out.reports.size(), 3U);
    EXPECT_EQ(out.reports[0].sequence, 3U);
    EXPECT_EQ(out.reports[1].sequence, 5U);
    EXPECT_EQ(out.reports[1].participant, Participant::Resting);
    EXPECT_EQ(out.reports[1].kind, ReportKind::Filled);
    EXPECT_EQ(out.reports[2].sequence, 6U);
    EXPECT_EQ(out.reports[2].participant, Participant::Incoming);
    EXPECT_EQ(out.reports[2].kind, ReportKind::Filled);
    EXPECT_EQ(out.remaining, 0U);
    EXPECT_FALSE(e.find(OrderId{1}));
}

TEST(Matching, IdenticalStreamsHaveIdenticalOutputs) {
    MatchingEngine a{SymbolId{1}}, b{SymbolId{1}};
    for (auto r : {req(1, Side::Sell, 3, 4), req(2, Side::Sell, 3, 2), req(3, Side::Buy, 3, 5),
                   req(4, Side::Buy, 0, 5, OrderType::Market)}) {
        auto x = a.submit(r), y = b.submit(r);
        ASSERT_EQ(x.error, y.error);
        ASSERT_EQ(x.trades.size(), y.trades.size());
        ASSERT_EQ(x.reports.size(), y.reports.size());
        EXPECT_EQ(x.command_sequence, y.command_sequence);
        for (std::size_t i = 0; i < x.trades.size(); ++i) {
            EXPECT_EQ(x.trades[i].id, y.trades[i].id);
            EXPECT_EQ(x.trades[i].sequence, y.trades[i].sequence);
            EXPECT_EQ(x.trades[i].price, y.trades[i].price);
            EXPECT_EQ(x.trades[i].quantity, y.trades[i].quantity);
        }
        for (std::size_t i = 0; i < x.reports.size(); ++i)
            EXPECT_EQ(x.reports[i].sequence, y.reports[i].sequence);
    }
    EXPECT_EQ(a.depth(Side::Sell, 10).size(), b.depth(Side::Sell, 10).size());
}

TEST(OrderBookFill, RejectsStaleAndInvalidSelections) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(Order{OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 1, 5, 1}),
              Result::Success);
    const auto selection = book.best_resting(Side::Sell);
    ASSERT_TRUE(selection);
    EXPECT_EQ(book.apply_fill(*selection, 0), Result::InvalidQuantity);
    EXPECT_EQ(book.apply_fill(*selection, 6), Result::InvalidQuantity);
    ASSERT_EQ(book.apply_fill(*selection, 2), Result::Success);
    EXPECT_EQ(book.apply_fill(*selection, 1), Result::StaleSelection);
    EXPECT_EQ(book.find(OrderId{1})->quantity, 3U);
    auto updated = book.best_resting(Side::Sell);
    ASSERT_TRUE(updated);
    ASSERT_EQ(book.cancel(OrderId{1}), Result::Success);
    ASSERT_EQ(book.add(Order{OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 1, 5, 1}),
              Result::Success);
    EXPECT_EQ(book.apply_fill(*updated, 1), Result::StaleSelection);
    auto other = book.clone();
    EXPECT_EQ(other->apply_fill(*book.best_resting(Side::Sell), 1), Result::StaleSelection);
    auto current = book.best_resting(Side::Sell);
    EXPECT_EQ(book.apply_fill(*current, 5), Result::Success);
    EXPECT_FALSE(book.best_resting(Side::Sell));
}

TEST(OrderBookFill, RejectedMutationKeepsSelectionButSuccessfulMutationInvalidatesIt) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(Order{OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 5, 3, 0}),
              Result::Success);
    auto selected = book.best_resting(Side::Sell);
    ASSERT_TRUE(selected);
    const auto copied_order = selected->order();
    EXPECT_EQ(book.modify(OrderId{1}, 0, 2), Result::InvalidPrice);
    EXPECT_EQ(book.cancel(OrderId{99}), Result::NotFound);
    EXPECT_EQ(book.apply_fill(*selected, 1), Result::Success);
    EXPECT_EQ(copied_order.quantity, 3U);
    auto after_fill = book.best_resting(Side::Sell);
    ASSERT_TRUE(after_fill);
    ASSERT_EQ(book.add(Order{OrderId{2}, SymbolId{1}, Side::Buy, OrderType::Limit, 1, 1, 0}),
              Result::Success);
    EXPECT_EQ(book.apply_fill(*after_fill, 1), Result::StaleSelection);
    EXPECT_EQ(book.find(OrderId{1})->quantity, 2U);
}
