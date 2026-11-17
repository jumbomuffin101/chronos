# Phase 7C.2A.1 implementation report

This report records the earlier isolated-only milestone. The later
[live gateway owner](phase7c2a1b-implementation-report.md) changes the
gateway integration described below; its other limitations remain in force.

The implemented prerequisite is `OfflineOwner`, an isolated-exchange wrapper
with an owner-context check, no exposed mutable `Exchange`, and a synchronous
freeze scope that blocks every exposed mutator. It is not a gateway owner,
general transaction preparation, cross-thread request queue, or a durable
publication gate. The complete existing `Exchange` API remains callable and
continues to require caller-side serialization. See
[owner boundary design](phase7c2a1-owner-boundary-design.md) for the full
mutator inventory, exclusions, and required migration.

The new tests exercise all exposed mutations during a freeze, wrong-context
read and mutation, nested scopes, exception-safe scope release, rejection
atomicity and order-ID continuity. Existing gateway, simulation, replay and
restoration behavior remains on the original path. The previous Phase 7C.2A
regressions and documentation remain intact.

Allocator registration, live gateway migration, publication gating,
prepared-state installation, storage, recovery and durable output remain
unimplemented.

## Validation

Debug, Release and ASan/UBSan each passed **147/147 CTest cases** in a
WSL-native source copy checked against the Windows checkout. Four new owner
tests cover frozen mutators, wrong-context access, scope exception handling,
and context lifetime; the three earlier boundary tests are also retained.
Matching and simulation CLI demos passed. Phase 7B replay verified 12 records,
five commands and four trades; CHROVIEW comparison and the 9,348-byte
CHRSTATE isolated restoration demo passed. The direct two-client gateway fill
test passed, as did formatting and Git whitespace checks. No browser-facing
contract changed.

## Restricted offline performance comparison

The optional `chronos_offline_owner_bench` target compares a directly owned
isolated exchange to the wrapper in the same optimized binary. On WSL2 with
the Intel i7-13620H and GCC 13.3, five repetitions with a 0.005-second minimum
per repetition yielded the following median *repetition-level means* of wall
time, not per-operation latency percentiles:

| Operation | Direct isolated | OfflineOwner |
| --- | ---: | ---: |
| Open and close a session | 47.1 ns | 45.5 ns |
| Noncrossing submit | 1.035 µs | 1.047 µs |
| Cancel | 1.176 µs | 1.203 µs |
| Consume a report | 624 ns | 583 ns |

For submit, cancel and report consumption the fixture and preceding commands
are prepared with timing paused, and fixture destruction is also excluded.
The session measurement includes both open and close. Pause/resume and timer
overhead, short samples, OS scheduling, WSL virtualization and concurrent
validation can dominate small differences; negative apparent overhead is not
a speedup claim. The fixture uses an empty or one-order book, not a production
workload. No independent owner-dispatch or gateway round-trip benchmark was
run because neither uses this wrapper. The existing production gateway and
exchange command paths are unchanged. These results do not establish p95/p99
or production latency.

Configure `-DCHRONOS_ENABLE_BENCHMARKS=ON`, build target
`chronos_offline_owner_bench`, and run
`build/bench/benchmarks/chronos_offline_owner_bench --benchmark_min_time=0.005s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`.
Raw machine-readable output from this run is in ignored
`build/phase7c2a1-owner-benchmark.json`.
