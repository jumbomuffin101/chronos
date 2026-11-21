# Phase 7C.2A.2b: owner-held prepared scopes

Status: in-memory experimental owner API. No gateway command uses this path,
and no journal, durability, recovery or durable acknowledgment is provided.
The gateway's ordinary submit/cancel and protocol-v3 publication are unchanged.

## Authority and lifecycle

Only a bound `LiveExchangeOwner` with an active exclusive allocation lease
can enter `with_prepared_submit` or `with_prepared_cancel`. The owner enters
its existing synchronous freeze scope before preparation and leaves it only
after installation or abort and cleanup. All owner mutators, nested prepared
scopes and `finish()` reject while frozen; other contexts cannot access the
owner. The decision callback is synchronous, must not destroy the owner, and
must not retain references into the call. It receives only the preliminary
`Result`, never a staged exchange, report, market event or reserved ID. It
returns true to install an accepted command and false to abort. A rejected
command is never installed even if the callback returns true.

The returned `PreparedScopeResult` contains the preliminary result and an
independent `installed` flag. **Accepted with `installed == false` is an
aborted proposal, not an authoritative command or acknowledgment.** A
callback exception unwinds the candidate, reservation and freeze scope, and
leaves the live exchange unchanged. The prepared object is private to the
exchange and cannot escape the synchronous owner scope, so a caller cannot
double-install, install on another exchange, or retain a stale prepared handle.
The callback can run for an arbitrary duration; callers must bound it. This
is not a queue or asynchronous shutdown protocol. The callback is trusted
owner code: it must not send an acknowledgment or publish the preliminary
result. The scope cannot prevent arbitrary caller I/O, so it is not itself a
network output gate.

## Staging and allocation

Preparation reuses `Exchange::submit` and `cancel`, including the matching,
risk, ownership, sequence, report-capacity and market-data code. It first
clones the current exchange state into a private isolated exchange. Existing
command processing then stages its own state clone inside that private exchange.
On acceptance, the resulting state is retained privately; otherwise no state
is installable. The state contains sessions, risk limits and positions,
requests, order ownership, matching engines/books, audit trades, counters,
private reports, subscriptions, queued market events and feed sequences.
All candidate allocations and output-capacity checks occur before installation.
The double clone increases work and peak memory; no optimization is attempted.

Submit preparation reserves the next **real process allocator ID** under the
existing allocator mutex before building the candidate. Its private temporary
allocator is seeded to generate that exact reserved ID; an accepted candidate
is checked against the reservation. This is not promotion of an independently
generated private ID. An exhausted reservation seeds the temporary allocator
at the maximum so normal validation and exhaustion ordering are preserved.
Cancellation does not allocate a new ID. A discarded candidate or failed
preparation destroys the reservation without advancing the process high-water.
The allocator mutex remains held throughout submit preparation and the
decision callback. The exclusive lifetime lease prevents another shared live
exchange from allocating in that interval. Isolated exchanges remain separate.
Holding the mutex across a long callback blocks other users of that allocator;
no filesystem synchronization is performed here.

## Installation proof and output ordering

The private `Exchange::install_prepared` is reached only within the frozen
scope for an accepted candidate. It performs a `unique_ptr<State>` swap and,
for submit, the already-held `Reservation::commit()` assignment. Compile-time
checks verify the state-pointer swap and reservation commit are nonthrowing;
the function performs no heap allocation, I/O, callbacks, queue operation or
arithmetic. Its caller has already checked acceptance and candidate identity.
The owner is frozen and the allocator reservation is held until after the
swap and high-water update, so no supported observer can see a partial live
publication. Old state destruction and reservation release occur during
subsequent scope cleanup, not inside the install function.

Private reports and public events stay in the private candidate until the
swap; the callback can inspect only the pre-existing live state. They become
visible through ordinary owner reads after installation. No network output is
sent by the scoped API. The normal gateway still publishes its non-durable
results after its existing synchronous exchange call. A future journal must
place its durable decision before this installation and explicitly gate
business acknowledgments, private reports and command-derived market data;
transport errors and handshake responses require a separate policy.
The gateway's existing dispatch currently calls ordinary submit/cancel, queues
business rejections in `OrderAck`, and drains execution and market-data queues
afterward. Integrating prepared scopes there requires an owner-controlled gate
that withholds those outputs until installation and a future durable decision;
this milestone deliberately leaves that dispatch unchanged.

An active exclusive owner can capture bounded exact continuation words for
diagnostic comparison outside a frozen scope. This acquires the process
allocator mutex without committing a reservation and includes its current
high-water value. Capture is rejected while frozen to avoid reentering a held
reservation; it is not a snapshot publication or recovery API.

## Boundaries for future work

### Proposed bounded canonical decision

The following is a design requirement, not an implemented serialization or
journal format. An owner-controlled decision should contain a schema version;
command kind; session, request and client-order identity; and the complete
validated command fields. It should record acceptance or the precise rejection
reason, and the reserved internal ID only where applicable. Accepted decisions
must include ordered trades, private reports with destination sessions, public
market-data effects, and the before/after command, trade, report and feed
counters needed to check their sequencing. Rejections need their specified
sequence effects and must not invent an allocated ID. The future implementation
must define one authoritative order for every output, including ties among
destinations, instead of deriving it from unordered container iteration.

Use fixed-width fields with explicitly specified signedness and byte order, and
checked length and count arithmetic before allocation or encoding. Independent
hard limits must be specified for command fields, total encoded bytes, trades,
reports and market events; existing configurable capacities alone do not set
a universal serialization bound. Oversize and allocation failure must abort
before installation. The private decision and reservation remain confined to
the owner and publication gate; only the installed outcome may be externally
published. The existing callback is trusted arbitrary application code: it can
call outside code or publish its preliminary result, and exceptions cannot undo
external side effects. This API alone does not enforce a publication gate.

There is no persistent prepared handle, no stale-install retry, no exposed
standalone reservation and no live restored-exchange publication. Shutdown
cannot finish reentrantly inside the frozen scope; the caller must return or
throw before the owner can stop. The API does not itself coordinate gateway
ingress, control queues, slow clients or filesystem failures. The registration
guarantee assumes one linked `chronos_core` instance in one process. Physical
durability, cross-process exclusivity, authenticated sessions and exactly-once
delivery are not claimed. An eventual asynchronous or durable implementation
must separately prove pending-object lifetime, bounded backpressure, failure
semantics and the durable publication gate.
