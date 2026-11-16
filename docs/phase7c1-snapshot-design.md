# Phase 7C.1: verification checkpoint and restoration gap

**Status: incomplete.** This change provides a bounded on-disk diagnostic
checkpoint, not a restorable exchange snapshot. It cannot be used for crash
recovery, event-log suffix replay, or restarting a live exchange.

Default exchanges use a process-wide internal order-ID allocator. Phase 7B
replay now uses an explicitly isolated private allocator by default, while an
explicit shared mode retains the earlier ID-consuming behavior. Private IDs
cannot be published into the gateway without a future identity protocol. The
normalized `verification_state()` omits actual IDs, allocator high-water mark,
configuration capacities and order-book revision tokens. The tokens govern
selection invalidation and exhaustion. Its output is therefore observational
and cannot be used to reconstruct all continuation behavior. An accepted
exchange-command counter is also not the simulation ingress position.

The implemented owner-thread-only `snapshot::capture()` reads the current
normalized verification vector at a single synchronous boundary. The caller
must serialize it with all exchange reads and writes. It has no mechanism to
capture a concurrently mutated exchange consistently. The diagnostic reader
never mutates an exchange, returns a new value only after successful parsing,
and comparison uses the existing normalized authoritative view. A match says
only that the represented observations agree; it is not proof that future
commands, actual internal IDs, selection tokens or allocator behavior agree.

## Diagnostic format 1

The file starts with the eight-byte ASCII magic `CHROVIEW`, then four
little-endian `u64` fields: format version `1`, caller-supplied generation,
exchange accepted-command counter, and verification-word count. The counted
`u64` words are followed by IEEE CRC-32 (reflected polynomial `0xedb88320`,
initial/final XOR `0xffffffff`) as little-endian `u32`, covering all prior
bytes including the header. No native structs are serialized. The counter is
copied from the observed exchange, not an ingress or log-suffix position.
No relationship to any Phase 7B file or log epoch is established.

The reader bounds the complete encoded file to 8 MiB before allocating the
payload, rejects unsupported versions, truncated data, invalid metadata,
trailing bytes and checksum errors, and leaves its output unchanged on error.
It validates framing, not the internal consistency of the opaque state words.
CRC is not authentication. The writer flushes its stream but does not `fsync`.
Capture and comparison use a bounded verification traversal with an exact,
allocation-free shape preflight before building the output and ID normalization table. Intermediate
containers and allocator overhead mean the 8 MiB encoded cap is not an exact
peak-memory cap. These properties are insufficient for a production snapshot
reader or an untrusted reconstruction interface.

See [the continuation-state audit](phase7c1a-continuation-state-design.md) for
the allocator decision and remaining candidate-publication requirements.

## Required work before verified restoration

Define a dedicated, versioned continuation image and restricted import path
covering exact IDs or an explicitly isolated mapping; allocator epoch and
high-water behavior; all exchange capacities; order-book revision and
selection semantics; matching counters; ownership and request history; FIFO
levels; queues, subscriptions, positions and risk. Validate cross-references,
overflow, reservations and aggregate quantities before constructing a private
candidate. Introduce a real owner/log boundary with a run identity before
claiming suffix compatibility. Either isolate allocator namespaces with a
reviewed publication policy or coordinate restore with all live exchanges;
do not reset or advance the shared process-wide allocator behind them.
Then test equivalent follow-on matching, cancellation, rejections and ID
allocation, plus corruption and allocation failures. This is a prerequisite
for 7C.1 completion, not implemented Phase 7C.2 persistence.
