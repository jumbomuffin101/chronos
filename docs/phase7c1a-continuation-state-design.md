# Phase 7C.1A continuation-state ownership

Update: 7C.1A.2 implements the shared/private allocator boundary and isolates
Phase 7B replay by default. The historical audit and proposal below describe
the earlier state; see [the allocator isolation report](phase7c1a-allocator-isolation-report.md)
for the implemented contract and remaining publication requirements.

## Historical allocator audit and decision

Before 7C.1A.2, `src/exchange/exchange.cpp` owned a process-static `last_internal_id` and
`id_mutex`. Each accepted submit locked that mutex before cloning the exchange,
matching, creating reports and publishing market data. It uses the provisional
`last_internal_id + 1`, swaps the staged state into place, then advances the
counter. Rejections, exceptions and failed staging do not advance it. The
maximum value returns `CounterExhausted` before addition. The exchange
constructor, `State::clone`, matching-engine clone, cancellation, subscriptions
and book reads allocate no internal order ID. An accepted market order does,
even when none rests. The same allocator is used by simulations and the Phase
7B replay verifier, so a replay consumes IDs observable by other exchanges.
The added regression demonstrates this interference.

IDs are intentionally process-wide unique in the current process epoch:
`ExchangePrototype.InternalIdsAreUniqueAcrossInstances` checks it, and the TCP
execution report exposes `internal_order_id` alongside client-order and session
correlation. IDs are not durable across process restart. Matching, fill reports,
audit records, cancel ownership lookup and resting-book indexes refer to them.
Client IDs are scoped to a session; requests are duplicate-checked within a
session. The mutex currently spans staging and matching, not only incrementing
the counter. No allocator exhaustion injection hook currently exists, so the
maximum-ID branch is source-audited rather than directly tested in this change.

| Approach | Uniqueness and multiple exchanges | Restoration and hot path |
| --- | --- | --- |
| Exchange-owned scalar counter | Collides across exchanges unless a new namespace is encoded; changes existing ID contract. | Easy isolated candidates, but incompatible without a gateway/protocol migration. |
| Injected allocation service | Shared service preserves today's process-wide numeric uniqueness and transactional reservation. | Candidate uses a private, nonpublishing identity context; atomic publication needs an ownership/epoch and reservation protocol. Existing shared lock can initially remain. |
| Namespace plus per-exchange counter | Unique only if namespace is globally allocated and encoded into the existing ID width; namespace reuse, overflow and restart need rules. | Easy private replay, but changes identifier representation and requires gateway/client migration. |

**Decision:** retain current global allocation behavior for this change. The
recommended future architecture is an explicitly injected allocation service
whose normal context shares a process-wide namespace and preserves the existing
API, with an isolated candidate context that does not reserve or publish IDs.
Candidate IDs may be exact only while private. Publication must run under a
defined owner/service coordination boundary, verify that IDs belong to the
replaced exchange or are otherwise unclaimed, reserve a collision-free
high-water mark with checked arithmetic, and atomically replace the old state.
Two independently live exchanges cannot simultaneously publish the same exact
IDs. Failure before publication must leave live allocation state untouched.
An epoch and restart policy are additional requirements, not a durability claim.

At the time of this audit, the necessary candidate ownership, conflict resolution, and atomic publication
contract do not exist today. Introducing a nominal per-exchange counter would
break global uniqueness, and an injectable allocator without a publication
protocol would give a false safety guarantee. Consequently no allocator or
gateway implementation was changed, no candidate restorer was added, and no
candidate noninterference or ID-exhaustion test is claimed.

## Continuation-state inventory

`CHROVIEW` below means the current normalized diagnostic vector, not a
restorable snapshot. All changes occur on the serialized exchange-owner thread
unless stated otherwise. Derived data can be rebuilt only after validating its
authoritative inputs; diagnostics do not supply a restoration contract.

| Field | Owner / initialization | Mutation sites | Required restore and invariant | CHROVIEW / class |
| --- | --- | --- | --- | --- |
| Actual internal IDs | Exchange order history; allocated at accepted submit | submit commit, used in reports/trades | preserve or map one-to-one, nonzero and unique across published exchanges | ordinal only; authoritative |
| Global ID high-water | process-static allocator, initially zero | accepted submit under mutex | checked continuation, no collisions or failed-candidate side effects | no; authoritative process state |
| Client IDs and ownership | Exchange order history, per session | accepted submit/cancel/fills | session ownership; retained historical client IDs cannot be reused, even after closure; retain correlations | yes; authoritative |
| Exchange capacities | Exchange constructor arguments | fixed for lifetime | preserve session/order/report/request limits exactly | no; authoritative configuration |
| Sessions, request history, kill | Exchange session map, open/configure | open/close, submit/cancel, kill | unique session, duplicate-request behavior and retained owner state | yes; authoritative |
| Risk limits and symbol permissions | Session defaults/configure | configure risk | preserve limits and allowed symbols, validate positions | yes; authoritative |
| Open reservations | computed from live owned book orders and prices | recomputed during pre-trade checks | recompute with checked arithmetic and reconcile positions/limits | inputs represented; derived |
| Positions | session symbol map, initially absent/zero | fills | preserve signed amounts; checked arithmetic and risk limits | yes; authoritative |
| Symbol engines and FIFO book | matching engine/book, created on first accepted symbol use | add, consume, cancel, modify | preserve sides, prices, quantities, FIFO order and checked aggregates | yes, normalized; authoritative |
| Book lookup index | order book, built on insert | add, cancel, fill, modify | rebuild with unique IDs and valid nodes; no stale iterators | no; derived |
| Book revision and identity | order book, revision zero/new identity | successful book mutations; clone copies revision with new identity | preserve revision/exhaustion semantics; use a new identity so old selections cannot target candidate | no; authoritative continuation plus ephemeral token |
| Matching counters and mode | engine constructor, counters zero/default planned | accepted commands, reports/trades, maintenance | preserve command/event/trade counters and mode; no overflow | counters yes, mode no; authoritative |
| Exchange command/event counters | exchange state, zero | accepted submit/cancel and private reports | preserve last-issued values and checked exhaustion | yes; authoritative |
| Trade audit | exchange trade vector, empty | successful fills | preserve order, participants, price, quantity, ID references and counters | normalized IDs; authoritative |
| Report queues | session vectors, empty | accepted submit/cancel/fill; pop | preserve order, owner, counters and IDs; enforce queue capacity | yes, normalized; authoritative pending state |
| Feed sequences/subscriptions | initial feed config; empty maps | subscribe/unsubscribe, publish, pop, close | preserve generation, gap flags, queue and sequence; validate references | yes; authoritative |
| Phase 7A generator and active targets | simulator, seeded at start | generated commands, report drain | separate runner continuation image if resuming a simulation | no; authoritative runner state |
| Socket/session transport | gateway connection/owner, ephemeral | connect, frames, disconnect, shutdown | separately designed identity and delivery policy; never infer restored sessions | no; external ephemeral state |
| Normalized observation/digest | verification API / Phase 7A hash | inspection / simulation hashing | compare only; not a substitute for continuation data | yes; diagnostic |

## Implemented diagnostic hardening

`Exchange::verification_state_bounded(max_words)` performs a checked,
allocation-free shape preflight on histories, sessions and book counts before
building the output or ID normalization map. It bounds output growth, requested depth and per-level
order extraction; it relies on the existing live-book invariant that level
counts match actual orders. `snapshot::capture` and `compare` use the 8 MiB
CHROVIEW word limit. Temporary allocations remain bounded by that limit's
derived counts, but the limit is not a strict peak-byte memory budget: maps,
vectors and the writer's encoded copy have overhead. Capture runs on the owner
thread, throws on size/allocation failure and never changes exchange state.
The format remains diagnostic, with normalized IDs and no continuation image.

## Restricted future interface and acceptance

1. A bounded reader validates version, lengths, checksum, configuration and
   semantic fields without mutating the exchange or shared allocator.
2. A private builder reconstructs books, queues, ownership and counters using
   an isolated identity context. It checks all cross-references, FIFO,
   positions, reservations, revision exhaustion and aggregate arithmetic.
3. A verifier compares the candidate against an authoritative continuation
   image and exercises fixed follow-on submits, fills, cancels, risk rejections,
   reports and counters. Its test environment must isolate the original and
   candidate allocator contexts so tests do not invalidate global uniqueness.
4. Only a separate, explicitly authorized publication operation coordinates
   the service and owner, checks identity conflicts and atomically replaces the
   old exchange. A failure before that point changes neither live state nor the
   shared allocator. Restored exact IDs cannot be published beside a live copy.

7C.1B can proceed with the allocator-service and restricted-import design,
but a full restorer cannot safely be shipped until these contracts and tests
exist. Crash-consistent journaling, session recovery and durable publication
remain separate future work.

## Validation and performance

Debug, Release and ASan/UBSan each passed 124/124 CTest cases. The matching
CLI, simulation and Phase 7B record/replay CLI passed; formatting and Windows
Git whitespace checks passed. New tests demonstrate shared-ID consumption by
valid replay, bounded capture at and below an exact word limit, and allocation
failure without exchange mutation. Existing tests cover two-exchange ID
uniqueness, cancellation, owner reports, partial fills, matching differential
behavior, simulation reproducibility and gateway behavior. No candidate
construction test or direct global-ID-exhaustion injection is claimed.

No allocator, matching, submit, cancel or gateway hot path was changed, so no
allocator-speedup claim is made and candidate-construction benchmarks are not
available. Diagnostic capture was rerun with the existing Release benchmark on
WSL2 (Intel i7-13620H, GCC 13.3, three repetitions, 0.03-second minimum).
Median repetition-mean wall times were 102 ns empty, 7.561 µs at 100 resting
orders and 120.197 µs at 1,000. The prior 7C.1 measurements were respectively
159 ns, 6.439 µs and 126.464 µs; the separately collected figures differ by
about -36%, +17% and -5%. Short runs, WSL2 scheduling and concurrent builds
make these descriptive observations, not a controlled regression estimate. The
100-order committed Phase 7B state-capture baseline was 6.418 µs. All of these
measure observations rather than exchange command latency or durable I/O.
