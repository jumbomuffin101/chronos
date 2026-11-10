#pragma once

#include "chronos/core/types.hpp"

namespace chronos::core {

// A trade is a value record; creation and validation of executions belong to the future engine.
struct Trade {
    TradeId id;              // Unique execution identifier.
    SymbolId symbol;         // Instrument that was traded.
    OrderId buy_order_id;    // Participating buy order.
    OrderId sell_order_id;   // Participating sell order.
    Price price;             // Execution price in instrument ticks.
    Quantity quantity;       // Executed units.
    SequenceNumber sequence; // Execution's position in the event stream.
};

} // namespace chronos::core
