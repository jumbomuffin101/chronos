# Phase 2 proposal: limit order book

This is a design proposal, not an implementation or a commitment to a particular API. The book owns and organizes **resting** limit orders for one instrument. It exposes the best levels and controlled operations to insert, reduce, and remove resting orders. The matching engine owns incoming-order policy, selects whether and how liquidity trades, assigns event sequences and trade IDs, and produces execution events. Neither the book nor engine should depend on a TCP protocol, account policy, or a dashboard.

## Representation alternatives

1. **Ordered price maps with FIFO lists and an ID index.** Use `std::map<Price, Level, std::greater<Price>>` for bids and `std::map<Price, Level>` for asks. Each level has a `std::list` of resting orders in arrival order and an aggregate quantity. An `std::unordered_map<OrderId, Location, OrderIdHash>` indexes each order by ID; a location identifies its side, level, and list iterator. This is straightforward and permits stable iterators and direct cancellation, at the cost of allocations and cache misses.
2. **Dense tick-indexed arrays with queues and an ID index.** An array maps each tick in a bounded range to a level, with FIFO queues and an ID index. Level access can be constant time and locality can be better, but a wide or changing price range wastes memory and finding the next nonempty level needs a scan or an additional index. Range limits and resizing must be explicitly handled.

Recommend the ordered-map design for the first correct implementation. Its range is not predetermined and its complexity is easy to test. Measure representative workloads before considering a dense ladder, intrusive storage, or custom allocation. The current identifier wrapper does not provide hashing; add an explicit hash when implementing the index rather than relying on implicit conversions.

## Operations and priority

The highest bid and lowest ask are each the first entry in their respective maps. A new resting order is appended to the FIFO at its price. At a given price, earlier accepted sequence has priority; the engine assigns sequences and must ensure that event order is deterministic. The book checks instrument, side, positive limit price and positive remaining quantity, and rejects duplicate live IDs before mutation. Market orders are incoming engine instructions and never rest in this initial design.

Cancellation finds an ID, removes its node, updates the level total, removes an empty level, and erases the index entry. An unknown ID returns a clear not-found result and makes no change. For modifications, propose retaining priority only for a same-price quantity reduction; price changes or quantity increases cancel and reinsert with a new sequence. A zero quantity should be an explicit cancellation, not an ambiguous modification. Exact policy and atomic failure semantics must be agreed and tested before implementation.

The engine decides a fill's price and quantity and then requests a reduction of the selected resting order. The book verifies `0 < fill <= remaining`, decreases remaining and level total, and removes a fully filled order; partial fills retain their FIFO position. The engine creates trade events and updates the incoming order. Book mutation and event production need a defined failure/commit boundary so an exception cannot leave state and emitted events inconsistent.

## Ownership and invariants

The book owns each resting order in exactly one level list. Index entries are non-owning locations and must be erased before their pointed-to node or level becomes invalid. No external pointer or iterator survives removal; consumers should use IDs or short-lived views. One thread exclusively mutates a book; event sequencing and any communication with other threads belong outside this design.

Prices are positive signed integer ticks with an instrument-defined tick size; no floating-point ordering is permitted. Quantities are positive unsigned units, and `remaining <= original`; level aggregate equals the sum of remaining quantities. Reject a level addition if the sum would overflow, and check subtraction before applying it. Sequence wraparound and ID reuse must be handled explicitly rather than silently wrapping. No arithmetic should assume unsigned subtraction is safe. Zero IDs are currently valid at the model layer; any stricter allocation policy belongs at an explicit boundary.

## Complexity and failure handling

For `L` price levels and `N` orders, best-price access is constant time via `begin()`. Insertion is `O(log L)` for level lookup plus average `O(1)` ID indexing and list append. Cancellation and reduction are average `O(1)` lookup and node removal, with `O(log L)` worst-case level removal by key; hash lookups have `O(N)` worst case. Finding a level is `O(log L)`. Space is `O(N + L)`. These are asymptotic expectations, not measured performance guarantees.

Return explicit errors for invalid inputs, duplicate IDs, missing orders, overfills and arithmetic overflow. Allocation can fail; stage changes or roll them back to preserve the book and index invariants. Error policy, event persistence and recovery are engine-level decisions; do not silently accept partially applied operations.

## Test strategy and edge cases

Test bid/ask ordering, FIFO at a level, multiple levels and instruments, insertion at best and non-best prices, duplicate IDs, missing cancellations, removal of the last node at a level, partial and full reductions, and all modification priority rules. Exercise quantity and price boundaries, aggregate overflow, overfill, invalid sides and market orders, sequence and ID reuse policy, and allocation failure if an injectable allocator becomes justified. Assert index, aggregate and membership invariants after each operation. Compare randomized operation sequences against a simple reference model and check deterministic output. Separately test engine crossing and trade decisions: the book itself does not match incoming orders.

Benchmark only after correctness using representative depth, order lifetimes, cancellation ratios, and price distributions. Measure end-to-end and per-operation costs, allocations, and latency distributions; do not assume a container is faster without data.
