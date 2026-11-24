#include "chronos/exchange/live_owner.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0, bytes = 0;
} // namespace
void *operator new(std::size_t n) {
    if (void *p = std::malloc(n ? n : 1)) {
        if (counting) {
            ++allocations;
            bytes += n;
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
    std::cout << "mode,background_orders,sample,latency_ns,scalar_allocations,requested_bytes\n";
    for (int scenario = 0; scenario < 6; ++scenario) {
        const int mode = scenario % 3;
        const int background = scenario < 3 ? 0 : 32;
        for (int sample = -10; sample < 200; ++sample) {
            exchange::LiveExchangeOwner owner(16, 128, 128, 128,
                                              exchange::AllocationMode::Exclusive);
            owner.bind();
            if (mode != 0 && !owner.open_session(1))
                return 1;
            if (background != 0) {
                if (!owner.open_session(2))
                    return 1;
                for (int i = 1; i <= background; ++i) {
                    const auto id = static_cast<std::uint64_t>(i);
                    if (owner.submit(2, id, id, core::SymbolId{1}, core::Side::Buy,
                                     core::OrderType::Limit, 100, 1) != exchange::Result::Accepted)
                        return 1;
                }
            }
            exchange::RiskLimits limits;
            limits.max_order_quantity = 10;
            allocations = bytes = 0;
            counting = true;
            const auto begin = Clock::now();
            bool valid = false;
            if (mode == 0)
                valid = owner.with_prepared_open_session(1, [](exchange::Result) { return true; })
                            .installed;
            else if (mode == 1)
                valid =
                    owner
                        .with_prepared_risk_update(1, limits, [](exchange::Result) { return true; })
                        .installed;
            else {
                const auto state = owner.exact_continuation_state(4096);
                valid = !state.empty();
            }
            const auto end = Clock::now();
            counting = false;
            if (!valid || !owner.finish())
                return 1;
            if (sample >= 0)
                std::cout
                    << (mode == 0   ? "open"
                        : mode == 1 ? "risk"
                                    : "exact-state")
                    << ',' << background << ',' << sample << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()
                    << ',' << allocations << ',' << bytes << '\n';
        }
    }
}
