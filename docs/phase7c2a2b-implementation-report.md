# Phase 7C.2A.2b implementation report

The new owner-held prepared scope stages submit or cancel through the existing
exchange on an isolated clone. It retains a real process-ID reservation for
submit until installation or abort. Installation swaps a prebuilt state and
commits that reservation through a private `noexcept` function; no staged
handle or reserved ID escapes. The scoped result explicitly distinguishes an
accepted proposal from one actually installed. See
[the design](phase7c2a2b-prepared-transactions-design.md) for the proof and
limits.

Tests compare exact bounded continuation state and internal order/trade IDs
after multi-level partial and complete fills, FIFO-sensitive orders,
cancellation and rejection; they also
cover frozen mutation and owner-context rejection, abort, callback exceptions,
report capacity, competing shared construction, allocator ID continuity and
allocation-failure sweeps. The installation test arms allocation failure after
preparation for both submit and cancellation. A private scoped design prevents
double or stale installation structurally rather than exposing an install
handle to test. This is not a durable transaction or gateway publication gate.

## Validation

The final review passed **178/178 CTest cases** in each of Debug, Release and
ASan/UBSan in a native WSL copy (0.92 s, 0.98 s and 3.96 s respectively).
The earlier review also ran the prepared-scope and allocation-failure tests
directly.
The matching CLI and mixed simulation passed. A five-command Phase 7B replay
verified 12 records and four trades; CHROVIEW and CHRSTATE demonstrations
passed (generation 1, ingress 5, 9,348 bytes). The ordinary two-client gateway
test and all three exclusive-gateway tests passed. Protocol-v3 tests ran in the
full suites; formatting and Git whitespace checks passed.

The final review adds a separately linked test-only core and process to seed the
actual synchronized process allocator before use. It verifies that abort and
risk rejection preserve exact state at the last-ID boundary, installation uses
the last ID, the next submission is rejected without wrapping or changing
state, and cancellation still works. The production core has no test-seam
symbol. A rejected proposal, capacity failure and allocation-failure sweep also
verify no live state or ID advance. No journal, manifest, fsync, restart or
durable client delivery is implemented. The gateway does not invoke the new
scopes; its business acknowledgments and outputs are not durability-gated.

The synchronous decision callback is arbitrary trusted code. It can invoke
external code or publish a preliminary decision and such side effects cannot be
rolled back if it throws. The reserved ID and candidate are not exposed through
the callback, but no structural gateway publication gate exists yet.

## Performance and allocation observations

An optimized WSL2 Ubuntu 24.04 build on an Intel i7-13620H with GCC 13.3 used
five repetitions with a 0.005-second minimum. Median repetition-level wall
means, not per-command percentiles, were:

| Operation | Existing path | Prepared and installed | Prepared and aborted |
| --- | ---: | ---: | ---: |
| Noncrossing submit | 1.823 µs | 2.220 µs | 2.343 µs |
| Cancel | 2.050 µs | 3.011 µs | 2.911 µs |

The submit fixture starts empty; cancellation starts with one resting order.
Fixture construction, session setup and destruction are outside measured
time. Prepared timings include candidate construction, cloning, decision,
installation or abort and cleanup; they do not isolate installation. The
prepared-install submit and cancel measurements had 75% and 46% coefficients
of variation, respectively. Short samples, high host load and WSL scheduling
make the apparent differences unreliable as precise overhead estimates.

A separate one-operation scalar `operator new` probe counted 18 allocations
and 1,552 requested bytes for ordinary submit versus 24 and 2,440 for prepared
submit. Ordinary cancel used 19 allocations and 1,944 bytes versus 38 and
3,864 for prepared cancel. Counts exclude allocator metadata and allocation
forms not routed through the overridden scalar operator; they are not peak
memory. `/usr/bin/time -v` reported **4,480 KiB** maximum RSS for the entire
benchmark process, including runtime and fixtures, not incremental staging
memory. Preparation makes an extra full-state clone, and these measurements
show additional allocation, but no component-level profile was collected, so
they do not prove cloning dominates elapsed time.

The final review repeated five 0.01-second minimum repetitions in the same
kind of WSL environment. Median repetition-level wall means were 1.590 µs for
ordinary submit, 1.382 µs for prepared submit, 1.330 µs for ordinary cancel,
and 1.897 µs for prepared cancel. The respective coefficients of variation
were 16.69%, 8.32%, 9.87%, and 18.52%; these are not command-latency
percentiles, and the apparent submit speedup is not a reliable conclusion.
The allocation probe repeated the same counts above. The prepared operation
first clones exchange state, then calls the existing path, which stages its own
transaction and associated order-book, output and ownership containers. The
current public scope does not expose a separate install timer: abort and install
measurements include cleanup, and subtracting them is not a defensible isolated
installation latency measurement. The install function itself performs only a
state pointer swap and, for submits, an already-reserved counter assignment.

To reproduce, configure an optimized build with
`-DCHRONOS_ENABLE_BENCHMARKS=ON`, build `chronos_prepared_scope_bench` and
`chronos_prepared_allocation_probe`, then run the probe and run the benchmark
with `--benchmark_min_time=0.005s --benchmark_repetitions=5
--benchmark_report_aggregates_only=true`. Raw results are in ignored
`build/phase7c2a2b-final-benchmark.json`,
`build/phase7c2a2b-allocation-probe.csv` and
`build/phase7c2a2b-memory.txt`.
