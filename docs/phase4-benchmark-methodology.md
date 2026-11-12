# Phase 4A benchmark methodology

This describes the historical clone-based baseline. With Phase 4B code, set `CHRONOS_TRANSACTION_MODE=reference` to select the clone implementation; the default is the planned implementation. The saved Phase 4A measurements predate the mode switch. See the Phase 4B report for same-binary comparisons.

## Build and run

On Linux or WSL2 Ubuntu, install a C++20 compiler, CMake 3.20+, Make, Git and `/usr/bin/time`. The first configure fetches Google Benchmark v1.8.3, pinned to commit `344117638c8ff7e239044fd0fa7085839fc03021`. From the repository root:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DCHRONOS_ENABLE_BENCHMARKS=ON -DBUILD_TESTING=OFF
cmake --build build/bench --parallel --target chronos_bench chronos_allocation_probe
mkdir -p docs/assets/phase4
# Use a fresh samples path: the executable appends, without a header.
env CHRONOS_TRANSACTION_MODE=reference CHRONOS_BENCH_SAMPLES=docs/assets/phase4/my-samples.csv \
  /usr/bin/time -v -o docs/assets/phase4/my-time.txt \
  ./build/bench/benchmarks/chronos_bench \
  --benchmark_filter='EmptyMarket|/100/|/1000/' --benchmark_color=false \
  --benchmark_out=docs/assets/phase4/my-results.json --benchmark_out_format=json
env CHRONOS_TRANSACTION_MODE=reference CHRONOS_BENCH_SAMPLES=docs/assets/phase4/my-10000-samples.csv \
  /usr/bin/time -v -o docs/assets/phase4/my-10000-time.txt \
  ./build/bench/benchmarks/chronos_bench \
  --benchmark_filter='Noncrossing/10000/' --benchmark_color=false \
  --benchmark_out=docs/assets/phase4/my-10000-results.json --benchmark_out_format=json
env CHRONOS_TRANSACTION_MODE=reference ./build/bench/benchmarks/chronos_allocation_probe > docs/assets/phase4/my-allocations.csv
python3 benchmarks/summarize.py docs/assets/phase4/my-samples.csv docs/assets/phase4/my-10000-samples.csv --output docs/assets/phase4/my-summary.csv
```

Add `--plot docs/assets/phase4/my-latency.png` to the summary command if Python has matplotlib. Run `--benchmark_list_tests` to list the registered 10,000- and 100,000-order variants; these can take a long time to populate. The default filter above avoids the full large-size matrix. `BUILD_TESTING=OFF` only affects this benchmark build; run the separate test presets for correctness.

## Workload and timing boundaries

The input is fixed, with no random generation or seed. Each case creates a fresh engine with N background sell orders, quantity one at 200 ticks. Single-target cases add one sell at 100; same-price and sweep cases add eight sells at 100 or at 100 through 107. Timed operations are: empty-book market buy; noncrossing buy at 100; buy matching one sell at 100; market buy matching eight at one price or across eight levels; cancel; quantity 2→1 or 2→3; price 100→110; duplicate active ID; and cancellation of a missing ID. Numbers in output are **background** orders; target cases have N+1 or N+8 total resting orders before the timed call.

Each repetition populates a fresh engine and performs three untimed warmup operations with restoration. It then measures 64 calls using `steady_clock` around the public command, including the engine's clone, commit, old-book destruction, report/trade construction, and tiny harness dispatch/anti-optimization overhead. Google Benchmark is configured with manual wall time. Input values are fixed; no random generation occurs. After each timed call, the harness checks results against fixed expected IDs, prices and quantities, checks/restores state, and verifies book counts and totals. Timed outputs are retained until verification and are destroyed outside the command interval. Restoration uses the public API: cancellation/reinsertion, inverse modification, or reinsertion of eight filled orders. Each restored state is equivalent in live prices, quantities and FIFO order, but counters advance and allocator/cache state is not reset. Output `reset_verify_ns` records restoration **plus verification**, separately from command samples. It is not pure reset cost. Setup time includes population and three warmups; JSON's `setup_ms` is reported separately. Google Benchmark's CPU time includes harness/reset work and must not be interpreted as command latency.

There are three repetitions per case: 192 individual command samples per combined workload/size. `samples-final.csv` has no header and fields `workload,background_orders,latency_ns,reset_verify_ns`; samples occur in repetition blocks of 64. `summary.csv` uses nearest-rank p50/p95/p99 across the 192 samples and computes commands/second as 1 divided by the mean command latency. This is a serial service-rate estimate, not throughput under concurrent load. p95 has about ten observations above it and p99 only about two; the latter is a noisy descriptive order statistic, not a reliable production tail estimate. Clock reads and instrumentation materially affect very short operations. JSON includes per-repetition and aggregate Google Benchmark results; its aggregate counters are aggregates of per-repetition percentiles, not pooled quantiles.

The separate allocation probe counts only calls to the replaceable scalar C++ `operator new` and requested bytes while a single verified command executes; it excludes direct `malloc`, aligned or other allocation paths and does not measure frees, live bytes, or peak memory. Its instrumentation changes cost and is never used for latency. `/usr/bin/time -v` measures process-wide peak RSS and wall time including all setup, warmup, verification and reset, not a per-command memory peak. The probe runs one sample per operation and size; it is a structural count, not a statistical distribution.

## Profiling and caveats

When available, `perf`, heap profilers or other native tools should be used on suitable hardware. For this WSL2 run they were unavailable. A separate `gprof` executable was built with `-O2 -g -pg` from benchmark and core sources, linked to the existing benchmark library, and run with `--benchmark_filter='Noncrossing/1000/'`; `gprof` output is retained. It captures setup and restoration as well as measured commands, does not instrument shared libraries, and its optimized symbol attribution is unreliable (including implausible fill attribution for a noncrossing workload). The tiny number of time samples is insufficient to rank hotspots; use it only as a limited diagnostic. Profiling and allocation-probe numbers are not benchmark timing samples.

```sh
g++ -std=c++20 -O2 -g -pg -Iinclude -Ibuild/bench/_deps/googlebenchmark-src/include \
  benchmarks/matching_bench.cpp src/matching/matching_engine.cpp src/orderbook/order_book.cpp \
  build/bench/_deps/googlebenchmark-build/src/libbenchmark.a -pthread -o build/bench/chronos_bench_gprof
(cd build/bench && env CHRONOS_TRANSACTION_MODE=reference ./chronos_bench_gprof --benchmark_filter='Noncrossing/1000/' --benchmark_color=false > gprof-run.txt && \
  gprof ./chronos_bench_gprof gmon.out > ../../docs/assets/phase4/my-gprof.txt)
```

Measurements were made in a shared, unpinned WSL2 VM and are internal, in-process simulator command timings. They do not represent network round trips, isolated production hardware, or a latency guarantee. The 100,000-order cases were not run: even one 10,000-order case required substantial quadratic public-API setup. No benchmark result should be extrapolated as a measured 100,000-order result.
