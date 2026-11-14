# Phase 6 proposal: local simulated trading terminal

This document is a design, not an implementation. Chronos remains a C++
simulation. The browser never speaks the exchange's raw TCP protocol and does
not make matching or risk decisions. The Phase 5C service is loopback-only,
volatile and unauthenticated; this design does not claim that it is suitable
for real trading, public hosting or multiple untrusted users.

## Boundary and deployment

```text
Browser: React + TypeScript + Tailwind
  | HTTPS/HTTP for page and commands; WebSocket for live owned values
  v
Next.js app / local reverse proxy ----> long-lived Node.js adapter
                                         | bounded protocol-v3 TCP connections
                                         v
                                  Chronos C++ gateway (loopback)
```

Run the Next.js application and a separate, long-lived Node.js adapter in local
development; a local reverse proxy can present the WebSocket and command API
under the same origin. The adapter owns TCP sockets, frame parsing, request
correlation and fan-out. It cannot run in the browser or assume a request-scoped
serverless function will keep sockets alive. Keeping it separate avoids making
a custom Next.js server a prerequisite; Next.js documents its standard Node.js
deployment and the tradeoffs of a [custom server](https://nextjs.org/docs/app/guides/custom-server).
All processes bind to loopback by default. A development launcher should start
only explicitly configured executables and ports, display their status and shut
them down cleanly. It must not accept arbitrary shell commands.

The present gateway maps **each TCP session to a distinct local account**. A
private trading session must therefore not be pooled across unrelated browser
identities or replaced transparently on reconnect. There is no production login,
account recovery or reattachment to a disconnected session. The first version
should support one explicitly local operator and clearly label its account as
ephemeral. Enforce loopback binding, WebSocket origin checks, bounded messages
and a local session/CSRF mechanism at the adapter boundary. Those measures are
development controls, not production authentication. Do not expose the service
outside the local machine without a separately designed identity, authorization,
transport-security and operational model.

## Current data and missing capabilities

| Terminal feature | Current source | Required work / honest UI behavior |
|---|---|---|
| Best bid, ask, aggregate quantity and spread | Protocol v3 snapshot and best-book events | Available after snapshot; compute spread only when both sides exist. |
| Market depth beyond best | No network depth feed or depth snapshot | Show best level only; disable the depth ladder beyond it until a sequenced depth API exists. |
| Recent public trades | Live public trade events | Show trades observed during this adapter session; no historical trade query or replay exists. |
| Price and volume charts | Live trades contain price and quantity | Aggregate only observed trades using adapter receipt time, labelled as such. Exchange timestamps, historical candles and authoritative volume need new APIs. A gap invalidates continuity. |
| Simulated limit/market orders and cancel | Protocol v3 order, cancel and private reports | Forward with unique session-scoped IDs and surface exchange rejections. Replace is unsupported. |
| Orders and private fills | Private reports for this TCP session | Build a bounded session-local view from reports; there is no complete order query or reconnect recovery. |
| Risk limits, positions and exposure | In-process C++ owner state and local administration | No protocol-v3 account-state query exists. Show unavailable until an authorized, consistent query API is added; do not present a client-derived balance as authoritative. |
| Simulation start/stop | No remotely controlled simulation service | Disable controls until an explicit, allowlisted supervisor and status API exist. Never execute browser-provided commands. |
| Performance metrics | Optional local trace hooks and offline benchmarks | Show unavailable until real live counters/histograms with units and collection scope are exposed. Adapter-observed RTT may be displayed separately with its definition. |

Existing market events have no exchange timestamp or durable history. Do not
invent historical candles, depth, balances, risk headroom or latency numbers.
If chart bars are built from live observations, label their clock and coverage,
bound their memory, and mark a feed gap or adapter restart as a discontinuity.
The adapter may use [Lightweight Charts](https://tradingview.github.io/lightweight-charts/docs)
for line/candlestick and histogram series; follow its license and attribution
requirements and use incremental updates for streaming data.

## UI composition

- `TerminalShell` supplies responsive layout, navigation, symbol selector and a
  persistent simulation-only banner. `ConnectionStatus` shows connecting,
  live, stale, resynchronizing and disconnected states with last-update time.
- `BestQuote` presents bid, ask, quantities and spread. `BookPanel` renders
  only supported levels; a future depth ladder must identify its snapshot and
  update source. Never visually imply a full book from top-of-book data.
- `TradeTape` shows observed public trades, price, quantity and adapter receipt
  time. `PriceVolumeChart` shows explicitly labelled observed price and volume;
  a gap or unknown historical range is visible rather than interpolated.
- `OrderTicket` accepts symbol, side, limit or market type, integer price ticks
  and quantity, and offers submit/cancel for simulated orders. Show validation,
  pending, accepted, rejected, partial, filled, cancelled and **unknown** states.
  Disable actions while disconnected or when account/session state is ambiguous.
- `OrdersAndExecutions` consumes only this session's private reports.
  `RiskAndPositions` and `SimulationControls` show unavailable until the required
  authoritative APIs exist. `MetricsPanel` displays only actual measurements,
  with clock, units, sample window and collection point.

Use semantic controls, keyboard navigation, visible focus, labelled errors,
sufficient contrast and non-colour indicators for buy/sell and connection
state. Support a compact laptop layout without hiding order confirmation or
risk errors. Avoid animations that obscure updates. No fictitious market data is
displayed when the backend is absent.

## Adapter contracts

These are proposed adapter interfaces, not existing Chronos endpoints. Version
and validate every browser message. Encode all 64-bit IDs, sequences, prices
and quantities as decimal strings in JSON; JavaScript `number` cannot safely
represent every protocol value. Validate ranges before converting to `BigInt`
and serialize binary protocol fields explicitly in network byte order.

| Direction | Proposed message | Essential fields / behavior |
|---|---|---|
| Browser → adapter | `subscribe` / `unsubscribe` | `symbol`, request correlation; adapter manages v3 subscription and snapshot. |
| Adapter → browser | `market.snapshot` | `symbol`, `sequence`, best bid/ask price and quantity or null, adapter receipt time. |
| Adapter → browser | `market.book` / `market.trade` | `symbol`, `sequence`, public book or trade values and adapter receipt time. |
| Adapter → browser | `market.gap` / `market.status` | `symbol`, expected/received sequence when known, stale/resynchronizing status and explicit reason. |
| Browser → adapter | `order.submit` / `order.cancel` | Client request ID, session-scoped client order ID, symbol, side, type, ticks and quantity where applicable. |
| Adapter → browser | `order.rejected` / `order.report` | Correlation IDs, exchange result or private execution fields, command and event sequences. |
| Adapter → browser | `connection.status` / `error` | Adapter and exchange connection state, retryability and a non-sensitive explanation. |
| Adapter → browser | `metrics.sample` | Only measured metrics with units, sampling interval and source; absent when instrumentation is unavailable. |

Future messages, unavailable until the listed backend work is implemented,
would include `account.snapshot` (account identity, revision, positions,
reservations and limits), `market.depth.snapshot` / `market.depth.update`
(symbol, sequence and bounded levels), and `simulation.status` /
`simulation.control` (allowlisted simulation ID, desired action and resulting
state). The adapter must report capability absence explicitly rather than
fabricating responses for these message types.

An HTTP command endpoint may wrap the same adapter command contract if useful;
it must not create an independent TCP account per request. WebSocket messages
are not durable acknowledgments. Bounded per-browser output queues must have an
explicit overflow policy: mark the affected feed stale and request a new
snapshot, or close the client if a private report cannot be delivered. Do not
silently discard private reports or promise recovery that the exchange lacks.

## State, sequencing and failure behavior

The adapter decodes complete length-prefixed frames incrementally, enforces the
4096-byte limit, rejects invalid version/type/length and preserves partial writes.
It accepts only protocol version 3 and fails visibly on incompatibility. The
adapter and browser each keep a small state store keyed by symbol and session;
React components subscribe to selectors rather than mutating the source state.
Coalesce render work where safe but never coalesce away required sequence or
gap information. Bound trade history, candle buffers and output queues.

For each symbol, install snapshot sequence S before accepting increments.
Apply only the next sequence; on an explicit gap, unexpected sequence,
subscription failure or adapter queue overflow, mark that symbol stale, stop
using its increments and resubscribe for a fresh snapshot. A replacement
snapshot is a new synchronization boundary. Sequence numbers are per symbol,
not a global ordering across symbols. A new snapshot restores the current best
quote only; it does not reconstruct missed trades, candles or private reports.
Subscriber disconnect or exchange restart loses volatile state. Never label a
partially observed chart as complete.

Track each order request by its client request and client order IDs; consume
the correlated private reports in order. A socket failure after sending creates
an **unknown outcome**, not an automatic rejection. Do not blindly retry or
generate a new ID: reconnection is a new exchange session and exactly-once
recovery is unavailable. Disable trading until a user starts a clearly new
session; surface that old resting orders may still exist. A future design needs
durable account identity, order lookup and reconciliation before safe retry.

## Required backend and operations work

Before enabling the corresponding panels, add and independently test:

1. Sequenced multi-level depth snapshots and increments, with bounded recovery.
2. Authorized owner-thread queries for account limits, positions, reservations,
   open orders and their consistency with private reports; account identity and
   reconnect/reconciliation semantics must be specified first.
3. Exchange timestamps, bounded history or durable replay for trades and bars,
   plus instrument metadata defining tick size and units.
4. Explicit simulation lifecycle/status interfaces and an allowlisted local
   supervisor; the current CLI is a finite demonstration, not such a service.
5. Live, opt-in metrics exports with defined units, collection overhead and
   windowing for queue depth, rejections, throughput and latency. Adapter RTT
   must not be labelled engine latency.
6. Production-grade authentication, authorization, TLS, durable event delivery,
   audit/recovery and threat/operations design before any non-local deployment.

## Verification plan

Test the adapter's fragmented/coalesced v3 frames, byte order and 64-bit bounds;
unknown versions and malformed messages; bounded queues and slow WebSocket
clients; per-symbol snapshot/update ordering, duplicates, gaps and resnapshot;
and private/public isolation. Exercise disconnects before and after acceptance,
unknown outcomes, duplicate IDs, restart and lack of replay. Use an integration
test with two simulated trading sessions and a separate subscriber against the
real local C++ gateway. Test accessible keyboard flows, loading/error/stale
states, narrow layouts and truthful empty states. Measure adapter RTT and
rendering with named clocks and distributions; do not extrapolate WSL2 or
loopback results to production.
