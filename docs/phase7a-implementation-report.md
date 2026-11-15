# Phase 7A implementation report

The C++ simulation module supplies a bounded deterministic command generator,
synchronous runner, five synthetic scenarios, explicit local participants and
exchange risk/capacity handling. The runner owns a fresh `Exchange` and uses its
normal order and cancellation API. See the [design](phase7a-simulation-design.md)
for counter semantics, digest serialization and lifecycle limitations.

Example, from the repository root after a Release build:

```sh
./build/release/chronos_sim --scenario liquidity-sweep --seed 42 --participants 4 --commands 10
```

```text
Scenario: liquidity-sweep seed: 42 generator: 2 participants: 4 symbols: 1
Generated: 10 admitted: 10 accepted orders: 10 rejected orders: 0 cancellations: 0 rejected cancellations: 0
Trades: 8 quantity: 8 steps: 10
Symbol 1 bid 0 x 0 ask 0 x 0
Digest: 239e36527e260294
```

## Performance methodology and initial measurements

The optional Google Benchmark target `chronos_sim_bench` measures generation
alone (no active cancellation targets), complete in-process runs including
final summaries, and batches of ten submissions after 0, 100 or 500 resting
orders. Runner construction and background population are paused outside the
timed region. Results use fixed seed 42, Release, three repetitions and a 0.05 s
minimum per repetition. Allocation counts were not measured. Commands:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DCHRONOS_ENABLE_BENCHMARKS=ON
cmake --build build/bench --target chronos_sim_bench --parallel
./build/bench/benchmarks/chronos_sim_bench --benchmark_min_time=0.05s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

On WSL2 Ubuntu 24.04, Linux 6.6.87.2, GCC 13.3, Intel i7-13620H (16 logical
CPUs), observed median CPU time was 3.325 µs per 100 generated commands (two
participants); 0.565 ms per complete 100-command run (two participants); and
0.018, 0.131 and 0.515 ms per batch of ten with 0, 100 and 500 resting orders,
respectively. These are small samples on shared virtualized hardware, not
production trading or network latency. Existing exchange transactions clone
state and books, so latency grows with retained orders, requests, sessions and
trades; these measurements include generator, exchange, report collection and
digest work, and do not isolate individual bottlenecks.

One separate `/usr/bin/time -v` run of the 1,000-command balanced CLI reported
4,352 KiB maximum resident set size and 0.03 s elapsed. This includes the whole
process and loader, is a single WSL2 sample, and is not per-command allocation.

The machine-readable benchmark output is at `build/bench/phase7a-review.json`
in the local ignored build directory.
Debug, Release and ASan/UBSan each passed **101/101** CTest tests. The existing
matching CLI, the new sweep CLI, and a 1,000-command balanced run succeeded.
Formatting and Git whitespace checks passed. Web tests were not rerun because
the web application and its contracts did not change.

No event persistence, durable recovery, production identity, strategy trading
or dashboard controls are implemented. Internal IDs are process-wide and not
bit-identical across in-process runs; the documented digest excludes those IDs.

Final review introduced version-2 rejection sampling to avoid modulo bias.
Report collection is reserved before execution, committed trade count is
separate from successfully aggregated trades, and `summary_complete` identifies
whether the fingerprint and summary were finalized. Allocation and cumulative
quantity failure tests verify that a failed run does not claim rollback or
completion. See the design for the remaining limits of the fingerprint.
The generator uses fixed-width unsigned operations and explicit serialization;
the validation in this report used GCC on WSL2, not a cross-compiler test matrix.
