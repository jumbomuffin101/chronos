#include "chronos/exchange/live_owner.hpp"
#include <benchmark/benchmark.h>

using namespace chronos;
namespace {
template <bool Exclusive> void construction(benchmark::State &state) {
    for (auto _ : state) {
        (void)_;
        exchange::LiveExchangeOwner owner(16, 1024, 128, 1024,
                                          Exclusive ? exchange::AllocationMode::Exclusive
                                                    : exchange::AllocationMode::Shared);
        benchmark::DoNotOptimize(&owner);
        benchmark::ClobberMemory();
    }
}
template <bool Exclusive> void submit(benchmark::State &state) {
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        {
            exchange::LiveExchangeOwner owner(16, 1024, 128, 1024,
                                              Exclusive ? exchange::AllocationMode::Exclusive
                                                        : exchange::AllocationMode::Shared);
            owner.bind();
            if (!owner.open_session(1)) {
                state.SkipWithError("session failed");
                state.ResumeTiming();
                return;
            }
            state.ResumeTiming();
            auto result = owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                       core::OrderType::Limit, 100, 1);
            benchmark::DoNotOptimize(result);
            benchmark::ClobberMemory();
            state.PauseTiming();
            if (result != exchange::Result::Accepted || !owner.finish())
                state.SkipWithError("submission failed");
        }
        state.ResumeTiming();
    }
}
BENCHMARK_TEMPLATE(construction, false);
BENCHMARK_TEMPLATE(construction, true);
BENCHMARK_TEMPLATE(submit, false);
BENCHMARK_TEMPLATE(submit, true);
} // namespace
BENCHMARK_MAIN();
