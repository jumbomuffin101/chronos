# Phase 6B implementation report

The terminal now presents best bid/ask, exact spread and sequence, an observed
session price and volume chart, a top-of-book view, bounded public trades,
simulated order entry, and session-observed private order reports with
cancellation. Risk rejections are displayed using exchange rejection codes.
Observed timestamps are local browser observations, not exchange timestamps.
No historical or depth data is invented. See the [UI design](phase6b-ui-design.md)
and [development instructions](development.md).

The real-gateway browser exercise used two independent sessions: a sell limit
order for five at 100 rested; a market buy for three filled at 100; the seller
saw two remaining, the buyer saw a complete fill, and both saw the trade and
updated ask. The seller canceled the remainder. An ask at 1,000,001 followed
by a market buy produced the exchange's market-price-bound risk rejection.
The test order was canceled afterward. Desktop at 1440 × 900 and mobile at
375 × 812 were inspected; the page did not overflow horizontally. This was a
local development browser check, not a production security or load test.

Validation uses Node 22.22.2 in a WSL-native web copy because npm installation
on the Windows-mounted filesystem stalled. Run `npm ci`, `npm run typecheck`,
`npm run lint`, `npm run format:check`, `npm test`, and `npm run build` there.
Set `CHRONOS_TEST_GATEWAY` to the Debug gateway executable for the integration
test if the web copy is outside the repository. See the README for the three
local startup commands. The npm installer reports the upstream ESLint 9
end-of-support warning.

The final clean install, TypeScript check, ESLint, Prettier and Next.js
production build passed. The Node test suite passed 18/18: nine adapter tests,
eight state tests and one server-rendered component test. These include the real
gateway test, disconnect uncertainty, market gaps, integer precision, risk
rejection and a fill racing a rejected cancel. The final review also protected
confirmed fills from late cancellation errors, ignored unsolicited stale
snapshots, and corrected the disconnected feed status and new-session control.
No C++ source was changed in
Phase 6B; the preceding Phase 6A validation passed 87/87 C++ tests in each of
Debug, Release and ASan/UBSan configurations.

The final visual review inspected 1440 × 900 and 1920 × 1080 desktop,
768 × 1024 tablet, and 375 × 812 mobile viewports against the running exchange.
No page-level horizontal overflow was observed. Labels below 10 pixels were
increased to improve readability. Screenshots were inspected in the browser
review tool but not saved as repository files.

Reconnect creates a new session; previous resting orders and reports are not
recoverable. There is no durable delivery, replay, historical data, full depth,
account snapshot, or production authentication. For Phase 6C, specify and
implement authoritative account/order snapshots, depth and replay contracts,
secure session identity, and measured telemetry before adding corresponding
views. Do not imply exactly-once delivery without a durable protocol design.
