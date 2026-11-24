# Partial recovery-state effects: implementation report

The preceding partial-summary milestone was committed as `2de154a6bd07270f95fb570f1c298838c7384604`. This effect-capture milestone is also limited: it is not a complete journal or durable transaction system. See [the state inventory](phase7c2a3b2-recovery-state-inventory.md) for mutation ownership, classification and the proposed recovery boundary.

## Implementation and limits

The optional post-install summary now includes bounded, sorted `participant_effects()` and `ownership_effects()`. Before installing, the private candidate is inspected for each trade participant and initiator, and for each internal order referenced by a new private report. Position, derived exposure, relevant risk policy and touched ownership/resting-order state are recorded before and after. Arithmetic is checked; invalid consistency or allocation throws before installation. No candidate reference escapes. The existing ordinary gateway path does not use this opt-in summary and its observable protocol is unchanged.

The effect summary is deliberately incomplete: it omits the full policy symbol list, positions for other symbols, untouched historical orders, administrative operations, public subscriber queues and private report consumption. Its exposure is derived from current resting orders; the implementation has no independent reservation ledger. Rejections have no installed effects. There is no incremental book/risk digest; the existing bounded exact continuation-state vector is diagnostic and is not a stable wire format. These omissions, allocator ownership and startup gating block a complete canonical outcome codec and live recovery.

## Validation

On a WSL-native source copy of the Windows worktree, GCC 13.3.0, Debug, Release and ASan/UBSan each passed **218/218** CTest cases (1.19 s, 0.82 s and 3.52 s respectively). Five added cases cover multi-party partial/full fills, cancellation exposure release, policy/kill capture, disconnected resting owner, self-trade, three counterparties across levels and cross-symbol exposure. A sixth sweeps scalar allocation failures through the complete summary operation and checks exact live-state equality for every failed attempt. Existing summary tests cover abort/no early publication, subscriber gaps and resubscription, risk rejection and exact state; a separate existing injected failure targets public-event capture. Existing risk configuration and report-consumption tests cover those operations, but no new standalone journal effect is claimed for them.

The subsequent final review added a same-price FIFO regression with multiple orders from the same counterparty, another counterparty, a partial fill and cancellation of the residual; an unknown-owner cancellation also preserves exact state. Fresh Debug, Release and ASan/UBSan runs each passed **219/219** tests (0.95 s, 0.82 s and 3.76 s).

The Release protocol, two-client routing, exclusive gateway and publication-gate selection passed **18/18**; the full suites also cover these. Matching CLI produced two fills and a 30-unit remainder. Simulation seed 42 generated/admitted five orders, four trades; Phase 7B replay verified 12 records, five commands and four trades. The CHROVIEW diagnostic comparison passed; CHRSTATE restored a 9,348-byte isolated image at generation 1/ingress 5 and exact state matched after follow-on commands. These are not live publication or durability tests. Repository clang-format check and Git whitespace check passed.

## Performance observations

Environment: WSL2 Linux 6.6.87.2, Intel Core i7-13620H, GCC 13.3, Release. These are local in-process measurements, not production or network latency. Build with `cmake -S . -B build/effects -DCMAKE_BUILD_TYPE=Release -DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON`, `cmake --build build/effects --target chronos_recovery_effect_probe chronos_decision_capacity_probe`, then run those executables. The effect probe resets its exchange outside timing, warms up five times, and times complete prepare/install/destruction for 100 samples per mode: eight distinct seller sessions, eight price levels and a 24-unit market sweep. It verifies eight fills and nine affected participants/orders. Scalar `operator new` calls and requested bytes are counted only on the measured thread; aligned/other allocation paths and peak RSS are not measured. The no-summary mode is a useful baseline but differs in summary creation and destruction.

| Workload | Samples | p50 | p95 | p99 | Median scalar allocations | Median requested bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Eight-counterparty prepared, no summary | 100 | 11.385 µs | 14.537 µs | 18.911 µs | 162 | 24,384 |
| Eight-counterparty summary with effects | 100 | 15.294 µs | 19.537 µs | 39.793 µs | 173 | 32,296 |
| Benchmark-only control: same summary with new effect-building block disabled | 100 | 10.038 µs | 12.010 µs | 24.570 µs | 169 | 29,072 |
| 128-level sweep, one resting seller, summary with effects | 20 | 173.626 µs | 190.760 µs | 202.389 µs | 865 | 385,096 |

The control was compiled from a temporary source copy with only the participant/ownership construction block removed and expected counts changed to zero; it is not a valid recovery implementation. Its unchanged prepared-path median was 7.769 µs versus 11.385 µs in the changed run, showing substantial run-to-run drift. Consequently the 5.256 µs median difference between summary runs is not a reliable isolated effect cost; the observed allocation difference was four calls and 3,224 requested bytes in this fixture. The 20-sample p99 is the maximum and is only descriptive. Raw samples are in [the benchmark assets](assets/phase7c2a3b2/). Larger books, repeated interleaved runs and independent peak memory measurements remain necessary. No new verification-metadata constructor was implemented, so its cost was not measured; full diagnostic-state construction should be benchmarked separately before choosing a verifier.

## Next work

Specify complete admin/session and report-consumption ordering, bounded full risk/ownership/book verification, rejection semantics, exact allocator identity and a versioned canonical encoding. Then separately design and validate a journal commit/sync protocol, storage failure handling, exclusive startup/reconciliation and failure-atomic publication. Do not infer authenticated reconnection, private report recovery, crash durability or exactly-once delivery from the current code.
