#include "chronos/matching/matching_engine.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string_view>

namespace {
thread_local bool tracking = false;
thread_local std::size_t allocation_count = 0;
thread_local std::size_t allocation_bytes = 0;
} // namespace

void *operator new(std::size_t size) {
    if (void *p = std::malloc(size == 0 ? 1 : size)) {
        if (tracking) {
            ++allocation_count;
            allocation_bytes += size;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }

using namespace chronos::core;
using namespace chronos::matching;
using chronos::orderbook::Result;

int main() {
    std::cout << "workload,background_orders,scalar_new_calls,requested_bytes\n";
    for (std::uint64_t size : {100ULL, 1000ULL}) {
        const char *mode = std::getenv("CHRONOS_TRANSACTION_MODE");
        if (mode != nullptr && std::string_view{mode} != "planned" &&
            std::string_view{mode} != "reference")
            return 1;
        MatchingEngine engine{SymbolId{1},
                              {},
                              mode != nullptr && std::string_view{mode} == "reference"
                                  ? TransactionMode::CloneReference
                                  : TransactionMode::Planned};
        for (std::uint64_t i = 1; i <= size; ++i) {
            const auto out =
                engine.submit({OrderId{i}, SymbolId{1}, Side::Sell, OrderType::Limit, 200, 1});
            if (out.error != Error::None)
                return 1;
        }
        auto measure = [&](const char *name, auto action, auto verify) {
            allocation_count = 0;
            allocation_bytes = 0;
            tracking = true;
            auto result = action();
            tracking = false;
            if (!verify(result))
                throw std::runtime_error("allocation probe verification failed");
            std::cout << name << ',' << size << ',' << allocation_count << ',' << allocation_bytes
                      << '\n';
        };
        measure(
            "duplicate",
            [&] {
                return engine.submit(
                    {OrderId{1}, SymbolId{1}, Side::Sell, OrderType::Limit, 200, 1});
            },
            [](const Outcome &out) { return out.error == Error::DuplicateId; });
        measure(
            "missing_cancel", [&] { return engine.cancel_resting(OrderId{1000000}); },
            [](Result result) { return result == Result::NotFound; });
        measure(
            "noncrossing",
            [&] {
                return engine.submit(
                    {OrderId{1000000}, SymbolId{1}, Side::Buy, OrderType::Limit, 100, 1});
            },
            [](const Outcome &out) { return out.error == Error::None && out.trades.empty(); });
        if (engine.cancel_resting(OrderId{1000000}) != Result::Success)
            return 1;
        measure(
            "single_fill",
            [&] {
                return engine.submit(
                    {OrderId{1000001}, SymbolId{1}, Side::Buy, OrderType::Limit, 200, 1});
            },
            [](const Outcome &out) {
                return out.error == Error::None && out.trades.size() == 1 &&
                       out.trades[0].quantity == 1;
            });
    }
}
