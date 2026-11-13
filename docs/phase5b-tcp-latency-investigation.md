# Phase 5B.1 TCP latency investigation

## Trace boundaries

Tracing is optional. `TransportOptions::trace` accepts a nonblocking, `noexcept`
callback and caller-owned context. When no callback is supplied, no diagnostic
timestamps are taken. The gateway callback runs on the socket/exchange owner
thread; client callbacks run on the thread invoking `Client`. Callers must make
the callback safe for concurrent use and keep its context alive until both sides
are finished. The benchmark uses a fixed-capacity in-memory buffer and processes
it only after the gateway has stopped. Instrumentation adds clock calls and
callback overhead; its measurements are not production latency.
Exceptions, rejected commands or disconnections can leave a trace incomplete;
the hooks do not guarantee one terminal-report event for every request.

All timestamps use `std::chrono::steady_clock` in one process. Client send start
precedes encoding and send end follows `write_all`. Frame receipt means a full
frame has been decoded, not the first byte received. Command enqueue is after
insertion into the bounded ingress batch; dispatch is immediately before the
owner processes that command. There is no cross-thread engine queue: networking
and exchange mutation share the same owner thread. Transaction start/end enclose
the exchange API call, including state cloning and matching. Report queued means
the selected private report was encoded into the bounded socket output buffer;
it is later than the exchange's staged report creation.

The socket-write timestamp is taken immediately before the first successful
`send()` attempt for that output; its callback is made after the call succeeds.
It is not a packet-transmit or remote-delivery timestamp. A partial write can
produce multiple events; this benchmark selects the first successful write for
the terminal report. Client response receipt is when a decoded frame is returned
to the calling client thread, which may be later than kernel receipt. A server
can process bytes before the client's send call returns, so the interval between
client send end and decoded server frame may be negative. Callback order across
threads is not a substitute for timestamp order.

An initial low-latency run exposed exactly this cross-thread issue: a timestamp
taken *after* the server's `send()` returned could be recorded after the client
had already received the bytes. The benchmark retained strict causal checks and
the timestamp was moved to before the successful send attempt; the failed
partial run was discarded and the matrix repeated.

## Method and limits

Use a Linux Release build and run the controlled matrix:

```sh
cmake --preset release -DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON
cmake --build --preset release --target chronos_tcp_latency
python3 benchmarks/run_tcp_latency.py --samples 50 --repeats 3 \
  --output build/tcp-latency.csv
python3 benchmarks/summarize_tcp_latency.py build/tcp-latency.csv
```

The diagnostic matrix varies server and client `TCP_NODELAY` independently,
with 10 and 100 background sell orders, noncrossing limit buys and single-fill
market buys. It uses three fresh gateway fixtures per condition, five warmups
and 50 measured commands per fixture. A persistent connection and book are used
within each fixture: noncrossing orders are cancelled after measurement and
single fills reduce the first resting order. Historical request, order and trade
records therefore grow during a run. Both settings use the same deterministic
workload, but these samples are not independent fresh-book trials and are not
directly interchangeable with the Phase 5B nine-sample baseline. The benchmark
checks accepted, resting, fill and cancellation reports and verifies execution
price. Raw trace deltas are in [the CSV](assets/phase5b1/tcp-latency.csv).

The matrix records send duration, cross-thread send-end-to-decoded-frame,
enqueue-to-dispatch, exchange transaction, transaction-end-to-terminal-report
queueing, report-queue-to-write, write-to-client-return, and total duration.
Gateway frame-to-write duration is derived from the timestamp differences.
Percentiles are nearest-rank and p99 is descriptive only: 150 correlated samples
per condition across three runs do not establish a reliable tail distribution.

The host is WSL2 Ubuntu 24.04.1 with Linux `6.6.87.2-microsoft-standard-WSL2`,
GCC 13.3.0 and an Intel Core i7-13620H. Host scheduling and CPU frequency are
uncontrolled. `strace`, `tcpdump`, `tshark`, `perf`, and `bpftrace` were not
available in this WSL environment, so no packet or syscall trace confirms the
packet-level mechanism. The socket-option intervention and application
timestamps are the evidence, not a production-network measurement.

## Findings and correction

The dominant delay is sensitive to `TCP_NODELAY` **on the accepted server
socket**. With both endpoints using the prior setting, the terminal report was
submitted to `send()` quickly, then the client typically did not return it for
about 44–45 ms. Enabling `TCP_NODELAY` only on the client did not remove this
delay; enabling it only on the server did. This supports server-side TCP
buffering associated with small successive writes and Nagle behavior as the
cause of the observed plateau. A delayed-ACK interaction is plausible, but the
specific ACK timing and packet sequence were not observed and cannot be proved
without packet or kernel tracing. The trace interval also includes the send
syscall, kernel buffering, client scheduling, decoding and callback overhead.

The gateway queues one report at a time and may make successive small writes.
TCP is a byte stream: the decoder already handles arbitrary fragmentation and
coalescing, and the correction does not change framing. The 25 ms `poll` value
is a maximum wait and readiness wakes it early; measured decoded-frame-to-write
times were far below the plateau. The owner thread dispatches in the same poll
iteration, so no cross-thread queue notification is involved. There is no
intentional request-path sleep or busy wait. WSL2 and host scheduling remain
uncontrolled and may contribute to the remaining variability.

The gateway now enables and verifies `TCP_NODELAY` on accepted sockets by
default. A failure to set or verify the option closes that connection through
the existing socket-setup failure path. The client default remains Nagle
enabled; `TransportOptions` permits an explicit setting for controlled tests.
An unspecified setting preserves each endpoint's default even when a trace
callback is supplied.
The matrix was recorded before a follow-up API change made the socket setting
optional, so that supplying only a trace callback preserves each endpoint's
default. Explicit `true` and `false` settings in the matrix have the same
socket behavior. A final-build five-sample spot check of the 100-order,
server-only NODELAY single-fill case passed, with a 68.601 µs median; it is not
a replacement for the repeated matrix.
Buffers, partial-write handling, timeouts, sequencing, reports and transaction
semantics are unchanged. Socket-option and trace-boundary regressions check
configuration and behavior without asserting an environment-dependent latency
threshold.

## Repeated controlled results

The table shows microseconds for 100 background orders, 150 measured samples
per cell across three fixture runs. Values are p50 / p95 / descriptive p99.
The client setting alone does not eliminate the plateau. The complete 10- and
100-order data for all configurations is in the raw CSV.

| Server NODELAY | Client NODELAY | Workload | Round trip p50 / p95 / p99, µs |
|---:|---:|---|---:|
| 0 | 0 | Noncrossing | 45450.719 / 45958.837 / 46310.936 |
| 0 | 1 | Noncrossing | 45424.868 / 46182.141 / 46994.086 |
| 1 | 0 | Noncrossing | 161.963 / 393.538 / 456.501 |
| 1 | 1 | Noncrossing | 110.425 / 211.010 / 298.893 |
| 0 | 0 | Single fill | 44039.879 / 46133.103 / 46308.120 |
| 0 | 1 | Single fill | 44018.149 / 45956.166 / 46591.967 |
| 1 | 0 | Single fill | 116.734 / 190.937 / 233.538 |
| 1 | 1 | Single fill | 103.262 / 281.779 / 438.168 |

For the 100-order noncrossing case, the following components compare the old
setting with the new server-only default. They overlap with other intervals and
their independently calculated percentiles must not be added together.

| Component | Before p50 / p95 / p99, µs | Server NODELAY p50 / p95 / p99, µs |
|---|---:|---:|
| Decoded frame to terminal report write start | 104.644 / 225.665 / 377.771 | 71.522 / 143.694 / 200.788 |
| Ingress enqueue to owner dispatch | 0.755 / 2.294 / 3.148 | 0.473 / 1.501 / 1.689 |
| Exchange API transaction | 37.758 / 79.943 / 166.437 | 25.888 / 55.219 / 78.376 |
| Terminal report write start to client return | 45202.851 / 45813.857 / 46185.079 | 27.947 / 100.908 / 147.173 |

## Comparison with the prior fresh-fixture benchmark

The original uninstrumented benchmark was rerun after the correction with the
same one-warmup, nine-sample fresh fixtures used in Phase 5B. At 100 background
orders, the noncrossing TCP median changed from 43944.828 µs to 145.205 µs,
and the single-fill TCP median from 44015.759 µs to 71.897 µs. The corresponding
after-run exchange API medians were 13.829 µs and 29.777 µs. These are small,
separate samples and should not be interpreted as stable tail estimates. The
[after-run CSV](assets/phase5b1/comparable-after.csv) contains all samples.

For Phase 5C, repeat the experiment on controlled native Linux hardware with
packet capture and independent processes, include slow readers and congestion,
and assess TCP_NODELAY's packet-rate tradeoff. Separately benchmark exchange
snapshot costs and preserve the existing transaction and backpressure guarantees.
Do not infer durability, application acknowledgment or production performance
from this local experiment.

## Validation

Debug, Release and ASan/UBSan builds each passed **72/72** CTest cases. The
two-client loopback demonstration delivered a three-unit fill at the resting
price of 100 to both participants, with two units remaining for the seller.
The existing matching CLI and the formatting check passed, and Git's tracked
whitespace check was clean. The benchmark verified every recorded command and
the final-build spot check passed. GNU make emitted WSL/Windows-mounted
filesystem clock-skew warnings during incremental builds; affected targets
were rebuilt and tests rerun.
