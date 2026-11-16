# Phase 7B isolated event logging and replay

Phase 7B records isolated in-process `chronos_sim` runs and verifies them by
replaying the recorded commands against a fresh exchange. Network sessions,
restart recovery, authenticated reconnect, exactly-once delivery, and live
trading are outside this implementation. A log includes private reports and
account state and should be treated as sensitive local data.

Allocator update: replay now defaults to a private exchange ID allocator and
does not consume the process-wide live ID sequence. API callers can explicitly
request `ReplayAllocation::SharedProcess` for the former behavior. Numeric IDs
remain normalized in either mode; the log format is unchanged. See
[allocator isolation](phase7c1a-allocator-isolation-report.md).

## Format 1.0

All integers are little-endian; signed prices and positions are represented as
64-bit two's-complement bit patterns. The file starts with the eight bytes
`43 48 52 4f 4e 4f 53 00` (`CHRONOS\0`). The remainder is a sequence of
records. Each has a 20-byte prefix: `u32 total_length`, `u16 type`, `u16 flags`
(must be zero), `u64 ingress_sequence`, `u32 payload_length`; its payload is
followed by a `u32 CRC`. Total length includes prefix, payload, and CRC.
The maximum record is 1 MiB and the maximum file is 64 MiB. The maximum
record count is 2050. CRC is IEEE CRC-32 (reflected polynomial `0xedb88320`,
initial and final XOR `0xffffffff`); it covers the entire record including
length and prefix, excluding the checksum itself. The magic is validated
separately. CRC detects accidental corruption; it is not authentication.

Payloads consist of explicitly encoded `u64` words and length-prefixed arrays
of such words, never native struct memory. Record types and contents are:

1. Header, ingress 0: format major 1, minor 0; exchange verification schema 1;
   generator version 2; scenario, seed, participant count, command count,
   order-history and request capacities, reference price, variation, quantity
   bounds, buy and cancellation weights, symbol count and symbols, followed by
   risk quantity/notional/open-order/position/market-price limits and the
   allowed-symbol array. Configuration validation is mandatory.
2. Command: generator step, session, request, client order, kind, symbol, side,
   order type, price, quantity. Enum values are the currently documented C++
   values. The record ingress is checked independently and must match the next
   expected per-run admission number.
3. Outcome: exchange result enum followed by a counted normalized authoritative
   exchange state. This includes reports, execution/trade details, counters,
   FIFO book levels and orders, ownership, risk limits, positions, request
   history, subscriptions, market data and feed sequences. Process-global
   internal IDs are normalized by insertion-history ordinal; risk reservations
   are represented by the open orders and limits from which the existing exchange
   calculates them, not by an invented independent reservation ledger.
4. Terminal: generated, admitted, accepted and rejected orders, accepted and
   rejected cancellations, trade count, aggregated trade count, executed
   quantity, supplementary FNV digest, then the final counted state.

The counted state begins with its `u64` word count, followed by these words in
order. Every nested array starts with its own `u64` element count; maps are
iterated in key order and vectors retain their order:

- Schema, exchange command counter, private event counter, initial feed
  sequence, then sessions. Each session contains ID, kill flag, risk limits and
  allowed-symbol array in header order; request IDs; `(symbol, position)`
  entries; reports as `(sequence, command, request, client order, normalized
  internal order, trade, symbol, price, quantity, remaining, kind)`; subscription
  generation; and subscriptions. A subscription contains symbol, generation,
  gap sequence, gap and gap-sent flags, then events as `(symbol, sequence,
  generation, kind, bid price, ask price, trade price, bid quantity, ask
  quantity, trade quantity)`.
- Order-history entries `(session, client order, request, symbol, open)` in
  ordinal order, then trades `(command, trade, buyer session, seller session,
  normalized buy order, normalized sell order, symbol, price, quantity)`.
- Feed sequences `(symbol, sequence)`, then symbol engines. An engine contains
  symbol and matching command/event/trade counters, followed by buy levels and
  sell levels. Each level contains price, aggregate quantity, order count, and a
  counted FIFO array of `(normalized internal order, quantity, sequence)`.

Boolean fields are `0` or `1`. Enum words are fixed to their v1 schema values;
the complete golden fixture and the encoder/decoder source provide byte-level
examples. The state is opaque verification data, not a general snapshot format.
The values start at zero in these listed orders: scenario `Balanced`,
`HighVolume`, `Volatile`, `LiquiditySweep`, `Mixed`; command `Submit`, `Cancel`;
side `Buy`, `Sell`; order type `Limit`, `Market`; report `Accepted`,
`PartialFill`, `Filled`, `Resting`, `Cancelled`, `MarketRemainder`; market event
`Snapshot`, `Book`, `Trade`, `Gap`. Exchange result values 0 through 16 are
`Accepted`, `UnknownSession`, `DuplicateRequest`, `DuplicateOrder`, `NotOwner`,
`NotFound`, `InvalidOrder`, `Capacity`, `CounterExhausted`, `RiskKilled`,
`RiskSymbol`, `RiskQuantity`, `RiskNotional`, `RiskOpenOrders`, `RiskPosition`,
`RiskMarketBound`, `RiskOverflow`, in that order.

The header and terminal use ingress 0 and the last admission, respectively;
each command and outcome pair uses the same ingress. Generator step, ingress,
exchange accepted-command sequence, trade ID, private-event sequence, and
market-feed sequence have distinct meanings even when values happen to match.
Version 1 accepts exactly major 1, minor 0 and generator/exchange schema
versions shown above; unknown records, versions, flags, invalid lengths and
checksum failures are rejected. No forward-compatibility or cross-version
equivalence is promised. FNV is stored for reference only and is not a proof.
The normalization table rejects zero or duplicate actual order IDs and maps
each nonzero ID to its order-history ordinal. Book, report and trade references
must resolve to that table, so replay ignores only the process-wide numeric ID,
not order identity, owner, FIFO placement, or cross-reference relationships.
For this one-command-at-a-time simulator, step and ingress happen to have the
same numerical value; the reader validates both and neither is an exchange
accepted-command, trade, event, or market-feed counter.

## Admission, failure, and replay

The simulator's single owner generates a command, writes and flushes its
command record, and only then admits it to the exchange. A pre-admission write
or flush failure stops the run without calling the exchange. Exchange rejections
after admission have outcome records, including their unchanged state. After
the exchange returns, its result and state are serialized and flushed. If this
fails, the simulation stops as failed and reports that any committed operation
was not rolled back. No terminal record is written for a stopped or failed run.
The reader rejects truncated, partial, missing, extra, out-of-sequence, and
otherwise invalid records; it parses and bounds the entire file before creating
an exchange. Terminal completeness is required. Stream flush means `fflush`
for the CLI file sink and is **not** `fsync`: there is no crash durability or
atomicity between the exchange and the file. A failed flush may have written
some or all bytes. `--record` creates a new file exclusively and does not replace
an existing file. An incomplete file must be removed or renamed by its owner.
The CLI also returns failure and labels the recording uncertain if closing the
file reports an error after an otherwise completed run.

The file alone cannot distinguish every failure window:

| Observed tail | What can be concluded |
| --- | --- |
| No complete command record | A reported command-write failure prevents admission; surviving bytes after a crash do not prove the prior in-memory state. |
| Complete command, no complete outcome | Execution may not have started, may have thrown or rejected, or may have committed before outcome recording failed. The file cannot distinguish these cases. |
| Complete pairs, no terminal | Present outcomes can be examined, but the session is incomplete and is not verified as complete. |
| Partial outcome or terminal | The reader rejects the file; no partial record is accepted. |
| Complete terminal | The reader verifies the surviving bytes and replayed state, but cannot prove the writer reported success or that storage is durable. A terminal write can reach the stream even if its subsequent flush fails. |

The last case is regression-tested: an injected terminal flush failure makes the
runner fail even though the bytes left in the test stream form a verifiable log.
Verification is a statement about the surviving file, not the writer's
acknowledgment, crash state, or storage durability.

Replay uses recorded typed commands, not the generator, then compares result
and normalized state after each command before draining private reports. It
performs the same final snapshot operations as the simulator and checks final
state, counts, and executed quantity. Its first divergence includes sequence,
offset and, for state words, index and expected/actual values. State capture
allocates and grows with exchange history, and records repeat the full state;
recording can therefore reach the file limit before the simulator's 1024-command
limit. A limit or allocation failure is a visible failed, incomplete run.
This is a verification artifact, not a streaming recovery mechanism. The
verifier reads the bounded file into memory; it does not promise constant-space
replay or independent verification of the supplementary digest.

## Usage

```sh
cmake --build --preset release
build/release/chronos_sim --scenario liquidity-sweep --seed 42 --commands 5 --record session.chronos
build/release/chronos_replay --verify session.chronos
```

The recorder is optional; omitting it leaves the simulation command stream and
digest unchanged. Verification returns nonzero for missing, malformed, incomplete,
or divergent logs. A stop requested while recording leaves no terminal record;
the runner reports `Failed` with an incomplete summary because the recorder
does not mark a partial run complete. Unrecorded stop behavior is unchanged.

## Validation and limitations

Debug, Release and ASan/UBSan each passed **118/118** CTest cases (0 failures).
Tests include CRC known vectors,
endianness, replay of partial fills and risk rejections, cancellation and
multi-symbol runs, all byte truncations of a small log, malformed lengths,
version/type/sequence corruption, mismatch reporting, deterministic output,
short writes, failed flushes, and stopped runs. CRC-preserving tampering is
detected by replay when it changes compared authoritative state; CRC is not
cryptographic protection. Allocation failures inside the recorder are surfaced,
including allocation failure before command serialization and during outcome
capture. Exhaustive allocation-fault injection for every replay site is not
included. `tests/eventlog/golden_v1.hex` is the complete, text-encoded v1
fixture for a one-command default simulation; tests compare every byte and
replay it. Its expected value is independent of process-global ID allocation.

The Release CLI demo above produced five accepted commands and four trades and
verified 12 records. A separate balanced 20-command run produced 20 accepted,
10 trades, and verified 42 records. The matching CLI ran successfully.
`bash scripts/format.sh --check` and Windows Git's `git diff --check` passed.
An unconfigured WSL Git invocation treated the Windows checkout's CRLF files as
new trailing whitespace throughout the pre-existing tree; it is not the same
line-ending configuration as the intended Windows Git checkout.

## Initial performance measurements

These are development measurements, not production latency or durable I/O.
The machine was an Intel i7-13620H (16 logical CPUs), WSL2 Linux
6.6.87.2-microsoft-standard, GCC 13.3.0, Release build, Google Benchmark 1.8.3.
The file benchmark used an unnamed `/tmp` file on the WSL ext4 filesystem,
with `fflush` and no `fsync`. Three repetitions used a 0.1 s minimum time;
table values are medians of repetition-level wall-clock means, not p50/p99
command latency. Setup is paused; teardown may still contribute. File creation
and header writing are outside the timed section. Full-simulation verification
is performed with timing paused. The stream-only cases use memory, not disk.

| Workload | Median time per iteration | Iterations per repetition |
| --- | ---: | ---: |
| CRC-32 of 4096 bytes | 44.827 µs | 3,063 |
| Encode and append one command to memory stream, including stream flush | 1.909 µs | 72,207 |
| Encode, append and `fflush` one command to temporary file | 2.471 µs | 55,287 |
| Run 20 exchange commands, no recording | 39.900 µs | 2,802 |
| Run the same 20 commands with memory recording | 651.493 µs | 214 |
| Validate and replay that 20-command log | 654.521 µs | 225 |

The replay input was 46,144 bytes. Google Benchmark reports CPU-time throughput
of approximately 459,215 commands/s for the unrecorded simulation, 30,785/s for
the recorded simulation, and 30,549/s for replay. Recording includes repeated
full-state capture and checksums; these measurements do not isolate their
individual shares. Allocation counts were not instrumented. A separate single
`/usr/bin/time -v` 20-command CLI run reported peak RSS of 3,840 KiB for
recording and 3,840 KiB for replay, including process/runtime overhead. These
are not per-command memory figures. Raw repetitions and machine metadata are in
the local `build/phase7b-benchmark-wsl2.json` artifact (not versioned). Reproduce with:

```sh
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DCHRONOS_ENABLE_BENCHMARKS=ON -DBUILD_TESTING=OFF
cmake --build build/bench --target chronos_eventlog_bench -j 2
build/bench/benchmarks/chronos_eventlog_bench --benchmark_min_time=0.1s --benchmark_repetitions=3
```

WSL virtualization, host scheduling, filesystem cache, short runs and the lack
of durable sync prevent generalizing these figures to production. The stream
benchmark is a component cost, not exchange or network latency.

## Final review performance breakdown

The review reran optimized benchmarks with 0.05 seconds minimum and three
repetitions on the same WSL2 machine. Values below are medians of per-run
wall-clock means, not individual-operation percentiles. Counts per repetition
for the simulation workloads are respectively 3,520/247, 1,282/93, 377/22,
and 105/6 (without/with recording); replay counts are 274, 100, 23, and 7.
Short runs and WSL2 scheduling make especially the 100-command samples noisy.

| Commands | Simulation without recording | With recording | Replay | Log size |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 20.056 µs | 268.739 µs | 239.624 µs | 18,488 B |
| 20 | 47.536 µs | 684.182 µs | 577.141 µs | 46,144 B |
| 50 | 189.311 µs | 2.943 ms | 2.578 ms | 198,776 B |
| 100 | 569.162 µs | 9.890 ms | 8.772 ms | 657,024 B |

The unrecorded simulation includes generation, matching, exchange, and summary
work, so it is an execution-cost proxy, not a direct engine-only measurement.
For independently prepared 10/50/100-order books, one full normalized capture
cost 0.699/3.142/6.418 µs; capturing, serializing, checksumming and appending
one outcome cost 13.682/48.294/94.741 µs. A command encode/checksum/memory
append cost 1.890 µs; file append with `fflush` cost 2.619 µs. CRC alone took
43.427 µs per 4,096-byte input. Synthetic equality checks of 1,000 and 10,000
words took 0.131 and 1.432 µs respectively; replay timing includes parsing,
checksums, exchange execution, capture and comparison, not only comparison.
Component timings use different inputs and cannot be subtracted as exact
attribution. Allocation counts and serialization in isolation were not
measured. These results identify repeated full-state encoding/checksumming as
the dominant measured area for this workload; the original linear ID lookup
was replaced with a checked ordered normalization table, without changing the
wire representation. The older 20-command result of 651.5 µs was obtained in
a separate run and is not a controlled before/after optimization measurement.

For `n` order-history entries, `r` references, and `s` serialized state words,
normalization now costs approximately `O((n + r) log n + s)` per snapshot;
record bytes and CRC cost `O(s)`. If retained state grows linearly with command
count, repeating snapshots takes at least quadratic total bytes and checksum
work, with normalization adding a logarithmic factor. Future work should first
benchmark incremental canonical verification with independently checked
checkpoints; it must retain the current transactional and identity checks.
The complete raw review measurements are in the local, ignored
`build/phase7b-review-benchmark.json` and can be regenerated with the commands
above; the benchmark now includes the 10/20/50/100-command cases and component
workloads. These are not production or durable-write measurements.

For Phase 7C, specify and test journal/filesystem sync ordering, atomic snapshot
publication and generation, torn-tail handling, restart identities and report
reconciliation before making any durability or recovery claim. The full-state
per-command capture and bounded in-memory reader are candidates for a later,
measured redesign, not a production transaction journal.
