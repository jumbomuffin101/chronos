# Phase 7C.2A.3a: in-memory gateway publication boundary

The explicitly exclusive gateway dispatches submit and cancel on its existing
single owner thread through `submit_prepared_non_durable` and
`cancel_prepared_non_durable`. These owner methods pass an internal,
unconditional in-memory authorization to the existing prepared scope; they
expose no caller-supplied decision callback, staged state, or reserved ID.
Preparation clones and runs the same matching, risk, ownership, counters and
report code as ordinary exchange operations. An accepted candidate is installed
once before the call returns; rejected candidates and exceptions are discarded.
The exclusive process allocator reservation remains held during preparation
and is committed only by installation. The existing callback-based prepared
API remains available to trusted in-process callers, but is not used by gateway
dispatch and cannot guarantee that callers avoid external side effects.

This is an owner-controlled ordering boundary for the supported exclusive
gateway path, not a language-level guarantee about unrelated global references
or an interface for journal authorization. The intended future order is prepare,
durable decision, install, publish. This milestone uses an immediate non-durable
authorization and does not provide a persistent decision. Adding a journal
requires a new controlled authorization mechanism, bounded canonical outcome,
I/O failure policy and a proof of owner and allocator lifetimes.

## Output classification and sequencing

Handshake, health, echo, framing, malformed protocol and ingress-full errors
are transport or pre-admission responses, not transaction decisions. Subscribe
and unsubscribe control responses and snapshots use the existing owner-thread
path; they are not prepared trade commands. A submit/cancel rejection produces
an `OrderAck` after the prepared call returns. Accepted commands expose their
private execution reports and command-derived public book/trade events only
after candidate installation, when the event loop later drains the installed
exchange queues. Gateway trace callbacks for transaction completion also run
after the prepared call returns. No socket write or gateway output queue is
passed to the authorization function. The production gateway contains no
pre-install test hook.

Exchange business and risk rejections do not commit request identity, command
or event counters, reports, market events or an ID in the current implementation.
The gateway returns the existing protocol-v3 rejection status; retrying an
otherwise valid request is therefore possible. Protocol validation failures
can occur before exchange admission. These semantics are in-memory and do not
create durable ingress numbers. Exact chronological delivery across different
client sockets is not promised.

## Capacity, disconnect and shutdown

Before preparation, the exclusive gateway pre-encodes a possible 33-byte
rejection and reserves its slot in the session's bounded 8,192-byte output
buffer. If insufficient room exists or allocation fails, the session is closed
without installing the command. The candidate itself checks the existing
128-report per-session limit before installation. Subscription queues hold at
most 32 events per symbol; overflow marks a feed gap rather than reserving
unlimited data. An accepted candidate contains the already-created private
reports and bounded public events, or the gap indication. The socket buffer is
not reserved for all recipients: reports remain in exchange state until their
frames are written; market data follows the existing gap policy. A later socket
allocation, write failure, disconnect or shutdown can prevent receipt without
undoing an installed exchange command. A completed TCP write is not a client
application acknowledgment. Disconnected owners and pending private reports
follow the existing in-memory session retention rules; no reconnect or replay
is provided.

The ingress batch and administrative queues each hold at most 32 entries. The
owner completes an already-dispatched synchronous prepared call before its
terminal `finish`; once stopped, no prepared command can install. An admitted
command from a session marked dead before dispatch is skipped. Stop and socket
failure do not roll back installed state. Queueing a response after installation
is not durable publication, and pending socket data may be lost on shutdown.

If an in-memory authorization refuses an otherwise accepted candidate, the
gateway closes the originating session without emitting an authoritative
response; the candidate and its ID reservation are discarded. Production's
current non-durable authorizer always permits installation. The refusal path is
exercised through a separately linked test-only seam.

The special pre-install barrier used by the deterministic gateway test exists
only in a separately linked test gateway; the ordinary gateway library is built
without it. The test pauses after successful preparation, checks that the trader
and subscriber sockets have no corresponding output, then permits installation
and verifies private reports and the public book event.

## Remaining work before journal I/O

The scope has no durable authorization, persisted outcomes, manifest, restart
or authenticated sessions. It does not gate non-command subscription changes,
generic prepared callbacks or the ordinary shared gateway. A journal design
must specify rejection ordering, reserve all required publication resources,
handle ambiguous I/O by fail-stop, prevent early external outcomes on every
supported path, and independently verify crash recovery. Gateway tests now
inject failures before preparation, at output reservation and after preparation,
exercise full and exactly fitting output buffers, abort submission and
cancellation, and synchronize disconnect and concurrent stop at the
pre-install boundary. These bounded tests are not exhaustive allocator or
kernel fault injection. Slow subscribers and post-install output failures need
additional stress testing.
