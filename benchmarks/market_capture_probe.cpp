#include "chronos/exchange/live_owner.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0, bytes = 0;
} // namespace
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) {
        if (counting) {
            ++allocations;
            bytes += size;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

int main() {
    using namespace chronos;
    using Clock = std::chrono::steady_clock;
    std::cout << "mode,subscribers,sample,latency_ns,scalar_allocations,requested_bytes,events\n";
    for (int mode = 0; mode < 3; ++mode) {
        for (std::uint64_t subscribers : {0U, 8U}) {
            for (int sample = -5; sample < 100; ++sample) {
                exchange::LiveExchangeOwner owner(10, 16, 32, 16,
                                                  exchange::AllocationMode::Exclusive);
                owner.bind();
                for (std::uint64_t id = 1; id <= 2 + subscribers; ++id)
                    if (!owner.open_session(id))
                        return 1;
                for (std::uint64_t id = 3; id <= 2 + subscribers; ++id)
                    if (!owner.subscribe(id, core::SymbolId{1}))
                        return 1;
                for (std::uint64_t i = 1; i <= 3; ++i)
                    if (owner.submit(1, i, i, core::SymbolId{1}, core::Side::Sell,
                                     core::OrderType::Limit, static_cast<core::Price>(99 + i),
                                     1) != exchange::Result::Accepted)
                        return 1;
                allocations = bytes = 0;
                counting = true;
                const auto start = Clock::now();
                bool accepted = false;
                std::size_t events = 0;
                if (mode == 0) {
                    accepted =
                        owner.submit(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                     core::OrderType::Market, 0, 3) == exchange::Result::Accepted;
                } else if (mode == 1) {
                    const auto r = owner.submit_prepared_non_durable(
                        2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 3);
                    accepted = r.installed && r.result == exchange::Result::Accepted;
                } else {
                    const auto r = owner.submit_with_summary(
                        2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 3);
                    accepted = r.installed() && r.result() == exchange::Result::Accepted &&
                               r.trades().size() == 3;
                    events = r.public_events().size();
                }
                const auto end = Clock::now();
                counting = false;
                if (!accepted || owner.trade_count() != 3 || !owner.finish())
                    return 1;
                if (sample >= 0)
                    std::cout
                        << (mode == 0   ? "direct"
                            : mode == 1 ? "prepared"
                                        : "summary")
                        << ',' << subscribers << ',' << sample << ','
                        << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                        << ',' << allocations << ',' << bytes << ',' << events << '\n';
            }
        }
    }
    std::cerr << "sizeof_market_event=" << sizeof(exchange::MarketEvent) << '\n';
}
