# Phase 6A implementation report

## Delivered

The Next.js terminal connects to a loopback Node adapter over WebSocket. Each
browser session has separate private and public protocol-v3 TCP connections to
the C++ gateway. The adapter validates framing and JSON inputs, encodes binary
requests, decodes reports and public market data, and keeps 64-bit values exact
as decimal strings. It correlates client commands and execution reports,
rejects untrusted browser origins and enforces bounded client, frame, request,
subscription and output-buffer limits. On private disconnect, pending commands
are marked unknown and accepted orders are explicitly treated as unrecoverable.

Public snapshots establish sequence state; a gap clears the visible quote and
triggers a new subscription and snapshot. A subscription generation protects a
replacement from an old acknowledgment. The UI shows actual top-of-book quotes,
observed trades, private reports, and simulated order entry and cancellation.
No matching or risk decision is performed in the adapter. See the
[adapter design](phase6a-adapter-design.md) for the message contract and limits.

## Validation

On WSL Ubuntu 24.04 with Node 22.22.2, `npm ci` completed using the lockfile in
a WSL-native web source copy. `npm run typecheck`, `npm run lint`,
`npm run format:check`, and `npm run build` passed. `npm test` passed 9/9 tests,
including a real C++ gateway with two separate WebSocket sessions: an order
rested, a second session partially filled it, both received their own reports,
the public feed published the trade, the remainder was canceled, and the
exchange rejected an out-of-bound market order. Tests also exercised private
isolation, hostile origins, oversized messages, bounded pending commands, gap
recovery, stale subscription acknowledgments, and disconnect uncertainty.

The C++ Debug, Release and ASan/UBSan builds and CTest suites each passed
87/87 tests; C++ formatting and the CLI demo passed. The tests do not establish
production security, durable delivery, or exhaustive fault coverage.

## Environment and limitations

An npm installation on the Windows-mounted WSL filesystem stalled in the 9p
filesystem path. Installing in WSL-native storage completed; the original Git
checkout and history were preserved. Development commands and a copy-based
workaround are documented in [development](development.md). The pinned ESLint
9 dependency reports an upstream end-of-support warning; upgrading requires
checking the Next.js plugin ecosystem's peer compatibility.

The adapter is unauthenticated and must stay on loopback; an origin header can
be forged by another local process. Reports and market data are not durable,
and there is no reconnection, replay, application acknowledgment, or exactly
once guarantee. A browser disconnect can leave resting orders behind and lose
reports. A slow consumer is disconnected once the bounded output limit is
reached. Snapshots recover only current best bid/ask, not missing trades. The
gateway does not expose full depth, historical candles, positions, risk
balances, simulation controls, or system metrics to this UI. The chart is an
explicit placeholder. Phase 6B should first specify and implement any required
backend APIs and reliable session/recovery model; it must not infer these data.
