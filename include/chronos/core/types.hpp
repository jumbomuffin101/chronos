#pragma once

#include <compare>
#include <cstdint>

namespace chronos::core {

// Identifiers are distinct types even when their underlying representation is the same.
template <typename Tag, typename Rep> class Identifier {
  public:
    explicit constexpr Identifier(Rep value) noexcept : value_(value) {}
    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    auto operator<=>(const Identifier &) const = default;

  private:
    Rep value_;
};

struct OrderIdTag;
struct TradeIdTag;
struct SymbolIdTag;
using OrderId = Identifier<OrderIdTag, std::uint64_t>;
using TradeId = Identifier<TradeIdTag, std::uint64_t>;
using SymbolId = Identifier<SymbolIdTag, std::uint32_t>;

// Prices are signed integer ticks: exact comparisons avoid floating-point rounding.
// Tick size and the interpretation of a tick are defined per instrument in a later phase.
using Price = std::int64_t;
using Quantity = std::uint64_t;
using SequenceNumber = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class OrderStatus : std::uint8_t { New, PartiallyFilled, Filled, Cancelled, Rejected };

} // namespace chronos::core
