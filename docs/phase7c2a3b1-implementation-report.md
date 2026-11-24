# Canonical decision prerequisite report

The inspector was finalized separately. This uncommitted milestone preserves
the fixed-size command input codec and adds an opt-in bounded post-install
summary built from the private candidate. It captures the new submit ID,
exchange counters, trade deltas, recipient-tagged private reports and
authoritative public trade/book events in event order; rejected operations return no candidate outputs. The summary cannot be
used to install state and is returned only after nonthrowing installation.
It now retains exact command input, allocator high-water, per-symbol matching
command/event/trade counters, book revision and feed sequence before and after,
plus the initiating position and last matching private report when available.
Cancel samples allocator high-water under the existing mutex while exclusive
registration prevents competing live allocators. These are bounded partial
verification fields, not proof of full state equivalence.
Allocation or summary-limit failures occur before installation. It does not
capture subscriber-specific gap transitions, implement a complete decision
codec or participate in the normal gateway hot path. The complete decision
remains blocked on the other missing interfaces described in
[the design](phase7c2a3b1-canonical-decision-design.md).

The codec allocates no heap memory: it stores and returns fixed `std::array`
values, and its operations are `noexcept`. This is a source-level property,
not an allocation count for full exchange preparation. Gateway behavior and
the provisional inspector are unchanged. No storage, fsync, durable success,
recovery or session replay has been added.

Final summary tests cover resting and partial-fill reports and trade identities,
cancellation and rejection, multi-level FIFO, wrong context and shared-owner
refusal, allocation-failure sweeps, and a 1,025-fill limit rejection compared
against exact live continuation state. Existing suites cover additional abort,
counter and gateway cases. The summary is not a complete pre-install decision.
The prior summary-only review passed **204/204 CTest cases** in each of Debug,
Release and ASan/UBSan in 1.59 s,
1.25 s and 6.46 s respectively. Matching and simulation CLIs, Phase 7B replay,
CHROVIEW comparison and CHRSTATE isolated restoration passed. Replay verified
12 records and four trades. Formatting and Git whitespace checks passed; web
tests were not repeated because gateway behavior did not change.

On WSL2 Ubuntu, GCC 13.3 Release, the optional
`chronos_canonical_command_probe` ran three repetitions of one million varying
submit inputs. Construction plus copying the 52-byte encoded value averaged
40.31, 39.18 and 42.77 ns per loop iteration; the loop includes index and sink
work and these are not per-command latency percentiles. Configure with
`-DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON` to reproduce. WSL scheduling and short
samples limit comparison. Prepared-decision construction, outcome serialization,
submit/cancel latency, allocation counts and allocator lock duration were not
measured in that command-only probe; it is not integrated in preparation.

The additional `chronos_prepared_summary_probe` is enabled by
`-DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON`. It runs five warmups and 100 measured
single-operation samples per cell with fixture construction excluded. On WSL2
Ubuntu, GCC 13.3 Release, an empty-book noncrossing submit had a 715 ns median
and 755 ns descriptive p95 for existing prepared execution, versus 748 ns and
784 ns with the summary. A cancellation of one resting order had medians of
1,165 ns versus 1,253.5 ns and p95s of 1,221 ns versus 1,312 ns. These
figures include preparation, installation and local cleanup and do not isolate
summary construction. The scalar `operator new` probe observed 24 allocations
and 2,440 requested bytes versus 25 and 2,632 for submit, and 38 and 3,864
versus 39 and 3,960 for cancel. It excludes allocation forms not routed through
that override and is not a peak-memory measurement. Small samples and WSL2
variance preclude a production-latency claim. Raw data is retained locally in
ignored `build/prepared-summary-probe.csv`. Outcome encoding, decoding and full
decision size are unavailable and were not measured.

## Public-event capture performance

`chronos_market_capture_probe` measures a three-level, three-fill market sweep
with zero or eight subscribers, five warmups and 100 samples per cell. Fixture
creation and subscription setup are excluded. WSL2 Ubuntu on an Intel
i7-13620H, GCC 13.3 Release produced the following medians / descriptive p95
in nanoseconds. Scalar allocation counts and requested bytes include the entire
measured command and exclude allocation forms not routed through the probe's
scalar `operator new` override.

| Subscribers | Mode | Median / p95 ns | Allocations / bytes |
| ---: | --- | ---: | ---: |
| 0 | Direct, capture disabled | 1,731.5 / 1,828 | 43 / 6,120 |
| 0 | Prepared, capture disabled | 2,552.5 / 2,646 | 70 / 9,184 |
| 0 | Summary, capture enabled | 2,655.5 / 2,774 | 75 / 10,632 |
| 8 | Direct, capture disabled | 4,197 / 5,133 | 83 / 17,960 |
| 8 | Prepared, capture disabled | 6,087 / 6,749 | 134 / 25,184 |
| 8 | Summary, capture enabled | 6,726 / 13,605 | 139 / 26,632 |

A separately built **temporary benchmark-only** source variant disabled the
summary's capture sink and associated capture-consistency checks. Its summary
medians were 3,009.5 ns with zero subscribers and 7,264 ns with eight, with
72 / 10,072 and 136 / 26,072 scalar allocations / bytes respectively. Runs
were sequential, not interleaved; WSL scheduling and run-to-run variation make
their timing differences inconclusive. This synthetic variant is not a supported
production mode. It captured zero events versus four in the normal summary.
To reproduce the comparison, copy the source to a separate disposable tree,
change only the summary calls' `capture_public` arguments to `false`, and omit
the `summarize_prepared` public-event consistency checks in that copy. Configure
both trees in Release with `-DBUILD_TESTING=OFF` and
`-DCHRONOS_ENABLE_EXCHANGE_BASELINE=ON`, then run the same probe. Do not use the
modified copy for trading or correctness validation.
The 560 additional requested bytes and three scalar allocations in this fixture
match vector growth for four events; `sizeof(MarketEvent)` was 80 bytes on this
build, excluding vector metadata and capacity overhead. These figures are not
production feed latency. Raw benchmark CSVs are retained in ignored `build/`.

## Public-event capture validation

The final Debug, Release and ASan/UBSan suites each passed **209/209** CTest
cases (1.66 s, 1.25 s and 6.66 s). Focused tests compare explicit trade and
book values with zero, one and two subscribers, overflow a delivery queue,
verify the gap and resnapshot sequence, exercise multi-level and multi-symbol
ordering, abort a prepared command, and verify the candidate sink is cleared
before later ordinary commands. The capacity regression rejects a 1,025-fill
candidate without changing state, then accepts 1,024 fills and exactly 1,025
captured events with continuous feed sequences. An isolated
test-only actual vector allocation failure both verify exact live state and ID
continuity on failure. The initial sanitizer run found an allocation-family
mismatch in the new fault-test harness; adding its missing nothrow allocation
overload fixed it, and the complete sanitizer suite was rerun without suppression.
The capture-enabled summary has no caller-controlled abort callback; the abort
case exercises the existing prepared path, while the capture-failure tests
exercise candidate discard after capture begins.

Matching and simulation CLI demonstrations passed. Phase 7B replay verified
12 records and four trades; CHROVIEW comparison and CHRSTATE isolated
restoration passed. Shared/exclusive gateway and protocol-v3 tests ran in the
full suites. Web tests were not rerun because the gateway and wire behavior do
not enable the new summary capture path. Formatting and Git whitespace checks
passed. No journal I/O, durable authorization or recovery was implemented.

## Decision identity and counter follow-on

The opt-in post-install summary now includes the exact 52-byte command value;
the initiating session's position before and after when its symbol is known;
the final initiator private report when present; allocator high-water before
and after; and per-symbol matching command/event/trade counters, book revision,
and feed presence and sequence before and after. Rejections preserve their
precise `Result`, have no new ID or generated output, and retain before/after
counter values. These fields are captured from live and privately staged state
before installation. An accepted summary is returned only after the existing
nonthrowing state and ID installation. The snapshot is not a pre-install handle.

The complete canonical decision remains **unimplemented**: exchange identity
binding, counterparty position/risk effects, full ownership and book-state
verification, subscriber gap transitions and a bounded deterministic complete
decision encoder/decoder are missing. The existing 52-byte codec encodes only
command input, and the provisional inspector still treats outcomes as opaque.
No digest or partial counter set is represented as proof of full state equality.

Final synchronized validation passed **212/212 CTest cases** in each of Debug,
Release and ASan/UBSan (1.58 s, 1.24 s and 6.26 s). Tests added in this
follow-on cover exact command golden bytes, allocator high-water at exhaustion,
counter and feed snapshots for accepted, rejected and cancelled commands,
duplicate and capacity rejection, and signed initiating position. The matching
and simulation CLIs passed; Phase 7B replay verified 12 records and four trades;
CHROVIEW comparison and isolated CHRSTATE restoration passed. Shared/exclusive
gateway and protocol-v3 tests ran in the full suites. Web tests were not rerun
because gateway-observable behavior did not change. Formatting and Git
whitespace checks passed. An earlier sanitizer run mixed objects from source
updates made during the build and failed; all affected targets were rebuilt from
a synchronized source copy and the unsuppressed full sanitizer suite then passed.

`chronos_decision_capacity_probe` measures a privately prepared 128-price-level
sweep, with 20 samples and two warmups, zero subscribers and fixture setup
outside timing. On WSL2 Ubuntu, GCC 13.3 Release, it reported a 132.291 µs
median and 112.474–256.203 µs range, with 861 scalar allocations and 356,960
requested bytes per sample. Each result had 128 trades, 257 private reports and
129 public events. The allocation override does not count every allocation form;
this is not incremental peak memory. The timed interval includes preparation,
summary construction and installation, not encoding or decoding. Raw samples
are in ignored `build/canonical-capacity-probe.csv`.

For empty-book submit and one-order cancel, 100-sample medians were 931.5 ns
and 1,608 ns for existing prepared execution versus 1,219 ns and 1,963 ns for
the metadata summary. Scalar allocations/bytes were 24/2,592 versus 26/2,864
for submit and 38/4,016 versus 40/4,192 for cancel. Fixture setup is excluded;
these include local cleanup and do not isolate individual components. WSL2
scheduling and sample ranges make the figures unsuitable as production
latencies. No complete outcome encoding or decoding cost can be measured yet.
