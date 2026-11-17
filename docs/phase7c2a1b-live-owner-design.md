# Phase 7C.2A.1B: live gateway owner

The gateway's existing `poll()` thread is the sole live exchange owner. The
gateway constructs a fresh `LiveExchangeOwner` before starting that thread;
the loop binds it once before processing sessions or commands. The owner stores
its `Exchange` privately and exposes no pointer, reference, conversion or
constructor accepting an existing exchange. Its noncopyable, nonmovable
capability checks a retained per-thread context token, so a reused OS thread ID
cannot acquire authority. A failed bind cannot be retried. The token may
allocate on a thread's first check; subsequent checks do not take an additional
mutex. Callers must not destroy the owner while a call is in progress.

The gateway's separate public APIs submit bounded control requests or exchange
protocol frames; they do not expose the capability. All live `open_session`,
`close_session`, `configure_risk`, `set_killed`, `submit`, `cancel`,
`subscribe`, `unsubscribe`, `market_pop` and `pop` operations pass through it.
Private reports, market events and audit trades are returned as copies. While
active, reads require the owner context; after the owner has stopped, immutable
copies can be inspected by other contexts. The stopped phase is published with
release/acquire synchronization after the loop's last exchange access. A
gateway must be joined before destruction, as provided by its existing stop
and destructor path. The public standalone `Exchange` API remains mutable and
caller-serialized, but it has no reference to the gateway's encapsulated live
instance. `OfflineOwner`, simulation, replay and isolated restoration retain
their prior paths and private allocator behavior.

The full mutation inventory remains in
[the earlier owner design](phase7c2a1-owner-boundary-design.md). Internal
matching, risk positions, ownership, feed and private-report state are changed
only by the corresponding exchange method; the wrapper does not duplicate
business logic or change order-ID allocation. The owner offers a synchronous
freeze scope for regression testing: all its mutators and nested scopes reject
while frozen, including session, risk, subscription and output consumption.
This is not a prepared transaction and is not used to hold state across I/O.

## Dispatch and shutdown

The owner loop processes a bounded administrative batch, polls sockets,
accepts sessions, decodes frames, drains a bounded order/subscription ingress
batch, and removes dead sessions. Admin controls and frame ingress are separate
queues, not one global FIFO. Within an ingress batch, frames retain insertion
order as observed by the loop; simultaneous clients have the order imposed by
poll/session iteration and decoder delivery, not a wall-clock ordering promise.
Each ingress batch holds at most 32 commands and excess receives
`IngressFull`; socket outputs and per-session reports are bounded. Up to 32
controls can wait in the shared queue, in addition to a detached batch of up
to 32 being processed; full or stopping control admission returns false.
Control start and stop request are serialized under the control mutex. If stop
wins, a detached but unstarted control fails without execution; a control
marked started first completes normally. Controls timed out before starting are cancelled;
once started, the caller waits for their outcome. The loop uses a 25 ms poll
timeout and no additional worker or busy wait.

Lock ordering is `stop_mutex` then `controls_mutex` for a stop request, and
`controls_mutex` then the individual control mutex when deciding whether a
detached command may start. The waiting requester does not hold the control
queue mutex, and the shutdown drain releases it before locking individual
controls. Control trace callbacks run outside the queue and individual-control
locks; the stop-request trace runs while `stop_mutex` is held. Trace callbacks
must not invoke blocking gateway operations.

A disconnect detected before dispatch causes that session's queued ingress
entries to be skipped. Orders and undelivered private reports can remain in
exchange state according to the existing disconnected-session policy. A stop
request ends the loop after the current iteration; work already being
processed may complete. The owner then becomes terminal, rejects future
mutations, marks the gateway stopped under the controls mutex, and fails and
notifies pending controls. Unprocessed ingress and socket output are not
durably retained or acknowledged; clients can have uncertain outcomes. The
gateway joins the owner before destruction. This does not provide application
acknowledgments for reports or durable disconnect recovery.
Concurrent external `stop()` calls are serialized so that only one caller
joins the owner thread; subsequent calls observe the completed shutdown.
Calling `stop()` from an owner-thread trace callback is unsupported: trace
callbacks must not call blocking gateway operations.

## Publication and preparation boundary

Handshake, health/echo, malformed-frame, ingress-capacity and transport errors
are protocol/transport responses. Order business/risk rejections become
`OrderAck` responses; accepted commands produce private execution reports,
and trades/book changes generate public market data. Socket writes and their
successful completion trigger report or market-event consumption. These
operations are currently non-durable. A future durable gate must hold order
outcomes, private reports and command-derived market data until both durable
publication and nonthrowing state installation; subscription acknowledgment
and initial snapshot sequencing need a separate policy. No such gate exists.

Synchronous owner serialization and the freeze test show that wrapper
mutators cannot run reentrantly in a frozen scope. Future asynchronous
preparation must additionally prevent returning to dispatch while a candidate
is pending, bound queued work and backpressure during sync, and prove safe
owner lifetime, allocator reservation and nonthrowing installation. The public
freeze callback must not destroy its owner. Gateway tracing callbacks can run
on several threads and must remain nonblocking; they are not given the owner
or an exchange reference. Durable allocator registration, cross-process
exclusivity, prepared candidates, journal, recovery and delivery guarantees are
not implemented.

The optional trace sink also receives control-queued, batch-taken, control-started,
control-completed and stop-requested events. These may be emitted from control,
stop or owner contexts; callbacks must be thread-safe, nonblocking and
`noexcept`. Enqueue and completion observations can race across threads and
are not journal or durability boundaries. Tests deliberately use a blocking
trace callback as a scheduling seam; production callbacks must not do so.
