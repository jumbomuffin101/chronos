# Phase 7C.1 implementation status

This is a partial implementation: `chronos::snapshot` provides only a
read-only, versioned diagnostic checkpoint and comparison interface. It does
not implement `ExchangeRestorer`, state-equivalent reconstruction, a snapshot
CLI, log-suffix compatibility, or recovery. The format deliberately uses
`CHROVIEW` rather than presenting itself as a restorable snapshot. See the
[design and limitation analysis](phase7c1-snapshot-design.md).

The reader enforces an 8 MiB encoded-size limit before allocating its payload;
it validates header, lengths, checksum and basic metadata and leaves the output
unchanged on failure. The writer reports stream errors. Tests cover a populated
book, comparison after cancellation, all truncations of a small checkpoint,
corruption, unsupported version, oversized length and output stream failure.
The reader does not independently validate semantic state relationships.
Capture now uses a bounded preflight and bounded verification traversal; the
encoded-size cap is not an exact limit on temporary allocation overhead.

The original 7C.1 run passed 121/121 CTest cases in each configuration. After
7C.1A hardening, Debug, Release and ASan/UBSan each passed 124/124. The existing
matching CLI, five-command simulation and record/replay CLI passed; formatting
was checked with `bash scripts/format.sh --check`.

The optional Release `chronos_checkpoint_bench` measured only diagnostic
capture, with setup excluded. On the WSL2 development machine (Intel
i7-13620H, GCC 13.3, Google Benchmark, three repetitions, 0.03-second minimum),
median repetition-mean wall times were 159 ns for an empty book, 6.439 µs
for 100 resting orders, and 126.464 µs for 1,000 resting orders. These are not
individual-operation percentiles or production measurements. No serialization,
reconstruction, or memory-overhead benchmark is claimed.

No snapshot reconstruction, follow-on equivalence, or snapshot demonstration
is claimed. Completing those requires an explicit
allocator and trusted-import design as described in the design document.
