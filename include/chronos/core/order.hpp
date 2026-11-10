#pragma once

#include "chronos/core/types.hpp"

#include <stdexcept>

namespace chronos::core {

// A newly constructed order has positive original quantity, equal remaining quantity,
// and New status. Limit prices must be positive; market orders have price zero.
// No lifecycle mutation or matching is implemented in Phase 1.
class Order {
  public:
    Order(OrderId id, SymbolId symbol, Side side, OrderType type, Price price, Quantity quantity,
          SequenceNumber sequence)
        : id_(id), symbol_(symbol), side_(side), type_(type), price_(price),
          original_quantity_(quantity), remaining_quantity_(quantity), sequence_(sequence) {
        if (side != Side::Buy && side != Side::Sell) {
            throw std::invalid_argument("invalid order side");
        }
        if (quantity == 0) {
            throw std::invalid_argument("order quantity must be positive");
        }
        if ((type == OrderType::Limit && price <= 0) || (type == OrderType::Market && price != 0) ||
            (type != OrderType::Limit && type != OrderType::Market)) {
            throw std::invalid_argument("invalid order type or price");
        }
    }

    [[nodiscard]] OrderId id() const noexcept { return id_; }
    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }
    [[nodiscard]] Side side() const noexcept { return side_; }
    [[nodiscard]] OrderType type() const noexcept { return type_; }
    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] Quantity original_quantity() const noexcept { return original_quantity_; }
    [[nodiscard]] Quantity remaining_quantity() const noexcept { return remaining_quantity_; }
    [[nodiscard]] OrderStatus status() const noexcept { return status_; }
    [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }

  private:
    OrderId id_;
    SymbolId symbol_;
    Side side_;
    OrderType type_;
    Price price_;
    Quantity original_quantity_;
    Quantity remaining_quantity_;
    OrderStatus status_{OrderStatus::New};
    SequenceNumber sequence_;
};

} // namespace chronos::core
