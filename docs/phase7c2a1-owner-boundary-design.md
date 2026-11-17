# Phase 7C.2A.1: exchange owner boundary

Status: **restricted offline prerequisite**. A subsequent
[live gateway owner](phase7c2a1b-live-owner-design.md) encapsulates the
gateway's exchange; standalone `Exchange` objects remain caller-serialized.
`OfflineOwner` owns a newly constructed isolated exchange and never returns a
mutable exchange pointer or reference. Existing `Exchange` remains public and
is used directly by the simulator, replay and restorer. Neither boundary
implements a durable transaction or acknowledgment.

## State mutation inventory

| Entry point | Authoritative effects |
| --- | --- |
| Constructors / `isolated` | Allocate state, initialize limits, allocator and initial feed sequence. |
| `open_session` | Insert session and initial risk, request, report and subscription state. |
| `configure_risk` / `set_killed` | Replace risk limits or kill flag; risk configuration clones state. |
| `subscribe` / `unsubscribe` | Change subscription generation, event queue and snapshot state through a state clone. |
| `market_pop` | Consume a public event or mark a gap delivered, subject to generation checks. |
| `close_session` | Clear subscriptions and possibly erase the session; orders or reports retain it. |
| `submit` | Stage matching engine/book, request and order history, positions, counters, audit trades, private reports, public events and allocator ID; swap candidate on success. |
| `cancel` | Stage book, request/order history, counters, reports and public book event; swap on success. |
| `pop` | Consume a private report. |

Internal `State::report`, `update_position`, `publish`, the matching engine's
book mutation and the allocator's reservation commit are indirect mutation
paths reached by the public operations above. `State::clone`, matching clones,
replay and restoration construct private candidates; they do not grant
permission to replace a live gateway. `front`, `market_front`, `find`,
`position`, `trade`, `trade_count` and verification views return values or
read-only copies but still require serialization with mutations. A completed
isolated `SimulationRunner` currently exposes a borrowed mutable `Exchange*`;
that path is not covered by `OfflineOwner`.

The gateway network loop serializes its own `exchange_` calls. Cross-thread
admin controls enter a bounded queue and are processed by that loop; ingress
and output queues have their own bounds. The public standalone `Exchange`
interface relies on its caller for serialization, and the gateway constructs
its exchange before starting its network thread. A global construction-thread
check would therefore break valid gateway ownership transfer and would not
remove the direct mutator bypasses.

## Implemented isolated boundary

`OfflineOwner` retains a per-thread context token created at construction and
creates its own isolated exchange. Retaining the token prevents a later thread
from gaining access through reuse of an operating-system thread ID. Creating a
thread's token can allocate; a failed context lookup denies access. It is
noncopyable and nonmovable. Its only exposed state access is
through value-returning methods, and every wrapper mutation checks the owner
context first. Wrong-context calls return `OwnerAccess::WrongContext` without
touching the underlying state; reads are similarly confined. A denied
value-returning call carries an empty result, so it cannot be mistaken for an
accepted command. The caller must
keep the owner alive for every call and must not concurrently destroy it.
This context check is combined with encapsulation of the owned exchange; it is
not an enforcement mechanism for any separate or existing `Exchange` object.

`with_frozen_state` is a synchronous, non-escaping scope used to exercise
exclusion. While its callback runs, every exposed owner mutator returns
`Frozen`, including session, risk, subscriptions, report consumption and
order commands. Nested scopes are rejected. Owner-context reads remain
available as copies. The flag is cleared even when the callback throws. No
prepared candidate, allocator reservation, install operation, queue or
cross-thread dispatcher is provided. Callbacks must not destroy the owner.
In particular, this scope must not be used as evidence that live gateway
transactions can be suspended over filesystem I/O.

Direct standalone callers, simulation, replay and CHRSTATE use their existing
interfaces and semantics. The wrapper deliberately cannot accept an existing
exchange or promote its private IDs into the shared process namespace. It
does not alter exchange command/event counters or introduce an LSN.

## Sequencing and future integration

A complete owner would accept bounded requests from other contexts, assign a
deterministic dequeue order and exclusively process session creation/removal,
risk and kill updates, commands, subscription changes, event/report delivery
and report consumption. Only a successful send currently triggers report or
market-event removal; socket buffering and parsing are delivery/transport
operations, whereas the corresponding exchange `pop` is a mutation. Shutdown
must stop admission, resolve or explicitly fail queued requests and join the
owner before its state or capabilities can be destroyed. Current gateway
controls and order ingress are separate queues; a future contract must define
their combined order rather than assume one global FIFO.

Future prepare must retain exclusive authority from validation through
candidate construction, allocator reservation, persistence, installation and
output publication. Other mutations must be queued or explicitly rejected
while this scope is held; long syncs require bounded backpressure and a
fail-stop policy. A stale-generation check after journal synchronization is
insufficient. A move-only pending object must have a safe exchange lifetime,
single install/abort semantics and proven nonthrowing, allocation-free install.
No such object is exposed here.

The process allocator's mutex serializes normal live reservations through
state swap and high-water commit. Future durable registration must cover all
live constructors and reservations, exclude conflicting writers, preserve
exhaustion checks, and define an owner-before-allocator lock order and
startup/shutdown release; a private isolated ID cannot become a live ID. This
requires separate review in 7C.2A.2 and is not implemented.

The gateway's handshake, protocol errors, framing errors and ingress-full
responses are transport-level. Order business/risk rejections, accepted
command outcomes and private execution reports must await the future durable
decision; market-data publications caused by the command must also await
installation. Subscribe/unsubscribe acknowledgments and initial snapshots
need their own sequencing rules. Socket errors and disconnects are not durable
command acknowledgments. The existing `OrderAck`, execution and market-data
queues are unchanged and provide no durability gate or durable delivery.
