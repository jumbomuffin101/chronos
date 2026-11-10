# Architecture

Phase 1 contains only data models and a demonstration CLI. The components below describe planned boundaries, not completed services.

- **Core and matching engine:** C++20 domain types and, later, deterministic single-threaded processing of sequenced events. Matching owns order lifecycle decisions.
- **Limit order book:** owns per-instrument resting orders and price-time priority; it will be used by the engine, independently of networking.
- **TCP gateway:** will parse and validate protocol messages and pass requests to the engine. It must not own matching logic.
- **Market-data publisher:** will turn engine events into outbound updates, separate from order-entry transport and matching.
- **Risk validation:** will enforce pre-trade limits before requests reach matching; policy and account state are future work.
- **Python clients:** will submit simulated requests and run strategies or simulations through public interfaces.
- **React/Next.js dashboard:** will display simulated market and account state through service interfaces.
- **Event logging and replay:** will record ordered events and enable deterministic reconstruction; persistence and recovery are not yet implemented.
- **Benchmarking:** will measure realistic workloads and latency after correctness is established, informing profiling and optimization.

The intended flow is client → gateway → risk → matching and book → events → market data and eventual log. Exact protocols, threading, storage, and failure handling remain design work.
