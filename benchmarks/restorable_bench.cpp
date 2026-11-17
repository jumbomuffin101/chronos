#include "chronos/eventlog/event_log.hpp"
#include "chronos/snapshot/restorable.hpp"
#include <benchmark/benchmark.h>
#include <sstream>

using namespace chronos;
namespace {
struct Fixture {
    simulation::ScenarioConfig config;
    std::ostringstream log{std::ios::binary};
    eventlog::EventRecorder recorder{log};
    simulation::SimulationRunner runner;
    static simulation::ScenarioConfig make(std::int64_t orders) {
        simulation::ScenarioConfig c;
        c.max_commands = orders == 0 ? 1 : static_cast<std::size_t>(orders);
        c.buy_weight = 100;
        c.price_variation = 0;
        c.cancel_percent = 0;
        if (orders == 0)
            c.risk.max_order_quantity = 0;
        return c;
    }
    explicit Fixture(std::int64_t orders) : config(make(orders)), runner(config, &recorder, true) {
        runner.start();
        runner.run_batch(config.max_commands);
    }
};
} // namespace
static void exact_capture(benchmark::State &state) {
    Fixture fixture(state.range(0));
    auto *book = fixture.runner.completed_isolated_exchange();
    if (!book) {
        state.SkipWithError("simulation failed");
        return;
    }
    for (auto _ : state) {
        (void)_;
        auto words = book->isolated_continuation_state(1024 * 1024);
        benchmark::DoNotOptimize(words.data());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(exact_capture)->Arg(0)->Arg(10)->Arg(100)->Arg(1000);
static void image_write(benchmark::State &state) {
    Fixture fixture(state.range(0));
    auto *book = fixture.runner.completed_isolated_exchange();
    if (!book) {
        state.SkipWithError("simulation failed");
        return;
    }
    const auto log = fixture.log.str();
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::ostringstream out(std::ios::binary);
        state.ResumeTiming();
        auto result = snapshot::write_image(out, log, *book, 1);
        benchmark::DoNotOptimize(result.ok);
        state.PauseTiming();
        if (!result.ok)
            state.SkipWithError("write failed");
        state.ResumeTiming();
    }
}
BENCHMARK(image_write)->Arg(0)->Arg(10)->Arg(100)->Arg(1000);
static void image_restore(benchmark::State &state) {
    Fixture fixture(state.range(0));
    auto *book = fixture.runner.completed_isolated_exchange();
    if (!book) {
        state.SkipWithError("simulation failed");
        return;
    }
    std::ostringstream out(std::ios::binary);
    if (!snapshot::write_image(out, fixture.log.str(), *book, 1).ok) {
        state.SkipWithError("write failed");
        return;
    }
    const auto image = out.str();
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::istringstream in(image, std::ios::binary);
        std::unique_ptr<exchange::Exchange> candidate;
        state.ResumeTiming();
        auto result = snapshot::restore_image(in, candidate);
        benchmark::DoNotOptimize(result.ok);
        state.PauseTiming();
        if (!result.ok || !candidate)
            state.SkipWithError("restore failed");
        candidate.reset();
        state.ResumeTiming();
    }
    state.counters["image_bytes"] = static_cast<double>(image.size());
}
BENCHMARK(image_restore)->Arg(0)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_MAIN();
