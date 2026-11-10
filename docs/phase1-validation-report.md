# Phase 1 validation report

## Executive summary

On 2026-11-10, the Phase 1 foundation was reviewed and built in WSL2 Ubuntu 24.04.1. Debug, Release, and ASan/UBSan configurations passed all eight discovered GoogleTest cases. The CLI ran, formatting passed, boundary coverage was expanded, and a Phase 2 book design was written. No matching, networking, or book implementation was added.

## Build results

GCC/G++ 13.3.0, CMake 3.28.3, Ninja 1.11.1, clang-format 18.1.3, Git 2.43.0, and GDB 15.1 were available; installation was unnecessary. GCC compiled the project with C++20 and the configured strict warnings. Debug, Release, and sanitizer configure and build commands succeeded. Release initially reported a Make clock-skew warning on the Windows-mounted workspace; a subsequent build and test run completed without the warning. GoogleTest fetched successfully; its checkout matched the pinned v1.14.0 commit. The sanitizer test executable was confirmed to link `libasan` and `libubsan` and compiled with both instrumentation flags.

## Test results

CTest discovered and passed **8 of 8** tests in each of Debug, Release, and sanitizer builds: **24 executions, 0 failures** for the three-configuration matrix. Release was repeated after the clock warning and Debug was repeated during the final audit; each passed another **8 of 8**, for **40 executions, 0 failures** across these recorded runs. Debug and Release CLI runs printed the version and a valid sample order. Formatting check passed. These results are not proof of absence of defects.

## Code quality findings

The formatter corrected existing layout inconsistencies. Added tests exercise minimum positive price and quantity, maximum price and quantity, minimum signed price rejection, and compile-time identifier/sequence separation. No compiler warnings or sanitizer findings appeared. The initial clock warning was a build-tool timestamp warning, not a compiler diagnostic, and did not recur on the repeat run. `clang-tidy` was not installed and was not run.

Order IDs, trade IDs and symbol IDs are distinct explicit wrappers; sequence numbers remain plain unsigned integers and are not distinct from quantities. Prices use exact signed ticks, limit prices must be positive, and market price zero is a sentinel. Unsigned quantity inputs cannot detect a negative value already converted by a caller. The constructor rejects zero and initializes remaining quantity equal to original; no status or remaining-quantity mutation is exposed, so invalid relationships and statuses are not constructible via this API. The passive trade record does not validate executions. The constructor performs no arithmetic that can overflow; future aggregate and sequence arithmetic requires explicit overflow checks. Normal valid construction uses no dynamic allocation, while exception handling for invalid inputs may allocate.

## Architecture review

The small header-only `chronos_core` interface target is appropriate for the current value models. Networking and matching are absent, and documentation clearly marks them as planned. The CI workflow uses Ubuntu 24.04, installs the needed build tools, and runs the same Debug configure, build and CTest flow locally documented. Debug, Release and sanitizer presets use separate directories; a Release test preset was added for consistency.

## Phase 2 readiness

The foundation is ready for **design and implementation work** on a limit order book, subject to deciding its lifecycle API, policies and invariants. See [Phase 2 design](phase2-orderbook-design.md), which compares ordered maps with a dense tick ladder and recommends ordered maps plus FIFO lists and an ID index for the initial implementation. This is a proposal, not validated implementation or a performance claim.

## Remaining issues

- `clang-tidy` was unavailable. Install it and review findings when available.
- Inputs from future protocols must validate signed quantities before conversion, and future mutations must preserve quantity/status invariants and check arithmetic overflow.
- Sequence type safety, ID uniqueness, trade validation, order lifecycle, sequence wraparound and event failure semantics need explicit decisions as those APIs are developed.
- If filesystem clock warnings recur, investigate host/WSL clock and filesystem timestamps or build on the Linux filesystem.

## Changes made

- `tests/core/test_order.cpp`: boundary cases, direct standard-library include, and a compile-time type-safety check.
- `CMakePresets.json`: Release test preset.
- `docs/development.md`: signed-input, type-safety and overflow limitations.
- `docs/phase2-orderbook-design.md`: proposed responsibilities, data structures, complexity, invariants and tests.
- `docs/phase1-validation-report.md`: this record.
- C++ files under `include/`, `apps/`, and `tests/`: formatting according to `.clang-format`; no model semantics changed during the audit.

## Reproduction commands

From the repository root in WSL Ubuntu, with the tools above installed and network access for the pinned dependency:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
./build/debug/chronos_cli
bash scripts/format.sh --check
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel
ctest --preset sanitizers --output-on-failure
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
./build/release/chronos_cli
```
