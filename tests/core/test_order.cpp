#include "chronos/core/order.hpp"
#include "chronos/core/trade.hpp"

#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace chronos::core;

static_assert(!std::is_convertible_v<OrderId, TradeId>);
static_assert(!std::is_convertible_v<std::uint64_t, OrderId>);
static_assert(!std::is_constructible_v<SymbolId, OrderId>);
static_assert(!std::is_convertible_v<OrderId, SequenceNumber>);

TEST(Order, ConstructsLimitOrdersOnBothSides) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        const Order order{OrderId{11}, SymbolId{3}, side, OrderType::Limit, 123, 7, 9};
        EXPECT_EQ(order.id(), OrderId{11});
        EXPECT_EQ(order.symbol(), SymbolId{3});
        EXPECT_EQ(order.side(), side);
        EXPECT_EQ(order.type(), OrderType::Limit);
        EXPECT_EQ(order.price(), 123);
        EXPECT_EQ(order.original_quantity(), 7U);
        EXPECT_EQ(order.remaining_quantity(), order.original_quantity());
        EXPECT_EQ(order.status(), OrderStatus::New);
        EXPECT_EQ(order.sequence(), 9U);
    }
}

TEST(Order, ConstructsMarketOrderWithoutPrice) {
    const Order order{OrderId{2}, SymbolId{3}, Side::Sell, OrderType::Market, 0, 1, 4};
    EXPECT_EQ(order.type(), OrderType::Market);
    EXPECT_EQ(order.price(), 0);
    EXPECT_EQ(order.remaining_quantity(), 1U);
}

TEST(Order, AcceptsMaximumQuantityAndPrice) {
    constexpr auto quantity = std::numeric_limits<Quantity>::max();
    constexpr auto price = std::numeric_limits<Price>::max();
    const Order order{OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Limit, price, quantity, 0};
    EXPECT_EQ(order.price(), price);
    EXPECT_EQ(order.original_quantity(), quantity);
    EXPECT_EQ(order.remaining_quantity(), quantity);
}

TEST(Order, AcceptsMinimumPositiveLimitPriceAndQuantity) {
    const Order order{OrderId{0}, SymbolId{0}, Side::Sell, OrderType::Limit, 1, 1, 0};
    EXPECT_EQ(order.price(), 1);
    EXPECT_EQ(order.remaining_quantity(), 1U);
    EXPECT_EQ(order.id(), OrderId{0});
}

TEST(Order, RejectsMinimumSignedPrice) {
    EXPECT_THROW((Order{OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Limit,
                        std::numeric_limits<Price>::min(), 1, 1}),
                 std::invalid_argument);
}

TEST(Order, RejectsZeroQuantity) {
    EXPECT_THROW((Order{OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Limit, 1, 0, 1}),
                 std::invalid_argument);
}

TEST(Order, RejectsInvalidPricesAndEnumValues) {
    for (const Price price : {Price{0}, Price{-1}}) {
        EXPECT_THROW((Order{OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Limit, price, 1, 1}),
                     std::invalid_argument);
    }
    for (const Price price : {Price{-1}, Price{1}}) {
        EXPECT_THROW((Order{OrderId{1}, SymbolId{1}, Side::Buy, OrderType::Market, price, 1, 1}),
                     std::invalid_argument);
    }
    EXPECT_THROW((Order{OrderId{1}, SymbolId{1}, static_cast<Side>(99), OrderType::Limit, 1, 1, 1}),
                 std::invalid_argument);
    EXPECT_THROW((Order{OrderId{1}, SymbolId{1}, Side::Buy, static_cast<OrderType>(99), 1, 1, 1}),
                 std::invalid_argument);
}

TEST(Trade, StoresExecutionFields) {
    const Trade trade{TradeId{5}, SymbolId{6}, OrderId{7}, OrderId{8}, 100, 4, 12};
    EXPECT_EQ(trade.id, TradeId{5});
    EXPECT_EQ(trade.symbol, SymbolId{6});
    EXPECT_EQ(trade.buy_order_id, OrderId{7});
    EXPECT_EQ(trade.sell_order_id, OrderId{8});
    EXPECT_EQ(trade.price, 100);
    EXPECT_EQ(trade.quantity, 4U);
    EXPECT_EQ(trade.sequence, 12U);
}
