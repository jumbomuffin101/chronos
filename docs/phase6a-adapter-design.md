# Phase 6A local adapter design

The C++ gateway remains the source of matching, risk, private reports and public
market data. A long-lived Node.js process translates protocol-v3 TCP frames to
a browser WebSocket; the browser does not open a TCP exchange connection. The
adapter never implements matching, position accounting or risk decisions.

## Local session and transport

The adapter binds IPv4 `127.0.0.1` and accepts WebSocket upgrades only at `/ws`,
from loopback peers with the exact configured browser Origin. It permits at most
eight browser connections, limits incoming WebSocket messages to 8192 bytes,
outgoing buffering to 65536 bytes and disables compression. This is a local demo
boundary, not production authentication; another local process can impersonate
the configured origin. Do not expose it remotely. Each browser WebSocket owns
two independent gateway TCP sessions: one private trading session and one
public market-data session. No private report is broadcast to another browser.
The current gateway assigns an account per TCP connection; neither account
identity nor its outstanding orders can be reattached after reconnect.

The adapter uses a fixed 4096-byte streaming frame buffer, checks lengths,
version, flags, message types and payload sizes, and handles TCP fragmentation
and concatenation. It uses Node's ordered socket write buffering with a 65536-byte
cap: a `write()` return of false means Node accepted the data and it must not be
resent. Request IDs are monotonically allocated per connection, checked for
overflow and correlated through a bounded 256-entry pending map. IDs, price and
quantity use `bigint` for binary conversion and decimal strings in JSON; no
64-bit field is converted to a JavaScript number. Symbols use checked uint32
numbers. Health requests keep the gateway's inactivity timeout from closing an
otherwise idle connection.

No automatic reconnect or order retry is attempted. If a private connection
closes with unresolved commands, the adapter reports `order.unknown`; a separate
`session.uncertain` warning explains that previously accepted resting orders
may remain. If the browser connection itself is lost, those messages cannot be
delivered; the UI must treat the old session as unrecoverable. The adapter
terminates a slow WebSocket when its bounded output capacity is exceeded.

## WebSocket contract

`web/adapter/messages.ts` defines the TypeScript contract. JSON messages from
the browser are `subscribe` / `unsubscribe` with a uint32 `symbol`,
`order.submit` with a unique session-local `browserId`, decimal-string
`clientOrderId`, `price`, `quantity`, symbol, side and order type, or
`order.cancel` with `browserId` and `clientOrderId`. The server validates all
fields and limits browser correlation IDs to 1024 per socket. It never accepts a
browser-supplied exchange session ID as identity.

Outgoing messages include `connection.status`, `connection.error`, `market.status`,
`market.snapshot`, `market.book`, `market.trade`, `market.gap`, `order.report`,
`order.rejected`, `order.unknown`, `session.uncertain`, `protocol.error` and
`command.error`. Execution kinds and risk rejection codes retain the numeric
values documented in [protocol v3](phase5-protocol-spec.md). A submitted order
is not considered accepted until the exchange reports it. WebSocket and TCP
delivery are not durable acknowledgments.
Locally rejected order commands include their valid `browserId` when available.

Per symbol, the adapter waits for a successful subscription acknowledgment and
snapshot before declaring the feed live. It checks that each later book or
trade sequence is exactly the prior sequence plus one. A gap event or unexpected
sequence marks the feed stale and triggers resubscription; updates are ignored
until a replacement acknowledged snapshot arrives. This restores current best
bid/ask only, not missed trades or history. Disconnect clears feed state and
does not silently resubscribe on a new exchange session.
Subscription generations prevent a delayed acknowledgment from activating a
newer subscription for the same symbol.

## Scope

The terminal displays the current best bid/ask and observed live trades, and
can submit and cancel simulated orders. It does not show invented level-2 depth,
historical candles, account balances, authoritative risk snapshots, simulation
controls or live exchange metrics. Those backend capabilities remain absent.
The Phase 6B terminal plots only session-observed trades using
`lightweight-charts`, with attribution; no historical candles are implied.
