# Chronos

Chronos is a C++20 electronic exchange and trading **simulator**. It does not execute real financial transactions. Its loopback-only development gateway routes simulated limit and market orders to the in-memory matching engine, applies local account risk controls, returns private execution reports and publishes public best-book and trade data.

The planned system separates a deterministic matching core and limit order book from TCP order entry, market-data publishing, risk checks, event logging and replay. Python clients and a React/Next.js dashboard will interact with the eventual gateway. See [architecture](docs/architecture.md) and [supported orders](docs/supported-orders.md).

## Status and roadmap

1. **Foundation:** C++ models, CLI demonstration, CMake, tests, and CI.
2. **Complete:** in-memory resting limit order book.
3. **Complete:** deterministic matching and in-memory execution reports.
4. **Complete:** benchmark baseline and initial transaction optimization; further profiling remains planned.
5. **Current:** loopback order routing, basic in-memory account risk and public market data; durable recovery and production identity remain planned.

The stack is C++20, CMake 3.20+, GoogleTest, optional Google Benchmark, and GitHub Actions. Linux with GCC or Clang is the target; Windows development uses WSL2 Ubuntu. See the [matching design](docs/phase3-matching-design.md) for API and limitations.

## Build and run

Install a C++20 compiler, CMake 3.20+, Make, Git, and network access to fetch the version-pinned test dependency. From the repository root:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
./build/debug/chronos_cli
```

On Linux/WSL, run `./build/debug/chronos_gateway 8088` in one terminal and `./build/debug/chronos_gateway_client 8088` in another for the handshake and echo demo; stop the server with Ctrl+C. Port `0` requests an ephemeral port and prints the selected value. The [protocol specification](docs/phase5-protocol-spec.md) describes version 3 order entry, risk rejections and public feed messages; the [risk and feed design](docs/phase5c-risk-marketdata-design.md) describes policy and transaction limits. The service has no authentication and is intended only for local development.

For an optimized build use `cmake --preset release` and `cmake --build --preset release`. For AddressSanitizer and UndefinedBehaviorSanitizer use `cmake --preset sanitizers`, `cmake --build --preset sanitizers`, and `ctest --preset sanitizers` on a supported Linux GCC/Clang toolchain. See [development](docs/development.md) for formatting and dependency details.

Optional performance benchmarks and their limitations are described in the [benchmark methodology](docs/phase4-benchmark-methodology.md); the measured WSL2 baseline is in the [report](docs/phase4-benchmark-report.md). The [Phase 4B design](docs/phase4-optimization-design.md) explains the planned transaction path and retained clone reference.

## Repository structure

```text
apps/exchange_cli/    Demonstration executable
include/chronos/core/ Foundation data models
include/chronos/orderbook/ and src/orderbook/  Resting order book
include/chronos/matching/ and src/matching/    Matching engine
tests/core/           GoogleTest tests
benchmarks/           Future benchmark guidance
docs/                 Architecture, development, and decisions
scripts/              Developer utilities
.github/workflows/    CI
```

Subsystem source directories are created when implementation begins; empty directories are not tracked by Git.
