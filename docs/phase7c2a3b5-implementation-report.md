# Typed operation prerequisites: implementation report

The diagnostic dispatcher milestone was committed and pushed as `d1d0bae3d59694a15921006ee64c03a38a8a72a1`. This milestone implements **partial** typed observations and a conditional private report dequeue, not a complete replay stream, stable journal format, durable writer or recovery system. The [design](phase7c2a3b5-typed-operation-design.md) defines its domain, codecs, identity limitations and remaining operation variants.

## Implemented behavior

`submit_recorded` and `cancel_recorded` invoke the existing direct Exchange transaction through the owner dispatcher and return a `CommandRecord` containing the exact canonical command, actual `Result` and diagnostic index. They do not run the prepared gateway path and are not used by the gateway. `consume_expected_report` compares recipient and expected report sequence to the current front before popping. It returns a `DequeueRecord` with consumed, empty, mismatch or invalid status and observed sequence. Wrong identity and duplicate attempts leave another report untouched; unrelated sessions cannot consume this session's report. Neither the record nor a successful pop means that a client received anything.

The records use fixed 68-byte and 40-byte big-endian experimental encodings. Encoding validates known outcomes and decoding rejects unknown versions, kinds and statuses, nonzero reserved bytes, invalid field combinations, truncation and trailing data. No native struct layout, pointers or heap allocation are required by the codec. Golden-byte tests cover both record forms. Command result codes are explicitly mapped; accepted versus business rejection comes from the actual exchange result. Errors thrown before completion and prepared aborts remain exceptions/abort returns from existing APIs and do not manufacture serialized outcomes. A complete internal classification and schemas for these paths are not implemented.

The diagnostic index may have gaps because other owner operations emit no record. No lifecycle, epoch, process-wide allocator authority or gateway integration is encoded, and existing blind `pop` remains. A restricted fully ordered owner, complete session/risk/subscription/gap records, expected public delivery identity, private-obligation reconciliation, exact allocator high-water and full counter/book/risk outcomes remain prerequisites. No complete replay or exactly-once delivery is claimed.

## Validation

WSL2, GCC 13.3: Debug, Release and ASan/UBSan each passed **228/228** CTest cases (0.83 s, 0.65 s and 3.32 s). New tests cover actual accepted/rejected submit/cancel results, conditional report dequeue, duplicate and wrong identities, empty/invalid queues, recipient isolation, wrong-owner context, reentrant mutation, exact live state after mismatch, both golden encodings, every truncated length, trailing bytes and malformed/reserved/version/result fields. Existing tests cover session/risk changes, subscription gaps, allocation failures, abort, diagnostic exhaustion, concurrent controls and gateway shutdown. These existing tests do not imply typed recording or replay of those operations.

The final review additionally tests two live recipients with distinct report sequences and a recorded-submit allocation failure that preserves exact state and the diagnostic index. Report sequences are globally allocated within one exchange lifetime, so two distinct reports cannot have the same sequence in that scope; separate exchange instances can reuse a sequence and require explicit identity binding. Final Debug, Release and ASan/UBSan runs each passed **230/230** tests (1.17 s, 0.93 s and 5.59 s). The sanitizer target was explicitly rebuilt after a copied source timestamp initially prevented rediscovery of the new test.

Matching and simulation CLIs ran. Phase 7B verified 12 records, five commands and four trades. CHROVIEW comparison and isolated CHRSTATE restore with follow-on exact-state comparison passed. Selected protocol, two-client, exclusive-gateway and publication-gate regressions passed **18/18**. Formatting and Git whitespace checks passed. The gateway's externally visible behavior was not changed; no browser check was run.

## Performance

On WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3 Release, the [probe](../benchmarks/typed_operation_probe.cpp) used 10 warmups and 200 samples per operation with fixture creation outside timing. Codec calls use no-inline wrappers and verify the returned value; conditional dequeue verifies the expected sequence. Fixed-order unpinned samples and clock overhead dominate at these scales; these are not production or network measurements. Scalar `operator new` counts are limited to the measuring thread and exclude other allocation paths.

| Operation | p50 | p95 | p99 | Median scalar allocations / requested bytes |
| --- | ---: | ---: | ---: | ---: |
| Encode command record | 52 ns | 56 ns | 65 ns | 0 / 0 |
| Decode command record | 87 ns | 91 ns | 93 ns | 0 / 0 |
| Conditional report dequeue | 41 ns | 66 ns | 68 ns | 0 / 0 |

The probe does not independently isolate record construction, prepared submit/cancel, risk update or dispatcher overhead. Prior dispatcher and risk measurements are in the preceding milestone report and are separate, unpaired runs; they must not be presented as controlled overhead deltas. More repeated/interleaved runs, populated books, allocation-failure coverage for any future recording integration and real peak-memory measurements are needed. Raw samples are in [the benchmark asset](assets/phase7c2a3b5/typed-operation-probe.csv). Reproduce after a Release build with `c++ -O3 -DNDEBUG -std=c++20 -Iinclude benchmarks/typed_operation_probe.cpp build/release/libchronos_core.a -pthread -o /tmp/typed-operation-probe` and run it.

## Blockers

Define and enforce one covered mutation domain, typed lifecycle/admin and queue records, result classification for internal failures, and an authoritative operation identity distinct from LSN. Bind exchange identity/epoch and reconcile process-wide allocator high-water under exclusive startup admission gating. Define a stable canonical state/outcome representation, durable storage authorization and failure semantics before implementing a writer or recovery. Do not infer crash durability, authenticated reconnection, report replay or exactly-once delivery from these prerequisites.
