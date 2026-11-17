# Phase 7C.2A.2a: process allocation registration

This is an in-memory exclusivity mechanism, not durable allocation, a prepared
transaction, journal, recovery or durable acknowledgment. `AllocationMode`
selects ordinary shared or explicitly exclusive live allocation. A private
process registry issues move-only RAII leases to live `Exchange` instances.
The registry itself is accessible only to `Exchange`; constructing another
`detail` allocator or an empty lease cannot grant access to the process
allocator or its registry.

Every ordinary live exchange acquires a shared lease before constructing its
state. Multiple shared leases may coexist. An exclusive lease is granted only
if there are no shared or exclusive leases; while it exists, new ordinary and
exclusive live exchanges fail with `std::runtime_error`. An invalid mode fails
with `std::invalid_argument`. The rule deliberately covers the entire live
exchange lifetime, even if it is idle or has never submitted an order. A
failed registration does not touch the order-ID counter. If subsequent state
construction throws, the lease is released by stack unwinding. An isolated
exchange has no process lease and retains its private allocator; it cannot be
adopted by the live owner or converted to an exclusive live exchange.
The existing `SimulationRunner` defaults to shared allocation, so a default
run in the same process cannot start while an exclusive owner is registered;
callers requiring concurrent offline work must explicitly select its isolated
mode. Default event-log replay and CHRSTATE reconstruction use isolated
allocation, while explicit shared-process replay is rejected during exclusivity.

The process registry is initialized on first use and outlives registered
exchange objects. The registration member is destroyed after exchange state.
The existing process ID allocator remains the sole live counter. Its
reservation mutex is held across exchange cloning, matching, state swap and
counter commit; rejected commands and allocation failures do not publish the
reserved ID. Counter exhaustion is checked without wrap. Registration does
not expose a free-standing reservation or alter that commit sequence.

## Gateway and lifetime

`LiveExchangeOwner` accepts an explicit `AllocationMode` at construction;
`Gateway` passes it as its final optional constructor argument. The default is
`Shared`, preserving existing protocol and startup behavior. Exclusive
registration happens during gateway construction before the owner thread
starts; conflicts throw rather than falling back. It does not switch on a
journal or change any protocol-v3 response. The added C++ constructor
parameter preserves source calls through its default but changes the constructor
signature; existing separately compiled C++ clients must be rebuilt.

For an exclusive owner, `finish()` first publishes a nonmutating finishing
phase, then releases the lease, then publishes its terminal phase. There is no
callback or asynchronous operation in that interval; all mutation entry points
reject both finishing and terminal phases. A failed
bind releases the lease; destruction also releases it if no successful finish
occurred. The gateway joins its owner and resolves controls under the existing
shutdown rules. As with the existing API, destruction must not race with
member calls. A stopped ordinary owner keeps its shared lease until destruction;
this conservative rule avoids treating a still-live ordinary exchange as an
exclusive participant.

## Locking and limits

The registry mutex protects only shared/exclusive lease counts and is released
before state allocation, matching or allocator reservation. The allocator's
existing mutex protects its high-water value and reservation through the
transaction. There is no nested registry-to-allocator lock in current command
processing. An ordinary live exchange with an outstanding reservation already
holds its shared lifetime lease, so an exclusive attempt fails at registration
without waiting for the reservation or acquiring the allocator mutex.

Gateway stop uses stop mutex then control-queue mutex; owner control dispatch
uses control-queue mutex then individual-control mutex. Registry release occurs
on the owner after its event loop, without holding these gateway mutexes.
No cross-process registry or persistent high-water exists. The explicit mode
does not make current state swaps and allocator advancement crash-atomic.
The process-wide guarantee assumes one linked instance of `chronos_core` owns
the process allocator and registry. Independently loaded modules each carrying
their own statically linked copy would have separate singletons and are not a
supported deployment; a shared allocation service or enforced single module
is required before such a deployment can claim global uniqueness.

## Future transaction work

A future owner-only prepared transaction must retain both a valid exclusive
lease and an unpublished allocator reservation, prevent all alternate mutation
while pending, stage complete outcomes, and prove a single-use nonthrowing
install/allocator publication boundary. This milestone exposes no such API:
existing reservations are scoped to `Exchange::submit`. Long-lived reservation
ownership, future sync/fail-stop behavior, durable publication gating and
restart reconciliation remain separate work.
