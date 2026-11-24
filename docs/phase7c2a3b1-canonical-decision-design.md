# Canonical prepared-decision prerequisites

Status: **incomplete**. `CanonicalCommand` is an immutable-value input encoding,
not a prepared decision, journal payload, admission check, owner capability or
durable authorization. The opt-in summary copies it but it does not drive
gateway or exchange execution.
It cannot install an order or expose a candidate. It can be retained indefinitely
as a copy of input, and therefore intentionally has no claim to be a scoped view
of a live prepared transaction. No outcome is manufactured from it.

An additional `InstalledSummary` path derives a bounded **partial** snapshot
from an accepted private candidate before the nonthrowing install, and returns
it only after installation. It contains the result, new submit ID, exchange
command/event counters, trade deltas, recipient-tagged private-report deltas,
authoritative public events, exact command input, allocator high-water,
per-symbol matching counters, book revision, feed sequence and initiating
position before and after. It also retains the initiator's final report when
one exists. Rejections return unchanged counters and no candidate outputs. It is
not a prepared view, install handle, complete outcome or journal encoding. It
is opt-in and does not alter the ordinary gateway path.

## Field inventory

| Field | Before installation | Current exposure and remaining requirement |
| --- | --- | --- |
| Session, request, client ID, symbol, side, type, price, quantity | Exact inputs to owner call | Summary retains raw command values; it does not validate business rules. Cancel has only session/request/client. |
| Accepted or rejection reason | `Prepared::result` | Existing scoped callback receives `Result`; rejected commands have no candidate. |
| Reserved internal order ID | Accepted submit's held allocator reservation | Summary exposes the accepted ID after installation; rejection exposes no new ID. Cancellation does not allocate an ID. |
| Trades | Candidate audit trade vector | Candidate has full history. An ordered, bounded delta with both participants must be extracted; order and starting index must be checked. |
| Private reports | Candidate sessions' report queues and global event counter | Existing queues include old and new reports. A delta must retain destination and event order, including disconnected sessions, without exposing queue references. |
| Public events and gaps | Candidate feed sequences and per-session subscriptions | The opt-in summary captures generated trade/book events before subscriber filtering. Snapshot, recipient delivery, gap and resubscription remain separate and are not captured. |
| Command, report and feed counters | Live and candidate states | Summary captures exchange counters and, for a known symbol, matching command/event/trade counters, book revision and feed existence/sequence before and after. |
| Allocator high-water | Held reservation for submit; short mutex-protected sample for cancel | Before/after values are returned only after the scope. Exclusive registration excludes competing live allocators. |
| Ownership, books, positions and risk | Candidate state and engines | The initiating session's position for a known symbol is captured; counterparties' position/risk and complete ownership/book deltas are not. Exact state words remain diagnostic, not a stable journal encoding. |

The summary builder compares candidate and live report queue lengths, requires
the same session set, checks the exact global event increment, copies only the
suffix, and sorts reports by unique exchange event sequence. It takes trade
suffixes in execution order and checks the command counter advances once. It
caps summaries at 1,024 trades and 4,096 reports using checked counts. A larger
candidate or allocation failure throws before installation, destroying the
candidate and releasing the ID reservation. These limits can reject an
otherwise acceptable command through this opt-in API; ordinary submit/cancel
do not use it. The snapshot can persist as a historical value and cannot
install or mutate state.

The opt-in summary sets a private, non-owning vector sink only on the isolated
work exchange. `State::clone` propagates it to the transaction candidate; the
publisher captures after checked per-symbol sequence allocation and before any
subscriber filtering. The pointer is cleared before the candidate is returned
for installation. Events preserve symbol, sequence, kind, integer price and
quantity fields; the subscriber-specific generation token is zero in the
authoritative copy. One event is captured per trade and at most one best-book
change per command. With the existing 1,024-trade summary limit this is at most
1,025 captured events. The sink throws before installation if it would exceed
that limit; allocation exceptions also discard the candidate. Summarization
checks event sequences against the before/after feed counter and trade values.
Normal exchange and gateway operations do not enable the sink or allocate the
capture vector, although publication performs a null-pointer check.

The 1,025-event limit follows from the current call sites: an accepted submit
publishes exactly one Trade event per execution, followed by at most one Book
event if its before/after best bid or ask changed; a successful cancel can
publish at most one Book event. Market orders use the same submit path. Risk,
capacity and other rejected transactions do not install their private feed
changes. Subscribe snapshots reuse the current sequence, and gap markers use
the recorded missing sequence rather than allocating another one. No other
exchange call site increments the feed sequence. The summary's 1,024-trade
cap therefore requires at most 1,025 captured public events; this is specific
to the present implementation, not a protocol-wide invariant.

Initial subscription snapshots, per-session queueing, dropped updates, gap
markers and resubscription are delivery artifacts. They are not extra
authoritative trade/book events. They remain represented in subscription state,
not in this public-event list. The last initiator report gives an authoritative
remaining quantity and report kind, but is not a separately defined canonical
order-status enum. A complete decision still needs subscriber/gap transitions,
external exchange identity binding, counterparty risk/position and full
ownership/book verification metadata, and a strict complete encoder/decoder.

In addition, session creation/closure, risk configuration and kill state,
subscription changes and report consumption mutate continuation state outside
the submit/cancel record. A replay verifier needs an explicit ordered treatment
of those owner operations or a separately verified state boundary. Merely
serializing these selected command summaries cannot reconstruct that history.
An exchange identity/epoch is also absent from the in-memory exchange; binding
one is a separate lifecycle contract, not something the summary can infer.

Exchange rejection currently leaves authoritative state, exchange counters and
request identity unchanged. The gateway generates a rejection frame separately;
the future journal must explicitly decide how to sequence that outcome without
inventing an exchange report or consuming an ID.

## Input encoding v1

The standalone input codec uses exactly 52 bytes, big-endian: u16 version 1,
u8 kind (1 submit, 2 cancel), u8 zero, u64 session, u64 request, u64 client,
u32 symbol, u8 raw side, u8 raw order type, u16 zero, i64 price encoded as
two's-complement bits, and u64 quantity. All bytes from offset 28 onward must
be zero for cancel. Decode rejects any other length, version, kind, reserved
bits or nonzero cancel fields. Submit retains even invalid enum and integer
values exactly so a rejected command is not silently rewritten. Domain/risk
validation remains exclusively in the exchange. Encoding uses a fixed array,
no heap allocation and no integer-size arithmetic beyond fixed positions.

This is a preliminary command component, **not** a frozen whole-decision or
provisional inspector payload. There is no checksum or digest in these 52
bytes. A future versioned outcome envelope must explicitly define SHA-256 and
CRC coverage, collection counts, size ceilings, ordering and reserved fields;
the current inspector treats payload bytes as opaque. Any incompatible
inspector format requires an explicit version and migration policy. Do not
reuse or reinterpret Phase 7B or CHRSTATE formats.

## Lifetime and allocation authority

The current `Prepared` is private and held in a synchronous frozen owner
scope. No install handle escapes, and install occurs at most once inside that
scope; abort and exceptions destroy the candidate. The process-wide allocator
reservation owns its mutex from submit preparation through decision and
installation/abort. The exclusive registry lease prevents competing live
exchange registration; isolated simulation/replay allocators remain independent.
The owner thread is frozen against reentrant mutation and `finish` until return.

Holding this lock through synchronous storage is mechanically simple but makes
storage stalls block owner shutdown and allocator access, and arbitrary callback
I/O can deadlock. An owner-scoped durable lease could shorten the mutex hold,
but is unsafe unless every allocation entry point recognizes it, high-water and
candidate lifetime are tied to exclusive registration, all mutations remain
frozen, and shutdown/failure cannot release authority prematurely. A separate
process allocation gate still needs an atomic high-water publication boundary
and process-wide coverage. None is implemented or proven here. Continue holding
the present reservation in existing non-durable scopes; do not introduce disk
I/O or early unlock. The smallest defensible next step is a private owner-held
bounded decision builder and lifetime proof before selecting or implementing a
durable lease.

The future internal storage result needs `NotAttempted`, `Authorized` and
`FailedStop` states, created only by the owner/storage state machine. No caller
boolean may impersonate authorization. Any ambiguous persistent write/sync or
manifest failure must stop admission and withhold outputs; a durable decision
before installation has an uncertain client outcome and requires separate
recovery. No such state machine or durable mode exists today.
