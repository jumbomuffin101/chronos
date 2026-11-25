# Restricted isolated operation stream

This is an experimental, bounded **in-memory** replay domain. It is not a journal, durable storage, live gateway recovery, authenticated session system or exactly-once delivery mechanism. `RestrictedReplay` owns a private isolated `Exchange`, has no gateway attachment or mutable Exchange accessor, and accepts only the six operations below. All calls on an instance must occur on its creating thread context; it cannot be copied or moved. There are no callbacks or externally accessible mutation paths. Destruction ends the context; callers must not use an object or borrowed references after destruction.

## Domain and initial state

Each instance begins with `Exchange::isolated(16, 128, 128, 128, 0, 0)`: no sessions, no orders, no subscriptions, zero initial feed sequence and private order-ID high-water zero. Symbols are not preconfigured; exchange validation applies to submitted numeric symbols and policies. Risk starts at the existing Exchange defaults until a recorded update. Private IDs can repeat across independent isolated contexts and cannot be published through this API to a live gateway. The process-global allocator and other owners are not used or advanced.

The starting identity is an instance-local owner-context token; it is not a serialized exchange UUID or epoch. Every verifier invocation deliberately constructs a new equivalent isolated context from the fixed configuration. Separate streams can therefore contain the same session and order IDs and must not be combined or attributed to a live exchange on the basis of these bytes alone.

Only `Open`, `Risk`, `Submit`, `Cancel`, `Consume` and `Close` are representable. Unknown kinds, nonzero reserved arguments, oversized symbol/enum representations and risk symbol lists above four entries are rejected before mutation. Kill switch, subscription changes, public queue operations, gateway disconnect, live session authentication and arbitrary state import are excluded. With no subscription API or initial subscriptions, subscriber queues and gap transitions do not arise; public feed counters still belong to exact exchange state. This exclusion does not imply an existing live subscriber can survive recovery. Private report queues are retained and explicitly consumed by expected recipient and front sequence.

## Operation and record schema

`Operation` contains a kind and twelve u64 argument words. Inputs use exact integer values; signed prices use their two's-complement bit representation. Reserved words must be zero. The supported layouts are:

| Kind | Argument words |
| --- | --- |
| Open / Close | session |
| Risk | session, max order quantity, max order notional, max open notional, max open orders, max absolute position, max market price bits, symbol count, up to four allowed symbols |
| Submit | session, request, client order, symbol, raw side byte, raw order-type byte, price bits, quantity |
| Cancel | session, request, client order |
| Consume | session, expected front report sequence |

Open and risk return outcome 1 for success, 2 for rejection. Close returns 1 if exact state changed and 2 for a completed no-op. Consume returns 1 consumed, 2 empty/unknown session, 3 mismatched front and 4 invalid zero session or sequence. Submit/cancel outcomes are 16 plus the zero-based index of the explicit `operation_result_codes` table in `operation_record.hpp`, preserving the actual Exchange `Result`. These numbers are local experimental schema values, not network protocol codes. Business rejections and completed no-ops are recorded and advance the sequence; pre-validation failures and exceptions do not. `apply(op, false)` evaluates a private candidate and returns an aborted result without changing state, records or sequence.

The bounded record is big-endian u64 words: magic/version `CHROPR01`, sequence, kind, outcome, twelve argument words, exact-state word count and that many exact-state words. Header size is 136 bytes; state is bounded to 8,192 words, so a record is at most 65,672 bytes. At most 128 records are allowed, including rejections and no-ops; the cap prevents u64 sequence exhaustion by construction. Decoding rejects invalid magic, unsupported kinds/argument layouts, truncation, trailing data, invalid state count and oversize. The outcome is verified by executing the operation, not trusted as a replacement result. This encoding embeds the current diagnostic state words and is **not** a stable persistent format. It has no checksum or authentication and a fully consistent alternative history is not detectable as tampering.

## Ordering, publication and verification

There is one owner-local sequence, beginning at one, separate from the live diagnostic index and any future journal LSN. Before every new operation the implementation reconstructs an isolated candidate from the initial state by decoding and executing every prior record and comparing the real outcome and exact state. It then executes the new input, obtains bounded exact state and encodes the full record. The outer record vector is reserved at construction; record bytes move without throwing. Only after record creation succeeds is the record appended and the candidate installed by nonthrowing unique-pointer swap. Allocation, decoding, execution or record-size failure before that boundary leaves the current candidate and record sequence unchanged. There is no user callback between record append and swap. No output is published externally as part of this boundary.

`verify(records, expected_count)` independently constructs a fresh isolated exchange, requires the supplied expected count and contiguous sequence starting at one, decodes every record, executes its actual operation with existing Exchange logic, and compares actual outcome and every exact-state word to the recorded expected values. The caller must supply the expected terminal count from an independent trusted boundary to detect a missing suffix; passing an altered count is not evidence of completeness. A verifier error does not mutate a live gateway. Allocation failures propagate instead of being reclassified as business rejection. This verifier shares the same matching/risk implementation as recording, so it detects divergence from recorded outputs and state but is not an independent implementation proof.

The existing bounded `isolated_continuation_state` includes exact internal IDs and allocator high-water, FIFO levels and remaining quantities, ownership/history, risk limits, positions, private reports/queues, exchange and matching counters, revisions, audit trades, feed counters and configured limits. The vector is an implementation diagnostic, not a canonical storage contract. Exact equality is checked after every record; no digest is treated as proof. Expected report consumption is scoped by session and exchange-local report sequence, checks the front first and cannot remove a later report on duplicate/mismatch. This is not TCP delivery acknowledgment.

## Limits and future work

The prototype intentionally rebuilds prior history and stores full exact state per operation: if `n` operations have state size `S`, recording can require O(n²) replayed operation work plus O(n·S) state-comparison/storage work, with bounded but potentially large allocation. Worst-case encoded bytes alone are bounded by 128 × 65,672 bytes; copied streams and candidates add memory. It is a correctness baseline, not a low-latency design. There is no stable exchange identity, lifecycle epoch, live allocator reconciliation, authenticated reconnect, durable journal admission/sync, manifest, startup gating, fail-stop storage protocol or live publication. Those require separate design and fault testing before any crash-durability claim.
