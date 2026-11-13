# Development

On Ubuntu or WSL2 Ubuntu, install `build-essential`, `cmake`, `git`, and `clang-format` (CMake 3.20 or newer). Run the configure, build, and CTest commands in the [README](../README.md). CMake FetchContent fetches GoogleTest v1.14.0 when `BUILD_TESTING` is enabled (the default); the first configure needs network access. To build only the CLI without fetching tests, configure separately with `-DBUILD_TESTING=OFF`. Google Benchmark v1.8.3 is fetched only when `CHRONOS_ENABLE_BENCHMARKS=ON`; see the [benchmark methodology](phase4-benchmark-methodology.md) for its targets. For disconnected builds, CMake's `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` and `FETCHCONTENT_SOURCE_DIR_GOOGLEBENCHMARK` can point to local checkouts of those versions.

Run `bash scripts/format.sh --check` to verify formatting or `bash scripts/format.sh --fix` to apply it. The sanitizer preset enables ASan and UBSan for project executables on supported Linux GCC/Clang toolchains. Debug and Release use separate build directories.

Identifiers are distinct explicit types. Prices are signed integer ticks, not currency units; instrument-specific tick sizes are not modeled yet. Order construction throws `std::invalid_argument` for zero quantity, invalid side or type, nonpositive limit price, or nonzero market price. The matching engine rejects duplicate active IDs and assigns its own command, event and trade counters. Trade is a passive record; the engine generates executions.

Sequence numbers and quantities currently share an unsigned 64-bit representation, so the compiler does not distinguish them; identifiers are distinct types. Callers must reject negative external quantities before conversion to `Quantity`, because an unsigned parameter cannot detect a value that has already wrapped. The book checks aggregate overflow and matching checks its counters. The Phase 1 `Order` itself remains immutable; book snapshots expose current resting quantity. Tick-size definitions, durable ID allocation and recovery remain future work.

On Linux, the loopback gateway uses protocol version 3. Run the gateway tests
with `ctest --preset debug --output-on-failure`; they exercise separate order
clients and a market-data subscriber. Risk defaults can be supplied to the
`Gateway` constructor, and local embedding code can configure a session or
toggle its kill switch through the owner-thread control interface. The local
session/account policy is not authentication. See the [risk and feed design](phase5c-risk-marketdata-design.md).

For the optional Phase 5C exchange benchmark, configure Release with
`-DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON`, build target
`chronos_phase5c_baseline`, and run `./build/release/chronos_phase5c_baseline`.
Its samples exclude fixture construction and compare zero with four subscribers;
they are not end-to-end network or production hardware measurements.
