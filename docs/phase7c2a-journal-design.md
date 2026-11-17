# Phase 7C.2A transaction boundary review

Phase 7C.2A.1 adds an isolated-only wrapper and synchronous freeze scope;
the later [live gateway owner](phase7c2a1b-live-owner-design.md) now protects
the gateway instance. Neither provides asynchronous prepared transactions or
durable allocator registration. See [the earlier owner boundary design](phase7c2a1-owner-boundary-design.md).

Status: **blocked prerequisite; no durable journal or acknowledgment path is
implemented**. This review narrows the work to regression tests and an
ownership design. The proposed journal in
[phase7c2-durable-recovery-design.md](phase7c2-durable-recovery-design.md)
must not be treated as an implemented binary format or a supported guarantee.

## Existing commit boundaries

`Exchange::submit` reads session, request, order and risk state first. It then
constructs an allocator reservation, holding the process allocator mutex for a
normal live exchange. It computes the previous top of book, clones the full
exchange state and engines, and performs matching, risk position changes,
ownership updates, audit trades, report queue and market-data updates on the
clone. Capacity and counter failures discard the clone and the reservation.
Success swaps `state_` with the candidate and calls `Reservation::commit()`
while the lock remains held. The existing assertions establish that swapping
the owning pointer and destroying the state are nonthrowing; they do not
establish a durable commit, a reusable staged API or crash atomicity.

`cancel` validates the current session and order, clones the exchange and
engine, mutates the clone, then swaps it into place. It does not reserve an
order ID. Neither operation exposes its candidate or its full outcome before
the final swap. Matching's optimized commit mutates the *candidate* engine;
its own no-throw assumptions cannot make a separately published exchange
candidate safe.

Rejected exchange commands currently return before the state swap: they do not
consume an exchange request ID, increment its command/event counters, or
enqueue an exchange report. The gateway constructs rejection acknowledgments
outside the exchange after the call returns. A journal can assign an ingress
LSN to a rejection, but must preserve these separate exchange and gateway
semantics rather than inventing an exchange-side rejection report.

## Blocking ownership contract

For standalone `Exchange` instances the prototype owner-thread rule remains
a caller obligation. The gateway-specific owner added later encapsulates its
live instance and routes the methods below through its capability.
`open_session`, `configure_risk`, `set_killed`, `subscribe`,
`unsubscribe`, `market_pop`, `close_session`, and `pop` can change the same
authoritative state independently of submit/cancel. In particular, kill state
and report consumption can change without an order command or a book revision.
A candidate captured earlier could overwrite those changes if installed later.
There is no exchange-wide generation covering all mutators, exclusive pending
transaction guard, or mechanism to prevent destruction while a pending
candidate refers to its exchange. A public prepare/commit split without such
a contract would risk stale publication and partial visibility.

The shared allocator serializes normal live reservations but has no
process-wide durable-owner registration. Holding its lock across journal I/O
would block other exchange submissions, and accepting another path that
allocates without the same protocol would risk ID reuse. The current state
swap precedes the allocator high-water update; it is safe only under today's
serialized owner and allocator-lock contract, not a process-crash atomic
operation. Isolated allocators deliberately do not provide global uniqueness.
The gateway calls submit/cancel and can immediately enqueue a rejection or
drain reports; it has no durable-boundary gate.

## Required implementation decisions

Before a production journal is introduced, establish and test all of the
following together:

1. One owner capability with explicit lifetime and a single outstanding
   preparation. Route *every* authoritative mutator, including session,
   subscription and output-consumption operations, through that owner or
   prohibit it while a prepared candidate exists. A complete generation check
   must detect all changes and fail before persistence; a check after syncing
   cannot safely roll back a durable command.
2. A process-wide allocator coordination protocol covering all live exchange
   instances and allocation paths, including exclusivity, lock order, ID
   exhaustion, exception handling and lifetime. Preserve private allocator
   isolation. Prove global uniqueness while a reservation remains pending.
3. A restricted prepared object retaining the candidate, canonical command,
   complete outcome and all output capacity. Specify how accepted and rejected
   commands preserve their existing exchange/gateway behavior. An install after
   durable publication must allocate nothing, call no external code and have
   a verified nonthrowing path for state and allocator publication.
4. A gateway owner protocol preventing acknowledgments and report delivery
   before the manifest's durable boundary. Any ambiguous storage failure must
   fail-stop admission; a durable but uninstalled command has an uncertain
   outcome and requires later roll-forward, not a rollback claim.
5. Freeze a bounded format only after the canonical outcome contains all
   information required to verify the transaction, including multi-party
   reports, trades, counters and resulting state. The existing Phase 7B log is
   best-effort and flushes streams; it is not a durable journal. CHRSTATE is an
   isolated, history-backed restorer, not a live publication mechanism.

The proposed write/sync/manifest/rename/directory-sync order and filesystem
assumptions remain in the Phase 7C.2 design. No format constants, inspector,
POSIX storage writer, fault-injection backend or recovery behavior are
implemented by this milestone. Do not configure the gateway as durable.
