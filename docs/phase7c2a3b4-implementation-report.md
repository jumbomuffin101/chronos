# Unified owner mutation boundary: implementation report

The preceding prepared-administration milestone was committed and pushed as `bc8d3d245a2c45c9d272374818e59f0ddc205d76`. This milestone adds an owner-local, non-reentrant dispatcher, a checked **diagnostic completed-call index**, and opt-in prepared session closure. It does not implement an authoritative replay sequence, durable journal, crash recovery or live restored-exchange publication. See the [design and call-site inventory](phase7c2a3b4-unified-mutation-design.md).

Owner submit/cancel, their prepared/summary paths, session and risk changes, subscription operations, and both dequeue paths go through the dispatcher. Accepted calls, business rejections and no-ops increment the index after completion. A prepared callback returning false, preparation failure, exception or wrong-context call does not. The existing result and installed flag distinguish outcomes; the counter alone does not distinguish committed state from rejection. Overflow fails before the operation, and shutdown remains possible. The index is not present in exact-state words and is not a journal LSN. It does not cover owner bind/finish, transport activity, other owners, isolated exchanges, process-wide shared allocator activity or test-only seed seams. There is no ordered input/outcome record, durable authorization or replay support.

Prepared closure clones state and invokes existing `close_session`. It preserves resting orders and pending reports, clears subscriptions, and retains a session with any historical order or pending report. A missing session is an administrative rejection; abort/exception destroys the candidate. This differs from direct closure's missing-session no-op only in the returned prepared result. No gateway call site was switched to prepared closure; shared and exclusive gateway paths retain their existing publication and backpressure behavior.

## Validation

GCC 13.3 on WSL2: Debug **225/225** (1.08 s), Release **225/225** (0.78 s), ASan/UBSan **225/225** (3.66 s). New tests cover mixed operation ordering, abort without index/state change, closure preserving a resting order, a subsequent counterparty fill, private dequeue, business rejection, subscriber gap consumption/resubscription, diagnostic exhaustion before mutation and allocation failure without index advancement. Existing tests exercise reentrancy, wrong owner, queued controls, shutdown, shared/exclusive gateway and protocol paths. The first gap fixture exceeded its report capacity; the fixture was corrected and all suites rerun.

Final review added an explicit test of business rejection, no-op, callback exception, authorized rejection and mutation after shutdown. Debug, Release and ASan/UBSan each passed **226/226** tests (0.92 s, 0.84 s and 3.49 s).

The selected protocol/two-client/exclusive/publication-gate suite passed **18/18**. Matching CLI, five-command liquidity-sweep simulation and Phase 7B replay passed (12 records, four trades). CHROVIEW comparison and isolated CHRSTATE restore passed, including exact state and follow-on operations. Repository formatting and Git whitespace checks passed. No browser checks were run because the wire protocol and ordinary gateway output semantics were not changed; counter exhaustion is an internal fail-closed limit, not a durable mode.

## Measurements

Environment: WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3 Release; 10 warmups and 200 samples per row. Setup and checks are outside each timed operation. Fixed-order, unpinned runs and sub-microsecond clock effects limit interpretation. Scalar allocations/bytes cover only the measuring thread and scalar `operator new`; peak RSS and aligned allocations were not measured. The direct `Exchange` comparison differs from owner calls in context checks and lifecycle, so it does not isolate only dispatch or index increment.

| Operation | Direct p50 / p95 / p99 | Owner p50 / p95 / p99 | Direct / owner median allocations | Direct / owner median bytes |
| --- | --- | --- | --- | --- |
| Noncrossing submit | 0.516 / 0.582 / 3.331 µs | 0.512 / 0.553 / 0.597 µs | 18 / 18 | 1,560 / 1,560 |
| Cancel | 0.675 / 0.725 / 0.792 µs | 0.679 / 0.737 / 0.764 µs | 19 / 19 | 1,952 / 1,952 |
| Private report pop | 0.038 / 0.040 / 0.040 µs | 0.044 / 0.046 / 0.046 µs | 0 / 0 | 0 / 0 |

The observed 6 ns median pop difference includes owner checks and timing noise; it is not a reliable isolated estimate of index assignment. Submit/cancel differences also fall within these measurement limitations. In a separate prepared-operation probe, with 32 background orders, prepared open measured p50/p95/p99 **5.643/7.227/9.444 µs**, 84 allocations and 13,208 requested bytes; prepared risk measured **8.495/11.150/12.181 µs**, 165 allocations and 25,912 bytes. Risk still clones once for the private candidate and again inside existing risk configuration; this comparison includes different operation logic and cannot isolate the second clone's time. At zero background orders, corresponding p50s were 0.190 µs and 0.306 µs. Samples are in [the raw assets](assets/phase7c2a3b4/).

After a Release build, reproduce with `c++ -O3 -DNDEBUG -std=c++20 -Iinclude benchmarks/mutation_dispatch_probe.cpp build/release/libchronos_core.a -pthread -o /tmp/mutation-dispatch-probe` and run the executable. Substitute `benchmarks/ordered_mutation_probe.cpp` to reproduce prepared administration and exact-state measurements. The existing Part A report contains prior measurements and raw inputs; they are separate unpaired runs, not a controlled pre/post comparison.

## Remaining work

A future owner boundary must record full typed inputs and terminal outcomes, recipient and expected front report identity, lifecycle/epoch and allocator authority; distinguish authorized rejections from internal failures and bind any authoritative index to installation and external publication. Private obligations require reconciliation or an explicit admission gate; public subscribers require a fresh-session/resnapshot policy. A stable canonical state codec, persistent writer, durable authorization, fail-stop storage behavior and crash-consistency tests are not implemented. No authenticated reconnect or exactly-once delivery is claimed.
