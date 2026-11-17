# Phase 7C.2A.1B implementation report

`LiveExchangeOwner` encapsulates the gateway's fresh live exchange and binds
once to the existing poll thread. Every gateway mutation uses this boundary;
normal matching, risk, order IDs and protocol v3 remain implemented by the
existing exchange and gateway. The owner cannot adopt a restored exchange or
expose the mutable one. Direct standalone `Exchange` and the offline tools
remain independently caller-serialized.

The wrapper tests cover binding and terminal lifecycle, foreign-context
mutation/read/finish rejection, frozen-scope exclusion across all exposed
mutators, unconsumed IDs after blocked submissions, exception-safe unfreeze,
rejection atomicity and a thread exiting without returning its capability.
Existing protocol, bounded ingress, owner-admin, isolation, matching, replay
and snapshot tests remain enabled. Gateway shutdown now fails and notifies
unprocessed administrative controls. The existing owner loop handles frame
ingress and control queues separately; see
[the design](phase7c2a1b-live-owner-design.md) for ordering and limitations.

No durable
allocator registration, prepare/install, publication gate, journal, manifest,
crash recovery, authenticated session recovery or exactly-once delivery is
implemented.

## Validation

Debug, Release and ASan/UBSan each passed **153/153 CTest cases** in a
WSL-native source copy at the earlier milestone. After final review and the
shutdown fixes, all three configurations passed **156/156**. The matching CLI and five-command mixed simulation
completed. A five-command Phase 7B liquidity-sweep log replayed 12 records
and four trades. CHROVIEW comparison and isolated CHRSTATE restoration passed
(generation 1, ingress 5, 9,348 bytes). The direct two-client gateway fill
test passed; the full suites include protocol-v3 tests. Formatting and Git
whitespace checks passed. No web contract changed.

The subsequent final review added deterministic tests of both control/shutdown
interleavings. One applies a kill control before stop and checks its single
successful completion. The other gates the owner during dispatch, fills the
32-entry control queue, disconnects the client, waits for a stop request and
then releases the owner. All queued controls return failure, none starts, each
gets one completion, and repeated stop and post-stop inspection are checked.
A third test holds a detached batch before its first control starts and proves
that a stop request winning that race rejects the control. Starting a control
and requesting stop now serialize on the existing control mutex; concurrent
stop callers are also serialized. The scheduling gate is test-only;
production trace callbacks must not block.

## Loopback comparison

On WSL2 Ubuntu 24.04 on an Intel i7-13620H with GCC 13.3 Release builds,
the existing correctness-checking `chronos_exchange_baseline` was run on
the current source and on a separate archive of committed `HEAD`. Each cell
has one discarded warmup and nine samples; values are medians in microseconds.
The runs are sequential, with uncontrolled host load and VM scheduling.

| Mode and workload | Background | Committed | Live owner |
| --- | ---: | ---: | ---: |
| TCP noncrossing | 10 | 167.297 | 117.943 |
| TCP noncrossing | 100 | 112.283 | 102.574 |
| TCP single fill | 10 | 266.070 | 92.789 |
| TCP single fill | 100 | 140.643 | 154.048 |

The TCP benchmark uses one connection for both simulated sides and includes
framing, scheduling and reports. It is not a multi-client benchmark or an
individual matching-latency measurement. Nine samples do not justify p95/p99
claims, and these noisy differences do not establish a performance change.
The direct exchange benchmark does not use the new wrapper. The gateway
retains its existing command queue and thread; the owner adds phase/context
checks and a first-use per-thread context-token allocation. No per-order
mutex or extra worker is introduced. Administrative control start now
reacquires the existing control mutex to serialize against stop, and shutdown
has a separate mutex for concurrent stop callers. Raw CSV files are retained under ignored
`build/phase7c2a1b-baseline.csv` and `build/phase7c2a1b-current.csv`.

## Direct owner-call comparison

`chronos_live_owner_bench` compares the unchanged direct live exchange API
with the new owner wrapper in one Release build, using an empty or one-order
fixture. Five repetitions at a 0.005-second minimum gave these medians of
repetition-level wall-time means:

| Operation | Direct exchange | Live owner |
| --- | ---: | ---: |
| Open and close session | 60.3 ns | 49.4 ns |
| Noncrossing submit | 1.103 µs | 1.150 µs |
| Cancel | 1.288 µs | 1.351 µs |
| Consume report | 607 ns | 602 ns |

For command cases fixture preparation, preceding commands and fixture
destruction are paused; session timing includes both open and close. Timer and
pause/resume overhead, short samples, CPU scheduling and WSL variability can
dominate differences, especially sub-microsecond results. Apparent negative
overhead is not evidence of a speedup. These are not per-command latency
percentiles and do not measure multi-client dispatch. No multi-client
performance benchmark was run; multi-client behavior was verified by tests.

Configure an optimized build with `-DCHRONOS_ENABLE_BENCHMARKS=ON` and build
`chronos_live_owner_bench`, then run
`build/bench/benchmarks/chronos_live_owner_bench --benchmark_min_time=0.005s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`.
Raw results are retained under ignored
`build/phase7c2a1b-owner-benchmark.json`.

The final review reran the same benchmark after the shutdown changes. The
median repetition-level wall-time means were 1.099 µs direct versus 1.070 µs
owner for submission, and 2.003 µs direct versus 1.257 µs owner for
cancellation. The direct cancellation repetitions had a 15.26% coefficient of
variation, and concurrent validation and WSL host load were uncontrolled.
These differences do not establish an improvement or regression. The final
raw run is `build/phase7c2a1-final-review-benchmark.json` (ignored).
