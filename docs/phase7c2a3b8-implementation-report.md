# Canonical-native isolated recording: implementation report

The canonical verification milestone was committed and pushed as `eb2c23d40fd86f465e3d12cda3eacb33b8de5b26`. This milestone captures each restricted operation's canonical continuation state from the original private candidate before installation; it does not implement durable storage, a journal writer, a manifest or live recovery. The [design](phase7c2a3b8-canonical-native-recording-design.md) specifies the lifecycle, bounds and anchor requirements.

The restricted owner clones its current isolated Exchange and private allocator high-water, executes the new operation once through existing logic, and constructs diagnostic words, canonical bytes, encoded operation/outcome and an allocated record before installation. It then swaps in the candidate, swaps the prebuilt record into a fixed checked slot, and increments its bounded sequence. Both swaps and cleanup are nonthrowing; no callback, allocation or observer occurs in the handoff. Failed validation, allocation, canonical encoding, record construction or deliberate abort leaves state, records and sequence unchanged. Accepted commands, business rejections and completed no-ops each get one record. No mutable candidate or install handle is exposed.

Sealing decodes installed operation metadata and copies canonical bytes captured from the original candidate; it does not replay prior operations to create expected snapshots. It compares the last captured bytes to the owner's current canonical state. The offline verifier still executes every operation on a fresh isolated Exchange and compares actual outcome and canonical bytes with the original recording. The verifier shares matching/risk code with recording and is not an independent implementation oracle. An identity/terminal/digest anchor requires an independent trusted source; a self-supplied anchor is not authentication.

## Validation

WSL2 GCC 13.3: Debug **243/243** (1.31 s), Release **243/243** (0.99 s), and ASan/UBSan **243/243** (5.96 s). New coverage checks original candidate bytes against the installed owner after every operation, preservation of earlier captured snapshots, fresh replay, abort, out-of-range record access, a forced canonical-size limit before installation, and an allocation-failure sweep comparing both exact state and all previous records. A test-only execution counter proves that successive `apply` calls each execute only the new operation and that `seal` executes none; offline verification executes the recorded operations. Existing tests cover multi-level FIFO, partial fills, cancellation, expected-report consumption, session closure, rejection/no-op outcomes, capacity/sequence bound, wrong context, private allocator isolation, tampered outcome/state, incomplete stream and wrong anchor.

The final review strengthened the failed-size/retry check to require exactly one installed record. Fresh Debug, Release and ASan/UBSan suites each passed **243/243** (1.38 s, 1.01 s and 6.25 s).

The matching and simulation CLIs ran; Phase 7B verified 12 records, five commands and four trades. CHROVIEW comparison and isolated CHRSTATE restoration with follow-on exact-state comparison passed. Selected protocol, two-client, exclusive-gateway and publication-gate regressions passed **18/18**. Formatting and Git whitespace checks passed. The gateway protocol and externally visible behavior were not changed, so browser checks were not run.

## Performance and memory

Measured on WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3 Release. [The probe](../benchmarks/native_recording_probe.cpp) uses the same deterministic 8/32/128-operation workloads for the committed history-reconstructing headers and the native headers: three sessions, a risk update, growing sell-side books, crossing market orders and pending private reports. Setup has one warmup and three measured runs per size. Every operation's outcome and each sealed stream's independent verification are checked. The old and new streams had identical SHA-256 digests and byte lengths for each workload; a matching digest is not proof of equality, so the tests also compare exact states.

The table gives the median and range of the **last operation** in each run and sealing time. These are three samples per cell, not statistically reliable tail estimates. `apply` includes clone, authoritative execution, state captures, record construction and installation; record construction and the pointer-swap installation were not separately instrumented. The old `apply` rebuilds history; the new path captures canonical bytes immediately, so the timings cover different work placement.

| Operations | Last operation, old median (range) | Last operation, native median (range) | Seal, old median | Seal, native median | Envelope bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 23.432 µs (22.573–27.022) | 15.737 µs (15.436–16.931) | 131.058 µs | 78.619 µs | 7,824 |
| 32 | 497.816 µs (362.107–514.950) | 74.194 µs (60.040–93.541) | 2.492 ms | 1.635 ms | 142,384 |
| 128 | 6.368 ms (5.789–6.701) | 0.306 ms (0.300–0.439) | 39.582 ms | 23.716 ms | 2,393,264 |

For the last operation at 128, median scalar allocations/requested bytes were **20,813 / 10,405,528** old and **466 / 338,535** native. Independent verification of the 128-operation envelope had median **27.642 ms** old and **28.016 ms** native; replay still performs the same verification work. A separate 30-sample small-state probe measured canonical state encoding plus equality at **0.896 µs p50**, with nine scalar allocations and 511 requested bytes for a 224-byte state. These fixed-order WSL runs were not interleaved or pinned and must not be treated as production trading latency.

`getrusage` reported process-wide peak RSS of **14,000 KiB** for the old benchmark process and **15,576 KiB** for the native process. These are cumulative maxima across sizes, warmups and verification, not per-operation peaks or controlled resident-memory deltas. Full canonical snapshots and legacy diagnostic states remain stored per operation; removing repeated history execution does not remove full-state memory amplification. Separate raw samples, allocation counts, stream lengths, digest values and cumulative RSS are in [the benchmark assets](assets/phase7c2a3b8/).

Reproduce after a Release build with `c++ -O3 -DNDEBUG -std=c++20 -Iinclude benchmarks/native_recording_probe.cpp build/release/libchronos_core.a -pthread -o /tmp/native-recording-probe`. For the historical comparison, place `restricted_replay.hpp` and `canonical_replay_envelope.hpp` from commit `eb2c23d40fd86f465e3d12cda3eacb33b8de5b26` under a temporary `include/chronos/exchange` directory and put that include directory first. The benchmark times no filesystem I/O.

## Remaining blockers

The stream remains isolated and in-memory. The canonical-native installed record is not a durable journal record, and the external anchor has no authentication or persistence mechanism. Live publication requires process-wide allocator and identity reconciliation, guarded admission, session/private-report recovery policy, and durable storage authorization with failure-atomic sync and crash testing. No authenticated reconnect, exactly-once delivery or production durability is claimed.
