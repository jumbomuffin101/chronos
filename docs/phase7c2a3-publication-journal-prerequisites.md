# Phase 7C.2A.3: publication and journal prerequisites

This is a design for two sequential milestones, not an implemented durability
contract. Phase 7C.2A.2b provides synchronous exclusive-owner prepared scopes;
the current gateway still uses ordinary non-durable exchange operations. The
decision callback is trusted application code and can cause external effects.
No gateway publication gate, journal, manifest or crash recovery is provided.

## Stage A: owner-controlled gateway publication gate

The single gateway owner must dequeue one bounded command, validate the session
and protocol, and reserve all resources needed to retain its eventual outcome.
That reservation covers bounded private responses and reports for every affected
session, and public events for each subscriber under an explicit slow-client
policy. Counts and byte sums must use checked arithmetic and enforce independent
hard limits, including limits on trades, recipients and encoded frame sizes.
No unbounded queue or implicit best-effort private delivery is acceptable. If
capacity cannot be reserved, reject or defer before any live mutation, with a
bounded, explicitly specified response path; fail closed if even that response
cannot be retained. A disconnected owner does not make an already-resting
order or its private reports public. The existing session and retention limits
continue to apply.

Under exclusive allocation registration, prepare a private exchange candidate
and a bounded canonical outcome. Neither its preliminary result, reserved ID,
private reports nor command-derived public data may reach a socket or external
callback. The gate, not an arbitrary callback, owns the prepared scope and
retained output slots. A future journal decision may authorize installation;
in the non-durable configuration the owner can authorize it immediately. Only
after successful installation may the owner transfer the already-reserved
outputs into their delivery queues, preserving the explicitly specified command,
trade, report and feed ordering. Admission and publication must be serialized
on the existing owner context. Partial socket writes and disconnects cannot
roll back an installed command; queued data remains subject to existing bounded
retention rules, not a guarantee of client receipt.

Define business rejections as ordered outcomes too. They must consume exactly
the counters specified by the exchange contract, must not reserve or consume an
order ID when the command does not allocate one, and must follow a deliberate
policy for logging and acknowledgment. Protocol errors and transport control
responses are separate from exchange decisions and must not reveal a pending
outcome. Gate all relevant paths: direct order acknowledgments, drained private
reports, subscriber snapshots and updates, slow-client gap messages, and any
administrative or diagnostic callback that could reveal staged state.

Stop admission when shutdown begins. A merely prepared command is either
explicitly aborted before an irreversible decision, or driven to completion
once the future durable decision requires it; never discard an uncertain durable
decision and continue serving. Disconnect during preparation must not change
the ownership identity or leak preliminary results; specify whether a command
already admitted completes and how bounded reports are retained. The owner and
allocator reservation must outlive the pending decision. Current synchronous
scopes hold the allocator mutex throughout preparation and cannot simply wait
indefinitely on asynchronous I/O: a later integration must prove bounded wait,
lifetime, shutdown and deadlock behavior before introducing that pattern.
Keep the ordinary shared, non-durable gateway functional and test both modes.

Stage A tests should inject capacity exhaustion at every destination, multiple
fills and subscribers, rejected commands, disconnect and shutdown at every
boundary, reentrant calls, allocation failure, and exceptions before and after
installation. Use observable socket and queue probes to prove absence of early
publication, and compare installed state and ordered outputs with the current
exchange. Add resource-bound and protocol-v3 regression tests. This milestone
does not by itself promise disk durability or exactly-once delivery.

## Stage B: durable journal and manifest

Stage B depends on the completed and verified publication gate. Specify a
versioned, endian-defined, length-delimited PREPARE record containing journal
and exchange identities, monotonically ordered ingress, canonical command and
bounded staged decision. Specify a terminal COMMIT or ABORT record that binds
the exact PREPARE by identity and checksum. Encode fixed-width integer types,
checked lengths and deterministic ordered lists of executions, destination
reports, counters and market effects. Reject unsupported versions, duplicate
or skipped ingress, impossible transitions, oversized records and checksum
errors. Select a checksum and record-size limits before implementation; neither
the checksum nor the wire encoding is defined by this design. A checksum
detects accidental corruption and is not an authentication mechanism.

For the proposed synchronous durable acknowledgment policy: reserve output
capacity and prepare without publication; write the complete PREPARE and COMMIT
records, handling interrupted and short writes; successfully synchronize the
journal file; install the exact candidate through the nonthrowing owner path;
then publish the acknowledgment, private reports and public events. A durable
COMMIT followed by process death before installation must be applied on restart.
An unsuccessful or ambiguous write or sync must fail-stop admission and
publication, because the process cannot safely infer whether a COMMIT reached
stable storage. Never reuse its potentially committed identity or report a
definitive abort until offline inspection or recovery resolves the journal.
For accepted and rejected business commands, specify which outcomes require a
terminal record before claiming this policy; do not silently acknowledge a
rejection outside the agreed ordering contract. There is no atomic disk/memory
commit; the recovery rule bridges the boundary.

A manifest should bind a journal identity, generation, format version and
durable replay boundary to any compatible snapshot identity and allocator
high-water metadata. Publish a new manifest via a same-directory temporary
file, checked complete write, file synchronization, atomic rename and directory
synchronization, only after its referenced journal and snapshot content are
durable. Keep the previous verified generation until the replacement is fully
published. Precisely define ordering for initial journal creation, rotation,
snapshot publication and manifest replacement; do not advance a replay boundary
past an unverified record. An error or ambiguous sync at any step fails startup
or stops admission. Do not infer durability from rename, `flush`, close, WSL
host caching or filesystem behavior without an explicit tested storage contract.

The supported guarantee must name the operating system, local filesystem,
mount and storage assumptions, required file and directory sync semantics, and
device power-loss behavior; network filesystems and unverified virtualization
are excluded unless separately tested. Recovery must reject interior corruption,
identity mismatches and sequence gaps. Define an incomplete terminal record at
the physical tail as unresolved until the specified inspection/recovery policy
handles it; never silently truncate corruption in the middle. An offline,
read-only inspector should report identities, validated boundaries, record
decisions, checksums, offsets and the first error without modifying the files.

Fault injection must cover short writes, EINTR, ENOSPC, file and directory sync
errors, rename errors, checksum corruption, every torn-record boundary,
duplicate and skipped sequences, process death before and after every write,
sync, install and publication boundary, and repeated restart. Test on supported
filesystems as well as a deterministic I/O model. Verify exact book, ownership,
risk, allocator high-water and sequence state against uninterrupted execution,
and prove startup admission remains closed until reconciliation and global
allocator registration succeed. Recovery and live restored-exchange publication
are separate subsequent implementation work, not delivered by this document.

Neither a synchronized journal nor replay alone establishes authenticated
sessions, private-report recovery, client receipt, exactly-once execution, or
reliable behavior on unsupported storage. Their contracts require separate
design, implementation and verification.
