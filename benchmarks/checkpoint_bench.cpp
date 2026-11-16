#include "chronos/snapshot/checkpoint.hpp"
#include <benchmark/benchmark.h>
#include <sstream>

using namespace chronos;
static void checkpoint_capture(benchmark::State &state) {
    exchange::Exchange book(2, 1024, 4, 1024);
    if (!book.open_session(1)) {
        state.SkipWithError("session failed");
        return;
    }
    for (std::uint64_t i = 1; i <= static_cast<std::uint64_t>(state.range(0)); ++i) {
        if (book.submit(1, i, i, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100,
                        1) != exchange::Result::Accepted) {
            state.SkipWithError("order setup failed");
            return;
        }
        while (book.front(1))
            book.pop(1);
    }
    for (auto _ : state) {
        (void)_;
        auto checkpoint = snapshot::capture(book, 1);
        benchmark::DoNotOptimize(checkpoint.state.data());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(checkpoint_capture)->Arg(0)->Arg(100)->Arg(1000);
BENCHMARK_MAIN();
