#include "chronos/matching/matching_engine.hpp"

#include <algorithm>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace chronos::core;
using namespace chronos::matching;
using chronos::orderbook::Result;

namespace {
enum class Workload {
    EmptyMarket,
    Noncrossing,
    SingleFill,
    SamePrice,
    Sweep,
    Cancel,
    Reduce,
    Increase,
    Reprice,
    Duplicate,
    MissingCancel
};
constexpr SymbolId symbol{1};
constexpr std::uint64_t target_base = 1000001;

Request request(std::uint64_t id, Side side, Price price, Quantity qty,
                OrderType type = OrderType::Limit) {
    return {OrderId{id}, symbol, side, type, price, qty};
}
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("benchmark verification failed");
}
void insert(MatchingEngine &engine, std::uint64_t id, Price price, Quantity quantity) {
    require(engine.submit(request(id, Side::Sell, price, quantity)).error == Error::None);
}

class Scenario {
  public:
    Scenario(Workload kind, std::size_t count) : kind_(kind), count_(count), engine_(symbol) {
        for (std::size_t i = 0; i < count_; ++i)
            insert(engine_, static_cast<std::uint64_t>(i) + 1, 200, 1);
        if (kind_ == Workload::SingleFill || kind_ == Workload::Cancel ||
            kind_ == Workload::Reduce || kind_ == Workload::Increase || kind_ == Workload::Reprice)
            insert(engine_, target_base, 100,
                   kind_ == Workload::Reduce || kind_ == Workload::Increase ? 2 : 1);
        if (kind_ == Workload::SamePrice || kind_ == Workload::Sweep)
            for (std::uint64_t i = 0; i < 8; ++i)
                insert(engine_, target_base + i,
                       kind_ == Workload::Sweep ? 100 + static_cast<Price>(i) : 100, 1);
    }

    void run() {
        if (kind_ == Workload::Cancel || kind_ == Workload::MissingCancel) {
            result_ = engine_.cancel_resting(
                OrderId{kind_ == Workload::Cancel ? target_base : target_base + 99});
            benchmark::DoNotOptimize(result_);
            return;
        }
        if (kind_ == Workload::Reduce || kind_ == Workload::Increase ||
            kind_ == Workload::Reprice) {
            result_ = engine_.modify_resting(
                OrderId{target_base}, kind_ == Workload::Reprice ? 110 : 100,
                kind_ == Workload::Reduce ? 1 : (kind_ == Workload::Increase ? 3 : 1));
            benchmark::DoNotOptimize(result_);
            return;
        }
        Request input = request(target_base + 100, Side::Buy, 100, 1);
        if (kind_ == Workload::EmptyMarket)
            input = request(target_base + 100, Side::Buy, 0, 1, OrderType::Market);
        if (kind_ == Workload::SamePrice || kind_ == Workload::Sweep)
            input = request(target_base + 100, Side::Buy, 0, 8, OrderType::Market);
        if (kind_ == Workload::Duplicate)
            input = request(1, Side::Sell, 200, 1);
        outcome_ = engine_.submit(input);
        benchmark::DoNotOptimize(outcome_.trades.data());
        benchmark::DoNotOptimize(outcome_.error);
    }

    void restore_and_verify() {
        const auto changed = engine_.find(OrderId{target_base});
        if (kind_ == Workload::Cancel || kind_ == Workload::SingleFill ||
            kind_ == Workload::SamePrice || kind_ == Workload::Sweep)
            require(!changed);
        if (kind_ == Workload::Reduce || kind_ == Workload::Increase ||
            kind_ == Workload::Reprice) {
            require(changed.has_value());
            require(changed->price == (kind_ == Workload::Reprice ? 110 : 100));
            require(changed->quantity ==
                    (kind_ == Workload::Reduce ? 1U : (kind_ == Workload::Increase ? 3U : 1U)));
        }
        if (kind_ == Workload::Noncrossing) {
            const auto inserted = engine_.find(OrderId{target_base + 100});
            require(inserted.has_value() && inserted->quantity == 1 && inserted->price == 100);
        }
        if (count_ != 0)
            require(engine_.find(OrderId{1}).has_value());
        if (kind_ == Workload::Cancel || kind_ == Workload::MissingCancel)
            require(result_ == (kind_ == Workload::Cancel ? Result::Success : Result::NotFound));
        else if (kind_ == Workload::Reduce || kind_ == Workload::Increase ||
                 kind_ == Workload::Reprice)
            require(result_ == Result::Success);
        else {
            require(outcome_.error ==
                    (kind_ == Workload::Duplicate ? Error::DuplicateId : Error::None));
            const std::size_t expected =
                kind_ == Workload::SingleFill
                    ? 1
                    : (kind_ == Workload::SamePrice || kind_ == Workload::Sweep ? 8 : 0);
            require(outcome_.trades.size() == expected);
            if (expected != 0) {
                for (std::size_t i = 0; i < expected; ++i) {
                    const auto &trade = outcome_.trades[i];
                    require(trade.sell_order_id == OrderId{target_base + i} &&
                            trade.quantity == 1 &&
                            trade.price ==
                                (kind_ == Workload::Sweep ? 100 + static_cast<Price>(i) : 100));
                }
            }
            if (kind_ == Workload::EmptyMarket)
                require(outcome_.remaining == 1);
        }
        if (kind_ == Workload::Noncrossing)
            require(engine_.cancel_resting(OrderId{target_base + 100}) == Result::Success);
        else if (kind_ == Workload::SingleFill || kind_ == Workload::Cancel)
            insert(engine_, target_base, 100, 1);
        else if (kind_ == Workload::SamePrice || kind_ == Workload::Sweep) {
            for (std::uint64_t i = 0; i < 8; ++i)
                insert(engine_, target_base + i,
                       kind_ == Workload::Sweep ? 100 + static_cast<Price>(i) : 100, 1);
        } else if (kind_ == Workload::Reduce || kind_ == Workload::Increase ||
                   kind_ == Workload::Reprice) {
            require(engine_.modify_resting(OrderId{target_base}, 100,
                                           kind_ == Workload::Reprice ? 1 : 2) == Result::Success);
        }
        const auto levels = engine_.depth(Side::Sell, 100);
        std::size_t n = 0;
        Quantity total = 0;
        for (const auto &level : levels) {
            n += level.order_count;
            total += level.quantity;
        }
        const std::size_t special =
            kind_ == Workload::SamePrice || kind_ == Workload::Sweep
                ? 8
                : (kind_ == Workload::SingleFill || kind_ == Workload::Cancel ||
                           kind_ == Workload::Reduce || kind_ == Workload::Increase ||
                           kind_ == Workload::Reprice
                       ? 1
                       : 0);
        require(n == count_ + special);
        require(total ==
                count_ + special +
                    ((kind_ == Workload::Reduce || kind_ == Workload::Increase) ? 1U : 0U));
        require(!engine_.find(OrderId{target_base + 100}));
        outcome_ = Outcome{};
    }

  private:
    Workload kind_;
    std::size_t count_;
    MatchingEngine engine_;
    Result result_ = Result::NotFound;
    Outcome outcome_;
};

void run_benchmark(benchmark::State &state, Workload kind, const char *name) {
    const auto count = static_cast<std::size_t>(state.range(0));
    try {
        const auto setup_start = std::chrono::steady_clock::now();
        Scenario scenario{kind, count};
        for (int i = 0; i < 3; ++i) {
            scenario.run();
            scenario.restore_and_verify();
        }
        const auto setup_end = std::chrono::steady_clock::now();
        std::vector<double> samples;
        std::vector<double> resets;
        samples.reserve(static_cast<std::size_t>(state.max_iterations));
        resets.reserve(static_cast<std::size_t>(state.max_iterations));
        for (auto iteration : state) {
            (void)iteration;
            const auto start = std::chrono::steady_clock::now();
            scenario.run();
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();
            state.SetIterationTime(seconds);
            samples.push_back(seconds * 1e9);
            const auto reset_start = std::chrono::steady_clock::now();
            scenario.restore_and_verify();
            const auto reset_end = std::chrono::steady_clock::now();
            resets.push_back(
                std::chrono::duration<double, std::nano>(reset_end - reset_start).count());
        }
        if (samples.empty())
            return;
        if (const char *path = std::getenv("CHRONOS_BENCH_SAMPLES")) {
            std::ofstream out(path, std::ios::app);
            require(static_cast<bool>(out));
            for (std::size_t i = 0; i < samples.size(); ++i)
                out << name << ',' << count << ',' << samples[i] << ',' << resets[i] << '\n';
            require(static_cast<bool>(out));
        }
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](std::size_t percent) {
            return samples[((samples.size() * percent + 99) / 100) - 1];
        };
        state.counters["p50_ns"] = percentile(50);
        state.counters["p95_ns"] = percentile(95);
        state.counters["p99_ns"] = percentile(99);
        state.counters["setup_ms"] =
            std::chrono::duration<double, std::milli>(setup_end - setup_start).count();
        state.SetItemsProcessed(state.iterations());
    } catch (const std::exception &error) {
        state.SkipWithError(error.what());
    }
}

#define CHRONOS_BENCH(name, kind)                                                                  \
    void name(benchmark::State &state) { run_benchmark(state, Workload::kind, #name); }            \
    BENCHMARK(name)                                                                                \
        ->Arg(100)                                                                                 \
        ->Arg(1000)                                                                                \
        ->Arg(10000)                                                                               \
        ->Arg(100000)                                                                              \
        ->Iterations(64)                                                                           \
        ->Repetitions(3)                                                                           \
        ->UseManualTime()                                                                          \
        ->Unit(benchmark::kMicrosecond)

void EmptyMarket(benchmark::State &state) {
    run_benchmark(state, Workload::EmptyMarket, "EmptyMarket");
}
BENCHMARK(EmptyMarket)
    ->Arg(0)
    ->Iterations(64)
    ->Repetitions(3)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);
CHRONOS_BENCH(Noncrossing, Noncrossing);
CHRONOS_BENCH(SingleFill, SingleFill);
CHRONOS_BENCH(SamePrice, SamePrice);
CHRONOS_BENCH(Sweep, Sweep);
CHRONOS_BENCH(Cancel, Cancel);
CHRONOS_BENCH(Reduce, Reduce);
CHRONOS_BENCH(Increase, Increase);
CHRONOS_BENCH(Reprice, Reprice);
CHRONOS_BENCH(Duplicate, Duplicate);
CHRONOS_BENCH(MissingCancel, MissingCancel);
} // namespace

BENCHMARK_MAIN();
