#include "chronos/exchange/live_owner.hpp"
#include <benchmark/benchmark.h>

using namespace chronos;
namespace {
// Mode 0: existing command; 1: prepare and install; 2: prepare and abort.
template <int Mode, bool Cancel> void command(benchmark::State &state) {
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        {
            exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
            owner.bind();
            bool ready = owner.open_session(1);
            if constexpr (Cancel)
                ready = ready &&
                        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                     core::OrderType::Limit, 100, 1) == exchange::Result::Accepted;
            if (!ready) {
                state.SkipWithError("fixture failed");
                state.ResumeTiming();
                return;
            }
            state.ResumeTiming();
            auto result = exchange::Result::InvalidOrder;
            bool installed = false;
            if constexpr (Mode == 0) {
                if constexpr (Cancel)
                    result = owner.cancel(1, 2, 1);
                else
                    result = owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                          core::OrderType::Limit, 100, 1);
                installed = result == exchange::Result::Accepted;
            } else {
                exchange::PreparedScopeResult outcome;
                if constexpr (Cancel)
                    outcome = owner.with_prepared_cancel(
                        1, 2, 1, [](exchange::Result) { return Mode == 1; });
                else
                    outcome = owner.with_prepared_submit(
                        1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1,
                        [](exchange::Result) { return Mode == 1; });
                result = outcome.result;
                installed = outcome.installed;
            }
            benchmark::DoNotOptimize(result);
            benchmark::DoNotOptimize(installed);
            benchmark::ClobberMemory();
            state.PauseTiming();
            if (result != exchange::Result::Accepted || installed != (Mode != 2) || !owner.finish())
                state.SkipWithError("operation failed");
        }
        state.ResumeTiming();
    }
}
BENCHMARK_TEMPLATE(command, 0, false);
BENCHMARK_TEMPLATE(command, 1, false);
BENCHMARK_TEMPLATE(command, 2, false);
BENCHMARK_TEMPLATE(command, 0, true);
BENCHMARK_TEMPLATE(command, 1, true);
BENCHMARK_TEMPLATE(command, 2, true);
} // namespace
BENCHMARK_MAIN();
