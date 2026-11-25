# Restricted replay prototype: implementation report

The experimental typed-owner-record milestone was committed and pushed as `17d09cf82382e28b9f53a4d43a038a6a5af37752`. This restricted-replay prototype is an in-memory isolated exchange experiment; it cannot attach to the live gateway, publish its private IDs, write a durable journal or perform crash recovery. The [design](phase7c2a3b6-restricted-replay-design.md) specifies the exact domain, schema, sequence, allocation and verification boundaries.

`RestrictedReplay` accepts only session open, bounded risk configuration, submit, cancel, expected private-report consume and session close. It has no subscription, kill-switch, mutable Exchange or gateway interface. The fixed initial exchange has no sessions or subscriptions and uses a private allocator starting at zero. Every supported call is represented by a contiguous record, including business rejections and no-ops; invalid inputs, allocation failure and deliberate abort do not advance the sequence. Its 128-operation cap prevents u64 sequence exhaustion by construction.

Before installation, the implementation reconstructs an isolated candidate from all prior records, executes the new operation using existing Exchange code, captures bounded exact continuation words and builds the encoded record. A reserved outer vector and nonthrowing byte-vector move and unique-pointer swap make record append and candidate installation a synchronous owner-only commit boundary. The offline verifier independently reconstructs the fixed initial exchange, decodes each record, checks contiguous order, executes actual logic and compares the actual outcome and every exact-state word. Its expected terminal count must come from outside the stream to detect a missing suffix. It shares the same Exchange implementation and has no authenticity guarantee.

## Validation

WSL2 GCC 13.3: Debug **236/236** (1.36 s), Release **236/236** (0.97 s), ASan/UBSan **236/236** (6.45 s). New tests exercise isolated multi-level partial fills and cancellations against an independently operated isolated Exchange, same-price FIFO and wrong-owner cancellation, risk configuration, report consumption and duplicate mismatch, report-capacity rejection and retry after consumption, closed sessions with retained history, exact state after each replayed record, allocation-failure sweeps, deliberate abort, owner-context denial, capacity exhaustion, unsupported input, changed outcome/state, duplicate/missing sequence, unsupported kind and truncated records. Full counter exhaustion is unreachable because the domain caps the sequence at 128 operations; capacity exhaustion is tested instead. There is no externally supplied callback or mutable Exchange handle through which reentrancy or excluded subscription mutation can occur.

Final review added explicit private-versus-process allocator isolation, oversized record, altered report identity/version and missing-suffix checks. A valid prefix still verifies if the caller supplies that shorter count, demonstrating the need for an independent trusted terminal count. Debug, Release and ASan/UBSan each passed **237/237** tests (1.47 s, 1.08 s and 6.86 s).

The full suites also include existing concurrent control, shutdown, shared/exclusive gateway and protocol tests. A selected protocol/two-client/exclusive/publication-gate run passed **18/18**. Matching CLI, the five-command liquidity-sweep simulation, Phase 7B replay (12 records and four trades), CHROVIEW comparison and isolated CHRSTATE restoration with follow-on state comparison passed. Formatting and Git whitespace checks passed. The gateway's observable behavior was not changed, so browser checks were not run.

## Performance

Environment: WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3 Release. The [probe](../benchmarks/restricted_replay_probe.cpp) runs five warmups; setup is outside timing. The isolated-open and recorded-open cases have 100 samples each, as does replay verification of eight session opens. An eight-counterparty, eight-level sweep after 17 prior operations has 30 samples. Results are fixed-order, unpinned and local; clock overhead and WSL variation limit small measurements. Allocation counts cover scalar `operator new` on the measured thread only; no process peak-memory measurement was made.

| Workload | Samples | p50 | p95 | p99 | Median allocations / requested bytes | Encoded stream bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Direct isolated session open | 100 | 0.082 µs | 0.086 µs | 0.109 µs | 1 / 272 | 0 |
| Recorded isolated session open | 100 | 0.969 µs | 2.046 µs | 2.078 µs | 5 / 1,224 | 368 |
| Verify eight session-open records | 100 | 7.593 µs | 9.303 µs | 45.962 µs | 26 / 12,512 | 6,080 |
| Record eight-level sweep after 17 operations | 30 | 99.114 µs | 178.876 µs | 185.246 µs | 620 / 132,576 | 34,008 |

The 30-sample p99 is the maximum and is descriptive only. The sweep timing includes reconstruction and verification of previous history, matching, exact-state capture and record encoding. The verifier and record paths were not broken down into separate codec and matching costs. The stream byte count is not peak memory: candidate exchanges, state words, copied streams and allocator overhead add to it. The per-operation full-history reconstruction is intentionally costly and can grow quadratically over a run; this prototype is a correctness baseline rather than a production latency design. Raw measurements are in [the benchmark asset](assets/phase7c2a3b6/restricted-replay-probe.csv).

Reproduce after a Release build using `c++ -O3 -DNDEBUG -std=c++20 -Iinclude benchmarks/restricted_replay_probe.cpp build/release/libchronos_core.a -pthread -o /tmp/restricted-replay-probe` and run it. The tests use no filesystem I/O for this stream.

## Remaining blockers

The encoded exact-state words are tied to the current implementation and are not a stable canonical storage codec. There is no authenticated stream identity, independently authenticated terminal count, crash-durable record boundary, filesystem sync, live session recovery, global allocator reconciliation or publication guard. Public subscriptions are excluded under the fresh isolated-session boundary; old clients cannot be silently reattached. A fully consistent maliciously substituted stream is not detected by state equality. Durable journal authorization, fail-stop storage behavior and fault injection require separate work.
