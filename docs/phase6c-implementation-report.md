# Phase 6C implementation report

Chronos remains a local simulated exchange. The new
`node scripts/chronos-dev.mjs` launcher checks dependencies and port availability,
starts the C++ gateway, Node adapter and Next.js dashboard in order, polls actual
readiness after each owned service announces startup, and terminates its own
child process groups on shutdown, including a group whose leader has exited. The
`CHRONOS_WEB_DIR` override supports a WSL-native web copy without moving or
overwriting the repository. See [development](development.md) for setup,
overrides and browser-test commands.

The terminal distinguishes browser unavailability and disconnect from private
exchange and public feed status. Active orders become uncertain on connection
loss, a persistent warning explains that prior-session orders may remain, and a
new session never retries or recovers old commands. Obsolete callbacks are
detached during session replacement; a delayed snapshot cannot revive a
disconnected feed. Regression tests cover these states. The UI adds a
keyboard-focusable scrolling order table, mobile touch targets, improved footer
contrast and non-colour status text. No market, account or chart data is mocked.

## Validation

On WSL2 Ubuntu 24.04 with Node 22.22.2, clean `npm ci`, TypeScript, ESLint,
Prettier, the Next.js production build and the CLI demonstration passed. The web
unit/integration suite passed **20/20**. The real Chromium end-to-end suite
passed **3/3**: occupied-port handling, cleanup after an owned process leader
fails, and a real isolated two-browser trade,
partial fill, private-report isolation, public book/trade updates, remainder
cancellation, market-risk rejection and shutdown with an uncertain resting
order. It checks released ports, responsive overflow, a keyboard-scrollable
order table, and an axe WCAG 2 A/AA scan in the populated session.

C++ Debug, Release and ASan/UBSan builds and CTest suites each passed **87/87**;
the C++ formatting check passed. `npm ci` reports the upstream ESLint 9
end-of-support warning. Axe is not a substitute for manual assistive-technology
testing; dedicated screen-reader tooling was unavailable. Keyboard Tab navigation
and the order table's horizontal keyboard scrolling were checked. The browser
automation surface did not apply zoom shortcuts, so an actual 200% zoom could not
be verified; a 640 CSS-pixel reflow check passed without page-level overflow, but
is not equivalent to a browser-zoom audit.

## Screenshots

The browser suite produced these full-page captures at the stated viewport
sizes, using actual simulated exchange events. They show a five-unit resting
sell after a three-unit buy filled at 100, leaving two units:

- [Desktop 1440 × 900](assets/dashboard/desktop-1440.png)
- [Desktop 1920 × 1080](assets/dashboard/desktop-1920.png)
- [Tablet 768 × 1024](assets/dashboard/tablet-768.png)
- [Mobile 375 × 812](assets/dashboard/mobile-375.png)

## Limitations and Phase 7

The service is unauthenticated and loopback-only. Reports are volatile; there
is no durable reconnect, replay, or exactly-once delivery. Full depth, historical
candles, authoritative account/position/P&L queries, simulation control APIs,
production identity and live system metrics are not implemented. Chart points
outside JavaScript's exact integer range are omitted. The demo uses local
observation timestamps, not exchange timestamps. Phase 7 should first define
durable event ordering, persistence, crash recovery and replay semantics and test
them with fault injection before exposing recovery claims in the dashboard.
