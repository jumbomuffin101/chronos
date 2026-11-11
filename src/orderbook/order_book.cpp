#include "chronos/orderbook/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace chronos::orderbook {

template <typename Map> Result OrderBook::insert(Map &levels, const OrderSnapshot &order) {
    auto [level_it, created] = levels.try_emplace(order.price);
    auto &level = level_it->second;
    if (order.quantity > std::numeric_limits<Quantity>::max() - level.total) {
        if (created)
            levels.erase(level_it);
        return Result::Overflow;
    }
    try {
        level.orders.push_back(order);
    } catch (...) {
        if (created)
            levels.erase(level_it);
        throw;
    }
    auto node = std::prev(level.orders.end());
    try {
        index_.emplace(order.id, Location{order.side, order.price, &level, node});
    } catch (...) {
        level.orders.erase(node);
        if (created)
            levels.erase(level_it);
        throw;
    }
    level.total += order.quantity;
    return Result::Success;
}

Result OrderBook::add(const core::Order &order) {
    if (order.symbol() != symbol_)
        return Result::WrongSymbol;
    if (order.type() != core::OrderType::Limit)
        return Result::UnsupportedOrderType;
    if (order.price() <= 0)
        return Result::InvalidPrice;
    if (order.remaining_quantity() == 0)
        return Result::InvalidQuantity;
    if (order.status() != core::OrderStatus::New ||
        order.original_quantity() != order.remaining_quantity())
        return Result::InvalidModification;
    if (index_.contains(order.id()))
        return Result::DuplicateId;
    const OrderSnapshot snapshot{
        order.id(),      order.symbol(), order.side(), order.price(), order.remaining_quantity(),
        order.sequence()};
    if (order.side() == Side::Buy)
        return insert(bids_, snapshot);
    if (order.side() == Side::Sell)
        return insert(asks_, snapshot);
    return Result::InvalidModification;
}

template <typename Map> void OrderBook::erase(Map &levels, const Location &location) {
    auto &level = *location.level;
    level.total -= location.order->quantity;
    level.orders.erase(location.order);
    if (level.orders.empty())
        levels.erase(location.price);
}

Result OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end())
        return Result::NotFound;
    const Location location = it->second;
    index_.erase(it);
    if (location.side == Side::Buy)
        erase(bids_, location);
    else
        erase(asks_, location);
    return Result::Success;
}

template <typename Map>
Result OrderBook::change(Map &levels, Location &location, Price price, Quantity quantity) {
    auto &old_level = *location.level;
    const Quantity old_quantity = location.order->quantity;
    if (price == location.price) {
        const Quantity other = old_level.total - old_quantity;
        if (quantity > std::numeric_limits<Quantity>::max() - other)
            return Result::Overflow;
        old_level.total = other + quantity;
        location.order->quantity = quantity;
        if (quantity > old_quantity)
            old_level.orders.splice(old_level.orders.end(), old_level.orders, location.order);
        return Result::Success;
    }
    auto [new_it, created] = levels.try_emplace(price);
    auto &new_level = new_it->second;
    if (quantity > std::numeric_limits<Quantity>::max() - new_level.total) {
        if (created)
            levels.erase(new_it);
        return Result::Overflow;
    }
    OrderSnapshot updated = *location.order;
    updated.price = price;
    updated.quantity = quantity;
    try {
        new_level.orders.push_back(updated);
    } catch (...) {
        if (created)
            levels.erase(new_it);
        throw;
    }
    auto new_order = std::prev(new_level.orders.end());
    new_level.total += quantity;
    const Location old = location;
    location = Location{old.side, price, &new_level, new_order};
    erase(levels, old);
    return Result::Success;
}

Result OrderBook::modify(OrderId id, Price price, Quantity quantity) {
    auto it = index_.find(id);
    if (it == index_.end())
        return Result::NotFound;
    if (price <= 0)
        return Result::InvalidPrice;
    if (quantity == 0)
        return Result::InvalidQuantity;
    auto &location = it->second;
    if (location.side == Side::Buy)
        return change(bids_, location, price, quantity);
    return change(asks_, location, price, quantity);
}

std::optional<OrderSnapshot> OrderBook::find(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end())
        return std::nullopt;
    return *it->second.order;
}

template <typename Map>
std::optional<LevelSnapshot> OrderBook::level_in(const Map &levels, Price price) {
    const auto it = levels.find(price);
    if (it == levels.end())
        return std::nullopt;
    return LevelSnapshot{it->first, it->second.total, it->second.orders.size()};
}

std::optional<LevelSnapshot> OrderBook::best_bid() const {
    if (bids_.empty())
        return std::nullopt;
    const auto &entry = *bids_.begin();
    return LevelSnapshot{entry.first, entry.second.total, entry.second.orders.size()};
}
std::optional<LevelSnapshot> OrderBook::best_ask() const {
    if (asks_.empty())
        return std::nullopt;
    const auto &entry = *asks_.begin();
    return LevelSnapshot{entry.first, entry.second.total, entry.second.orders.size()};
}
std::optional<LevelSnapshot> OrderBook::level(Side side, Price price) const {
    if (side == Side::Buy)
        return level_in(bids_, price);
    if (side == Side::Sell)
        return level_in(asks_, price);
    return std::nullopt;
}
template <typename Map>
std::vector<LevelSnapshot> OrderBook::depth_in(const Map &levels, std::size_t limit) {
    std::vector<LevelSnapshot> result;
    result.reserve(std::min(limit, levels.size()));
    for (const auto &[price, level] : levels) {
        if (result.size() == limit)
            break;
        result.push_back(LevelSnapshot{price, level.total, level.orders.size()});
    }
    return result;
}
std::vector<LevelSnapshot> OrderBook::depth(Side side, std::size_t limit) const {
    if (side == Side::Buy)
        return depth_in(bids_, limit);
    if (side == Side::Sell)
        return depth_in(asks_, limit);
    return {};
}
template <typename Map>
std::vector<OrderSnapshot> OrderBook::orders_in(const Map &levels, Price price) {
    const auto it = levels.find(price);
    if (it == levels.end())
        return {};
    return {it->second.orders.begin(), it->second.orders.end()};
}
std::vector<OrderSnapshot> OrderBook::orders_at(Side side, Price price) const {
    if (side == Side::Buy)
        return orders_in(bids_, price);
    if (side == Side::Sell)
        return orders_in(asks_, price);
    return {};
}

} // namespace chronos::orderbook
