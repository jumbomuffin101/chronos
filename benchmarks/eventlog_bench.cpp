#include "chronos/eventlog/event_log.hpp"
#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdio>
#include <sstream>
#include <vector>

using namespace chronos;
static void checksum(benchmark::State &state) {
    const std::vector<std::uint8_t> bytes(4096, 42);
    for (auto _ : state) {
        (void)_;
        benchmark::DoNotOptimize(eventlog::crc32(bytes.data(), bytes.size()));
    }
    state.SetBytesProcessed(state.iterations() * 4096);
}
BENCHMARK(checksum);
static void command_append(benchmark::State &state) {
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::ostringstream out(std::ios::binary);
        eventlog::EventRecorder recorder(out);
        if (!recorder.begin(c))
            state.SkipWithError("header failed");
        simulation::SimCommand command;
        command.step = 1;
        command.session = 1;
        command.request = 1;
        command.client_order = 1;
        state.ResumeTiming();
        bool ok = recorder.command(command);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        if (!ok)
            state.SkipWithError("append failed");
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(command_append);
namespace {
class FileBuffer final : public std::streambuf {
  public:
    explicit FileBuffer(std::FILE *file) : file_(file) {}

  protected:
    std::streamsize xsputn(const char *data, std::streamsize count) override {
        return static_cast<std::streamsize>(
            std::fwrite(data, 1, static_cast<std::size_t>(count), file_));
    }
    int sync() override { return std::fflush(file_); }

  private:
    std::FILE *file_;
};
} // namespace
static void file_command_flush(benchmark::State &state) {
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::FILE *file = std::tmpfile();
        if (!file) {
            state.SkipWithError("temporary file failed");
            state.ResumeTiming();
            break;
        }
        FileBuffer buffer(file);
        std::ostream out(&buffer);
        eventlog::EventRecorder recorder(out);
        if (!recorder.begin(c))
            state.SkipWithError("header failed");
        simulation::SimCommand command;
        command.step = 1;
        command.session = 1;
        command.request = 1;
        command.client_order = 1;
        state.ResumeTiming();
        bool ok = recorder.command(command);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        if (!ok)
            state.SkipWithError("file append failed");
        std::fclose(file);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(file_command_flush);
static void simulation_with_recording(benchmark::State &state) {
    simulation::ScenarioConfig c;
    c.max_commands = static_cast<std::size_t>(state.range(1));
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::ostringstream out(std::ios::binary);
        eventlog::EventRecorder recorder(out);
        simulation::SimulationRunner runner(c, state.range(0) ? &recorder : nullptr);
        runner.start();
        state.ResumeTiming();
        runner.run_batch(c.max_commands);
        auto digest = runner.result().digest;
        benchmark::DoNotOptimize(digest);
        state.PauseTiming();
        if (runner.result().status.state != simulation::Lifecycle::Completed)
            state.SkipWithError("simulation failed");
        if (state.range(0)) {
            std::istringstream check(out.str(), std::ios::binary);
            if (!eventlog::verify(check).verified)
                state.SkipWithError("verification failed");
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(1));
}
BENCHMARK(simulation_with_recording)
    ->Args({0, 10})
    ->Args({1, 10})
    ->Args({0, 20})
    ->Args({1, 20})
    ->Args({0, 50})
    ->Args({1, 50})
    ->Args({0, 100})
    ->Args({1, 100});
static void state_capture(benchmark::State &state) {
    exchange::Exchange book(2, 1024, 4, 1024);
    if (!book.open_session(1)) {
        state.SkipWithError("session failed");
        return;
    }
    for (std::uint64_t i = 1; i <= static_cast<std::uint64_t>(state.range(0)); ++i) {
        if (book.submit(1, i, i, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100,
                        1) != exchange::Result::Accepted) {
            state.SkipWithError("setup failed");
            return;
        }
        while (book.front(1))
            book.pop(1);
    }
    for (auto _ : state) {
        (void)_;
        auto snapshot = book.verification_state();
        benchmark::DoNotOptimize(snapshot.data());
        benchmark::ClobberMemory();
        if (snapshot.empty())
            state.SkipWithError("empty state");
    }
}
BENCHMARK(state_capture)->Arg(10)->Arg(50)->Arg(100);
static void state_comparison(benchmark::State &state) {
    const std::vector<std::uint64_t> expected(static_cast<std::size_t>(state.range(0)), 42);
    const auto actual = expected;
    for (auto _ : state) {
        (void)_;
        benchmark::ClobberMemory();
        bool equal = std::equal(expected.begin(), expected.end(), actual.begin(), actual.end());
        benchmark::DoNotOptimize(equal);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0) * 8);
}
BENCHMARK(state_comparison)->Arg(1000)->Arg(10000);
static void outcome_record(benchmark::State &state) {
    exchange::Exchange book(2, 1024, 4, 1024);
    if (!book.open_session(1)) {
        state.SkipWithError("session failed");
        return;
    }
    for (std::uint64_t i = 1; i <= static_cast<std::uint64_t>(state.range(0)); ++i) {
        if (book.submit(1, i, i, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100,
                        1) != exchange::Result::Accepted) {
            state.SkipWithError("setup failed");
            return;
        }
        while (book.front(1))
            book.pop(1);
    }
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    simulation::SimCommand command;
    command.step = 1;
    command.session = 1;
    command.request = 1;
    command.client_order = 1;
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::ostringstream out(std::ios::binary);
        eventlog::EventRecorder recorder(out);
        if (!recorder.begin(c) || !recorder.command(command))
            state.SkipWithError("setup failed");
        state.ResumeTiming();
        bool ok = recorder.outcome(command, exchange::Result::Accepted, book);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        if (!ok)
            state.SkipWithError("outcome failed");
        state.ResumeTiming();
    }
}
BENCHMARK(outcome_record)->Arg(10)->Arg(50)->Arg(100);
static void replay(benchmark::State &state) {
    simulation::ScenarioConfig c;
    c.max_commands = static_cast<std::size_t>(state.range(0));
    std::ostringstream out(std::ios::binary);
    eventlog::EventRecorder recorder(out);
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(c.max_commands);
    const auto data = out.str();
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        std::istringstream in(data, std::ios::binary);
        state.ResumeTiming();
        auto result = eventlog::verify(in);
        benchmark::DoNotOptimize(result.verified);
        state.PauseTiming();
        if (!result.verified)
            state.SkipWithError("replay failed");
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    state.counters["log_bytes"] = static_cast<double>(data.size());
}
BENCHMARK(replay)->Arg(10)->Arg(20)->Arg(50)->Arg(100);
BENCHMARK_MAIN();
