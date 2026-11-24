#include "chronos/exchange/live_owner.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocations = 0, bytes = 0;
} // namespace
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) {
        if (count_allocations) {
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
    std::cout << "mode,operation,sample,latency_ns,scalar_allocations,requested_bytes\n";
    for (bool summary : {false, true}) {
        for (bool cancel : {false, true}) {
            for (int sample = -5; sample < 100; ++sample) {
                exchange::LiveExchangeOwner owner(2, 16, 16, 16,
                                                  exchange::AllocationMode::Exclusive);
                owner.bind();
                if (!owner.open_session(1))
                    return 1;
                if (cancel &&
                    owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                 core::OrderType::Limit, 100, 1) != exchange::Result::Accepted)
                    return 1;
                allocations = bytes = 0;
                count_allocations = true;
                const auto start = Clock::now();
                bool installed = false;
                if (summary) {
                    const auto result =
                        cancel
                            ? owner.cancel_with_summary(1, 2, 1)
                            : owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                        core::OrderType::Limit, 100, 1);
                    installed = result.installed() && result.result() == exchange::Result::Accepted;
                } else {
                    const auto result = cancel ? owner.cancel_prepared_non_durable(1, 2, 1)
                                               : owner.submit_prepared_non_durable(
                                                     1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                     core::OrderType::Limit, 100, 1);
                    installed = result.installed && result.result == exchange::Result::Accepted;
                }
                const auto end = Clock::now();
                count_allocations = false;
                if (!installed || !owner.finish())
                    return 1;
                if (sample >= 0)
                    std::cout
                        << (summary ? "summary" : "prepared") << ','
                        << (cancel ? "cancel" : "submit") << ',' << sample << ','
                        << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                        << ',' << allocations << ',' << bytes << '\n';
            }
        }
    }
}
