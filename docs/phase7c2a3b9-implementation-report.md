# Storage authorization prerequisite report

Part A was committed as `e6f3e42a58aef796c783c2f589ed088dcc2d6df0` and pushed to `origin/main`. The recorder uses the current private owner as the candidate source, constructs canonical evidence and the record before nonthrowing installation, and independently verifies original captured bytes on replay. It provides neither durable publication nor live recovery.

The authorization prerequisite is a test-seam-only model. The model owns an isolated candidate and preconstructed record, bounds and hashes the exact decision request, accepts only a closed set of simulated test decisions, checks identity/sequence/configuration/prefix/length/digest, and publishes only after authorization. Mismatches and ambiguity fail-stop. It is not a storage API or a durable decision. The ordinary recorder remains unchanged; a production gate cannot be claimed until owner lifecycle and real authority coordination are implemented and audited. See [the design](phase7c2a3b9-storage-authorization-design.md).

The focused regressions cover successful installation and reference-state comparison, mismatch/ambiguity fail-stop, pre-attempt abort and refusal, shutdown around a decision, wrong-thread access, attempted reuse of a prior decision, illegal repeats and admission, unresolved-owner destruction, and an allocation-failure sweep across preparation. The fault sweep includes record construction allocations but does not individually identify each allocation site. The private token cannot be extracted and replayed directly, so the reuse test exercises the exposed lifecycle instead. No live IDs, outputs, or gateway paths are involved.

## Validation

Ubuntu 24.04 WSL2: full CTest passed Debug **249/249** (2.50 s), Release **249/249** (1.66 s), and ASan/UBSan **249/249** (13.03 s). The matching CLI ran with two fills and a remainder of 30. The simulation CLI with seed 42 and five commands accepted five, produced two trades, and its Phase 7B log verified 12 records. CHROVIEW comparison passed; CHRSTATE restoration produced a 9,348-byte image and matched exact state and follow-on commands. The full suites include the gateway and protocol regressions. Test-only changes do not affect browser behavior.

## Exploratory performance

Three runs on WSL2 Linux 6.6.87.2, Intel i7-13620H (16 logical CPUs), GCC 13.3.0, `-O3 -DNDEBUG`, 20 warmups and 300 samples per run; each sample opens one isolated session from an empty state, verifies canonical equality outside timing, and uses an 832-byte request. Nanoseconds are clock observations, not network or disk latency. Preparation includes cloning, prefix sealing, record construction and request hashing; installation includes request rehash. Decision is the in-memory test-double check. Total includes those stages and minor measurement/accessor overhead. Percentiles are descriptive and the extremely short decision interval is susceptible to clock and scheduling noise.

| Run | Direct apply p50 / p95 / p99 ns | Preparation p50 / p95 / p99 ns | Decision p50 / p95 / p99 ns | Install p50 / p95 / p99 ns | Total p50 / p95 / p99 ns |
| --- | --- | --- | --- | --- | --- |
| 1 | 2,001 / 3,477 / 4,587 | 16,059 / 24,913 / 41,102 | 67 / 126 / 169 | 6,755 / 9,208 / 11,417 | 22,936 / 34,573 / 53,108 |
| 2 | 2,780 / 5,231 / 9,806 | 18,782 / 40,638 / 111,203 | 92 / 202 / 423 | 7,489 / 11,140 / 47,210 | 26,690 / 58,051 / 122,227 |
| 3 | 1,997 / 3,451 / 4,122 | 15,969 / 24,234 / 46,608 | 67 / 142 / 189 | 6,748 / 10,137 / 30,723 | 22,830 / 36,533 / 64,881 |

These are exploratory WSL measurements, not production trading performance. The second run's long tail shows environmental variation. Allocation counts and a standalone hash-only timing were not instrumented; hashing is included in preparation and installation and must not be inferred as their sole cost. Larger states, real storage and durable sync are unmeasured.

Reproduce after configuring and building the Release test-seam library:

```sh
c++ -O3 -DNDEBUG -DCHRONOS_TEST_ID_SEAM -std=c++20 -Iinclude -Itests/exchange \
  tests/exchange/authorization_benchmark.cpp build/release/libchronos_core_id_test.a \
  -pthread -o /tmp/authorization-benchmark
/tmp/authorization-benchmark
```

The benchmark has no filesystem I/O in measured operations and verifies results outside the measured interval. The prototype deliberately has no recovery after an ambiguous external decision; future storage, allocator-lease, terminal-trust, restart, and publication requirements remain open.
