#include "chronos/exchange/exchange.hpp"
#include <benchmark/benchmark.h>
#include <memory>

using namespace chronos;
static void allocate(benchmark::State &state) {
    exchange::detail::OrderIdAllocator allocator(state.range(0) == 0);
    for (auto _ : state) {
        (void)_;
        exchange::detail::OrderIdAllocator::Reservation reservation(allocator);
        auto id = reservation.id();
        reservation.commit();
        benchmark::DoNotOptimize(&allocator);
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(allocate)->Arg(0)->Arg(1);
static void submit(benchmark::State &state) {
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        auto book = state.range(0) ? exchange::Exchange::isolated()
                                   : std::make_unique<exchange::Exchange>();
        if (!book->open_session(1))
            state.SkipWithError("session failed");
        state.ResumeTiming();
        auto result = book->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                   core::OrderType::Limit, 100, 1);
        benchmark::DoNotOptimize(result);
        state.PauseTiming();
        if (result != exchange::Result::Accepted || !book->find(1, 1))
            state.SkipWithError("submission failed");
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(submit)->Arg(0)->Arg(1);
static void construct(benchmark::State &state) {
    for (auto _ : state) {
        (void)_;
        auto book = state.range(0) ? exchange::Exchange::isolated()
                                   : std::make_unique<exchange::Exchange>();
        benchmark::DoNotOptimize(book.get());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(construct)->Arg(0)->Arg(1);
BENCHMARK_MAIN();
