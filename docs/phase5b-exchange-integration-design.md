# Phase 5B exchange integration design

The simulator has one owner for all engine and routing state. A network request is
admitted only to bounded storage; its place in the owner thread's input order
determines command order. The client request ID is scoped to a session and is
not an authentication credential. A client order ID is scoped to the session;
internal order IDs are process-epoch-wide and are not reused. Symbol engines
have their own existing counters; exchange sequence numbers are separate and
neither kind of sequence is durable.

## Transaction boundary

Before any live book changes, validate the request and ownership, check all
counter limits, stage the proposed internal ID and command sequence, and build
the complete result against an isolated engine snapshot. Stage all ownership
changes and mandatory private reports, then verify bounded egress capacity for
every recipient, including disconnected owners. An allocation failure, engine
rejection, counter exhaustion, or insufficient capacity must discard the staged
state without changing the live engine, ownership maps, identifiers or accepted
sequence. Publication must consist only of nonthrowing swaps and writes to
preallocated slots. Cancellation follows the same rule and verifies ownership
before the staged cancellation. Replacement is unsupported.

The current implementation clones all symbol engines, order history, sessions,
pending reports and the bounded trade audit while staging submissions and
cancellations; some engine-level rejections also incur this copy. It publishes the completed state through a
nonthrowing `unique_ptr` swap. A process-wide mutex serializes assignment of
the global internal order ID across independent exchange instances; the ID is
published while that mutex is held after the state swap. Rejected commands do
not consume an internal ID. The exchange command and report counters are per
gateway, not process-wide; trade counters remain per symbol. With N resting
orders and L price levels, rebuilding book indexes can take O(N log L) time
(assuming expected constant-time hash operations), in addition to copying
retained identities, pending reports, trades and command output. Transient
storage grows with all of that state. The ID mutex is held across staging, so
independent instances can delay each other.
This is a correctness-first integration and can erase Phase 4B's latency gains;
benchmark before changing it, and do not equate the existing Phase 4 results
with network order latency.

A disconnected session's resting orders and undelivered reports remain
accounted for in bounded in-memory state; if that storage is full, commands
requiring additional reports must be rejected before commit. TCP writes cannot
prove delivery to the client application. Neither this volatile state
nor sequence numbers provide durability, replay, exactly-once delivery or
authentication. Shutdown and process failure can lose in-memory state.

Accepted trades are also staged in a bounded, in-memory audit until Gateway
destruction, available to the embedding process after stop. It is not a durable
journal and does not prove delivery. Sessions with any order history remain
allocated even when disconnected, so a long-running instance can exhaust its
session limit. No reconnect, retry or recovery interface is implemented.
On shutdown, the owner may finish the current poll iteration and dispatch its
admitted batch before it observes the stop request. It does not drain all socket
input or output. The sockets close after the owner exits; the volatile audit
remains available only while the Gateway object remains alive. An inbound frame not yet dispatched when its
connection is closed can be discarded without acceptance. Successful TCP writes
mean only that the local kernel accepted bytes; peer application delivery is not
guaranteed.

The same poll thread owns sockets and the exchange; no callback mutates an
engine concurrently. Input is processed in bounded batches, with explicit
capacity rejection. Socket output and each session's private report queue are
bounded. A slow or disconnected owner can cause subsequent matches to be rejected
before mutation. Replacement and pre-trade risk are Phase 5C or later.
