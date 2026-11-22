# Phase 7C.2A.3a implementation report

The exclusive gateway now uses the restricted in-memory prepared submit and
cancel calls. Rejection-frame space is reserved before preparation, while
accepted private reports and public effects are retained in the staged exchange
state. Shared gateway behavior and protocol-v3 encoding remain unchanged. See
[the design](phase7c2a3a-publication-gate-design.md) for output classification,
capacity and delivery limits.

Regression tests cover an actual pre-install barrier with trader and subscriber
sockets, exclusive two-client partial fills and cancellation ownership,
business/risk rejections and request reuse, and a FIFO multi-level sweep.
Existing tests cover prepared abort, exception and allocation-failure cleanup,
counter exhaustion, wrong owner context and shared gateway compatibility. The
new isolated barrier is test-only. It does not itself establish a durable
publication protocol. The final review injects `std::bad_alloc` before
preparation, at output reservation, and immediately before installation. It
checks ID continuity, aborted cancellation, disconnect before and after
installation, concurrent stop, and exact-fit versus insufficient output space.
These are injected exceptions at specified boundaries, not exhaustive real
allocator and kernel fault injection. An accepted-but-aborted command closes
its session; the retry tested uses another session. The hooks are test-only.

## Validation

The final Debug, Release and ASan/UBSan suites each passed 188/188 tests in
1.32 s, 1.16 s and 4.54 s respectively. The real-gateway web suite passed
20/20 and browser end-to-end suite
passed 3/3 in each gateway mode. The exclusive web run used a temporary test
launcher linked against the built gateway, since the production CLI defaults
to shared mode. The matching CLI, mixed simulation, five-command record/replay,
CHROVIEW comparison and CHRSTATE isolated restoration passed; replay verified
12 records and four trades. Formatting and whitespace checks were run separately.

## Reproducible performance sample

On WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3, Release, configure with
`-DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON` and run
`chronos_gateway_gate_baseline`. Each sample builds a fresh two-client fixture
outside the timed interval, performs two warmups, and times 21 independently
verified operations per workload and mode. Noncrossing and cancellation end
after the relevant private reports; crossing ends after both participants'
fill reports. The measurements include TCP, scheduling, exchange work and
output draining, not isolated preparation or publication. They are not
production hardware latency measurements.

| Workload | Shared median / descriptive p95 µs | Exclusive median / descriptive p95 µs |
| --- | ---: | ---: |
| Noncrossing | 187.74 / 300.06 | 244.85 / 391.03 |
| Cancellation | 206.79 / 324.44 | 199.77 / 294.77 |
| Two-client crossing | 258.17 / 406.70 | 226.93 / 344.92 |

Only 21 samples were collected per cell; the nearest-rank p95 is descriptive
and the large observed ranges make small differences inconclusive. Raw samples
are retained locally in ignored `build/phase7c2a3a-gateway-baseline.csv`.
The existing order-count benchmark was also extended with an exclusive mode;
its nine samples per cell are in ignored `build/phase7c2a3a-size-baseline.csv`.
Gateway-thread allocations and preparation-only versus publication-only time
were not measured. Prior exchange-level probing found 18 versus 24 scalar
allocations for ordinary versus prepared submit and 19 versus 38 for cancel in
a small fixture; it does not measure the new gateway's total allocations.
Preparation adds an exchange-state clone on top of existing staging.

A final repeat with 21 samples per cell gave shared versus exclusive medians
of 178.00 versus 127.29 µs for noncrossing, 117.59 versus 90.30 µs for cancel,
and 135.66 versus 162.69 µs for crossing. The respective ranges were
73.10–367.30 versus 72.24–254.94 µs, 63.85–192.69 versus 63.24–324.39 µs,
and 73.87–344.18 versus 75.46–300.81 µs. This repeat is too variable for an
optimization claim; multi-level fills and gateway-thread allocations were not
separately timed.

No journal, fsync, crash recovery, durable delivery or authenticated reconnect
is implemented. Client-observed outcomes may be uncertain after disconnect.
