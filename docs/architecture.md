# Architecture

The models, demonstration CLI, single-threaded resting limit order book, matching engine, loopback-only simulated order gateway, basic local account risk and public market data are implemented. Durable recovery, production identity and other services remain planned.

- **Core and matching engine:** C++20 domain types and deterministic single-threaded processing of submitted orders. Matching decides executions and produces in-memory reports.
- **Limit order book:** owns per-instrument resting orders and price-time priority, supports checked fills, mutations and value snapshots, and is independent of networking. It does not decide matches.
- **TCP gateway and exchange:** parse bounded versioned frames and route simulated orders on one owner thread. Exchange transactions stage state and private reports before publication; delivery is volatile and subject to bounded capacity. See the [integration design](phase5b-exchange-integration-design.md).
- **Market-data publisher:** stages bounded per-symbol snapshots, trade and top-of-book events separately from private execution reports, with gap and resnapshot behavior.
- **Risk validation:** checks local session-account limits before acceptance and stages positions with the book; open-order reservations are derived from book-owned orders. It is not production authentication or portfolio risk.
- **Python clients:** will submit simulated requests and run strategies or simulations through public interfaces.
- **React/Next.js dashboard:** will display simulated market and account state through service interfaces.
- **Event logging and replay:** will record ordered events and enable deterministic reconstruction; persistence and recovery are not yet implemented.
- **Benchmarking:** will measure realistic workloads and latency after correctness is established, informing profiling and optimization.

The intended flow is client → gateway → risk → matching and book → events → market data and eventual log. Exact protocols, threading, storage, and failure handling remain design work.
