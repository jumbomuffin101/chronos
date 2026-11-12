#include "chronos/matching/matching_engine.hpp"

#include <iostream>

int main() {
    using namespace chronos::core;
    using namespace chronos::matching;
    MatchingEngine engine{SymbolId{1}};
    std::cout << "Chronos " << CHRONOS_VERSION << " - Phase 3\n";
    const auto first =
        engine.submit({OrderId{1001}, SymbolId{1}, Side::Buy, OrderType::Limit, 20000, 100});
    const auto second =
        engine.submit({OrderId{1002}, SymbolId{1}, Side::Sell, OrderType::Limit, 20000, 40});
    const auto third =
        engine.submit({OrderId{1003}, SymbolId{1}, Side::Sell, OrderType::Market, 0, 30});
    if (first.error != Error::None || second.error != Error::None || third.error != Error::None)
        return 1;
    std::cout << "Buy 100 at 20000 ticks rests.\n";
    for (const auto *outcome : {&second, &third}) {
        for (const auto &trade : outcome->trades)
            std::cout << "Trade " << trade.id.value() << ": buy " << trade.buy_order_id.value()
                      << ", sell " << trade.sell_order_id.value() << ", quantity " << trade.quantity
                      << " at " << trade.price << " ticks\n";
    }
    const auto remaining = engine.find(OrderId{1001});
    if (!remaining)
        return 1;
    std::cout << "Remaining buy: " << remaining->quantity << " at " << remaining->price
              << " ticks\n";
}
