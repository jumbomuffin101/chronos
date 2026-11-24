# Experimental journal envelope inspection

Phase 7C.2A.3b is **incomplete**. This change adds only a read-only structural
inspector and checksum primitives. It adds no journal writer, manifest publisher,
durable execution mode, transaction authorization, crash recovery or gateway
integration. No admitted order or acknowledgment becomes durable as a result.

The earlier [implementation plan](phase7c2a3b-journal-implementation-plan.md)
is a proposal, not an implemented format. Its 48-byte header and 80-byte
manifest omit the configuration digest and initial LSN required by the current
task. The experimental inspector therefore uses the layouts below. These are
not a frozen on-disk compatibility contract and no production data should be
written in this format. Phase 7B recordings and CHRSTATE images are distinct.

## Experimental envelope

All integers are big-endian. The journal header is 88 bytes: ASCII `CHRJRNL1`,
u16 version 1, u16 header length 88, u32 zero flags, 16-byte exchange identity,
16-byte epoch, 32-byte configuration digest, and u64 initial LSN 1. The manifest
is exactly 112 bytes: ASCII `CHRMAN01`, u16 version 1, u16 length 112, u32 zero
flags, matching identity, epoch and configuration digest, u64 durable LSN, u64
terminal end offset, u64 generation, u32 zero reserved, and u32 CRC-32C over
the preceding 108 bytes. CRC-32C uses reflected polynomial `0x82f63b78`, initial
and final XOR `0xffffffff`. It detects accidental corruption, not tampering.

Each record is at most 1 MiB: u32 total bytes, u16 version 1, u8 type, u8 zero
flags, u64 LSN, u32 payload length, u32 zero reserved, payload, and u32 CRC-32C
over all preceding record bytes. Total length must equal payload length plus 28.
Type 1 PREPARE begins with u16 schema 1; **the remaining payload is opaque**.
Types 2 and 3 are terminal envelopes with exactly 44 payload bytes: u64 PREPARE
offset, u32 PREPARE CRC and the 32-byte SHA-256 of the entire PREPARE record.
The inspector does not interpret type 2 versus 3 as a validated business
outcome. PREPARE and terminal records must alternate with identical, contiguous
LSNs starting at 1; the manifest offset must end immediately after the terminal
record matching its LSN. The empty prefix has LSN 0 and offset 88. The current
inspector rejects a terminal at `UINT64_MAX` rather than supporting LSN
exhaustion. The journal is bounded to 64 MiB and one record is allocated at a
time, at most 1 MiB.

The inspector reports bytes after the manifest offset as an **opaque, unverified
tail**. It does not identify valid versus torn tail records, validate a canonical
outcome, verify economic state, or reconstruct an exchange. It never modifies
either input. A successful structural check is neither a durability claim nor
proof of a transaction. SHA-256 is used to bind records, but this tool provides
no authentication and no protection against an adversary who can rewrite them.

## Integration blockers

`LiveExchangeOwner` currently confines the private candidate and reserves the
real allocator ID inside a synchronous scope. Its public decision view exposes
only `Result`, not the reserved identity, complete trades, private/public effects
or exact staged continuation state. Submit also holds the process allocator
mutex throughout that scope. Writing and syncing through the existing callback
would couple storage to an arbitrary trusted callback, omit the required
canonical outcome, and create unbounded shutdown and lock-hold concerns. A safe
implementation needs a restricted owner-held canonical decision, bounded output
reservation, a tested storage state machine and explicit lifetime and fail-stop
semantics before any durable service or gateway opt-in is offered.

The required writer, exclusive file ownership, partial-write and `EINTR`
handling, journal sync, same-directory temporary manifest, file sync, rename,
directory sync, fault and process-crash tests, durable-prefix tail policy,
restart admission gate and storage benchmarks remain unimplemented. No local
filesystem or WSL mount is qualified for durability by these tests.

The follow-on [canonical decision design](phase7c2a3b1-canonical-decision-design.md)
distinguishes its fixed command-input encoding from the still-unimplemented
authoritative staged outcome and documents the allocator ownership choices.
