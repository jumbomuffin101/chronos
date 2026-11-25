# Canonical verification for restricted isolated replay

This is an in-memory verification format for the [restricted isolated domain](phase7c2a3b6-restricted-replay-design.md), not a durable journal, manifest, live recovery or authenticated exchange identity. It uses an explicit exact-state schema, a bounded stream envelope and an externally supplied verification anchor. The ordinary gateway, CHRSTATE and the provisional journal inspector formats are unchanged.

## State schema and scope

`Exchange::canonical_isolated_state(max_bytes)` emits at most 65,536 bytes; it rejects non-isolated exchanges and any state with public subscriptions. Every numeric field is an unsigned 64-bit big-endian word. Signed prices and positions are represented modulo 2^64. Counts precede collections. Schema magic is `CHRCST01` followed by u64 version 1. Unknown report kinds and matching transaction modes fail closed. Counts and output appends are bounded; book level/order enumeration is checked against the byte cap before allocating its returned vectors.

The exact ordered fields are:

1. Magic, schema version, max sessions/orders/reports/requests, private allocator high-water, initial feed sequence, exchange command counter and exchange event counter.
2. Session count; sessions ordered by numeric ID. Each has ID, kill flag, max order quantity/notional, max open notional/orders, max absolute position, max market price, allowed-symbol count and vector order, request count and request order, position count and numeric-symbol order, report count and FIFO report order, and subscription-generation counter. A report contains sequence, command sequence, request, client order, internal order, trade, symbol, price, quantity, remaining and explicitly mapped kind.
3. Ownership-history count and existing history order; each contains session, client ID, internal ID, request ID, symbol and open flag. Audit-trade count and existing execution order; each contains command/trade sequences, buyer/seller sessions and internal IDs, symbol, price and quantity.
4. Per-symbol feed count and numeric-symbol order, each symbol and sequence. Engine count and numeric-symbol order; each engine has symbol, matching command/event/trade counters, book revision and explicitly mapped transaction mode.
5. For each engine, bid levels in descending price then ask levels in ascending price. Each side begins with a level count; each level has price, aggregate quantity and order count, then its FIFO order list of internal ID, remaining quantity and original sequence metadata.

Derived open exposure is reconstructed from exact ownership and resting orders; there is no independent reservation ledger. Lookup maps, iterators, pointer identity, thread state and transient selections are not encoded because they are derivable or do not survive between restricted operations. Public subscription queues/gaps are excluded by the domain and explicitly rejected by the exporter; new sessions start with none. The canonical bytes encode the complete supported continuation state, not delivery to external clients. The existing diagnostic word-vector comparison remains separately exercised by `RestrictedReplay::verify` and by sealing the source records; canonical envelope verification compares freshly encoded canonical bytes rather than relying on diagnostic-word equality.

There is no standalone general-purpose state decoder or importer. Inside an envelope, a state is accepted only when its declared length and every byte exactly match the freshly generated canonical state for the replayed operation; this also rejects malformed counts and trailing state bytes. The schema is versioned for this restricted verifier, not a live restoration format.

## Stream envelope

The envelope uses big-endian fields and a 168-byte header:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `CHRCAN01` magic |
| 8 | 8 | Envelope schema version 1 |
| 16 | 16 | Nonzero caller-supplied isolated-context identity |
| 32 | 32 | SHA-256 of the fixed configuration identity tag |
| 64 | 32 | SHA-256 of the initial canonical-state bytes |
| 96 | 8 | Allocator scope: 1 means isolated private allocator |
| 104 | 8 | Initial operation sequence, 1 |
| 112 | 8 | Terminal sequence; zero for an empty stream |
| 120 | 8 | Declared record count |
| 128 | 8 | Exact terminal byte offset, equal to total stream size |
| 136 | 32 | Complete-stream SHA-256 |

Each payload entry is u64 operation length, u64 canonical-state length, a fixed 120-byte operation and the exact canonical-state bytes after that operation. The operation contains u64 sequence, kind and actual Exchange outcome followed by twelve u64 argument words using the layouts in the restricted replay design. The verifier rejects unsupported kinds and nonzero reserved arguments. It does not carry or depend on the older diagnostic words. Sealing independently re-executes the prototype's source records, verifies their diagnostic words, derives each canonical state from that replay, and compares the replayed terminal canonical state against the owner's actual terminal state. The source prototype does not retain canonical snapshots directly at each original installation; canonical-native recording remains future work. State length is bounded to 65,536 bytes, the stream to 32 MiB and record count to 128. No trailing bytes are accepted.

Operation kinds 1–6 are open, risk, submit, cancel, expected-report consume and close. Open/risk outcomes are 1 success or 2 rejection; close is 1 changed or 2 no-op; consume is 1 consumed, 2 empty, 3 mismatch or 4 invalid. Submit/cancel outcomes 16–32 map in order to accepted, unknown session, duplicate request, duplicate order, not owner, not found, invalid order, capacity, counter exhausted, killed, symbol, quantity, notional, open-orders, position, market-bound and overflow results. Raw submit side/type bytes retain invalid inputs for business rejection; current valid side values are buy 0 and sell 1, and order types limit 0 and market 1. Reserved input words must be zero. An exception or aborted preparation has no operation entry.

The stream SHA-256 covers every byte of the header and payload with bytes 136–167 replaced by zero. The initial-state SHA-256 covers the complete versioned `CHRCST01` byte sequence. The configuration SHA-256 covers the exact UTF-8 ASCII tag `chronos-isolated-v1;sessions=16;orders=128;reports=128;requests=128;feed=0;allocator=private-zero;subscriptions=excluded`. These are mismatch detectors and identity bindings, not proofs of equality or authentication. Exact equality uses the complete canonical bytes; digest collisions remain theoretically possible.

`seal` returns bytes plus an `Anchor` containing the identity, terminal sequence and stream digest. `verify(bytes, anchor)` checks the header, configuration, allocator scope, initial-state hash, exact terminal count/offset and the anchor's identity/digest; then independently constructs a fresh isolated Exchange, decodes and executes each operation, compares the actual outcome and canonical state after every operation, and rejects divergence. The anchor must come from an independent trusted source to bind identity and completeness. An attacker who replaces a stream and its self-supplied anchor can produce a consistent alternative. No key, signature, authenticated source, durable publication or crash guarantee is supplied.

## Limits and failure behavior

Encoding and verification allocate bounded buffers. A failed seal or allocation exception leaves the restricted owner unchanged; a verification allocation exception propagates rather than becoming a business rejection. Malformed or divergent bounded data returns false. Record/state length fields are checked against the remaining bytes before slicing. SHA-256 verification currently copies the bounded complete stream to zero the digest field. Recording retains the preceding prototype's full-history reconstruction and diagnostic state per operation, and sealing adds another full canonical state per operation: memory and work grow with both operation count and state size. The verifier shares Exchange matching/risk logic with the recorder, so it is independent execution rather than an independent matching implementation.

Before durable or live use, define authenticated exchange identity and a trusted terminal boundary, stable canonical schema evolution and migration, process-wide allocator reconciliation and publication gating, private-report reconciliation, persistent authorization and fail-stop sync ordering, filesystem assumptions, fault injection and crash recovery. No exactly-once client delivery or authenticated reconnect is claimed.
