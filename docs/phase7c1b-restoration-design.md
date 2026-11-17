# Phase 7C.1B isolated, history-backed restoration

This implementation supports a deliberately narrow cold-start *candidate*:
a completed Phase 7A simulation created with a fresh private allocator and a
complete Phase 7B event log. It is not a general snapshot of an arbitrary
exchange. It cannot capture an empty exchange with no completed commands,
an active gateway, later unrecorded operations, a stopped run, or a source
whose allocator started at a different high-water mark. No snapshot/log suffix
coordination, authenticated session recovery, durable storage, or publication
of a candidate as a live exchange is implemented.

The result is an exchange candidate, not a resumable simulation runner. The
generator PRNG, participant next-request counters and runner active-target
list are not serialized; follow-on tests submit explicit exchange commands.
Continuing synthetic generation requires a separate runner image.

The image embeds the complete verification log rather than reconstructing
arbitrary internal structures from an untrusted serialized memory image. The
reader re-executes the admitted commands against a fresh isolated exchange,
checking each logged outcome and normalized exchange state, then checks the
candidate's exact continuation-state words against the image. Replaying the
complete history rebuilds order lookup indexes and FIFO queues through existing
matching and risk logic. Exact capture also cross-checks every resting order
against its owner history and lookup index, verifies uniqueness and checked
level aggregates, and verifies that every open history entry appears in a book.
Semantic changes to order ownership, quantities,
risk, positions, reports, counters, ID high-water or revisions cannot pass by
merely recomputing the outer checksum: the decoded exact state must match the
independently replayed candidate. This is a verification of a reachable state
under the current implementation, not an independent matching-engine oracle.

Capture and continuation-state reads are synchronous owner-thread operations.
The writer requires an isolated source and verifies that its exact state equals
the privately replayed terminal log state before emitting an image. The
simulation ingress sequence stored in the header comes from the verified
complete log; the accepted exchange-command counter is separately present in
the exact state and is not substituted for ingress. No compatibility with an
arbitrary Phase 7B log or a later log suffix is asserted.
The embedded log carries its complete initial scenario and risk configuration,
and the exact footer carries exchange capacities. There is no separate
configuration UUID, digest, exchange epoch or durable run identity in v1.

## Format `CHRSTATE` version 1

All integers are little-endian. The eight ASCII bytes `CHRSTATE` are followed
by five `u64` words: format version `1`, caller-selected generation, verified
terminal simulation ingress, embedded log byte count, and exact-state word
count. The payload is the complete Phase 7B log followed by the exact `u64`
words. A little-endian `u32` IEEE CRC-32 (reflected polynomial `0xedb88320`,
initial/final XOR `0xffffffff`) covers every preceding image byte. Trailing
bytes, unsupported versions, incomplete files, bad CRC, oversized fields,
incomplete logs and state mismatches are errors. A CRC is not authentication.
The embedded log is limited to 64 MiB, and the exact state to 8 MiB; total
encoded size is at most those limits plus the 48-byte header and 4-byte CRC.
There is no separate outer completion marker: exact length, the final CRC and
the embedded log's required terminal record define a complete image.
The reader bounds these counts before allocating the payload. It can hold
multiple bounded copies and replay state, so the file limit is not a strict
peak-memory limit. The writer flushes the supplied stream but does not fsync.

The exact-state schema is the Phase 7B verification traversal with actual IDs
in report, trade and FIFO references, plus the actual internal ID in each
order-history entry. Its final footer contains version `1`, exchange session,
order-history, report and request capacities, private allocator high-water,
then `(symbol, book revision, transaction mode)` for each engine in sorted
symbol order. The main traversal includes exchange and engine counters, sessions,
permissions, positions, request history, queued reports, order history,
audited trades, feed sequences, subscriptions and queued market events, and
complete price levels with quantities and FIFO orders. Risk reservations are
derived by the existing risk logic from the preserved positions, limits and
open orders. Selection identity pointers are intentionally not serialized: a
new book identity prevents selections from another book being applied to the
candidate. Exchange-level APIs do not expose such selections.

`CHROVIEW` remains a separate diagnostic format and is not accepted as a
restorable image. `restore_image` requires an empty output pointer and changes
it only after all framing, replay, boundary and exact-state checks succeed.
An allocation failure is reported by `ImageResult::allocation_failure`; a
failed writer or flush can leave a partial file, or even complete bytes whose
durability is uncertain. Neither case is an acknowledged durable commit.
All replay and follow-on operations use the candidate's private allocator and
do not consume process-wide live IDs. A private numeric ID may collide with a
live ID and must never be forwarded to a live gateway as if it were globally
allocated. Private reconstruction may run while other live exchanges exist;
this is not permission to publish the candidate beside them.

## Publication remains blocked

There is currently no enforced proof that no other live exchange or gateway
session exists, no registry of published IDs, and no atomic operation binding
the shared allocator high-water mark to exchange installation. Consequently
the API returns only a private candidate and rejects replacement of a nonempty
destination. Cold-start publication needs the allocator coordination and
failure-atomic boundary in [the allocator design](phase7c1a-allocator-isolation-report.md),
including explicit identity collision, exhaustion and session policy. It is not
implemented or implied by successful verification.

Before Phase 7C.2, this approach also needs a durable journal, filesystem and
directory sync policy, snapshot/log generation identity, crash-point tests,
and a policy for restoring real client identity and private-report delivery.
Phase 7B's write/flush ordering does not provide those guarantees.
