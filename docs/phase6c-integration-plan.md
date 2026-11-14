# Phase 6C integration plan

Phase 6C should improve the existing local simulation without assuming the C++
exchange exposes data or recovery features it does not yet have.

## Priorities

1. **Browser reliability:** establish repeatable end-to-end tests that start an
   isolated gateway and adapter, exercise two browser sessions, validate
   private reports and public sequences, and always clean up. Include disconnect,
   gap, slow-consumer and restart scenarios with bounded timeouts.
2. **Developer startup:** add a local, documented launcher with dependency and
   port checks, explicit process ownership, signal handling and cleanup. Support
   WSL-native web dependencies without silently overwriting either checkout.
3. **Visual polish:** improve dense tables, readable typography and chart
   interaction using actual session data. Capture reproducible desktop and
   mobile screenshots from a running gateway; avoid invented data.
4. **Errors and reconnection:** distinguish browser, adapter, private exchange
   and public-feed failures. Make new-session behavior explicit, retain unknown
   outcomes, and never silently resubmit a command. A new session is not
   recovery of the old one.
5. **Accessibility and responsive layout:** verify keyboard navigation, focus,
   screen-reader announcements, contrast, zoom and overflow across desktop,
   tablet and mobile sizes. Automate practical checks without replacing review.
6. **Real-data demonstration:** document a reproducible two-session order,
   partial fill, cancellation and risk rejection, with expected sequence and
   private-report behavior. Clearly label local simulation.
7. **Documentation and demo:** keep startup, test and troubleshooting commands
   current; add dated screenshots, environment details and a short demo script.

## Backend contracts required before expanding the dashboard

- **Full depth:** versioned, bounded level snapshots and ordered updates with
  per-symbol sequencing and an explicit gap/resnapshot contract.
- **Accounts:** authorized account and open-order snapshots, stable identities,
  pagination or limits, and a consistent snapshot-to-events boundary.
- **Positions and P&L:** authoritative position, cost and valuation definitions,
  units and overflow behavior, including the source and time of marks.
- **History:** durable, timestamped trades or candles with interval, retention,
  time-zone and pagination semantics; identify gaps and corrections.
- **Simulation controls:** authorized start/stop/status commands with lifecycle,
  concurrency, failure and ownership rules, isolated from live trading.
- **Performance metrics:** measured counters and latency distributions with
  sampling method, units, timestamps and collection overhead.
- **Identity and recovery:** authenticated sessions, authorization, durable
  event storage, explicit acknowledgments, idempotency and replay/reconciliation
  rules. Define crash, duplicate and uncertain-command behavior before claiming
  durable or exactly-once delivery.

These are planning items only. No new matching, risk, market-data, identity or
recovery implementation is included in the Phase 6B review.
