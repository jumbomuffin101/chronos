# Phase 7C.2A.3b: durable journal implementation plan

Status: design only. The current gateway has an in-memory installation gate,
not a journal authorization interface. The staged representation, file format,
manifest, recovery and fault guarantees below are proposed requirements; none
are implemented or verified. This plan does not authorize live publication of
restored exchanges or promise authenticated sessions, private-report replay,
exactly-once delivery or client receipt.

## Implementation sequence and ownership

1. Define an owner-confined, immutable canonical command and outcome; enforce
   all bounds before installation and test exact deterministic encoding.
2. Implement a single-writer journal and read-only offline inspector, with an
   injectable I/O interface and no gateway integration. Verify record and
   durable-prefix parsing, checksums and strict size limits.
3. Implement manifest replacement and qualified-filesystem tests, including
   every partial-write, sync, rename and directory-sync failure.
4. Extend the exclusive gateway's owner-controlled gate to accept only a
   successful internal durable decision; keep shared non-durable operation
   explicit. Prove all private, public and rejection publication paths obey the
   same boundary. Freeze admission on ambiguous I/O.
5. Implement and separately review restart reconstruction and live publication
   prerequisites in the recovery milestone. Until then, a restart after a
   committed record must fail closed rather than start a fresh live exchange.

Only the exclusive owner writes a journal; an OS-level exclusive file lock and
process allocation registration must both be held before admission. Define lock
acquisition and release ordering and reject a second writer. A journal has a
random 128-bit exchange UUID and a 128-bit journal epoch, generated before its
first durable manifest. Both are identities, not authentication credentials.
Persist and verify them across every file; do not silently regenerate either
when opening an existing journal. LSNs start at 1, advance by one per durable
terminal decision, never wrap or repeat, and exhaust the writer at `UINT64_MAX`.

## Canonical outcome and encoding

Use byte order big-endian and fixed-width unsigned integers except explicitly
signed two's-complement `i64` price and position fields. Version 1 requires
the exact schema below; reject unknown versions, nonzero reserved bits, unknown
enums, noncanonical order, excess counts, and trailing bytes. Every variable
array uses a `u32` count followed by fixed-schema entries. Check all additions,
multiplications, conversions and maximums before allocating or encoding. Set
independent hard limits: 1 MiB per record, 1,024 trades, 4,096 private reports,
4,096 public effects and 1,024 affected sessions per outcome. These are proposed
implementation limits, not current exchange guarantees; preparation must reject
or defer before installation if a command exceeds them. A future larger version
requires explicit compatibility and migration, not relaxed v1 parsing.

The canonical command contains `u8 kind` (1 submit, 2 cancel), `u64 session`,
`u64 request`, `u64 client_order`, `u32 symbol`, `u8 side`, `u8 order_type`,
`i64 price`, `u64 quantity`; cancel requires the last five fields to be zero.
The outcome starts with `u8 decision` (1 accepted, 2 rejected), `u16 result`,
`u8 has_internal_id`, `u64 internal_id` (zero when absent), and `u64` before
and after values for command, event and allocator high-water counters. Require
accepted submit to have its reserved nonzero ID; cancellation and rejection
must follow their explicitly documented ID semantics and never invent one.
Then encode, in deterministic execution order:

- Trades: `u64 command_seq, trade_id, buyer_session, seller_session,
  buy_internal_id, sell_internal_id; u32 symbol; i64 price; u64 quantity`.
- Private reports: `u64 destination_session, report_seq, command_seq, request,
  client_order, internal_id, trade_id; u32 symbol; u8 kind; i64 price; u64
  quantity, remaining`. Preserve each destination's report order.
- Public effects: `u32 symbol; u64 feed_seq; u8 kind; i64 bid_price,
  ask_price, trade_price; u64 bid_quantity, ask_quantity, trade_quantity`.
  Include gap transitions and explicitly encode recipient identity or define a
  separately versioned deterministic subscription projection before use.
- Feed counter effects: sorted unique `u32 symbol, u64 before, u64 after`.

Append a `u16 verification_algorithm` and 32-byte SHA-256 digest over the
versioned exact continuation-state encoding, including book, ownership, risk,
requests and allocator high-water. The actual continuation encoding and its
normalization must be versioned and frozen before enabling the writer; a digest
alone cannot reconstruct state or replace invariant checks. Hash calculation,
canonical outcome creation, output-capacity checks and all allocations must
finish privately before any record is written or state installed. A rejected
business outcome has an explicit decision and no fabricated ID; whether it
consumes an LSN is defined below, independently of exchange command counters.

## Journal v1 and terminal decisions

The file header is exactly: 8 ASCII bytes `CHRJRNL1`, `u16 version=1`,
`u16 header_bytes=48`, `u32 flags=0`, 16-byte exchange UUID and 16-byte epoch.
Each record begins with `u32 total_bytes` including header, payload and CRC,
`u16 version=1`, `u8 type`, `u8 flags=0`, `u64 lsn`, `u32 payload_bytes`,
and `u32 reserved=0` (24 bytes). The payload follows; a final big-endian `u32`
CRC-32C (Castagnoli, reflected polynomial `0x82F63B78`, initial and final XOR
`0xffffffff`) covers the record header and payload, excluding the CRC itself.
Require `total_bytes == 28 + payload_bytes`, `28 <= total_bytes <= 1,048,576`,
and checked offsets. File header identity is bound to the manifest. CRC detects
accidental corruption, not malicious modification.

Type 1 PREPARE contains `u16 outcome_schema=1`, canonical command, canonical
outcome and digest. Type 2 COMMIT contains the `u64` PREPARE start offset,
`u32` PREPARE CRC and 32-byte SHA-256 of the full PREPARE record. Type 3 ABORT
has the same binding plus a `u16` reason. A terminal record must immediately
follow its unique PREPARE with the same LSN. V1 permits at most one pending
PREPARE, strictly sequential LSNs, and exactly one terminal record. Treat both
accepted and business-rejected admitted commands as COMMIT decisions so their
ordering and outward rejection are recoverable; their exchange counters still
follow the current exchange semantics. ABORT is reserved for an explicitly
recorded pre-decision abort under a fully specified policy; it must not be
reported as durable after ambiguous I/O. Pre-admission framing and transport
errors do not receive journal LSNs. Specify duplicate request and identity
handling before integrating these decisions with the current gateway.

For each decision: prepare and reserve outputs privately; fully write PREPARE
and COMMIT with retry on `EINTR`, handling short writes and treating a zero
write as failure; `fdatasync` the journal (or `fsync` where required for file
creation and metadata); publish the manifest below; nonthrowingly install the
accepted candidate or retain unchanged state for a rejection; then enqueue and
publish the resulting outputs. The manifest, not an unsynced terminal record,
defines the durable committed prefix. Do not hold the existing allocator mutex
across potentially unbounded I/O without first redesigning and proving bounded
owner shutdown and exclusive lifetime; the current callback API is insufficient.

## Durable manifest and filesystem assumptions

The fixed 80-byte v1 manifest contains 8 ASCII bytes `CHRMAN01`, `u16
version=1`, `u16 bytes=80`, `u32 flags=0`, exchange UUID and journal epoch
(16 bytes each), `u64 durable_lsn`, `u64 terminal_end_offset`, `u64 generation`,
`u32 reserved=0`, and a final `u32` CRC-32C over the preceding 76 bytes. An
empty durable prefix has LSN 0 and offset 48. Generation and LSN must never
wrap. The terminal offset is the first byte after the bound COMMIT/ABORT record;
the inspector must independently parse from the header through that exact
offset and verify sequence, record binding, identity and checksums. A manifest
claim beyond EOF or ending inside a record is corruption and blocks startup.

After journal synchronization, write the full next manifest to a new,
same-directory temporary file using checked writes; `fsync` that file; atomically
rename it over the manifest; then `fsync` the containing directory. Report a
durable decision only after all these steps succeed. Initial journal creation
also needs journal-file and parent-directory synchronization before publication.
Retain or safely identify the preceding verified generation until replacement
is durable. Define ownership and cleanup of stale temporary files without ever
deleting a possible committed journal. Do not use ordinary stream flush as a
durability guarantee.

Qualify only named local Linux filesystems, mount options and storage stacks for
which file and directory sync and same-directory rename guarantees have been
verified. Exclude network filesystems, unqualified WSL host mounts and devices
that do not honor cache flushes. Process-kill tests cannot establish physical
power-loss behavior. Name the exact supported kernel and filesystem matrix when
the implementation has evidence; this design makes no claim about current WSL.

## Fail-stop and recovery boundaries

Any partial write, ENOSPC, I/O error, failed journal sync, failed rename, failed
manifest sync or failed directory sync after persistence begins is ambiguous.
Stop admission, suppress success and business responses for that decision,
preserve files and stop the owner; do not roll back and continue or reuse its
identity. A crash after the manifest boundary but before installation requires
replay of the committed outcome before admission. A crash after installation
or response enqueue leaves client observation uncertain. Bytes beyond the
manifest offset are not durable decisions and need an explicit, audited tail
policy. Corruption, a gap or a malformed record at or below that boundary is
fatal; never silently truncate it. Restart must reconcile exact allocator
high-water, ownership, books, positions, risk reservations and counters before
any candidate can be published. Session identity and private-report recovery
remain separate, unsupported problems.

## Verification and measurement

Inject short and zero writes, EINTR, ENOSPC, each file sync, rename and
directory sync failure, allocation failure and process termination at every
record, manifest, installation and output boundary. Check no acknowledgment or
command-derived publication precedes successful manifest publication and
installation. Verify durable-prefix behavior, all truncation positions, CRC and
digest corruption, duplicate or skipped LSNs, wrong UUID/epoch, invalid lengths,
oversize payloads and repeated inspection. Test real local filesystems in the
qualified matrix as well as deterministic simulated I/O. The read-only inspector
must display identities, offsets, LSNs and first errors without repairing files.

Measure journal bytes per command, write and sync distributions, manifest cost,
acknowledgment latency, throughput, allocations, allocator-lock hold time and
restart replay cost for noncrossing, cancel and multi-level fills. Report
sample counts, hardware, filesystem, mount and dispersion. Group commit,
batching, replication and cross-process failover are future work and require
their own ordering and failure proofs.
