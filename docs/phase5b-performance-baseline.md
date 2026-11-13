# Phase 5B exchange performance baseline

This is a small correctness-checked baseline, not a production latency claim.
It compares the optimized in-process matching engine, the exchange API without
socket I/O, and a loopback TCP request through the gateway. The latter is a
round trip through framing, scheduling, the exchange, report encoding and
client decoding; it is not matching-engine latency or external network latency.

## Environment and method

Measured on WSL2 Ubuntu 24.04, Linux `6.6.87.2-microsoft-standard-WSL2`,
Intel Core i7-13620H (16 logical CPUs exposed), about 7.6 GiB RAM visible,
GCC 13.3.0 and CMake 3.28.3, Release (`-O3`). Host load, VM scheduling,
frequency scaling, timer overhead and the Windows-mounted filesystem were not
controlled. These figures do not represent production trading hardware.

Build and run from the repository root on Linux/WSL:

```sh
cmake --preset release -DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON
cmake --build --preset release --target chronos_exchange_baseline
./build/release/chronos_exchange_baseline > build/phase5b-baseline.csv
```

Each cell below has one discarded warmup and nine timed samples. Every sample
constructs a fresh engine, exchange or gateway, populates N sell orders of one
unit at 110 on one symbol and price level, and drains setup reports before the
clock starts. The measured command is either a one-unit noncrossing limit buy
at 100, or a one-unit market buy that fills the best resting order at 110.
Setup, book restoration and post-command checks are outside the clock. The TCP
fixture uses a single connection for both simulated sides, so its single-fill
case includes both participants' reports on that connection. Two separate TCP
clients are covered by integration tests and the separate demonstration.

The benchmark checks acceptance and resulting trades or resting orders, and
checks TCP report kinds, correlation and execution price. It exits nonzero on a
failed check. It prints one CSV row per sample. Measurements include the calls
needed to submit and collect the response; TCP report parsing and correlation
are necessarily in the timed round trip. Fixed deterministic inputs need no
random seed. No benchmark-specific production engine changes were made.

The table reports median and observed range in microseconds. Nine samples are
too few for useful p95 or p99 estimates; min/max are included to show observed
spread, not as tail-latency estimates. Sub-microsecond results are particularly
sensitive to clock and scheduling overhead. Each case is independently prepared,
and this single run does not establish repeatability across machines or days.

| Workload | Background | Engine median (range), µs | Exchange median (range), µs | TCP median (range), µs |
|---|---:|---:|---:|---:|
| Noncrossing | 10 | 0.241 (0.227–0.495) | 2.409 (2.195–4.519) | 43982.684 (43788.222–45166.712) |
| Noncrossing | 30 | 0.152 (0.128–0.505) | 4.265 (3.903–5.684) | 43950.371 (41533.265–44584.203) |
| Noncrossing | 100 | 0.211 (0.125–1.006) | 13.883 (12.726–25.912) | 43944.828 (42849.452–45505.447) |
| Single fill | 10 | 0.248 (0.240–1.198) | 2.883 (2.560–4.544) | 43978.458 (42196.099–44158.147) |
| Single fill | 30 | 0.347 (0.234–1.471) | 4.399 (3.988–9.164) | 43974.742 (42805.913–44278.254) |
| Single fill | 100 | 0.261 (0.184–0.731) | 24.683 (13.798–35.210) | 44015.759 (43448.890–44171.478) |

The following median allocation counts and cumulative requested bytes count
calls to the benchmark's scalar C++ `operator new` on the calling thread during
the timed in-process command. They do not count allocator metadata, aligned or
non-C++ allocations, and are not peak memory. TCP uses a separate owner thread,
so its counts are unavailable; CSV zeros in TCP rows are placeholders.

| Workload | Background | Engine allocations / bytes | Exchange allocations / bytes |
|---|---:|---:|---:|
| Noncrossing | 10 | 5 / 272 | 41 / 3,952 |
| Noncrossing | 30 | 5 / 272 | 83 / 9,616 |
| Noncrossing | 100 | 5 / 272 | 224 / 27,992 |
| Single fill | 10 | 4 / 280 | 42 / 4,384 |
| Single fill | 30 | 4 / 280 | 84 / 10,048 |
| Single fill | 100 | 4 / 280 | 225 / 28,424 |

## Interpretation

The exchange results and allocation counts rise with background size, while the
direct engine results remain sub-microsecond in this small sample. This is
consistent with the exchange copying all books and retained session state on
each candidate command; it measures the overhead rather than proving a profile
of each allocation. The exchange wraps the optimized matching command in a full
state snapshot, so Phase 4's optimized matching path still runs but its gains
can be dominated by snapshot cost. Each submit holds a process-wide ID mutex
during cloning, matching and report staging. Cancellation also snapshots state
but does not acquire that mutex. See the [design](phase5b-exchange-integration-design.md).

The approximately 44 ms TCP medians dominate the measured round trip. Delayed
acknowledgments, Nagle interactions, poll scheduling and WSL virtualization are
possible contributors, but no packet trace or profile established their cause.
This suite does not test multiple price levels, load, sustained throughput,
concurrent gateways, large books or slow-reader tail behavior. A longer original
matrix was stopped during setup because the loopback fixture took too long; the
reported values are exclusively from the completed reduced matrix above.

For Phase 5C, establish repeatable multi-run distributions on controlled Linux
hardware, profile snapshot allocation and packet behavior, and compare candidate
prevalidated plans or bounded journals against the current atomic reference.
Preserve ownership, failure atomicity and report-capacity guarantees in any
subsequent optimization.
