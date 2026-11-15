# Phase 7 simulation, replay and recovery design

Phase 7B now implements isolated simulation recording and replay verification.
See [the implementation report](phase7b-implementation-report.md) for actual
behavior. Recovery, durability, and network-session replay remain proposals.

This is a proposal, not an implemented guarantee. Chronos remains an in-memory,
loopback-only simulator. Deterministic replay is distinct from durable storage,
crash recovery, authenticated sessions, and delivery guarantees. No exactly-once
delivery is claimed.

## Shared boundary and sequencing

The existing exchange owner thread remains the sole mutator of matching, risk,
ownership and feed state. Gateway sessions and a future simulation submit typed
commands to that boundary; neither the browser nor a generator matches orders or
decides risk. Assign a checked, monotonically increasing **ingress sequence** to
each admitted command, including commands later rejected by validation or risk;
record rejection reason and unchanged-state outcome. Commands refused before
admission (malformed frame, disconnected peer or full queue) have no ingress
sequence and are represented only by optional transport diagnostics. Record the
exchange's existing command, event, trade and market-feed counters separately;
do not conflate their scopes or invent values for rejected operations. Fail
closed before counter wrap. The owner defines one deterministic serialization
order independent of network arrival timing once commands are admitted.

## 7A — deterministic simulation engine

**Ownership and interfaces.** A C++ `ScenarioConfig` contains schema version,
symbols, participant count, bounded command count, limits and seed. A
`SimulationRunner` owns explicit participant identities, a specified PRNG
algorithm and state, and a logical step clock. A `Scenario` produces typed
`SimCommand` values; an owner-thread `submit_simulated(command)` returns an
admission result and later the authoritative exchange outcome. A control object
provides `start(config)`, `request_stop()` and immutable `status()` snapshots.
Each participant uses exchange-defined session/account and order IDs, never a
dashboard-generated internal ID. Use finite queues and explicit backpressure.

**Sequencing and persistence.** The runner emits in logical-step order with a
stable participant ordering, fixed integer distributions and explicit PRNG
version. Seed alone is insufficient without recording config, generator version,
initial exchange state and accepted-command order. 7A need not persist anything;
if live clients share an exchange, record their actual interleaving or isolate
the simulation for reproducible runs. Do not use wall time as a matching input.

**Failure and crash.** Reject invalid configuration before start; report queue
saturation and stop requests explicitly. Generator or exchange failure stops the
run without asserting rollback of already admitted commands. A process crash
loses in-memory state; 7A does not provide recovery. Cancellation on stop must
be a separately specified sequence of exchange commands, not implicit mutation.

**Tests, performance and dependencies.** Test fixed seeds against golden command
streams, repeated exchange outcomes, participant ID uniqueness, bounds,
backpressure, stop races, and rejection paths; compare existing clone-reference
and planned transaction modes where applicable. Benchmark generation separately
from matching and end-to-end gateway work; bound queues and avoid sleeping on the
owner thread. Depends on current exchange admission, session and risk APIs.
Out of scope: brokerage, strategy profitability, production market simulation,
distributed ordering and backend/dashboard controls before an API exists.

## 7B — append-only event log and deterministic replay

**Ownership and interfaces.** An owner-sequenced `EventRecorder` accepts an
immutable `CommandEnvelope` and `OutcomeEnvelope`; a `ReplayReader` validates
records, and a `ReplayVerifier` reconstructs a fresh exchange and compares
canonical outcomes. Proposed calls: `append(envelope) -> AppendResult`,
`flush(policy) -> FlushResult`, `next() -> Record | End | Error`, and
`verify(recorded, actual) -> Diff`. The writer owns the file; a bounded handoff
must have explicit full-queue behavior and cannot silently drop admitted events.

**Format and semantics.** Define a versioned file header and length-delimited
records with endian, length caps, record kind, ingress sequence, payload and
checksum. Include run/config identity, generator and engine versions, initial
state/config, accepted commands, validation/risk rejections after admission,
ordered outcomes and relevant counters. Specify checksum algorithm and coverage,
canonical integer/string encoding, migration and unknown-version handling before
implementation. The owner must define a commit protocol between mutation and
logging; merely writing asynchronously after a commit does not make it durable.
Replay uses recorded admitted inputs and checks outcome bytes or a versioned
canonical representation, counters, risk and book state. Never infer omitted
transport rejects as exchange commands.

**Failure and crash.** Distinguish write, flush and storage failures. Before a
durable acceptance promise exists, fail or pause further admission according to
an explicit policy; never pretend the exchange rolled back a committed command.
Detect truncated final records, bad lengths, checksums and sequence gaps. Report
corruption with offsets; do not silently skip or replay invalid data. Successful
deterministic replay does not imply fsync, crash durability or report delivery.

**Tests, performance and dependencies.** Test golden files, reproducibility,
mixed rejected/admitted commands, overflow, schema evolution, replay mismatch,
truncation at every boundary, bit flips, short writes, injected I/O failures and
bounded queue saturation. Measure serialization, checksum and flush latency and
throughput separately; bound buffers and compare synchronous versus batched
flush policies without weakening acceptance semantics. Depends on 7A's stable
inputs and current exchange owner sequencing. Out of scope: exactly-once
delivery, production-grade write-ahead logging and transparent session recovery.

## 7C — recovery and dashboard integration

**Ownership and interfaces.** A `RecoveryCoordinator` validates a
`SnapshotManifest`, loads a versioned exchange snapshot at a known ingress
sequence, and replays a verified log suffix before opening admission. A proposed
`recover(snapshot, log) -> RecoveryReport` returns the final sequence, state
checks and explicit errors. Owner-controlled snapshot creation must capture
books, ownership, sessions where supported, positions, risk reservations,
sequence counters and configuration at one consistent boundary. Dashboard
controls use a future authenticated/authorized backend `start/stop/status`
interface only after it exists; the browser never owns matching or risk.

**Persistence and crash policy.** Define write ordering, file and directory
sync, snapshot generation IDs, atomic publication, log rotation and the precise
durable acceptance point for the target filesystem. On restart, select only a
validated complete snapshot and contiguous verified suffix; reject or explicitly
quarantine a torn tail according to policy, never silently discard an interior
corruption. Reconcile active orders with ownership, positions, reserved risk,
book aggregates and all counters before resuming. Test and document the loss
window of each flush policy. A restart without a valid chain remains offline.

**Identity and delivery limits.** Current TCP sessions are ephemeral and have
no authenticated reattachment; an exchange state snapshot alone does not restore
client identity or prove a private report was delivered. Keep old outcomes
uncertain to clients until separate identity, authorization, acknowledgment,
idempotency and reconciliation protocols are specified and verified. Dashboard
progress must be sourced from the authoritative runner and visibly distinguish
starting, running, stopped, failed and recovering states.

**Tests, performance and dependencies.** Inject crashes at every snapshot,
append, flush, rename and reopen boundary; test partial records, stale snapshots,
counter boundaries, duplicate inputs, risk/book reconciliation, incompatible
versions, storage exhaustion and private-session limitations. Benchmark snapshot
pause time, suffix replay time, disk use and p50/p99 admission latency with
documented hardware and flush policy. Depends on verified 7B format and owner
state APIs, plus explicit future identity and dashboard contracts. Out of scope:
real brokerage, production authentication implementation, unverified durable
delivery claims and automatic reconciliation that guesses missing state.
