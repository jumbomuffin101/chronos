# Phase 5C risk and market-data design

This is a local loopback simulator. Session-to-account mapping is a development
policy, not authentication. No durability, recovery or exactly-once delivery is
provided. Replacement remains unsupported.

## Common transaction boundary

The existing gateway poll thread is the sole owner of exchange state. Risk,
positions, order ownership, per-symbol feed counters and subscriber queues must
be part of the same candidate state as the engines and private reports. A
command validates the account and limits before a live book can change; it
then stages the engine operation, all position and reservation changes, private
reports and bounded market events in a clone. A nonthrowing state pointer swap
publishes the entire candidate. Rejection, sequence exhaustion, allocation
failure or insufficient mandatory private-report capacity must leave the live
state and accepted counters unchanged. No network thread reads mutable books.

## Risk policy

An account has symbol permissions, an enabled/kill-switch flag, maximum order
quantity and notional, maximum open orders, and per-symbol long/short position
limits. Every arithmetic operation must be checked, including price multiplied
by quantity and the sum of current positions and reserved buys/sells. In the
local development policy, accounts and permissions are explicitly configured;
they are not authenticated. Orders are scoped to their owning account/session.
Rejections use explicit codes and cannot consume an accepted command sequence.

Outstanding limit orders reserve their remaining quantity and worst-case
notional at their limit price. Position checks include open-order reservations
in both directions, so fills cannot push the worst-case position beyond the
configured limit. Each fill updates both accounts' positions and reduces the
resting reservation; cancellation releases the remainder. These changes are
staged together with the book. A killed account cannot submit new orders, but
may cancel its outstanding orders; activation does not silently cancel them.
Unfilled market remainders are cancelled. Market orders require a configured
finite execution-price bound, checked against candidate liquidity and notional
before acceptance; otherwise they are rejected. Overflow fails closed.

The local policy maps each session to one account. The gateway constructor
accepts default limits and a local embedding process can configure individual
sessions and toggle the kill switch through a bounded owner-thread control
queue. Configuration is rejected while that account has open orders or an
existing position exceeds the proposed bound. A timed-out control request is
cancelled if it has not started; once started, the caller waits for the
definitive result. Local APIs are not network authentication or administration.
An empty symbol list permits all symbols; the list is capped at 1024. The
default finite market price ceiling is 1,000,000 ticks and other defaults are
deliberately permissive for simulation. All new submissions are blocked when
killed, including those that might reduce risk.

Each session's signed per-symbol position is copied with the candidate state.
Open order quantity and notional are derived from engine-owned resting snapshots
instead of an independently mutable reservation ledger. For a new buy (sell),
the limit check includes current position and all same-direction open quantity;
opposite-side orders do not offset this worst-case bound. The maximum open
notional covers all symbols and conservatively includes the entire incoming
order, even if it will immediately execute or is a market order. At the
open-order limit, a new limit order is rejected even if it might fully execute;
a market order does not count as a resting order. Market-order
worst-case notional uses the configured ceiling, and executable levels above it
are rejected. This is a maximum-price/notional risk bound; it is not a minimum
execution-price floor or slippage protection for market sells. Self-matches
leave that account's net position unchanged. A
partial or complete fill, cancellation, rejection and allocation failure cannot
publish a position independently of its corresponding book state.

A crossing sell limit can execute at a resting bid higher than its own limit.
Its incoming notional check uses the higher of its limit and the best executable
bid for the entire order. This deliberately conservative bound applies to both
per-order notional and aggregate open-notional checks; resting sell reservations
continue to use their own prices.

## Public feed

The owner constructs owned value events containing only symbol, per-symbol
sequence, public aggregate best bid/ask or executed trade price and quantity.
Private account/order identity and execution reports are never included. A
subscription atomically establishes a snapshot at sequence S followed only by
events sequenced after S. Trades are emitted in execution order, followed by a
best-book update when public top-of-book state changes. Subscribers detect gaps
using the symbol sequence and must resubscribe/resnapshot after a gap. Each
subscriber has bounded storage: on overflow it is marked out of sync, stops
receiving new increments, and is sent an explicit gap indication after any
already-in-flight data; it must resnapshot before receiving further increments.
The gateway transports owned snapshots/events and never reads book internals.

At most 32 symbols may be subscribed per session and each subscription retains
up to 32 events. The gap contains the first missed symbol sequence and is sent
after already queued events; no later increments are added until resubscription.
An explicit resubscription replaces pending events with a snapshot at the
current sequence. Internal per-subscription generation tokens prevent a stale
in-flight write from removing an event from a newer subscription. Disconnection
removes subscriptions while retaining the existing private order policy.

Trade events precede a single aggregate top-of-book update per command if the
top changes. Symbol sequences advance even without subscribers. Counter
exhaustion rejects the entire staged command. Empty book sides are encoded as
zero price and quantity. The exact version 3 wire layouts are in the
[protocol specification](phase5-protocol-spec.md).

The clone includes risk configuration, positions, feed counters, subscriptions,
and queued feed events in addition to all books, orders and private reports.
This grows both per-command copy time and temporary memory with account and
subscriber state. Performance should be measured before any Phase 8 redesign;
the consistency guarantees above are required regardless of implementation.
