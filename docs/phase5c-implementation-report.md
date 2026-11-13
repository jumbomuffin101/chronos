# Phase 5C implementation report

Phase 5C adds local session-account risk controls and a public sequenced market
feed to the loopback simulator. It does not provide production authentication,
durable delivery, replay, recovery, replacement or real-market connections.
Protocol version 3 rejects older versions and adds risk rejection codes,
subscribe/unsubscribe, snapshots, public trade and best-book events, and gaps.
The exact layouts and default local policy are in the [protocol](phase5-protocol-spec.md).

Each connection is a separate development account. The embedding application
can provide default limits, configure an account with no open orders, or toggle
its kill switch through a bounded control queue processed by the same owner
thread as network commands. Kill blocks all new submissions but permits
cancellation; outstanding orders are not automatically cancelled. Per-order
quantity and checked notional, aggregate open notional, open-order count,
symbol permission, position and finite market-price limits are enforced before
acceptance. Market limits are conservative: notional uses the configured price
ceiling, and encountered levels above that ceiling are rejected. Position
limits include same-direction open reservations. Reservations are computed from
the exchange-owned book; positions and the book change in the same staged
candidate. Fills update both participants, cancellations release book-derived
reservation, and self-trades have zero net position change.
Crossing sell limits use the higher of their limit and the best executable bid
as the conservative incoming notional price.

Every candidate clones the engines, positions, policy, identity maps, private
reports, feed counters and subscription queues. Matching and all risk and
output changes are staged before a nonthrowing state swap. Rejections,
counter exhaustion, private-report capacity failures and allocation exceptions
leave the live book and positions unchanged. Feed queues are bounded; overflow
marks the subscriber out of sync and produces a gap after already queued events.
Resubscription starts with a fresh snapshot at the current symbol sequence.
Generation tokens stop a stale in-flight frame from removing a newer snapshot.
Public events contain no account or order IDs. Disconnect removes subscriptions
but leaves the Phase 5B private-order retention policy in place.

## Demonstration and validation

A real loopback demonstration used separate seller, buyer and subscriber TCP
connections. The subscriber received an empty snapshot at sequence 0, an ask
of five at 100 at sequence 1, a trade of three at 100 at sequence 2, and the
remaining ask of two at sequence 3. The buyer received a full fill of three;
the seller received a partial fill with two remaining. The matching CLI also
ran successfully. Final Debug, Release and ASan/UBSan builds each passed
**87/87** CTest cases. The engineering review fixed crossing-sell notional
checks to use the best executable bid when it exceeds the incoming limit, and
added regression coverage for both order and exposure limits and multi-account
fills. Tests include risk boundaries, partial fills and
reservation release, owner cancellation and kill behavior, public feed order,
gap/resnapshot, generation protection, sequence exhaustion, version rejection,
three-client TCP flow and allocation failure while staging fills or snapshots.
Formatting and Git whitespace checks passed. WSL's Windows-mounted filesystem
emitted clock-skew warnings during incremental builds; affected files were
rebuilt and all suites passed.

## Performance assessment

These are exploratory Release measurements on WSL2 Ubuntu 24.04 with GCC
13.3.0 and an Intel i7-13620H. Each cell has one warmup and nine fresh-fixture
samples; p95/p99 would not be meaningful. Host scheduling and CPU frequency
were uncontrolled. The figures were collected before the final crossing-sell
notional correction; both benchmark programs passed correctness checks again
after that correction. In the prior fresh-fixture benchmark with 100 resting
orders and no market subscribers, Phase 5B.1 exchange medians were 13.829 µs
and 224 scalar C++ allocations for a noncrossing order, and 29.777 µs and 225
allocations for a single fill. Phase 5C measured 15.935 µs / 228 allocations
and 13.434 µs / 229 allocations respectively. These separate, small runs are
too noisy to infer a speedup or a precise incremental cost. The corresponding
Phase 5C loopback medians were 92.138 µs and 68.678 µs; these include socket
and scheduling costs, not external-network latency. The prior
[Phase 5B.1 samples](assets/phase5b1/comparable-after.csv) are retained. The
Phase 5C benchmark executables can regenerate CSV samples under the ignored
`build/` directory; generated measurement files are excluded from the commit.

A separate two-account exchange-only fixture with 100 resting orders measured
median noncrossing latency of 10.938 µs / 229 allocations with no subscribers
and 11.163 µs / 241 allocations with four subscribers. Single-fill medians were
10.936 µs / 232 allocations and 12.586 µs / 248 allocations respectively.
These are nine samples per cell, with setup and report draining outside the
timed command; only scalar C++ `operator new` calls on the calling thread are
counted. They are not peak memory or end-to-end network measurements. See the
`chronos_phase5c_baseline` in the [development instructions](development.md).
Clone cost grows with all books, retained accounts, risk state, feed counters,
subscriptions and queued events; measure with representative loads before a
Phase 8 transaction redesign.

## Limitations

Session IDs are not authenticated account identities. Positions, limits,
subscriptions, queued data and audit state are in memory only; a process crash
or object destruction loses them. TCP writes do not prove application receipt;
there is no replay or exactly-once guarantee. Private report capacity can reject
commands; slow feed subscribers can fall behind and must resnapshot. Feed
subscriptions are capped at 32 symbols and 32 queued events per symbol, and the
control queue is capped at 32 requests. A started local administration operation
waits for a definitive outcome even if its initial wait timeout expires. Public
feed scheduling is ordered per symbol but does not promise fairness across
symbols. Risk policy is conservative and does not model collateral, portfolio
netting, production instrument metadata or real-market risk. The market-price
ceiling is not a minimum sell-price floor or slippage protection.
