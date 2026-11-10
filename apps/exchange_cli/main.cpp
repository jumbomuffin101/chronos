#include "chronos/core/order.hpp"

#include <iostream>

int main() {
    using namespace chronos::core;
    const Order order{OrderId{1}, SymbolId{42}, Side::Buy, OrderType::Limit, 1250, 10, 1};
    std::cout << "Chronos " << CHRONOS_VERSION << '\n'
              << "Order " << order.id().value() << ": symbol " << order.symbol().value()
              << ", buy limit, price " << order.price() << " ticks, quantity "
              << order.remaining_quantity() << ", sequence " << order.sequence() << '\n';
}
