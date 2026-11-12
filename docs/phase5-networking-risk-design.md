# Phase 5 proposal: networking and pre-trade risk

This document is a design proposal. Phase 5A implements only a loopback handshake/health/echo gateway as specified in [the protocol](phase5-protocol-spec.md); risk, order routing and market data remain unimplemented. The Phase 4 engine is single-threaded, in-memory and per symbol. Its book-maintenance methods are not exchange-level cancel/replace operations and its command/event counters are per engine, not globally ordered across symbols. Phase 5 must not treat them as a durable log or as globally unique identifiers.

## MVP boundary and interfaces

```text
TCP client -> framed decoder -> authenticated session -> command queue
                                                   -> sequencer / risk / engine owner
TCP client <- bounded response queue <- reports <--'             |
read-only market-data subscribers <- bounded feed <- normalized events
```

The MVP is a single-process simulator bound to loopback by default, with explicit limits, a versioned protocol, deterministic sequencer, basic account-level risk, and observable rejection/failure behavior. One owner thread serializes risk state and engine mutations; network I/O may run separately but must communicate through bounded queues with owned message values. No network callback mutates an engine directly. Multi-symbol scheduling must define one command order on this owner even though each engine retains local counters. The engine remains independently testable without sockets.

Proposed logical interfaces, not APIs promised by current code:

- `FrameDecoder::consume(bytes)` emits validated frames or a protocol error, handling arbitrary TCP fragmentation and coalescing.
- `SessionManager::accept/close` owns per-connection state, identity, bounded input/output and connection lifecycle.
- `Sequencer::enqueue(command)` assigns a globally monotonic ingress position at a defined acceptance boundary and dispatches on the owner thread.
- `Risk::prepare(command, account_state)` produces an explicit accept/reject and staged reservation; commit/release applies consistently with engine outcome.
- `EngineAdapter::submit/cancel/replace` routes by symbol, maps session order identities to internal IDs and produces normalized results.
- `Publisher::publish(events)` fan-outs immutable, ordered value events to response and market-data queues.

## Wire protocol and sessions

Use a length-prefixed binary frame with a fixed-width header: protocol version, message type, flags, total length and client request ID. Specify one byte order (network/big endian), fixed-width signed integer price ticks and unsigned quantity/IDs; never serialize C++ struct memory, padding or host endianness. Set a small explicit maximum frame length and reject unknown versions, types, invalid enum values, malformed lengths, trailing data and out-of-range values before enqueue. Define exact byte layout, minimum lengths, response error codes and version negotiation in a separate protocol specification before implementation. Unknown fields are not silently accepted unless the version defines them. Fragmentation, concatenation, partial writes and end-of-stream mid-frame are normal TCP cases to handle explicitly.

Each session transitions through connected, authenticated, active, closing and closed states with defined timeouts and cleanup. An MVP authentication mechanism and account mapping must be explicitly selected before enabling non-loopback access; a connection ID alone is not authentication. For a local-only MVP, bind to loopback and configure a local account/session policy. Rate and connection limits prevent a single session from exhausting resources. Disconnect handling must choose and document whether outstanding orders remain or are cancelled; do not assume socket closure is a trading cancellation. Reconnection and duplicate request IDs need defined idempotency behavior before retries are supported.

The client supplies a session-scoped client order ID and request ID. The gateway maps `(account/session identity, client order ID)` to an engine `OrderId` allocated from a globally unique internal namespace across sessions and symbols, with checked exhaustion and explicit lifetime/reuse rules. Reject duplicates according to a specified scope; never trust a client-supplied internal ID or use the socket address as identity. If persistence/restart is absent, uniqueness is only within one process epoch and that limitation must be visible; durable IDs require an epoch and recovery policy.

## Commands, acknowledgments and risk

MVP commands are new limit/market order and cancellation; add replace only when its atomic semantics are implemented. A syntactically valid frame is not an accepted order: define separate receipt/validation, risk rejection and engine acceptance outcomes. Reports must include client request/order correlation, internal ID where allocated, symbol, status, executed and remaining quantities, price ticks where applicable, and sequencer/event identifiers. Route resting-order fills to the owning session/account as well as incoming-order reports; do not leak other accounts' private reports. A rejected command must not fabricate a trade or consume an engine accepted-command sequence.

Risk validation precedes engine mutation and includes account enablement, symbol permissions, positive bounded quantity, instrument price/tick range, maximum order notional using checked wide arithmetic, and position/exposure/open-order limits. Define market-order worst-case notional and no-price behavior explicitly; reject when a safe bound cannot be established. Maintain reservations for resting quantities, adjust them for fills and release them for cancellation or unfilled market remainder. Validate both sides' account state and define self-trade policy. Test exact boundaries and arithmetic overflow, not floating-point approximations.

Risk and engine must have a coordinated transaction boundary: reserve/stage all risk updates and required in-memory outputs before engine commit; after engine commit, applying risk state must be nonthrowing or use an explicit rollback/recovery protocol. Do not acknowledge a command if risk and book disagree. The current engine returns in-memory outputs but has no reservation transaction, durable outbox, or rollback after successful commit; the MVP must add and test the necessary adapter/engine contract before risk-backed order entry is considered complete. Durable publication is a further boundary and cannot be claimed by an in-memory queue.

Cancellation must verify order ownership and release reservations only after a successful engine change. Replace must define whether unchanged-price reductions retain priority and increases or price changes lose it, and must be atomic if validation/risk or reinsertion fails. The existing `modify_resting` can create a crossed book and does not match or emit exchange-level reports: it is **not** an implementation of exchange replace. Design a sequenced cancel/replace or transactional replace that handles crossing, priority, reservations and failure without exposing an intermediate state; until then reject replace as unsupported rather than emulate it unsafely.

## Market data, backpressure and sequencing

Publish only public normalized book/trade events, with symbol and feed sequence, separate from private execution reports. MVP may deliver best bid/ask and trades plus a snapshot mechanism. Define whether updates are level deltas or full snapshots and their ordering relative to trades before writing clients. Do not call existing mutable book containers from network threads; use value messages generated by the engine owner.

Bound all queues and frame sizes. When ingress fills, stop reading or reject before accepting a command; never drop an already accepted command silently. Reserve capacity for the corresponding private reports before engine commit, or stop accepting work until capacity is available. A slow private client must receive a defined disconnect/recovery response; if reports cannot be retained across disconnect, explicitly state that recovery is unavailable. Market-data subscribers may be disconnected on overflow and required to resnapshot; never silently drop a delta while claiming a continuous feed. Control idle, partial-frame and write timeouts, and define shutdown/drain behavior. Avoid blocking the engine owner on a slow socket.

The sequencer orders admitted commands deterministically; engine command and event counters are local per symbol and are not substitutes for a global feed sequence. Define how rejected commands are represented in the ingress stream and when a sequence is consumed. Future logging must atomically coordinate committed engine/risk state with durable ordered events, including crashes between commit, persistence and client acknowledgment. Phase 5 MVP without durable storage cannot guarantee exactly-once delivery, replay after restart, or recovery of outstanding orders. Idempotency across reconnect requires a persistent or explicitly bounded deduplication policy.

## MVP test plan

- Frame decoder: split at every byte, multiple frames per read, short writes, malformed and oversized lengths, unknown types/version, invalid signed values, disconnect mid-frame and bounded memory.
- Session lifecycle: authentication failure, request/ID collisions across clients and symbols, ownership checks, disconnect policy, reconnect behavior and sequence exhaustion.
- Risk: every limit boundary and overflow, market worst-case bounds, reservation/fill/cancel reconciliation, rejection without counter/book changes, and injected allocation failures at transaction boundaries.
- Engine integration: matching and per-party report ordering, duplicate IDs, partial/full fills, cancellation and unsupported replacement; compare deterministic command streams with direct engine execution.
- Backpressure: saturated ingress and egress, slow/disconnected clients, report capacity reservation, bounded queues, shutdown with pending commands and market-data gap/resnapshot behavior.
- End-to-end: loopback client sends fragmented frames and receives decoded acks/trades in order; verify engine, account state and feed agree after each command. Include deterministic replay within one process and fault injection before and after each in-memory commit boundary.

## Optional advanced work

TLS and production authentication/authorization, durable journal and restart replay, high availability, multi-process gateways, multi-threaded sharding, multicast or incremental recovery feeds, sophisticated portfolio risk, batching and performance tuning are follow-on work. They require independent threat, operational and consistency designs; none should be implied by the MVP.
