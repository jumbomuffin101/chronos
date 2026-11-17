# Phase 7C.2A.2a implementation report

The process registry guards the lifetime of ordinary live exchanges and an
explicit exclusive live owner. Conflicting constructors fail before any ID is
reserved; private exchanges remain independent. `Gateway` defaults to shared
mode and supports explicit exclusive registration. This is not a durable mode
and does not change matching, risk or protocol acknowledgments. See
[the registration design](phase7c2a2a-allocator-registration-design.md).

Focused tests cover shared/exclusive conflicts, process-ID continuity after
rejection and release, private IDs, cancellation identity, repeated
registration, exception and allocation-failure release, concurrent exclusive
and shared/exclusive attempts, exclusive gateway startup/shutdown, and a
two-client partial fill followed by cancellation. Existing allocator
exhaustion, concurrent shared allocation, simulation, replay and restoration
tests remain enabled. A replay regression also verifies that isolated replay
works during exclusive registration, shared-process replay fails explicitly,
and neither advances the owner's next ID. The process-wide counter cannot be driven to exhaustion
in a test without altering global state; the existing isolated allocator test
and independently constructed synchronized allocator exercise exhaustion.

The registration rule excludes an ordinary exchange for its entire lifetime.
A test-only allocation hook pauses a submit at its first state-clone
allocation, after it has acquired the process-ID reservation, and verifies
that a competing exclusive attempt fails without waiting for that reservation.
A reciprocal test pauses an exclusive owner's reservation and verifies that
ordinary live construction fails until the owner finishes.
The safety argument also relies on the documented prohibition on racing
object destruction with method calls. There is no separately exposed exclusive
reservation benchmark or capability.

## Validation

Debug, Release and ASan/UBSan each passed **167/167 CTest cases** in a
WSL-native source copy in the initial run. The final registration review
passed **171/171** in each configuration after adding shutdown, simulation,
bind-failure and conflict-allocation regressions. The native source was checked
against the Windows checkout before the final run.
Focused runs passed five registration tests, three allocation-failure and
reservation-interleaving tests, the replay-isolation test and two exclusive
gateway tests. Existing shared allocator exhaustion and concurrent uniqueness
tests remain part of each full suite. The matching CLI and five-command mixed
simulation passed; a five-command record/replay verified 12 records and four
trades. CHROVIEW comparison and isolated CHRSTATE restoration passed
(generation 1, ingress 5, 9,348 bytes). Both the ordinary two-client gateway
test and exclusive gateway tests passed. Protocol-v3 tests ran in the full
suites. Formatting and Git whitespace checks passed; web contracts did not
change.

The final review also introduced an explicit nonmutating owner phase before
releasing exclusive registration and moved conflict exception construction
outside the registry mutex. Competing shared construction cannot succeed while
the exclusive owner remains mutation-capable. Cross-process and independently
linked copies remain outside the registration guarantee.

## Performance observations

On WSL2 Ubuntu 24.04, Intel i7-13620H and GCC 13.3 Release, the optional
`chronos_allocation_registration_bench` measured five repetitions with a
0.005-second minimum. Median repetition-level wall-time means were 45.8 ns
for shared owner construction/destruction and 41.3 ns for exclusive owner
construction/destruction; these include exchange state allocation and are not
isolated registration-acquisition times. A noncrossing owner submission was
1.308 µs shared and 1.008 µs exclusive; setup and teardown were paused, but
matching and allocator reservation remain included.

The existing allocator microbenchmark measured 5.96 ns for a synchronized
local reservation and 1.00 ns for an unsynchronized private one. Those are
not registered-exclusive process reservations and sub-microsecond figures are
dominated by optimization and measurement uncertainty. No registered
exclusive reservation is exposed separately for measurement. The shared and
exclusive owner-submission measurements both use the same process allocator.
The existing benchmark's direct shared `Exchange` construction/destruction
median was 70.6 ns; its isolated counterpart was 65.1 ns. These are not a
controlled before/after measurement of the registry.
No lock-contention performance benchmark was run; concurrent correctness was
tested separately.

For the existing loopback TCP benchmark at 10 background orders, nine-sample
medians were 93.579 µs noncrossing and 134.588 µs single fill. The prior
Phase 7C.2A.1B run recorded 117.943 µs and 92.789 µs respectively. These
separate WSL runs have uncontrolled load and scheduling; they do not establish
a latency improvement or regression, and nine samples do not support p95/p99.
The TCP benchmark is one-client loopback, not production network latency.

Reproduce the owner comparison with an optimized CMake build configured with
`-DCHRONOS_ENABLE_BENCHMARKS=ON`, target
`chronos_allocation_registration_bench`, and
`--benchmark_min_time=0.005s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`.
The ID allocator uses `chronos_id_allocator_bench` with the same arguments.
The gateway uses the existing `chronos_exchange_baseline` target configured
with `-DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON`. Raw results are retained in the
ignored `build/phase7c2a2a-registration-benchmark.json`,
`build/phase7c2a2a-id-benchmark.json` and `build/phase7c2a2a-gateway.csv`.
