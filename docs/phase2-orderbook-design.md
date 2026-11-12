# Phase 2 proposal: limit order book

Historical Phase 2 design: Phase 3 has since implemented checked fills and matching; see [Phase 3 matching design](phase3-matching-design.md) for current behavior. The proposals below describe the earlier design state.

The Phase 2 implementation owns and organizes **resting** limit orders for one instrument. It exposes best levels and controlled operations to insert, modify, and remove resting orders. The matching engine will own incoming-order policy, decide executions, and produce trade events. Neither book nor future engine depends on a TCP protocol. The sections below retain the original design alternatives and future fill proposal; see the [implementation report](phase2-implementation-report.md) for the actual API and guarantees.

## Representation alternatives

1. **Ordered price maps with FIFO lists and an ID index.** Use `std::map<Price, Level, std::greater<Price>>` for bids and `std::map<Price, Level>` for asks. Each level has a `std::list` of resting orders in arrival order and an aggregate quantity. An `std::unordered_map<OrderId, Location, OrderIdHash>` indexes each order by ID; a location identifies its side, level, and list iterator. This is straightforward and permits stable iterators and direct cancellation, at the cost of allocations and cache misses.
2. **Dense tick-indexed arrays with queues and an ID index.** An array maps each tick in a bounded range to a level, with FIFO queues and an ID index. Level access can be constant time and locality can be better, but a wide or changing price range wastes memory and finding the next nonempty level needs a scan or an additional index. Range limits and resizing must be explicitly handled.

The implementation uses the ordered-map design. Its range is not predetermined and its complexity is easy to test. Measure representative workloads before considering a dense ladder, intrusive storage, or custom allocation. The index provides an explicit hash for the identifier wrapper rather than relying on implicit conversions.

## Operations and priority

The highest bid and lowest ask are each the first entry in their respective maps. A new resting order is appended to the FIFO at its price. Priority follows successful book calls, not externally supplied sequence values; the supplied sequence is retained as original metadata. The book checks instrument, side, positive limit price and positive remaining quantity, and rejects duplicate live IDs before mutation. Market orders do not rest.

Cancellation finds an ID, removes its node, updates the level total, removes an empty level, and erases the index entry. An unknown ID returns not-found. Same-price reductions and unchanged quantities retain priority; price changes or quantity increases lose priority. Zero quantity is rejected; use cancellation explicitly. No new sequence is allocated by the book.

Future work must define a dedicated fill operation: modification is not a fill and no trade or fill state is implemented. The engine will decide executions and create trade events. Book mutation and event production need a defined failure/commit boundary so an exception cannot leave state and emitted events inconsistent.

### Proposed Phase 3 integration boundary

Add a read-only `best_resting(side)` operation returning an optional value snapshot of the FIFO head at the best price in constant time; it should not expose a list iterator or copy an entire level. Add a separate `apply_fill` operation accepting the selected order identity and a positive executed quantity. It must verify the selected order is still the expected FIFO head, reject missing, stale, zero or excessive fills without mutation, subtract from both the order and level total, keep the position on a partial fill, and remove the order, index entry and empty level on a full fill. The engine determines crossing, execution price and trade IDs; the book never matches autonomously.

Because a cancelled ID can be reused, a bare ID or the existing externally supplied sequence cannot prove that a snapshot still refers to the same arrival. Either make selection and fill one strictly serialized operation with no intervening book mutations, or introduce a book-issued generation token with an explicit exhaustion policy and validate it at fill time. Do not infer identity from price, quantity or sequence metadata. Define whether fills of non-head orders are ever legal. The engine must also stage event resources and define an atomic commit boundary so an allocation or publication failure cannot leave the book and event stream inconsistent. These are proposed APIs and guarantees, not current functionality.

## Ownership and invariants

The book owns each resting order in exactly one level list. Index entries are non-owning locations and must be erased before their pointed-to node or level becomes invalid. No external pointer or iterator survives removal; consumers should use IDs or short-lived views. One thread exclusively mutates a book; event sequencing and any communication with other threads belong outside this design.

Prices are positive signed integer ticks; instrument-specific tick size is not yet modeled. Resting quantities are positive unsigned units and the level aggregate equals their sum. Additions check overflow; subtraction uses maintained invariants. The book exposes current amended quantity rather than tracking original quantity or fills. IDs may be reused after cancellation, including zero. The book does not allocate or increment sequence numbers; engine sequencing and wraparound policy remain future work.

## Complexity and failure handling

For `L` price levels and `N` orders, best-price access is constant time via `begin()`. Insertion is `O(log L)` for level lookup plus average `O(1)` ID indexing and list append. Cancellation and reduction are average `O(1)` lookup and node removal, with `O(log L)` worst-case level removal by key; hash lookups have `O(N)` worst case. Finding a level is `O(log L)`. Space is `O(N + L)`. These are asymptotic expectations, not measured performance guarantees.

Return explicit errors for invalid inputs, duplicate IDs, missing orders and arithmetic overflow. There is no fill API or overfill check yet. Allocation can fail; changes are staged or rolled back to preserve book and index invariants. Error policy, event persistence and recovery are engine-level decisions.

## Test strategy and edge cases

Test bid/ask ordering, FIFO at a level, multiple levels and instruments, insertion at best and non-best prices, duplicate IDs, missing cancellations, removal of the last node at a level, partial and full reductions, and all modification priority rules. Exercise quantity and price boundaries, aggregate overflow, overfill, invalid sides and market orders, sequence and ID reuse policy, and allocation failure if an injectable allocator becomes justified. Assert index, aggregate and membership invariants after each operation. Compare randomized operation sequences against a simple reference model and check deterministic output. Separately test engine crossing and trade decisions: the book itself does not match incoming orders.

Benchmark only after correctness using representative depth, order lifetimes, cancellation ratios, and price distributions. Measure end-to-end and per-operation costs, allocations, and latency distributions; do not assume a container is faster without data.
