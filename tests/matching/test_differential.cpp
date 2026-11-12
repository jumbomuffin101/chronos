#include "chronos/matching/matching_engine.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <random>

using namespace chronos::core;
using namespace chronos::matching;

namespace {
void compare_outcomes(const Outcome &a, const Outcome &b) {
    ASSERT_EQ(a.error, b.error);
    ASSERT_EQ(a.command_sequence, b.command_sequence);
    ASSERT_EQ(a.remaining, b.remaining);
    ASSERT_EQ(a.trades.size(), b.trades.size());
    ASSERT_EQ(a.reports.size(), b.reports.size());
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        const auto &x = a.trades[i];
        const auto &y = b.trades[i];
        EXPECT_EQ(x.id, y.id);
        EXPECT_EQ(x.symbol, y.symbol);
        EXPECT_EQ(x.buy_order_id, y.buy_order_id);
        EXPECT_EQ(x.sell_order_id, y.sell_order_id);
        EXPECT_EQ(x.price, y.price);
        EXPECT_EQ(x.quantity, y.quantity);
        EXPECT_EQ(x.sequence, y.sequence);
    }
    for (std::size_t i = 0; i < a.reports.size(); ++i) {
        const auto &x = a.reports[i];
        const auto &y = b.reports[i];
        EXPECT_EQ(x.order_id, y.order_id);
        EXPECT_EQ(x.participant, y.participant);
        EXPECT_EQ(x.kind, y.kind);
        EXPECT_EQ(x.remaining, y.remaining);
        EXPECT_EQ(x.sequence, y.sequence);
    }
}
void compare_books(const MatchingEngine &a, const MatchingEngine &b) {
    EXPECT_EQ(a.counters().command, b.counters().command);
    EXPECT_EQ(a.counters().event, b.counters().event);
    EXPECT_EQ(a.counters().trade, b.counters().trade);
    for (auto side : {Side::Buy, Side::Sell}) {
        const auto left = a.depth(side, 1000);
        const auto right = b.depth(side, 1000);
        ASSERT_EQ(left.size(), right.size());
        for (std::size_t i = 0; i < left.size(); ++i) {
            EXPECT_EQ(left[i].price, right[i].price);
            EXPECT_EQ(left[i].quantity, right[i].quantity);
            EXPECT_EQ(left[i].order_count, right[i].order_count);
            const auto x = a.orders_at(side, left[i].price);
            const auto y = b.orders_at(side, right[i].price);
            ASSERT_EQ(x.size(), y.size());
            for (std::size_t j = 0; j < x.size(); ++j) {
                EXPECT_EQ(x[j].id, y[j].id);
                EXPECT_EQ(x[j].symbol, y[j].symbol);
                EXPECT_EQ(x[j].side, y[j].side);
                EXPECT_EQ(x[j].price, y[j].price);
                EXPECT_EQ(x[j].quantity, y[j].quantity);
                EXPECT_EQ(x[j].sequence, y[j].sequence);
            }
        }
    }
}
} // namespace

TEST(MatchingDifferential, FixedSeedMixedCommandStream) {
    MatchingEngine planned{SymbolId{1}};
    MatchingEngine reference{SymbolId{1}, {}, TransactionMode::CloneReference};
    std::mt19937 rng{0xC0FFEEU};
    for (int step = 0; step < 1000; ++step) {
        SCOPED_TRACE(step);
        const auto id = OrderId{static_cast<std::uint64_t>(rng() % 35U)};
        const auto price = static_cast<Price>(rng() % 13U);
        const auto quantity = static_cast<Quantity>(rng() % 6U);
        const auto side = rng() % 2U == 0 ? Side::Buy : Side::Sell;
        const auto kind = rng() % 5U;
        if (kind <= 2U) {
            const auto type = kind == 2U ? OrderType::Market : OrderType::Limit;
            const auto symbol = rng() % 17U == 0 ? SymbolId{2} : SymbolId{1};
            const Request request{
                id, symbol, side, type, type == OrderType::Market ? 0 : price, quantity};
            compare_outcomes(planned.submit(request), reference.submit(request));
        } else if (kind == 3U) {
            EXPECT_EQ(planned.cancel_resting(id), reference.cancel_resting(id));
        } else {
            EXPECT_EQ(planned.modify_resting(id, price, quantity),
                      reference.modify_resting(id, price, quantity));
        }
        compare_books(planned, reference);
    }
}

TEST(MatchingDifferential, CounterAndAggregateBoundaries) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    for (const Counters seed :
         {Counters{max, 0, 0}, Counters{0, max, 0}, Counters{0, 0, max}, Counters{0, max - 8, 0}}) {
        MatchingEngine planned{SymbolId{1}, seed};
        MatchingEngine reference{SymbolId{1}, seed, TransactionMode::CloneReference};
        for (const Request request :
             {Request{OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 1, 1},
              Request{OrderId{2}, SymbolId{1}, Side::Sell, OrderType::Limit, 2, 1},
              Request{OrderId{3}, SymbolId{1}, Side::Buy, OrderType::Limit, 2, 2}}) {
            compare_outcomes(planned.submit(request), reference.submit(request));
            compare_books(planned, reference);
        }
    }
    MatchingEngine planned{SymbolId{1}};
    MatchingEngine reference{SymbolId{1}, {}, TransactionMode::CloneReference};
    for (const Request request :
         {Request{OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Limit, 10, max},
          Request{OrderId{2}, SymbolId{1}, Side::Sell, OrderType::Limit, 20, 1}})
        compare_outcomes(planned.submit(request), reference.submit(request));
    ASSERT_EQ(planned.modify_resting(OrderId{2}, 10, 1),
              reference.modify_resting(OrderId{2}, 10, 1));
    const Request request{OrderId{3}, SymbolId{1}, Side::Buy, OrderType::Limit, 10, 2};
    compare_outcomes(planned.submit(request), reference.submit(request));
    compare_books(planned, reference);
}

TEST(MatchingDifferential, RemainderInsertionAcrossIndexGrowthPreservesFills) {
    MatchingEngine planned{SymbolId{1}};
    MatchingEngine reference{SymbolId{1}, {}, TransactionMode::CloneReference};
    // Exercise a range of index sizes, including multiple implementation-specific
    // rehash thresholds, while preserving a multi-level opposing prefix.
    for (std::uint64_t id = 1; id <= 300; ++id) {
        const Request background{OrderId{id}, SymbolId{1}, Side::Buy, OrderType::Limit, 1, 1};
        compare_outcomes(planned.submit(background), reference.submit(background));
        if (id % 7 == 0) {
            const Request first{OrderId{1000}, SymbolId{1}, Side::Sell, OrderType::Limit, 10, 1};
            const Request second{OrderId{1001}, SymbolId{1}, Side::Sell, OrderType::Limit, 11, 2};
            compare_outcomes(planned.submit(first), reference.submit(first));
            compare_outcomes(planned.submit(second), reference.submit(second));
            // Two fills followed by a remainder insertion on the incoming side.
            const Request incoming{OrderId{2000}, SymbolId{1}, Side::Buy, OrderType::Limit, 11, 4};
            compare_outcomes(planned.submit(incoming), reference.submit(incoming));
            compare_books(planned, reference);
            ASSERT_EQ(planned.cancel_resting(OrderId{2000}),
                      reference.cancel_resting(OrderId{2000}));
        }
    }
    compare_books(planned, reference);
}
