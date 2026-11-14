# Phase 6B terminal design

The local terminal is a compact dark trading workspace. Navigation shows the
symbol, local-simulation label, exchange and feed status. The overview and book
use only the exchange's best bid and ask; empty sides and an unavailable spread
are displayed as dashes. The main chart plots session-observed trade price and
volume, with no synthetic history. Recent trades show local observation time,
quantity and feed sequence. A simulated order ticket and session-observed order
and execution table complete the layout. Desktop places the ticket beside the
market; narrow layouts stack panels and allow the order table to scroll.

`app/page.tsx` owns the WebSocket lifecycle and presentation. The pure reducer
in `app/terminal-state.ts` tracks separate private/public connection states,
selected symbol, sequenced market snapshot, bounded observed trades, submitted
orders and reports. `app/price-chart.tsx` renders the observed series with
Lightweight Charts. The server adapter owns independent private and public TCP
connections per browser session and remains the only browser-facing bridge to
the C++ gateway. No browser state decides matching or exchange risk.

The adapter resubscribes after a gap. The reducer additionally refuses
out-of-order or duplicate incremental messages and hides stale quotes. Values
remain decimal strings and spread is computed using `bigint`; charts omit
points that cannot be represented safely as JavaScript integers. Chart buckets
use the latest price and summed volume per observed UTC second. The trade list
uses the browser's local time. Trade history is bounded at 200, reports at 100,
and session orders at 1024. Order submissions are never retried automatically.
On connection loss, active orders are marked uncertain; a user can explicitly
start a new session, which cannot recover the old one.
Phase 6C exposes browser, exchange and feed connection status separately and
retains a visible prior-session uncertainty warning across a new session. A
disconnected feed cannot be revived by a delayed snapshot. The order table is
keyboard-focusable and announces horizontal scrolling on narrow screens.

Full depth, historical candles, account balance and position queries, P&L,
simulation controls and live system metrics are not supplied by the current
protocol. The UI does not synthesize them. The adapter is loopback-only and its
origin check is not authentication; this is not a production trading system.
