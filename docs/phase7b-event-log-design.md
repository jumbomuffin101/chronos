# Phase 7B event log and replay verifier

> Implementation note: the proposal below is retained for design history. The
> implemented version 1.0 differs in several details; the authoritative byte
> layout and semantics are in [the implementation report](phase7b-implementation-report.md).
> Its version and configuration are carried in a checksummed header record, all
> payload fields are 64-bit words, and ingress is scoped to one isolated run.

This is a design for a bounded, versioned verification log. It does not
implement persistence, crash recovery, authenticated sessions, or exactly-once
delivery. Phase 7A's FNV fingerprint is not a log or a complete state proof.
The current exchange owner remains the sole sequencer and mutator; neither the
gateway nor simulator writes a competing exchange event stream.

## Admission and file format

Introduce a checked global `admission_sequence` at the owner boundary for every
admitted typed command, including commands subsequently rejected by exchange
validation, risk, capacity, or counters. Malformed wire frames, disconnected
peers, and commands refused by a full ingress queue before admission do not get
an admission sequence. They may be counted in separate transport diagnostics,
but are not silently represented as accepted exchange commands. Stop or admin
operations must be explicitly typed and sequenced if they can affect state.
Counter exhaustion closes admission before wrap.

Proposal: files begin with magic `CHRONOS\0`, a uint16 format major/minor, a
uint32 header length, and a uint32 feature bitmap. All integers are little-endian
and fixed-width; signed values use two's-complement bit patterns. The header
contains a run identifier, exchange/schema and generator versions, seed,
scenario, complete initial exchange and risk configuration, ID namespace
policy, and explicitly bounded symbol/participant arrays. Strings are bounded
UTF-8 with explicit lengths; no native C++ layout, padding, pointers, or host
endianness appears on disk. Reserve fields must be zero. Unsupported major
versions, mandatory features, invalid lengths, and impossible configuration are
errors; minor-version compatibility must be specified per record type.

Each record has a uint32 total length, uint16 record type, uint16 flags, uint64
admission sequence, uint32 payload length, payload, and trailing uint32 CRC32C
covering the preceding record bytes including length. Limit total record length
to 1 MiB and file/run size through explicit configuration. Records cannot cause
an allocation before their lengths and limits are checked. CRC32C detects
accidental corruption, not malicious modification. A run-start record binds the
header and initial configuration. Versioned `COMMAND`, `OUTCOME`, and clean
`RUN_END` records carry, respectively:

- The exact admitted command type, session/run-local participant, request and
  client-order IDs, symbol, side, order type, integer price and quantity, or
  cancel target. Preserve generator step and version where applicable.
- Accepted or rejected status and precise reason; the actual exchange command
  sequence only when the exchange issued one; ordered trades and private reports
  including their owner, event sequence, trade ID, price, quantity and remaining
  quantity; and changed market-feed sequences and relevant counter values.
  Rejections do not acquire invented exchange sequence numbers.
- Final counts, terminal sequence and a versioned state digest. Absence of a
  clean end means the run is incomplete, not successfully verified.

The exchange currently allocates process-wide internal order IDs. Record their
mapping to deterministic run-local order labels and compare replay using that
mapping, while retaining the original ID as diagnostic data. Never ignore
participant identity, client-order correlation, FIFO order, risk rejection,
trade price/quantity, or report recipient. Matching trade IDs and feed counters
retain their actual symbol/domain scope; do not merge them into one fake counter.
Large outcomes must be split into explicitly numbered, bounded fragments with a
declared total count and aggregate checksum, or fail admission before mutation;
the implementation must choose and test one policy before shipping.

## Transaction and acknowledgment boundary

The owner reserves bounded memory for the command, possible outcome, and writer
handoff before calling the exchange. A proposed verifier-first 7B flow writes a
`COMMAND` record before invoking `submit`/`cancel`, applies the existing atomic
exchange transaction, then appends the matching `OUTCOME` record before admitting
the next command. The writer returns explicit success or failure for each append.
An outcome is authoritative only after the exchange call returns; a rejected
operation records its reason and unchanged-state result. Reproducibility requires
the actual admission order and initial state, not merely the PRNG seed.

If the command append fails, do not call the exchange. If an outcome append or
flush fails **after** a successful exchange commit, do not claim rollback: stop
admission, retain a bounded in-memory diagnostic of the committed outcome if
available, return a distinct committed-but-unlogged/unknown error to the local
operator, and mark the file/run unverifiable. Do not acknowledge a logged
outcome to a client as successful if its required record was not accepted by the
writer. An intent without an outcome is incomplete; never synthesize a success
or replay it as committed. This policy makes the gap visible but does not make
the command/log pair atomic against a process or machine crash.

Define acknowledgments precisely: an in-memory exchange acknowledgment, a
successful buffered append, an OS write, and a durable `fdatasync`/`fsync` are
different milestones. 7B should default to verification of cleanly completed
runs and make flush policy explicit in the header and result. A write return or
buffer flush is not a durable-storage promise. Durable acceptance requires a
separately designed write-ahead/commit protocol, filesystem and directory sync
policy, and crash-injection evidence; merely adding fsync after mutation cannot
guarantee recovery or exactly-once reporting.

## Replay and verification

The reader validates magic, supported versions, lengths, limits, checksums,
record order and configuration before applying anything. Require each admitted
sequence exactly once, starting from the recorded initial sequence, with one
command and one complete outcome in order; detect gaps, duplicates, overflow,
unmatched outcomes and incomplete fragments. A truncated final record, torn
payload, bad checksum or missing clean end is an explicit incomplete/corrupt
result. Do not silently skip records or discard an interior corruption. An
optional future recovery policy for a torn tail belongs to 7C, not this verifier.

Construct a fresh isolated exchange using the recorded initial configuration,
recreate the participant/session mapping, and replay the admitted typed commands
on its owner thread. Compare each returned outcome, normalized order mapping,
ordered trade and execution-report contents, ownership and relevant counters.
Report the first mismatch with file offset, admission sequence, record type,
field name and expected/actual values. Verify symbol-local ordering separately
from global admission ordering.

Top-of-book and positions are insufficient to establish complete state equality.
Before claiming full verification, add a read-only, owner-thread canonical state
export or equivalent test interface covering every resting order in FIFO order,
ownership and client mapping, positions, all risk reservations and limits,
session/request state, and exchange/matching/event/feed counters. Version that
representation and compare it at defined checkpoints and end of replay. This is
a proposed dependency; current Phase 7A does not expose all of it. A successful
verification shows deterministic equivalence for the compared state and recorded
run only; it does not establish crash recovery or private-report delivery.

## Test plan

- Golden binary fixtures and byte-level round trips across supported compilers;
  explicit endian, signed boundaries, maximum lengths and unknown versions.
- Multi-symbol interleavings, partial fills, cancellations, owner routing,
  accepted and rejected commands, risk state and normalized internal IDs.
- Truncation at every byte boundary, short writes, malformed lengths, bit flips,
  checksum failures, missing/duplicate/gapped sequences, incomplete fragments,
  unknown record types and exhausted counters.
- First-mismatch diagnostics for commands, reports, trades, FIFO book state,
  positions, reservations and counters; repeated deterministic runs.
- Allocation, file-open/write/flush failures, bounded queue saturation, failure
  before mutation and failure immediately after a committed exchange operation.
- Large bounded streams, maximum record/run limits, backpressure, cancellation
  and restart of the verifier; no tests that depend on wall-clock ordering.

## Performance plan

Benchmark serialization/deserialization latency and allocations separately from
exchange execution; measure CRC32C overhead with fixed payload sizes, append
throughput and p50/p95/p99 latency under bounded queue load, and replay commands
per second plus peak memory for increasing record and book sizes. Compare
in-memory capture, buffered append, OS writes, and explicitly configured flush
and sync policies as distinct workloads. Include setup and reset outside timed
regions, fixed seeds, sample counts, compiler, filesystem, hardware and WSL2
limitations. Report storage failures and backpressure; do not extrapolate a
buffered-file benchmark into a durable-storage or production-latency guarantee.
