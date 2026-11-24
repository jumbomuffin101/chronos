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
    std::cout << "mode,sample,latency_ns,scalar_allocations,requested_bytes,participants,orders\n";
    for (bool summary_mode : {false, true}) {
        for (int sample = -5; sample < 100; ++sample) {
            exchange::LiveExchangeOwner owner(9, 32, 32, 32, exchange::AllocationMode::Exclusive);
            owner.bind();
            for (std::uint64_t id = 1; id <= 9; ++id)
                if (!owner.open_session(id))
                    return 1;
            for (std::uint64_t id = 1; id <= 8; ++id)
                if (owner.submit(id, 1, 1, core::SymbolId{1}, core::Side::Sell,
                                 core::OrderType::Limit, static_cast<core::Price>(99 + id),
                                 3) != exchange::Result::Accepted)
                    return 1;
            allocations = bytes = 0;
            counting = true;
            const auto begin = Clock::now();
            bool accepted = false;
            std::size_t participants = 0, orders = 0;
            if (summary_mode) {
                const auto result = owner.submit_with_summary(
                    9, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 24);
                participants = result.participant_effects().size();
                orders = result.ownership_effects().size();
                accepted = result.installed() && result.trades().size() == 8 && participants == 9 &&
                           orders == 9;
            } else {
                const auto result = owner.submit_prepared_non_durable(
                    9, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 24);
                accepted = result.installed && result.result == exchange::Result::Accepted;
            }
            const auto end = Clock::now();
            counting = false;
            if (!accepted || owner.trade_count() != 8 || !owner.finish())
                return 1;
            if (sample >= 0)
                std::cout
                    << (summary_mode ? "summary" : "prepared") << ',' << sample << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()
                    << ',' << allocations << ',' << bytes << ',' << participants << ',' << orders
                    << '\n';
        }
    }
}
