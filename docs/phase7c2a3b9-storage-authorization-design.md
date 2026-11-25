# Storage authorization prerequisites

This milestone provides a **test-only model**, not a journal, durable commit, authenticated authority, or live-gateway feature. `AuthorizationTestHarness` is compiled only with the existing test seam and owns a restricted, private-allocator replay exchange. The existing recorder still installs immediately; there is no production authorization interface. A future production gate requires an owner/lifetime design and a real trusted storage boundary.

## Lifecycle and ownership

The owner thread exclusively owns the authoritative exchange, one private candidate, one preconstructed record, and the decision request. The legal transitions are:

| State | Legal next state | Meaning |
| --- | --- | --- |
| Idle | Preparing, Stopped | Admission allowed only here. |
| Preparing | Prepared, Idle | Preparation failure leaves the owner unchanged. |
| Prepared | AwaitingExternalDecision, Aborted, Stopped | A decision has not yet been attempted. |
| AwaitingExternalDecision | Authorized, Aborted, FailedStop | Explicit precommit refusal aborts; ambiguous or mismatched responses fail-stop. |
| Authorized | Installed, FailedStop | Install only the bound candidate; clean shutdown first installs. |
| Installed | Idle, Stopped | Exactly one preconstructed record is published with installation. |
| Aborted | Idle, Stopped | No record or candidate was published. |
| FailedStop | Stopped by explicit test handoff | No further mutation; the handoff is not recovery or rollback. |

Illegal transitions throw. Shutdown while awaiting a decision is ambiguous and enters fail-stop. In the test harness destruction with an unresolved decision terminates rather than silently pretending it was aborted. No background storage request exists in this model. Diagnostics and pending resources remain in memory until explicit test handoff; real operation requires an independently persistent recovery authority, operational procedures, and a defined process termination policy.

The future ordering is: prepare the private candidate and canonical outcome; enforce limits and build the exact record; submit exact bytes to a trusted authority; establish and verify its durable terminal decision; install through nonthrowing operations; then publish the result. A transport error after submission is ambiguous unless the authority can prove a refusal occurred before commitment. Fail-stop must withhold outputs and admission; a restart without implemented recovery must not admit new transactions or call an externally committed decision rolled back.

## Binding and trust

The bounded test request includes a format marker, isolated identity, configuration digest, operation sequence, preceding terminal count, preceding encoded envelope length and digest, command kind and arguments, outcome, and length-delimited original record and canonical candidate bytes. Its SHA-256 digest and exact length, alongside identity, configuration, sequence and prefix fields, bind the internal token. The request and candidate are owned privately, and the owner cannot admit another operation while they are pending. The token is private and consumed once. Its test decision source is deliberately not a real storage authority and is not exposed as a production callback. SHA-256 is an integrity binding, not an independent trust anchor or authentication mechanism.

A future trusted terminal publication must bind exchange and stream/journal epoch, configuration digest, initial state identity, terminal operation sequence, exact terminal record offset, digest of the complete committed prefix, and publication generation. Recovery must validate these against a separately trusted, identity-checked storage-backed manifest; a self-contained stream digest alone cannot prove completeness or authenticity. This milestone implements neither that manifest nor its authentication or durable publication.

## Allocators and publication

The prototype clones the isolated private allocator with the candidate. IDs and high-water state remain private until installation, with exact continuation captured in the record. It does not acquire or modify the live process-global allocator. A future live design needs an exclusive allocator lease/reservation capability, exact high-water reconciliation on recovery, and a publication precondition preventing any competing allocator or exchange owner from allocating overlapping IDs. Holding the existing shared allocator mutex across journal I/O is unacceptable: it can stall unrelated work and creates shutdown/owner-thread lock-order hazards. A separate durable reservation capability must specify ownership, lock order, failure behavior, and publication before any live integration; no allocator locking is changed here.

Installation in the model checks the binding and reserved slot before swapping the authoritative state and preconstructed record, then advances the count. The relevant pointer swaps are checked nonthrowing and no callback, allocation, or I/O occurs in that publication interval. This is an isolated-domain model, not evidence of a production crash-atomic transaction. Preparation and hashing may allocate or fail before any decision; a mismatch after a simulated external decision instead enters fail-stop.

After authorization, `install()` first verifies the owner context and recomputes the request SHA-256; the hash function is declared `noexcept` and does not allocate. On the creating thread the thread-local context was initialized by construction; a wrong-thread call throws before any installation and may initialize that thread's context. A failed binding check enters fail-stop. Token destruction, the pointer swaps, and count/state assignments do not allocate. These are properties of this test model only, not a proof of a future authority or recovery protocol.

## Remaining work

A real journal requires a bounded writer, admission and acknowledgment boundaries, record and terminal ordering, filesystem and sync assumptions, torn-record handling, fault injection, storage failure policy, trusted manifest publication, and restart reconstruction. Live recovery further requires exclusive owner and allocator leases, risk/book/ownership reconciliation, startup admission gating, failure-atomic publication, and explicit session/private-report limitations. No exactly-once delivery, authenticated sessions, crash durability, or restored live publication is provided here.
