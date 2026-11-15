#include "chronos/simulation/simulation.hpp"
#include <benchmark/benchmark.h>
#include <memory>

using namespace chronos::simulation;

static void generation(benchmark::State &state) {
    ScenarioConfig c;
    c.max_commands = static_cast<std::size_t>(state.range(0));
    c.participants = static_cast<std::size_t>(state.range(1));
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        auto generator = std::make_unique<CommandGenerator>(c);
        state.ResumeTiming();
        for (std::size_t i = 0; i < c.max_commands; ++i) {
            auto command = generator->next();
            benchmark::DoNotOptimize(command);
        }
        state.PauseTiming();
        generator.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(generation)->Args({100, 2})->Args({1000, 8});

static void simulation(benchmark::State &state) {
    ScenarioConfig c;
    c.max_commands = static_cast<std::size_t>(state.range(0));
    c.participants = static_cast<std::size_t>(state.range(1));
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        auto runner = std::make_unique<SimulationRunner>(c);
        runner->start();
        state.ResumeTiming();
        runner->run_batch(c.max_commands);
        auto digest = runner->result().digest;
        benchmark::DoNotOptimize(digest);
        state.PauseTiming();
        if (runner->result().status.state != Lifecycle::Completed ||
            runner->result().generated != c.max_commands ||
            runner->result().admitted != c.max_commands)
            state.SkipWithError("simulation failed");
        runner.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(simulation)->Args({100, 2})->Args({100, 8})->Args({500, 2})->Args({500, 8});

static void background(benchmark::State &state) {
    const auto existing = static_cast<std::size_t>(state.range(0));
    ScenarioConfig c;
    c.max_commands = existing + 11;
    c.buy_weight = 100;
    c.price_variation = 0;
    c.min_quantity = 1;
    c.max_quantity = 1;
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        auto runner = std::make_unique<SimulationRunner>(c);
        runner->start();
        runner->run_batch(existing);
        if (runner->result().status.state != Lifecycle::Running ||
            runner->result().accepted_orders != existing || runner->result().rejected_orders != 0) {
            state.SkipWithError("background preparation failed");
            state.ResumeTiming();
            break;
        }
        state.ResumeTiming();
        runner->run_batch(10);
        auto digest = runner->result().digest;
        benchmark::DoNotOptimize(digest);
        state.PauseTiming();
        if (runner->result().status.state != Lifecycle::Running ||
            runner->result().accepted_orders != existing + 10 ||
            runner->result().rejected_orders != 0)
            state.SkipWithError("background setup failed");
        runner.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * 10);
}
BENCHMARK(background)->Arg(0)->Arg(100)->Arg(500);
BENCHMARK_MAIN();
