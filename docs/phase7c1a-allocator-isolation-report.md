# Phase 7C.1A.2 allocator isolation

The default `Exchange` retains process-wide, mutex-serialized, monotonically
issued internal order IDs, unique for the lifetime of the process. The old
static counter and mutex are now one noncopyable allocation service. A
constant-initialized process service is available to other static initializers.
Each exchange additionally owns an unused private allocator in shared mode; it
does not introduce another lock or per-command allocation on the live path. A
reservation locks the shared service, proposes the next nonzero ID without
advancing the counter, and retains the lock through staged matching, reports,
market publication and the exchange state swap. Only after that nonthrowing
swap does `commit()` advance the service. Rejected or failed submissions and
exceptions release the reservation without consuming an ID. Exhaustion returns
`CounterExhausted` rather than wrapping. No per-order allocation or additional
global lock is introduced by this abstraction.

An explicit `Exchange::isolated(...)` factory creates a noncopyable,
nonmovable exchange owning a separate owner-thread-only allocator. Its counter
starts at zero by default; the optional last-ID value is scoped to that private
context for controlled boundary tests, not an import or publication API.
Private IDs may collide with live IDs and with other isolated exchanges; their
reports must remain within the caller's isolated context. Constructing,
submitting to, rejecting from, or destroying one does not touch the shared
allocator. The existing `Gateway` constructs and owns its own default shared
exchange and has no interface to accept a candidate. No conversion, promotion,
or publication method is provided. Arbitrary third-party code that manually
forwards a private report must honor this boundary; the type system cannot
prevent application code from copying numeric IDs into its own transport.

Phase 7B verification uses an isolated exchange by default. The log format and
logical order-history normalization are unchanged. `ReplayAllocation::SharedProcess`
explicitly opts into the prior behavior, consuming shared IDs; invalid enum
values fail rather than falling back to shared allocation. Normal simulations
and the gateway continue to use default shared exchanges. Exchange state clones
do not copy or fork allocator ownership: the reservation belongs to the outer
exchange and commits only when its staged state is installed.

The allocator service has a synchronized mode and an owner-thread-only mode;
both are noncopyable. Tests can construct a separate synchronized service with
an initial high-water mark to exercise exhaustion without changing the real
process service. There is no public injection or reset of the process service.
Neither allocator state nor `CHROVIEW` is a restorable snapshot, and no global
ID or report guarantee extends across a process restart.

## Future publication: cold start first

Prefer a cold-start protocol before live admission or gateway sessions exist.
Privately validate the entire continuation image, exact IDs, history,
cross-references, capacities, counters, revisions and risk state without
touching the process service. Under a coordinator lock, prove there are no
other active shared exchanges or outstanding old sessions/IDs; verify every
restored ID is valid and unique; calculate a checked high-water mark at least
as large as every restored ID; verify the allocator can issue the next ID;
then atomically install both the candidate state and allocator high-water mark
before opening admission. Failure before that boundary must leave both live
state and allocator unchanged. The exact irreversible publication operation,
locking order, rollback behavior, lifecycle and crash semantics require a
separate design and fault tests. Existing sessions are ephemeral and cannot be
reattached merely because state was restored. If live exchanges coexist,
coordinated publication additionally needs an active-ID ownership registry,
admission quiescence, collision checks and atomic replacement; it is not
implemented here. Exact IDs cannot be exposed by both a private candidate and
an independently live replacement as if both shared one global namespace.

Remaining 7C.1B prerequisites include an exact continuation image, bounded
trusted import, revision and selection identity handling, state consistency
validation, durable-ID policy, and independently tested future-command
equivalence. There is no snapshot reconstruction, publication, crash recovery,
authenticated reconnect, or durable delivery in this change.

## Validation

Debug, Release and ASan/UBSan each passed 132/132 CTest cases from a temporary
WSL-native source copy. A Windows-mounted build passed earlier but reported
clock-skew warnings; the native builds avoid that timestamp ambiguity. Tests
cover private lifecycle and submissions without global ID consumption,
private ID reuse across separate contexts, partial fills and owner cancellation,
private and independently constructed synchronized-service exhaustion,
rollback before allocator commit, concurrent live exchanges and shared
reservations, and both replay modes. The shared exhaustion test injects a
high-water mark into a separate synchronized service, not into the actual
process allocator. Static assertions cover exchange nonmovability and the lack
of a gateway constructor accepting a candidate.

The matching CLI, five-command mixed simulation, and five-command liquidity
sweep with record/replay passed; the replay verified 12 records and four trades.
`bash scripts/format.sh --check` and Windows Git whitespace checks passed.

## Performance observations

On WSL2, Intel i7-13620H, GCC 13.3 Release, Google Benchmark 1.8.3, three
repetitions with a 0.05-second minimum, medians of repetition-level wall-time
means were: synchronized reservation 4.67 ns; isolated reservation 0.665 ns;
empty-book shared submission 1.052 µs; isolated submission 1.020 µs; shared
construction 44.1 ns and isolated construction 38.1 ns. Setup and result
checking are excluded from submission timing. The sub-nanosecond private
reservation figure is an extremely small optimized microbenchmark, not a
standalone production latency or evidence of sub-nanosecond trading.

For 20 generated commands, unrecorded simulation measured 45.793 µs, recorded
simulation 693.850 µs, and isolated replay 646.116 µs. The previously recorded
Phase 7B comparison was 47.536 µs, 684.182 µs, and 577.141 µs respectively
(approximately -3.7%, +1.4%, and +12.0%). These are separate short runs on the
same WSL2 machine, with different host load and build filesystem; the current
tree also includes the 7C.1 bounded verification traversal. They do not isolate
allocator overhead or establish a statistically controlled regression. No
matching-engine or network latency claim follows. The benchmark source is
`benchmarks/id_allocator_bench.cpp` and the 20-command workloads are in
`benchmarks/eventlog_bench.cpp`.
