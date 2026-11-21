# Phase 7C.2: durable journal and cold-start recovery design

Status: proposed architecture, not implemented. Phase 7C.1B can verify and
restore a completed isolated simulation from its full history; it cannot
publish a live recovered exchange. Its stream flush is not a durable sync.
This design does not claim durable recovery, authenticated sessions or
exactly-once delivery for the current system.

Phase 7C.2A.2b adds an isolated, synchronous owner-held preparation experiment
described in [its design](phase7c2a2b-prepared-transactions-design.md). It is
not integrated with a journal or gateway output publication.

## Scope and prerequisites

Phase 7C.2A adds a durable, single-owner journal. Phase 7C.2B adds a bounded,
verified restart path and guarded publication. The first supported deployment
should be one local exchange process, one owner thread and one explicitly
configured exchange identity on a supported POSIX local filesystem. Gateway
authentication, distributed consensus, failover, brokerage connections,
durable socket delivery and cross-process concurrent writers are out of scope.

The ordinary exchange transaction clones state internally and commits during
`submit` or `cancel`. The later experimental synchronous prepared scope stages
and installs within one owner call, but does not provide the persistent
prepared handle or gateway publication gate required here. Any durable
extension must preserve its owner and failure guarantees without weakening
existing transaction tests.
Likewise, the existing CHRSTATE format is history-backed and restricted to
completed isolated simulations: it is not yet an arbitrary live snapshot or a
restart manifest. A generic exact-state codec, allocator publication guard and
process-wide live-exchange registry are prerequisites to 7C.2B.

## 7C.2A: durable journal

### Ownership and interfaces

The exchange owner alone admits commands, assigns monotonically increasing
journal sequence numbers (LSNs), stages transactions, writes records and
publishes state. Network threads may enqueue bounded requests but cannot mutate
the book or publish an acknowledgment. A command is admitted only after
framing, protocol and session checks and queue capacity checks; malformed
frames and failures before admission do not consume an LSN. Every admitted
command, including a risk or business rejection, has a terminal journal
outcome. LSN exhaustion stops admission; counters never wrap.

Proposed internal interfaces, with concrete error types and bounds fixed in
the implementation:

```cpp
PreparedCommand ExchangeOwner::prepare(const CanonicalCommand&);
JournalResult Journal::append_and_sync(const PreparedCommand&, Lsn);
ManifestResult ManifestStore::publish_durable_boundary(Lsn, Offset);
void ExchangeOwner::install(PreparedCommand&&) noexcept;
RecoveryResult recover(const Manifest&, JournalReader&, SnapshotReader&);
```

`prepare` must stage all book, risk, ownership, counters, reports and market
data changes, reserve bounded output capacity, calculate the canonical outcome
and acquire any publication resources before persistence. A rejected command
also stages its counter and report effects. The final install must have a
reviewed, tested nonthrowing path; no allocation, callback, blocking queue
operation or fallible external I/O may occur there. Allocation or capacity
failure before journal admission leaves live state unchanged. The existing
global ID allocator and its mutex require an explicit single-writer ownership
protocol: reserve IDs without exposing them, prevent other live allocators
from issuing them, and publish the high-water mark atomically with the staged
state. Holding the allocator lock through sync is a simple initial design but
can block unrelated users; the lock order and a hard single-exchange gate must
be audited before it is allowed. Do not infer that the current allocator or
transaction API already provides these guarantees.

### Format and sequence

Use a versioned little-endian binary format with fixed-width fields and checked
lengths. A journal header identifies the exchange UUID, journal epoch, format
version, schema/configuration digest, initial LSN and checksum. Each bounded
record contains length, version, kind, LSN, payload and CRC. A PREPARE payload
contains the canonical command and any deterministically assigned IDs; a
COMMIT payload binds that prepare record to the canonical result and state or
outcome digest. A REJECT terminal record binds the admitted command to its
rejection and resulting counters. Require exactly one terminal outcome for
each admitted LSN, strictly contiguous LSNs, matching record identities,
known versions and zero reserved fields. Specify concrete maximum lengths,
integer ranges and digest algorithms as format constants before coding;
unknown versions, overflow, inconsistent lengths and duplicate or skipped
sequences fail closed. CRC detects accidental corruption, not malicious
rewriting; authentication requires a separate trust design.

The manifest contains a version, exchange UUID, epoch, configuration digest,
durable LSN and exact terminal byte offset, journal identity, optional
snapshot identity and checksum. It must be encoded with bounded lengths and
strict validation. A manifest points only to a complete terminal record; the
empty journal has an explicit initial boundary. It is the authoritative
acknowledgment boundary, not merely the highest parseable log record.

### Write, sync and acknowledgment ordering

1. Admit and stage the complete command and outcome. Retain all resources
   needed for nonthrowing install; emit no externally visible result or ID.
2. Append PREPARE and COMMIT, or the corresponding admitted rejection,
   using checked partial-write loops. Synchronize the journal with
   `fdatasync` (or `fsync` where required by the supported filesystem).
3. Write a new manifest to a temporary file in the same directory, `fsync`
   it, atomically rename it over the manifest, and `fsync` the directory.
   Only then is its terminal offset a durable boundary.
4. Install the prepared state and allocator high-water with the nonthrowing
   owner operation. Publish bounded reports and acknowledge the command only
   after installation. Preserve committed event ordering for all outputs.

An acknowledged admitted command survives process or machine restart only
under the documented filesystem, hardware and sync assumptions and only after
the recovery procedure succeeds. A crash after the manifest is durable but
before install or response can leave a committed command with an uncertain
client outcome; recovery rolls it forward. Network delivery is not durable and
does not become exactly once. Client retries require a separately specified,
durably stored idempotency key bound to a stable authenticated principal;
current local sessions do not supply that identity.

Any write, sync, rename or directory-sync failure after journal I/O begins is
potentially ambiguous: stop admission, send no success acknowledgment and
enter a fail-stop state requiring restart or operator action. Do not claim that
an error means bytes were absent from storage. Disk-full, short writes,
interrupted calls and I/O errors must be handled explicitly. On recovery,
records strictly beyond the manifest boundary are unacknowledged and are
discarded or quarantined, even if complete; truncate only after verifying the
manifest and syncing the truncation. The recovered manifest may be either the
old or new complete version after an ambiguous rename/sync failure. A torn,
missing, corrupt or inconsistent record at or below the durable boundary is a
hard recovery failure, never silently skipped. A missing or invalid manifest
beside an existing journal also fails closed rather than guessing a boundary.

The baseline guarantee assumes tested local POSIX filesystem semantics for
file sync, atomic same-directory rename and directory sync, storage that
honors flushes, a single writer, and no undetected corruption. Network
filesystems, Windows/WSL host mount behavior, devices with dishonest caches,
filesystem bugs and media loss require separate qualification and are not
covered. Atomic rename alone and C++ stream flush alone are insufficient.

### Fault injection and acceptance

Inject allocation failure at each preparation allocation; short writes and
errors at each header, payload and checksum boundary; journal sync failure;
temporary manifest write/sync failure; rename failure; directory-sync
failure; disk-full; and process termination before and after every numbered
step. Reopen repeatedly and compare each recovered book, risk reservation,
ownership index, allocator high-water, counter and outcome to the expected
durable prefix. Test corrupt checksums, valid-checksum altered identities,
length overflow, duplicate/skipped LSNs, incomplete terminal records and
versions not understood by the reader. Prove no acknowledgment precedes the
durable boundary and no command is admitted after ambiguous I/O failure.
Hardware power-loss testing and filesystem qualification are additional to
unit tests; process-kill tests alone cannot prove power-loss durability.

## 7C.2B: cold-start reconstruction

### Identity, snapshots and replay

Startup begins with admission and gateway publication disabled. Open the
manifest and journal exclusively, validate their exchange UUID, epoch, schema,
configuration digest and durable boundary, then select only a complete,
verified snapshot of that same identity whose inclusive LSN is no greater
than the durable LSN. A snapshot manifest records format, exchange UUID,
epoch, snapshot LSN, journal identity and byte boundary, configuration digest,
content length, checksum and durable publication metadata. Snapshot creation
must sync the file, rename it atomically, and sync the directory before a
manifest can refer to it. Retain the preceding verified snapshot and required
journal segment until the new boundary is durable; no automatic deletion until
retention safety is proven.

Build a private candidate from the snapshot, then replay every complete
contiguous admitted command through the durable boundary, comparing each
canonical outcome and relevant sequence counters. Without a suitable snapshot,
replay from the verified journal origin only if the complete required prefix is
available. Reject missing segments, mismatched identities or configuration,
corruption, outcome divergence and any resource-limit violation. Recovery
must not route reports to live clients during replay. A snapshot is an
acceleration mechanism, not permission to ignore journal identity or durable
boundary checks.

The codec and validation must preserve exact IDs and FIFO sequences, complete
ownership history and indexes, book aggregates, counters and revisions,
sessions and permissions, risk limits, positions and open reservations,
subscriptions and bounded pending output where supported. Check arithmetic,
cross-references, risk/book/ownership agreement, unique IDs, sequence
monotonicity, noncrossed book invariants and all container bounds before
publication. Reconcile the exact allocator high-water against every retained
historical ID, including closed orders and tombstones; it must not be below
any previously issued ID. Exhaustion blocks new allocation. An absent,
ambiguous or inconsistent high-water is a hard failure, not a reason to
reset or reuse IDs.

### Guarded publication and limitations

Acquire an exclusive recovery/publication guard proving that no live exchange,
gateway or process-wide allocator user can issue IDs during reconciliation.
The guard must cover all constructors and allocator entry points, not only
this recovery function. Recheck the manifest identity and allocator state,
reserve all resources needed for publication, then atomically install the
candidate and its global high-water with a reviewed nonthrowing commit.
Release admission only after the exchange owner and required queues are ready.
Any failure destroys the private candidate and leaves the existing process
state unpublished; an ambiguous persistent I/O failure remains fail-stop.
The current implementation lacks the process-wide guard and live publication
API, so this design explicitly prohibits promotion of its isolated candidates.

Session identifiers are currently local development identities, not durable
authenticated principals. Do not attach a restarted socket to an old session
or deliver retained private reports merely because an integer matches. Mark
in-flight client commands and report delivery as uncertain. Durable private
delivery, application acknowledgments, secure reconnect and report replay
need separate identity, retention and protocol designs. A durable exchange
state is not a guarantee that any client received a report.

### Recovery tests, performance and exit criteria

Test snapshot/journal UUID, epoch and configuration mismatches; stale,
missing and corrupt snapshots; gaps and overlaps around snapshot LSNs;
incomplete tails before and after the manifest boundary; duplicate IDs and
exhausted high-water; inconsistent reservations, ownership and positions;
allocation failures at every reconstruction and publication step; competing
allocator users; repeated crash/restart and replay; and refusal to admit
commands or expose a candidate on every failure. Compare exact continuation
state and subsequent multi-level fills and cancellations against an
uninterrupted reference. Verify private reports never leak to new sessions.

Measure sync latency distributions, acknowledgment throughput, journal bytes,
snapshot time, peak memory, recovery time versus journal length and allocator
lock contention on each qualified filesystem. Consider batching/group commit
only after establishing a baseline and preserving a precise per-command
durable boundary and failure semantics. The present full-history CHRSTATE
replay and exchange cloning can grow expensive with state and history; neither
is evidence of production recovery performance.

7C.2A is complete only after ordering, fail-stop and durable-prefix fault tests
pass on explicitly qualified filesystems. 7C.2B additionally requires an
arbitrary exchange snapshot, exact replay and a proven process-wide
publication guard. Neither milestone promises authenticated sessions,
exactly-once client delivery, replicated availability or recovery from
undetected corruption.
