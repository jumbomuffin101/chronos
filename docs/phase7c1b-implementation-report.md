# Phase 7C.1B implementation report

The current implementation restores **only completed isolated simulation
runs with their full verified Phase 7B histories**. It is not a general-purpose
exchange snapshot or an atomic cold-start publication mechanism. The
[restoration design](phase7c1b-restoration-design.md) specifies the format,
limits, exact-state coverage and remaining boundaries.

`chronos_snapshot_demo --image NEW_FILE` runs a five-command private liquidity
sweep, writes a new `CHRSTATE` file, reconstructs a private candidate, checks
exact state, runs matching and cancellation commands independently on both,
and compares exact state again. The file is flushed, not crash-durable.

The restorer rejects a nonempty destination and never advances the shared live
ID allocator. Failure leaves the destination unchanged. Reconstruction uses
the existing exchange implementation and is validated by replay, not by an
independent matching implementation. Snapshots grow with full command history;
the embedded 64 MiB log limit can prevent large runs from being captured.
No direct arbitrary-exchange import, hot replacement, restart publication,
session reattachment, durable acknowledgment or crash recovery is provided.

## Validation

Debug, Release and ASan/UBSan each passed **140/140** CTest cases in a
temporary WSL-native source copy. Tests exercise exact-ID and revision
comparison, private allocator noninterference, multi-symbol sweeps and partial
fills, follow-on orders, cancellation and risk rejection, every truncation of
a small image, invalid header and checksum, oversized length, checksum-preserving
mutations of IDs, owners, FIFO, limits, positions, counters, revision and
allocator high-water, stream failures, occupied output, and an allocation
failure sweep. The direct two-client gateway fill test also passed. Formatting
and Windows Git whitespace checks passed.
The final review additionally tests checksum-preserving alterations of an
embedded command, duplicate and skipped ingress numbers, removal of the log
terminal, and repeated independent restores of the same bytes. Follow-on
commands compare exact state after each step, including order-ID continuity,
partial and complete fills across levels, and an invalid cancellation. CRC
does not authenticate an image: a fully consistent replacement history and
state is outside its corruption-detection threat model.
The restricted fresh-simulation history cannot produce an actually exhausted
64-bit allocator or book revision within the 1,024-command run limit. Tests
reject tampered high-water and revision fields, but do not claim restoration of
a legitimately exhausted arbitrary exchange.

The CLI demonstration wrote and restored a 9,348-byte image at generation 1,
ingress 5; exact state and follow-on commands matched. The separate diagnostic
`CHROVIEW` comparison passed. `/usr/bin/time -v` reported 3,840 KiB maximum
RSS for this one five-command process, including runtime and file I/O; it is
not a per-command allocation measurement. The existing matching CLI and a
Phase 7B record/replay demonstration also passed.

## Performance

Release benchmarks on WSL2 (Intel i7-13620H, GCC 13.3, Google Benchmark 1.8.3)
used fixed all-buy workloads, three repetitions and a 0.03-second minimum.
Values are medians of repetition-level wall-time means, not individual-command
percentiles. The empty-book case is one risk-rejected command. Setup and input
generation are outside timed sections.

| Resting orders | Exact capture | Write image | Restore image | Image size |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0.267 µs | 50.056 µs | 47.175 µs | 2,244 B |
| 10 | 1.165 µs | 396.896 µs | 385.008 µs | 16,636 B |
| 100 | 10.612 µs | 14.233 ms | 12.490 ms | 474,196 B |

A separate single-sample 1,000-order restoration took 1.087 s with a
37,125,796-byte image. It is descriptive, not a latency distribution.
The write measurement includes exact capture, full log verification/replay,
serialization and checksum; restore includes decoding, checksum, replay,
index construction and exact comparison. These components and allocation
counts were not measured independently. A previous diagnostic-only 100-order
capture measured 7.561 µs on the same WSL2 machine; it represented a different
state view and did not perform reconstruction, so it is not an equivalent
operation or a controlled comparison. A separate `/usr/bin/time -v` run of
the 1,000-order benchmark reported 321,700 KiB peak RSS, including fixture
generation and all process overhead; it is not incremental restore memory.
Full-history growth, the 64 MiB embedded-log cap, host scheduling, concurrent
validation work and lack of durable sync limit the usefulness of these figures
for production extrapolation.
Because each Phase 7B outcome contains a full verification view, a run whose
state grows linearly can produce quadratic total log bytes and checksum work.
The image additionally contains one exact final state; it is not a compact
checkpoint independent of prior history.

The reader retains the outer image, a copy of the embedded log, parsed log
records, the decoded exact words and a candidate exchange during verification.
It checks the outer image CRC, each embedded record CRC and each recorded
normalized state; replay also performs the exchange's existing staged state
clones. Normalizing order references uses an ordered map, while the final exact
comparison traverses books, owners, indexes and counters again. These combined
costs explain the growth, but the benchmark does not separately attribute
time to CRC, normalization, matching, decoding, or index reconstruction.
No change to the transaction or log format is proposed in this review.

Reproduce with an optimized CMake build configured with
`-DCHRONOS_ENABLE_BENCHMARKS=ON`, then run
`build/bench/benchmarks/chronos_restorable_bench --benchmark_min_time=0.03s --benchmark_repetitions=3`.
The local raw results are retained under ignored `build/phase7c1b-benchmark.json`
and `build/phase7c1b-benchmark-1000.json`. Destruction may contribute to some
timings; benchmark components are not independently isolated.
