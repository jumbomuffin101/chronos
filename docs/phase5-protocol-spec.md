# Chronos loopback protocol, version 3

This simulator binds IPv4 `127.0.0.1` only. Any local process can connect;
session IDs are not credentials. There is no TLS, production authentication,
durable log, replay or exactly-once delivery. The exchange integration is
experimental and must not be used for real trading.

All multibyte integers are big endian. Signed prices use two's-complement
64-bit ticks. Frames are bounded to 4096 bytes and consist of:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Total frame length including header, 16–4096 |
| 4 | 1 | Version, exactly 3 |
| 5 | 1 | Message type |
| 6 | 2 | Flags, zero |
| 8 | 8 | Client request ID |
| 16 | variable | Payload |

| Type | Name | Payload |
|---:|---|---|
| 1 | Hello | empty; first message |
| 2 | HelloAck | session ID u64 |
| 3 / 4 | Health / HealthAck | empty |
| 5 / 6 | Echo / EchoReply | arbitrary bytes |
| 7 | Error | protocol error u16 |
| 16 | SubmitOrder | client order ID u64, symbol u32, side u8, type u8, price i64, quantity u64 |
| 17 | CancelOrder | client order ID u64 |
| 18 | OrderAck | rejected client order ID u64, result u8, zero u64 |
| 19 | Execution | fields below |
| 20 / 21 | Subscribe / Unsubscribe | symbol u32 |
| 22 | MarketAck | symbol u32, status u8 (0 success, 1 failure) |
| 23 | MarketData | fields below |

Submit payload offsets are 0 client order ID, 8 symbol, 12 side (0 buy, 1
sell), 13 type (0 limit, 1 market), 14 price, and 22 quantity. Limit prices
and quantities must be positive; market price must be zero. The execution
payload is 69 bytes: offset 0 global report sequence u64, 8 global command
sequence u64, 16 client order ID u64, 24 internal order ID u64, 32 per-symbol
trade ID u64 (zero when no trade), 40 symbol u32, 44 kind u8, 45 price i64,
53 executed quantity u64, 61 remaining quantity u64. Kinds are 0 accepted,
1 partial fill, 2 filled, 3 resting, 4 cancelled and 5 market remainder
cancelled. Each fill generates an execution for each owner at the resting
price. The header request ID on resting fills is the original submission's ID;
for cancellation it is the cancellation request ID.
For cancellation, quantity is the cancelled amount and remaining is zero.
For a market remainder cancellation, remaining is the unfilled amount.

OrderAck result values follow the exchange `Result` enum: 1 unknown session,
2 duplicate request, 3 duplicate client order, 4 not owner, 5 not found,
6 invalid order, 7 capacity, 8 counter exhausted. A request ID is reserved
only by an accepted order or cancellation, within its session; rejected IDs may
be retried. Client order IDs are unique for the lifetime of a session, including
completed orders. Internal order IDs and command/report sequences are unique
within the running gateway; engine trade IDs are unique only within a symbol.
Global sequence assignment follows dispatch order; independent TCP connections
can deliver those reports at different times and do not provide a global
delivery order.

Risk rejections also use OrderAck: 9 kill switch, 10 symbol denied, 11 quantity,
12 notional or exposure, 13 open-order limit, 14 position limit, 15 market
price bound, 16 arithmetic overflow. They do not consume the request ID or
change the book. Each connection is a distinct local development account; this
is not authentication. An embedding application sets a common default policy
when constructing the gateway and may configure a session or toggle its kill
switch through the bounded local owner-thread control interface. Configuration
changes require no open orders; the kill switch blocks submissions, permits
owner cancellations and does not cancel outstanding orders. Empty symbol
permissions mean all symbols. Market orders use a configured finite maximum
price for checked worst-case notional and reject if executable levels exceed it.
This ceiling is not a minimum sell-price floor or market-order slippage guard.
For a crossing sell limit, notional checks conservatively price the full order
at the higher of its limit and the best executable resting bid.

MarketData has a 61-byte payload: symbol u32 at offset 0, per-symbol sequence
u64 at 4, kind u8 at 12, bid price i64 at 13 and bid quantity u64 at 21, ask
price i64 at 29 and ask quantity u64 at 37, trade price i64 at 45 and trade
quantity u64 at 53. Kind 0 is snapshot, 1 is best-book update, 2 is trade,
and 3 is a gap. Missing sides use zero price and quantity. Trade events contain
only public price and quantity, never account or order identifiers. MarketData
frames use request ID zero; the subscription acknowledgment echoes its request
ID. A subscription queues a snapshot at sequence S before subsequent events
greater than S. Trades are published in execution order, followed by a best-book
event when the aggregate top changes. A symbol sequence advances for every
trade and best-book event even without subscribers; exhaustion rejects the
associated order or cancellation before commit.

Each session can subscribe to at most 32 symbols, with at most 32 queued events
per subscription. Overflow stops incremental publication for that subscription;
once previously queued events drain, an explicit gap event reports the first
missed sequence. Subscribe again to replace the old queue with a fresh snapshot;
unsubscribe removes the subscription. A disconnect removes subscriptions even
if private orders are retained. There is no durable replay or guarantee that a
gap reaches a disconnected peer. The subscription generation used internally
to protect in-flight writes is not a wire identifier.

Protocol errors 1–10 retain their Phase 5A meanings; 11 is reserved for
exchange rejection and 12 reports a full ingress batch. Invalid framing closes
the connection after a best-effort error; a pre-handshake message does the same.
Incomplete EOF is truncated. A synchronous `Client::exchange` expects the next
response to match its request; use `send` and `receive` for asynchronous order
reports. Client pending decoded frames are capped at 32.

The poll thread is also the sole owner of exchange state. It batches at most 32
commands per loop; a full batch returns error 12. Session output is capped at
8192 bytes, and exchange private reports at 128 per session. The exchange retains
up to 1024 accepted order identities and 1024 accepted request IDs per session,
and at most 8192 trade audit entries. Capacity failure rejects the entire staged
command. Report delivery removes an entry only after the kernel accepts all its
bytes; this does not prove application receipt. Disconnect leaves orders and
unsent reports in the volatile exchange, without reconnection or replay. A
session with order history remains retained and counts against the configured
session limit until shutdown. Empty disconnected sessions can be recycled.

The bounded in-memory audit retains accepted trades until the gateway is
destroyed, including when a client disconnects; it can be inspected by the
embedding process after `stop()`. There is no disk durability. Stop joins the
owner and closes sockets; queued reports are not drained, although the audit
remains available while the Gateway object lives. Idle timeout is based on I/O
progress, not a total frame deadline. Unexpected worker failure stops service.
Replacement, durable recovery and production identity remain unimplemented.

The Linux gateway enables `TCP_NODELAY` on accepted sockets by default; the
sample client leaves it disabled by default. `TransportOptions` can explicitly
override either endpoint and can enable optional diagnostic tracing. These
transport settings do not change protocol framing or delivery guarantees. See
the [latency investigation](phase5b-tcp-latency-investigation.md).

Phase 5A used version 1 and Phase 5B used version 2. Version 3 explicitly
rejects both older versions; there is no version negotiation. Version 3 adds
risk rejection codes and market-data messages while retaining the version 2
order and execution payload layouts.
