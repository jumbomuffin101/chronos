# Phase 7C recovery architecture

7C.1B now includes a restricted, history-backed private candidate restorer for
completed isolated simulations. It does not publish a live exchange or supply
crash recovery; see [the restoration design](phase7c1b-restoration-design.md).

Implementation finding: the current 7C.1 work provides only a diagnostic
observation checkpoint, not snapshot restoration. The process-wide ID allocator,
order-book revision state and other continuation fields need an explicit design
before the reconstruction proposed below can be implemented safely. See
[the 7C.1 design](phase7c1-snapshot-design.md) for the concrete limits.

Allocator update: isolated candidate exchanges and replay now have private
owner-thread counters; default gateway and simulation exchanges retain the
shared process allocator. This is not candidate publication or recovery. See
[the allocator isolation report](phase7c1a-allocator-isolation-report.md).

This is a proposal, not implemented behavior. Phase 7B records isolated
simulation verification artifacts; its command-before-admission and
outcome-after-commit stream flush does **not** make exchange commits crash
durable. Phase 7C must separately establish exchange-state reconstruction,
crash-consistent persistence, and client identity and delivery. None follows
automatically from either of the others.

## A. Exchange-state reconstruction

The exchange owner is the only mutator. At an owner command boundary, it can
construct a versioned canonical snapshot of the committed state and a precise
inclusive log position. A snapshot must carry format and exchange schema
versions, engine/generator versions when applicable, exchange-instance epoch,
snapshot generation, last committed ingress and record positions, and a
checksum covering its metadata and contents. Use explicit fixed-width
encoding, bounded lengths and deterministic order; never serialize pointers,
iterators, native object layouts or process addresses.

The persisted representation must include complete price levels and FIFO
orders, remaining quantities and stable logical IDs, ownership, accepted
request/idempotency history, account permissions and kill state, risk limits,
positions, reservations or their authoritative recomputation inputs, audit
trades and all relevant exchange, engine, trade, private-event and public-feed
counters. Process-global internal IDs must either be durably restored with a
verified high-water mark or mapped through a durable exchange-scoped ID scheme.
The Phase 7B verification vector is not a reconstruction format and the FNV
simulation digest is not sufficient evidence of state equivalence.

At startup, validate versions, bounds and checksums before installing anything.
Load the newest *committed* snapshot with a valid associated contiguous log
suffix; replay committed records after its inclusive position in order into a
fresh offline state, checking recorded decisions and counters. Reconcile every
book aggregate and FIFO link, ID uniqueness and ownership, positions, risk
reservations and limits, trade history, account state, and counter high-water
marks before publishing the state or accepting commands. A corrupt,
incompatible, or orphaned snapshot must never be partially installed: fall
back only to an explicitly retained older valid generation and a contiguous
verified suffix, otherwise remain offline with an actionable error.

The owner can initially pause command admission while it copies a consistent
image. Incremental or background snapshotting requires a separately proven
immutable version boundary; a background thread must never traverse mutating
state. Snapshot duration, memory amplification, size, and suffix replay time
must be measured before choosing a cadence.

## B. Crash-consistent persistence

Use a new versioned journal protocol; do not reinterpret a Phase 7B terminal
record as a durable commit. A candidate design is a single-writer,
length-delimited, checksummed prepare record containing a canonical command,
sequence and deterministic inputs, followed by a commit record that identifies
that prepare and includes or authenticates its authoritative outcome. Enforce
sequence continuity and a bounded record size. Precisely define which rejected
commands consume durable ingress positions and retain their outcomes where
idempotency or client acknowledgment depends on them.

The exchange owner must prepare the transaction without publishing it, append
the required journal records, perform the configured durable file sync, then
install the already-prepared transaction with a proven nonthrowing commit and
only afterward issue the specified acknowledgment. Recovery applies only
valid, fully committed journal entries. A crash after the durable commit record
but before memory installation or acknowledgment means the operation is
recovered, while the client outcome remains uncertain. A crash before the
commit record is durably established must not make the operation visible after
recovery. The exact protocol must validate deterministic replay inputs,
allocation behavior and commit ordering; group commit can be introduced only
with equally explicit per-command acknowledgment and failure semantics.

On a supported POSIX filesystem, the implementation must check every write,
handle short writes and `EINTR`, use `fdatasync`/`fsync` according to the
specified file metadata requirements, and sync the containing directory for
creation, renames and deletions where required. Snapshot publication should
write a new temporary generation, sync the file, atomically rename it, and sync
the directory before publishing a durable manifest or retiring older files.
Log rotation must retain at least one verified snapshot plus its complete
suffix until the replacement and its directory entry are durably published.
The exact ordering, rename and sync guarantees must be tested on each supported
filesystem; WSL, caches, storage devices and remote filesystems can have weaker
or different guarantees. A successful system call is not an unconditional
promise against hardware faults or broken storage firmware.

Recovery may discard only an incomplete or checksum-invalid **tail** under a
documented policy, with the offset and reason reported; an invalid interior
record, a gap, a duplicate sequence, or a committed record after corruption
must fail recovery rather than silently skip events. Protect any truncation by
first preserving forensic data and proving that the last accepted record is
not being discarded. Disk-full, quota, partial-write, sync, rename, and close
errors stop new durable acknowledgments and transition to a visible degraded
or offline state; never imply rollback of already committed operations. The
policy must specify behavior for in-flight prepared operations, pending client
requests and operator intervention.

## C. Session identity and delivery

Current TCP session IDs are ephemeral local-development identities. Restarting
the exchange does not authenticate a reconnecting client, restore a socket,
prove a report was received, or establish who may access a recovered order.
Before supporting session recovery, define a separately authorized stable
principal and account identity, credential validation, ownership and access
rules, principal-to-session binding, revocation and epoch handling. Development
identifiers must not be presented as production authentication.

Define stable client order and request IDs within a documented principal/account
scope, durable duplicate detection and an idempotency retention window. A
retry with a matching key and payload can return the authoritative prior
decision; a conflicting payload must be rejected explicitly. This requires
durably recording the decision and key as part of the same transaction, and
does not by itself establish exactly-once processing or delivery. An
acknowledgment protocol, durable report sequencing, retention, client cursors,
authorization and replay policy must be designed and tested independently.
Loss of private report history or an expired idempotency window must return an
explicit reconciliation requirement, not a fabricated result.

The dashboard must retain uncertain outcomes across disconnect and show that
their status is unknown until an authoritative, authorized order/account query
or reconciliation API resolves them. A recovered book or position alone is
not proof that a particular private execution report was delivered. Public
feed epochs, snapshot-to-update continuity and gap recovery must also be
re-established after a restart. Until the backend implements and validates
those APIs, a restart can only be shown as disconnected/recovering and the
dashboard must not promise restored sessions or delivery.

## Delivery milestones

### 7C.1 — Snapshot serialization and verified reconstruction

- **Prerequisites and ownership:** define a stable logical-ID and snapshot
  schema, owner-thread snapshot boundary, bounded state APIs, version policy,
  and offline reconstruction entry point. Keep matching and risk decisions in
  C++; the dashboard and adapter are not state authorities.
- **Proposed interfaces:** an owner-only `capture_snapshot()` returns a bounded
  immutable image and inclusive committed position; a versioned codec validates
  a snapshot; `restore_offline(image)` produces an unpublished candidate with
  a structured reconciliation result. These are design sketches, not current
  API promises.
- **Tests and acceptance:** golden and cross-version fixtures, empty and
  multi-symbol books, FIFO/ownership, partial fills, reservations, limits,
  positions, counters, corrupted and oversized data, invalid IDs, allocation
  failures, and a deterministic round trip. No live state changes on failed
  restore; a candidate is installable only after full reconciliation.
- **Performance:** measure owner pause, copy size, peak memory, codec cost,
  and reconstruction time as book and account counts grow. No durability or
  network-session recovery is claimed by a passing in-memory round trip.

### 7C.2 — Durable journal and restart recovery

- **Prerequisites and ownership:** 7C.1 plus a reviewed commit/ack protocol,
  durable ID and idempotency design, explicit supported filesystems, and a
  nonthrowing prepared exchange commit. A single persistence owner serializes
  records with the exchange owner; filesystem I/O must not run concurrently
  with an uncoordinated mutation.
- **Proposed interfaces:** `prepare(command)`, `append_prepare(...)`,
  `append_commit(...)`, `sync()`, `install_prepared()`, and an offline
  `recover(snapshot, journal)` returning either a reconciled candidate or a
  failure with offset and reason. Exact types and acknowledgement boundaries
  are subject to implementation review.
- **Tests and acceptance:** inject process termination at every append, sync,
  commit, rename and directory-sync boundary; cover torn writes, checksums,
  missing and duplicate records, non-tail corruption, disk full, sync errors,
  incompatible versions, snapshot fallback, rotation, replays, and repeated
  restart. For each crash point, assert the explicitly specified committed
  prefix and reconcile all state before admission. Validate the real target
  filesystem separately from mock I/O.
- **Performance:** measure sync and group-commit latency distributions,
  admission throughput, snapshot pause, bytes per command, peak RSS, and
  restart time at representative loads. Durable acknowledgment is expected to
  cost more than the Phase 7B memory-stream benchmark.

### 7C.3 — Authoritative simulation controls and dashboard integration

- **Prerequisites and ownership:** an authoritative backend simulation
  lifecycle API with access controls appropriate to its deployment, a stable
  run ID and explicit state/sequence reporting, plus any validated 7C.2
  recovery status the UI displays. The adapter translates transport messages;
  it does not generate exchange decisions or infer recovered state.
- **Proposed interfaces:** start/stop requests with bounded configuration,
  status/progress and terminal reason; an initial status snapshot and
  sequenced updates over the existing server-side adapter; and an explicit
  recovering/degraded state. Define authorization and idempotency for each
  mutating control before exposing it.
- **Tests and acceptance:** real gateway/adapter/browser integration,
  independent sessions, restart during a run, stale callbacks, progress gaps,
  partial starts/stops, bounded queues, uncertain order outcomes, and
  accessible responsive controls. Controls must remain disabled or clearly
  unavailable when no authoritative API or identity policy exists.
- **Performance and scope:** measure control-plane latency and dashboard feed
  load without blocking the owner. Authenticated reconnect, private-report
  replay, durable delivery and exactly-once claims remain out of scope until
  each has a separate implemented and verified contract. No brokerage access.
