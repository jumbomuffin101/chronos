# Phase 7C.2A.2: transaction prerequisites

Status: architecture only. The gateway owner and synchronous freeze scope do
not provide prepared transactions, durable IDs, journal writes, recovery or
durable acknowledgments. This note defines an implementation sequence and
acceptance criteria; it does not authorize a live restored-exchange import.

The subsequent [allocation registration milestone](phase7c2a2a-allocator-registration-design.md)
implements a process-local shared/exclusive lifetime registry. It does not
implement prepared transactions, durable IDs or the later publication gate.

Phase 7C.2A.2b adds a synchronous experimental
[prepared owner scope](phase7c2a2b-prepared-transactions-design.md). It does
not implement a durable journal, asynchronous prepared handles or gateway
publication gating.

## 1. Exclusive durable-owner registration

Introduce a process-wide registry with a move-only registration owned by the
gateway's live exchange owner. Registration must be acquired before the
gateway accepts commands and released only after admission is closed, pending
transactions are resolved and the owner has stopped. Define construction and
destruction ordering so a failed startup leaves no registered owner or open
admission path. Do not let ordinary live `Exchange` constructors or alternate
allocator entry points bypass the registry; isolated exchange instances keep
private allocator contexts and cannot transfer private IDs into live state.

Acceptance: deterministic tests cover conflicting registrations, ordinary
live exchanges racing registration, failed construction, owner abandonment,
shutdown and registration handoff, with no duplicate live IDs. The gateway
must refuse startup or admission explicitly when exclusivity cannot be proven.
The registry is process-local; it does not coordinate another process or
establish crash durability.

## 2. Global allocator coordination

Make ID reservation an explicit part of the owner-controlled transaction. A
reservation holds or otherwise exclusively owns the shared allocator position
until the prepared state is installed or aborted. It must not publish the
high-water value during preparation. Installation publishes the state and
allocator advancement under a proven owner/allocator critical section;
aborted preparation consumes no ID. At `UINT64_MAX`, fail admission without
wrapping. Preserve today’s globally unique live IDs and isolated private IDs.

Specify and enforce a lock order before implementation: lifecycle/registry
authority precedes allocator reservation; the gateway stop mutex precedes its
control-queue mutex, and the control-queue mutex precedes an individual
control mutex. Do not hold the queue mutex across matching, user callbacks or
filesystem I/O. Any interaction between the owner/allocator and shutdown
locks must have a documented acyclic ordering; prove it with focused tests.
Holding the allocator mutex across a future sync is a possible conservative
starting point, but blocks other live allocators and requires an enforced
single-writer protocol rather than an undocumented assumption.

Acceptance: tests force aborted and throwing preparations, exhaustion,
concurrent reservation attempts, registration conflicts and stop races; they
verify no consumption on abort, no duplicate publication and no deadlock.
Private candidates cannot be registered or promoted through this interface.

## 3. Prepared transaction state

Replace the gateway's internal direct submit/cancel call with a restricted,
move-only prepared object owned by the bound context. It must retain the
candidate exchange state, canonical command and complete outcome, reservation,
counter changes and capacity for all affected private reports and public
events. Validation and all potentially throwing allocation occur before a
future persistence boundary. Preserve the existing rejection behavior:
business/risk rejections return an outcome without inventing exchange reports
or consuming exchange request/counter state.

Only one prepared object may exist for an owner. No mutable exchange pointer
or reference may escape. Session, risk, subscription, report-consumption,
order and shutdown mutations must be queued or rejected while it is pending;
an exchange-wide generation check alone cannot undo an already durable
decision. The prepared object's lifetime must not outlive its owner or retain
a dangling reference. Define abort, exception and owner-stop behavior before
exposing the object beyond the owner implementation. Keep both request queues
bounded and define how controls and socket commands are ordered during a
prolonged preparation or future sync.

Acceptance: compare accepted and rejected submit/cancel outcomes and exact
state with the existing path across partial fills, multiple levels, capacity
failure and risk checks. Fault-inject every preparation allocation. Attempt
every mutator, nested prepare, owner destruction and shutdown during the
pending interval; verify no unsequenced mutation, output or allocator advance.
Test queue-full and disconnect behavior with deterministic synchronization.

## 4. Nonthrowing installation

Provide a private, single-use install operation callable only by the bound
owner with its valid prepared object and reservation. It must perform no
allocation, external callbacks, socket I/O, locking that can fail, or other
throwing work. Prove that state swap, allocator publication and ownership
transfer are nonthrowing with static assertions and targeted allocation-failure
tests. Explicitly define the linearization point for owner-visible state and
allocator high-water; no observer may see one published without the other.
An abandoned or rejected preparation leaves the live book, risk, ownership,
counters, reports and allocator unchanged.

Acceptance: force allocation failure immediately before install, prove install
still succeeds, compare exact continuation state and follow-on behavior, and
test double install, abort-after-install and stale-owner attempts. An inability
to establish a nonthrowing installation blocks any durable success response.

## 5. Gateway output-publication gate

Phase 7C.2A.3a adds an in-memory exclusive-gateway path; see
[its design](phase7c2a3a-publication-gate-design.md). This does not satisfy the
future durable-boundary requirement. The ordinary shared pathway remains
non-durable, and the generic prepared callback remains a trusted API outside
the supported gateway path.

Add an owner-controlled output gate for each admitted order/cancel decision.
It must retain the already bounded private reports and command-derived market
events until installation and, in a later journal milestone, a verified
durable boundary. Business/risk rejection acknowledgments require the same
decision boundary. Protocol errors, handshake and health/echo responses are
transport events and need an explicit independent policy. Subscription
acknowledgments, snapshots and feed-gap sequencing also need defined ordering.

Acceptance: deterministic two-client tests show no private report, order
acknowledgment or command-derived market update escapes before the selected
boundary; rejection, partial/full fills, slow clients, queue exhaustion and
disconnect preserve isolation and existing capacity behavior. A test-only gate
must not be presented as durable storage. No acknowledgment implies delivery
to a client or exactly-once processing.

## 6. Failure and shutdown semantics

Before any future persistent write, preparation or capacity failure aborts
without mutating live state or consuming an ID. Once later journal I/O begins,
an ambiguous failure must stop admission and cannot be called a rollback.
If a future durable decision precedes installation, a crash or failure in that
window produces an uncertain client outcome requiring 7C.2B roll-forward.
Do not acknowledge success until the durable boundary and nonthrowing install
both complete. Define a terminal outcome for queued controls and socket
commands, preserve report isolation and join the owner before destruction.

Acceptance: deterministic barriers cover stop before prepare, during
preparation, after a future persistence decision, before and after installation,
and during output publication. Verify exactly one in-process terminal result
per accepted control, bounded queues, no post-stop mutation and no blocked
waiters. Add storage fault injection only when the journal exists; process-kill
tests do not prove physical power-loss durability. Authenticated reconnect,
durable private-report replay, snapshot/journal reconciliation, cross-process
writers and distributed replication remain separate work.
