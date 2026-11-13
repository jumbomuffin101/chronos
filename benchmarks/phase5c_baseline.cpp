#include "chronos/exchange/exchange.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
thread_local bool active = false;
thread_local std::size_t count = 0, bytes = 0;
} // namespace
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) {
        if (active) {
            ++count;
            bytes += size;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace chronos;
void check(bool v) {
    if (!v)
        throw std::runtime_error("phase5c benchmark verification failed");
}
void drain(exchange::Exchange &e, std::uint64_t id) {
    while (e.front(id))
        e.pop(id);
    while (auto event = e.market_front(id))
        e.market_pop(id, *event);
}
} // namespace
int main() {
    try {
        std::cout << "background,subscribers,fill,sample,latency_ns,allocations,bytes\n";
        for (const auto background : {10U, 100U})
            for (const auto subscribers : {0U, 4U})
                for (const bool fill : {false, true})
                    for (int sample = -1; sample < 9; ++sample) {
                        exchange::Exchange e{6, 200, 128, 200};
                        check(e.open_session(1));
                        check(e.open_session(2));
                        for (std::uint64_t j = 0; j < subscribers; ++j) {
                            check(e.open_session(3 + j));
                            check(e.subscribe(3 + j, core::SymbolId{1}));
                            drain(e, 3 + j);
                        }
                        for (std::uint64_t i = 1; i <= background; ++i) {
                            check(e.submit(1, i, i, core::SymbolId{1}, core::Side::Sell,
                                           core::OrderType::Limit, 110,
                                           1) == exchange::Result::Accepted);
                            drain(e, 1);
                            for (std::uint64_t j = 0; j < subscribers; ++j)
                                drain(e, 3 + j);
                        }
                        count = bytes = 0;
                        active = true;
                        const auto start = std::chrono::steady_clock::now();
                        const auto result =
                            e.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                     fill ? core::OrderType::Market : core::OrderType::Limit,
                                     fill ? 0 : 100, 1);
                        const auto end = std::chrono::steady_clock::now();
                        active = false;
                        check(result == exchange::Result::Accepted);
                        check(e.trade_count() == (fill ? 1U : 0U));
                        if (fill)
                            check(e.trade(0)->price == 110 &&
                                  e.position(2, core::SymbolId{1}) == 1);
                        else
                            check(e.find(2, 1).has_value());
                        for (std::uint64_t j = 0; j < subscribers; ++j) {
                            const auto event = e.market_front(3 + j);
                            check(event && event->kind == (fill ? exchange::MarketKind::Trade
                                                                : exchange::MarketKind::Book));
                        }
                        if (sample >= 0)
                            std::cout
                                << background << ',' << subscribers << ',' << fill << ',' << sample
                                << ','
                                << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
                                       .count()
                                << ',' << count << ',' << bytes << '\n';
                    }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
