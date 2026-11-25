# Canonical restricted replay verification: implementation report

The restricted replay milestone was committed and pushed as `16dca8be3a679dab7b77e560e998e5cbc2f78321`. This milestone adds an isolated-only canonical state encoder and a bounded in-memory replay envelope; it does not add journal I/O, a manifest, live restoration or authenticated delivery. See the [canonical state and envelope design](phase7c2a3b7-canonical-replay-state-design.md) for the complete field order, operation codes and validation rules.

`canonical_isolated_state` serializes the restricted domain's configuration, private allocator high-water, exchange and matching counters, session identity/risk/positions/requests/private reports, ownership and trade history, feed sequences, book revisions, price levels and FIFO orders. It rejects live exchanges and public subscriptions. Derived exposure follows from the encoded ownership and resting orders. The existing exact diagnostic word vector is preserved and is checked independently when sealing source records; canonical envelope verification uses newly generated canonical bytes for per-operation exact equality. A full empty-state golden encoding checks the schema header and fixed configuration.

`CanonicalReplayEnvelope::seal` takes a nonzero caller-supplied 16-byte identity and returns an envelope and an anchor containing identity, terminal sequence and stream SHA-256. The header also carries configuration and initial-state hashes, private allocator scope, initial sequence, terminal sequence, record count and exact terminal offset. Each entry binds the fixed operation input/outcome to exact canonical bytes. Sealing replays the source prototype's records, checks their diagnostic state, derives the canonical snapshots and compares the final replayed canonical state with the owner's actual final state. The source prototype does **not** retain canonical bytes at each original installation; canonical-native recording remains a limitation.

Verification requires the caller's anchor, checks the complete bounded envelope and digest, constructs a fresh isolated exchange, executes every operation through existing logic and compares actual results and canonical bytes. A self-contained digest or self-supplied anchor is not authentication: independently trusted identity, terminal count and digest are needed to detect a substituted complete stream. SHA-256 detects mismatches subject to collision limitations; exact equality is the byte comparison, not the digest. No standalone state importer/decoder or live publication is implemented.

## Validation

On WSL2 with GCC 13.3, Debug **240/240** (1.04 s), Release **240/240** (0.73 s), and ASan/UBSan **240/240** (4.27 s) passed. New coverage includes full empty-state golden bytes, bounds and excluded subscriptions, private allocator exhaustion, canonical verification of multi-level behavior, changed configuration/allocator/initial identity, result/sequence/kind/state corruption and reordered entries with recomputed stream digests, malformed lengths, unsupported schema, digest corruption, independent terminal-anchor rejection, truncation and allocation-failure sweeps during seal/verify. Existing restricted replay tests cover FIFO, positions, ownership, private queue capacity and report identity. The verifier shares Exchange logic with recording and is not a separately implemented matching-engine oracle.

The final review also exercises replacement streams and anchors, canonical schema/counter corruption, and terminal-offset mismatch. Fresh Debug, Release and ASan/UBSan suites each passed **240/240** (1.02 s, 1.24 s and 5.18 s).

The matching and simulation CLIs ran; Phase 7B verified 12 records, five commands and four trades. CHROVIEW comparison and isolated CHRSTATE restoration with follow-on exact-state comparison passed. Selected protocol, two-client, exclusive-gateway and publication-gate regressions passed **18/18**. Formatting and Git whitespace checks passed. No browser test was run because gateway behavior and wire protocol were not changed.

## Performance

On WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3 Release, [the probe](../benchmarks/canonical_replay_probe.cpp) used two warmups, 30 samples per row except five for the 128-operation case. State, comparison and digest cases use a 224-byte one-session state; stream workloads are one session open followed by no-op closes. Setup is outside timing. Fixed-order unpinned samples, small sample counts and clock overhead limit conclusions. Scalar `operator new` counts cover only the measuring thread; they exclude other allocation paths and do not measure peak RSS.

| Workload | Samples | p50 | p95 | p99 | Median scalar allocations / requested bytes | Encoded bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Encode and compare state | 30 | 1.443 µs | 2.173 µs | 3.074 µs | 9 / 511 | 224 |
| Compare two exact states | 30 | 0.110 µs | 0.249 µs | 0.436 µs | 0 / 0 | 224 |
| SHA-256 state | 30 | 2.844 µs | 3.904 µs | 6.863 µs | 0 / 0 | 224 |
| Seal eight operations | 30 | 61.179 µs | 89.113 µs | 95.329 µs | 146 / 24,557 | 3,048 |
| Verify eight operations | 30 | 26.561 µs | 42.310 µs | 57.863 µs | 98 / 11,263 | 3,048 |
| Verify 32 operations | 30 | 110.663 µs | 176.393 µs | 183.185 µs | 362 / 43,303 | 11,688 |
| Verify 128 operations | 5 | 364.795 µs | 419.288 µs | 419.288 µs | 1,418 / 171,463 | 46,248 |

The 30-sample p99 and five-sample p95/p99 are descriptive maxima, not reliable tail estimates. Stream bytes grow from 3,048 at eight operations to 46,248 at 128 for this small constant-state fixture; populated books and reports can amplify each full-state snapshot. These bytes exclude copied buffers and candidate state, so they are not peak memory. The prior prototype's full-history reconstruction can take O(n²) operation work over n recorded calls; sealing and verification additionally capture full state per operation. The prior milestone's recordings are separate, unpaired runs and do not provide a controlled overhead delta.

Raw samples are in [the benchmark asset](assets/phase7c2a3b7/canonical-replay-final6.csv). Reproduce after a Release build with `c++ -O3 -DNDEBUG -std=c++20 -Iinclude benchmarks/canonical_replay_probe.cpp build/release/libchronos_core.a -pthread -o /tmp/canonical-replay-probe` and run it. No filesystem I/O is timed by the benchmark.

## Remaining blockers

The isolated stream has no authenticated exchange identity, independently durable terminal boundary, persistent commit protocol, process-wide allocator reconciliation, startup publication guard, authenticated session recovery or exactly-once report delivery. The versioned exact-state schema has no standalone importer or schema-migration policy. Canonical snapshots are derived during sealing rather than retained at original operation installation. Production durability requires a separately reviewed journal/manifest protocol, filesystem assumptions, fail-stop semantics and crash-fault tests.
