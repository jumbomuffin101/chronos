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
    std::cout << "sample,latency_ns,scalar_allocations,requested_bytes,trades,reports,events\n";
    for (int sample = -2; sample < 20; ++sample) {
        exchange::LiveExchangeOwner owner(2, 256, 512, 256, exchange::AllocationMode::Exclusive);
        owner.bind();
        if (!owner.open_session(1) || !owner.open_session(2))
            return 1;
        for (std::uint64_t i = 1; i <= 128; ++i)
            if (owner.submit(1, i, i, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit,
                             static_cast<core::Price>(100 + i), 1) != exchange::Result::Accepted)
                return 1;
        allocations = bytes = 0;
        counting = true;
        const auto begin = Clock::now();
        const auto summary = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                       core::OrderType::Market, 0, 128);
        const auto end = Clock::now();
        counting = false;
        if (!summary.installed() || summary.trades().size() != 128 ||
            summary.reports().size() != 257 || summary.public_events().size() != 129 ||
            owner.trade_count() != 128 || !owner.finish())
            return 1;
        if (sample >= 0)
            std::cout << sample << ','
                      << std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()
                      << ',' << allocations << ',' << bytes << ',' << summary.trades().size() << ','
                      << summary.reports().size() << ',' << summary.public_events().size() << '\n';
    }
}
