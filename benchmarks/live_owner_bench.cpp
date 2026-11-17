#include "chronos/exchange/exchange.hpp"
#include "chronos/exchange/live_owner.hpp"
#include <benchmark/benchmark.h>

using namespace chronos;
namespace {
template <bool Wrapped> struct Fixture;
template <> struct Fixture<false> {
    exchange::Exchange book;
    bool open() { return book.open_session(1); }
    void close() { book.close_session(1); }
    exchange::Result submit() {
        return book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100,
                           1);
    }
    exchange::Result cancel() { return book.cancel(1, 2, 1); }
    void pop() { book.pop(1); }
};
template <> struct Fixture<true> {
    exchange::LiveExchangeOwner book;
    Fixture() { book.bind(); }
    ~Fixture() { (void)book.finish(); }
    bool open() { return book.open_session(1); }
    void close() { book.close_session(1); }
    exchange::Result submit() {
        return book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100,
                           1);
    }
    exchange::Result cancel() { return book.cancel(1, 2, 1); }
    void pop() { book.pop(1); }
};
template <bool Wrapped> void session(benchmark::State &state) {
    Fixture<Wrapped> f;
    for (auto _ : state) {
        (void)_;
        bool ok = f.open();
        benchmark::DoNotOptimize(ok);
        f.close();
        benchmark::ClobberMemory();
        if (!ok) {
            state.SkipWithError("session setup failed");
            break;
        }
    }
}
template <bool Wrapped, int Operation> void command(benchmark::State &state) {
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        {
            Fixture<Wrapped> f;
            const bool opened = f.open();
            auto setup = exchange::Result::Accepted;
            if constexpr (Operation != 0)
                setup = f.submit();
            if (!opened || setup != exchange::Result::Accepted) {
                state.SkipWithError("fixture failed");
                state.ResumeTiming();
                return;
            }
            state.ResumeTiming();
            auto result = exchange::Result::Accepted;
            if constexpr (Operation == 0)
                result = f.submit();
            else if constexpr (Operation == 1)
                result = f.cancel();
            else
                f.pop();
            benchmark::DoNotOptimize(result);
            benchmark::ClobberMemory();
            state.PauseTiming();
            if (result != exchange::Result::Accepted)
                state.SkipWithError("operation failed");
        }
        state.ResumeTiming();
    }
}
BENCHMARK_TEMPLATE(session, false);
BENCHMARK_TEMPLATE(session, true);
BENCHMARK_TEMPLATE(command, false, 0);
BENCHMARK_TEMPLATE(command, true, 0);
BENCHMARK_TEMPLATE(command, false, 1);
BENCHMARK_TEMPLATE(command, true, 1);
BENCHMARK_TEMPLATE(command, false, 2);
BENCHMARK_TEMPLATE(command, true, 2);
} // namespace
BENCHMARK_MAIN();
