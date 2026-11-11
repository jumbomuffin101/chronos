#include "chronos/orderbook/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <random>
#include <vector>

using namespace chronos::core;
using namespace chronos::orderbook;

namespace {
Order make_order(std::uint64_t id, Side side, Price price, Quantity quantity = 1,
                 SymbolId symbol = SymbolId{1}, SequenceNumber sequence = 0) {
    return Order{OrderId{id}, symbol, side, OrderType::Limit, price, quantity, sequence};
}
std::vector<std::uint64_t> ids(const std::vector<OrderSnapshot> &orders) {
    std::vector<std::uint64_t> result;
    for (const auto &order : orders)
        result.push_back(order.id.value());
    return result;
}
} // namespace

TEST(OrderBook, EmptyQueries) {
    OrderBook book{SymbolId{1}};
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.find(OrderId{1}));
    EXPECT_FALSE(book.level(Side::Buy, 1));
    EXPECT_TRUE(book.depth(Side::Sell, 10).empty());
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.cancel(OrderId{1}), Result::NotFound);
}

TEST(OrderBook, OrdersLevelsAndSnapshots) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(make_order(1, Side::Buy, 100, 4, SymbolId{1}, 99)), Result::Success);
    ASSERT_EQ(book.add(make_order(2, Side::Buy, 100, 3, SymbolId{1}, 1)), Result::Success);
    ASSERT_EQ(book.add(make_order(3, Side::Buy, 101, 2)), Result::Success);
    ASSERT_EQ(book.add(make_order(4, Side::Sell, 105, 5)), Result::Success);
    ASSERT_EQ(book.add(make_order(5, Side::Sell, 103, 6)), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 100)), (std::vector<std::uint64_t>{1, 2}));
    ASSERT_TRUE(book.best_bid());
    EXPECT_EQ(book.best_bid()->price, 101);
    ASSERT_TRUE(book.best_ask());
    EXPECT_EQ(book.best_ask()->price, 103);
    ASSERT_TRUE(book.level(Side::Buy, 100));
    EXPECT_EQ(book.level(Side::Buy, 100)->quantity, 7U);
    EXPECT_EQ(book.level(Side::Buy, 100)->order_count, 2U);
    EXPECT_EQ(book.order_count(), 5U);
    EXPECT_EQ(book.bid_levels(), 2U);
    EXPECT_EQ(book.ask_levels(), 2U);
    ASSERT_EQ(book.depth(Side::Buy, 10).size(), 2U);
    EXPECT_EQ(book.depth(Side::Buy, 10)[1].price, 100);
    ASSERT_EQ(book.depth(Side::Sell, 1).size(), 1U);
    EXPECT_EQ(book.depth(Side::Sell, 1)[0].price, 103);
    EXPECT_TRUE(book.depth(Side::Buy, 0).empty());
    auto copy = book.find(OrderId{1});
    ASSERT_TRUE(copy);
    copy->quantity = 99;
    EXPECT_EQ(book.find(OrderId{1})->quantity, 4U);
    EXPECT_EQ(book.find(OrderId{1})->sequence, 99U);
}

TEST(OrderBook, RejectsDuplicateWrongSymbolAndMarket) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(make_order(1, Side::Buy, 100, 2)), Result::Success);
    EXPECT_EQ(book.add(make_order(1, Side::Sell, 101)), Result::DuplicateId);
    EXPECT_EQ(book.add(make_order(2, Side::Sell, 101, 1, SymbolId{2})), Result::WrongSymbol);
    const Order market{OrderId{3}, SymbolId{1}, Side::Buy, OrderType::Market, 0, 1, 0};
    EXPECT_EQ(book.add(market), Result::UnsupportedOrderType);
    EXPECT_EQ(book.order_count(), 1U);
    EXPECT_EQ(book.find(OrderId{1})->quantity, 2U);
}

TEST(OrderBook, CancelsFirstMiddleLastAndReusesId) {
    OrderBook book{SymbolId{1}};
    for (std::uint64_t id = 1; id <= 4; ++id)
        ASSERT_EQ(book.add(make_order(id, Side::Buy, 10, id)), Result::Success);
    ASSERT_EQ(book.cancel(OrderId{1}), Result::Success);
    ASSERT_EQ(book.cancel(OrderId{3}), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 10)), (std::vector<std::uint64_t>{2, 4}));
    EXPECT_EQ(book.level(Side::Buy, 10)->quantity, 6U);
    ASSERT_EQ(book.cancel(OrderId{4}), Result::Success);
    ASSERT_EQ(book.add(make_order(1, Side::Buy, 10)), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 10)), (std::vector<std::uint64_t>{2, 1}));
    ASSERT_EQ(book.cancel(OrderId{2}), Result::Success);
    ASSERT_EQ(book.cancel(OrderId{1}), Result::Success);
    EXPECT_FALSE(book.best_bid());
    EXPECT_EQ(book.bid_levels(), 0U);
    EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBook, ModificationPriorityAndLookup) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(make_order(1, Side::Buy, 10, 5)), Result::Success);
    ASSERT_EQ(book.add(make_order(2, Side::Buy, 10, 5)), Result::Success);
    ASSERT_EQ(book.modify(OrderId{1}, 10, 3), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 10)), (std::vector<std::uint64_t>{1, 2}));
    ASSERT_EQ(book.modify(OrderId{1}, 10, 3), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 10)), (std::vector<std::uint64_t>{1, 2}));
    ASSERT_EQ(book.modify(OrderId{1}, 10, 6), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 10)), (std::vector<std::uint64_t>{2, 1}));
    ASSERT_EQ(book.modify(OrderId{2}, 11, 4), Result::Success);
    EXPECT_EQ(book.find(OrderId{2})->price, 11);
    EXPECT_EQ(book.level(Side::Buy, 10)->quantity, 6U);
    ASSERT_EQ(book.modify(OrderId{2}, 10, 4), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, 10)), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_FALSE(book.level(Side::Buy, 11));
    EXPECT_EQ(book.cancel(OrderId{2}), Result::Success);
}

TEST(OrderBook, RejectsInvalidModificationWithoutMutation) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(make_order(1, Side::Sell, 20, 3)), Result::Success);
    EXPECT_EQ(book.modify(OrderId{2}, 20, 1), Result::NotFound);
    EXPECT_EQ(book.modify(OrderId{1}, 0, 1), Result::InvalidPrice);
    EXPECT_EQ(book.modify(OrderId{1}, -1, 1), Result::InvalidPrice);
    EXPECT_EQ(book.modify(OrderId{1}, 21, 0), Result::InvalidQuantity);
    EXPECT_EQ(book.find(OrderId{1})->price, 20);
    EXPECT_EQ(book.find(OrderId{1})->quantity, 3U);
    EXPECT_EQ(book.ask_levels(), 1U);
}

TEST(OrderBook, DetectsOverflowOnInsertionAndModification) {
    OrderBook book{SymbolId{1}};
    constexpr Quantity max = std::numeric_limits<Quantity>::max();
    constexpr Price max_price = std::numeric_limits<Price>::max();
    ASSERT_EQ(book.add(make_order(1, Side::Buy, max_price, max - 1)), Result::Success);
    ASSERT_EQ(book.add(make_order(2, Side::Buy, max_price, 1)), Result::Success);
    EXPECT_EQ(book.add(make_order(3, Side::Buy, max_price, 1)), Result::Overflow);
    EXPECT_EQ(book.modify(OrderId{2}, max_price, 2), Result::Overflow);
    ASSERT_EQ(book.add(make_order(3, Side::Buy, 1, 1)), Result::Success);
    EXPECT_EQ(book.modify(OrderId{3}, max_price, 1), Result::Overflow);
    EXPECT_EQ(book.find(OrderId{3})->price, 1);
    EXPECT_EQ(book.level(Side::Buy, max_price)->quantity, max);
    EXPECT_EQ(ids(book.orders_at(Side::Buy, max_price)), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(book.order_count(), 3U);
}

TEST(OrderBook, RepeatedAlternatingInsertionAndCancellation) {
    OrderBook book{SymbolId{1}};
    for (std::uint64_t i = 0; i < 100; ++i) {
        const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
        ASSERT_EQ(book.add(make_order(i, side, 10)), Result::Success);
        ASSERT_EQ(book.cancel(OrderId{i}), Result::Success);
    }
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.bid_levels(), 0U);
    EXPECT_EQ(book.ask_levels(), 0U);
}

TEST(OrderBook, RehashAndLevelErasurePreserveOtherLocationsAndSnapshots) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(make_order(0, Side::Sell, 10, 5, SymbolId{1}, 900)), Result::Success);
    const auto saved = book.find(OrderId{0});
    ASSERT_TRUE(saved);
    for (std::uint64_t id = 1; id <= 512; ++id)
        ASSERT_EQ(book.add(make_order(id, Side::Sell, 11)), Result::Success);
    ASSERT_EQ(book.modify(OrderId{0}, 11, 4), Result::Success);
    EXPECT_FALSE(book.level(Side::Sell, 10));
    EXPECT_EQ(saved->price, 10);
    EXPECT_EQ(saved->quantity, 5U);
    EXPECT_EQ(book.orders_at(Side::Sell, 11).back().id, OrderId{0});
    ASSERT_EQ(book.cancel(OrderId{0}), Result::Success);
    for (std::uint64_t id = 1; id <= 512; ++id) {
        ASSERT_TRUE(book.find(OrderId{id}));
        ASSERT_EQ(book.cancel(OrderId{id}), Result::Success);
    }
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_FALSE(book.best_ask());
}

TEST(OrderBook, SellPriorityAndReusedIdFollowOperationsNotSequence) {
    OrderBook book{SymbolId{1}};
    ASSERT_EQ(book.add(make_order(1, Side::Sell, 10, 2, SymbolId{1}, 100)), Result::Success);
    ASSERT_EQ(book.add(make_order(2, Side::Sell, 10, 2, SymbolId{1}, 0)), Result::Success);
    ASSERT_EQ(book.modify(OrderId{1}, 10, 3), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Sell, 10)), (std::vector<std::uint64_t>{2, 1}));
    ASSERT_EQ(book.cancel(OrderId{2}), Result::Success);
    ASSERT_EQ(book.add(make_order(2, Side::Sell, 10, 1, SymbolId{1}, 0)), Result::Success);
    EXPECT_EQ(ids(book.orders_at(Side::Sell, 10)), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(book.find(OrderId{1})->sequence, 100U);
}

TEST(OrderBook, FixedSeedOperationsMatchReference) {
    struct Ref {
        Side side;
        Price price;
        Quantity quantity;
        std::uint64_t priority;
    };
    std::map<std::uint64_t, Ref> reference;
    std::mt19937 rng{12345};
    std::uint64_t priority = 0;
    OrderBook book{SymbolId{1}};
    for (int step = 0; step < 500; ++step) {
        const std::uint64_t id = rng() % 20U;
        const Price price = static_cast<Price>(rng() % 5U + 1U);
        const Quantity qty = rng() % 10U + 1U;
        const Side side = rng() % 2U == 0 ? Side::Buy : Side::Sell;
        const auto it = reference.find(id);
        switch (rng() % 3U) {
        case 0:
            if (it == reference.end()) {
                ASSERT_EQ(book.add(make_order(id, side, price, qty)), Result::Success);
                reference.emplace(id, Ref{side, price, qty, priority++});
            } else
                EXPECT_EQ(book.add(make_order(id, side, price, qty)), Result::DuplicateId);
            break;
        case 1:
            if (it == reference.end())
                EXPECT_EQ(book.cancel(OrderId{id}), Result::NotFound);
            else {
                ASSERT_EQ(book.cancel(OrderId{id}), Result::Success);
                reference.erase(it);
            }
            break;
        default:
            if (it == reference.end())
                EXPECT_EQ(book.modify(OrderId{id}, price, qty), Result::NotFound);
            else {
                ASSERT_EQ(book.modify(OrderId{id}, price, qty), Result::Success);
                if (price != it->second.price || qty > it->second.quantity)
                    it->second.priority = priority++;
                it->second.price = price;
                it->second.quantity = qty;
            }
        }
        ASSERT_EQ(book.order_count(), reference.size());
        for (std::uint64_t check_id = 0; check_id < 20; ++check_id) {
            const auto found = book.find(OrderId{check_id});
            const auto expected = reference.find(check_id);
            ASSERT_EQ(found.has_value(), expected != reference.end());
            if (found) {
                EXPECT_EQ(found->price, expected->second.price);
                EXPECT_EQ(found->quantity, expected->second.quantity);
                EXPECT_EQ(found->side, expected->second.side);
            }
        }
        for (Side s : {Side::Buy, Side::Sell}) {
            for (Price p = 1; p <= 5; ++p) {
                std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
                Quantity sum = 0;
                for (const auto &[ref_id, ref] : reference) {
                    if (ref.side == s && ref.price == p) {
                        expected.emplace_back(ref.priority, ref_id);
                        sum += ref.quantity;
                    }
                }
                std::sort(expected.begin(), expected.end());
                const auto actual = book.orders_at(s, p);
                ASSERT_EQ(actual.size(), expected.size());
                for (std::size_t j = 0; j < actual.size(); ++j)
                    EXPECT_EQ(actual[j].id.value(), expected[j].second);
                const auto level = book.level(s, p);
                ASSERT_EQ(level.has_value(), !expected.empty());
                if (level) {
                    EXPECT_EQ(level->quantity, sum);
                    EXPECT_EQ(level->order_count, expected.size());
                }
            }
        }
    }
}
