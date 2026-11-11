#pragma once

#include "chronos/core/order.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace chronos::orderbook {

using core::OrderId;
using core::Price;
using core::Quantity;
using core::Side;
using core::SymbolId;

enum class Result {
    Success,
    NotFound,
    DuplicateId,
    InvalidQuantity,
    InvalidPrice,
    WrongSymbol,
    InvalidModification,
    Overflow,
    UnsupportedOrderType
};

struct OrderSnapshot {
    OrderId id;
    SymbolId symbol;
    Side side;
    Price price;
    Quantity quantity;
    // Metadata supplied at original construction, not a priority or book event sequence.
    core::SequenceNumber sequence;
};

struct LevelSnapshot {
    Price price;
    Quantity quantity;
    std::size_t order_count;
};

// One book per symbol. Calls must be serialized. FIFO is the order of successful
// add/priority-losing modify calls, independent of caller-provided sequence metadata.
class OrderBook {
  public:
    explicit OrderBook(SymbolId symbol) noexcept : symbol_(symbol) {}
    OrderBook(const OrderBook &) = delete;
    OrderBook &operator=(const OrderBook &) = delete;
    OrderBook(OrderBook &&) = delete;
    OrderBook &operator=(OrderBook &&) = delete;

    Result add(const core::Order &order);
    Result cancel(OrderId id);
    Result modify(OrderId id, Price price, Quantity quantity);

    [[nodiscard]] std::optional<OrderSnapshot> find(OrderId id) const;
    [[nodiscard]] std::optional<LevelSnapshot> best_bid() const;
    [[nodiscard]] std::optional<LevelSnapshot> best_ask() const;
    [[nodiscard]] std::optional<LevelSnapshot> level(Side side, Price price) const;
    [[nodiscard]] std::vector<LevelSnapshot> depth(Side side, std::size_t limit) const;
    [[nodiscard]] std::vector<OrderSnapshot> orders_at(Side side, Price price) const;
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }

  private:
    struct Level {
        Quantity total = 0;
        std::list<OrderSnapshot> orders;
    };
    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level>;
    struct Location {
        Side side;
        Price price;
        Level *level; // Non-owning; map nodes remain stable until their level is erased.
        std::list<OrderSnapshot>::iterator order;
    };
    struct IdHash {
        std::size_t operator()(OrderId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

    template <typename Map> Result insert(Map &levels, const OrderSnapshot &order);
    template <typename Map>
    Result change(Map &levels, Location &location, Price price, Quantity quantity);
    template <typename Map> void erase(Map &levels, const Location &location);
    template <typename Map>
    static std::optional<LevelSnapshot> level_in(const Map &levels, Price price);
    template <typename Map>
    static std::vector<LevelSnapshot> depth_in(const Map &levels, std::size_t limit);
    template <typename Map>
    static std::vector<OrderSnapshot> orders_in(const Map &levels, Price price);

    SymbolId symbol_;
    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Location, IdHash> index_;
};

} // namespace chronos::orderbook
