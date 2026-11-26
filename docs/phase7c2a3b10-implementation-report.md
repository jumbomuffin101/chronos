# Isolated recorder authorization prototype report

The owner has an opt-in, synchronous, explicitly non-durable in-memory decision source, while the original constructor remains direct. The actual recorder prepares a private candidate and record before making its decision. Its bounded request includes owner identity, fixed configuration and capacity, sequence, chained prefix digest, command, outcome, exact record lengths and record bytes. A private sealed authority computes the request digest and length; no caller-provided token is accepted. A mismatch or simulated ambiguous decision fail-stops admission. Simulated refusal discards the candidate. The request allocation is retained through the pointer-swap and count-publication interval.

The prototype does **not** implement external storage, journal durability, a trusted terminal boundary, crash recovery, or a live gateway. Its authority is synchronous and cannot model concurrent shutdown or real storage exceptions. The prefix chain is not a separately trusted terminal. This is not a completed or independently audited authority interface and is not ready for journal integration.

## Validation performed

WSL2 Ubuntu 24.04: Debug **262/262** in 2.47 s; Release **262/262** in 1.41 s; ASan/UBSan **262/262** in 10.74 s. These include protocol and gateway regressions. The CLI produced two fills and a 30-unit remainder. Seed-42 simulation produced five accepted commands and two trades; Phase 7B replay verified 12 records. CHROVIEW comparison passed; the 9,348-byte CHRSTATE image restored with matching exact state and follow-on commands.

Recorder tests cover direct equivalence across multilevel partial fills, same-price FIFO, market orders, risk, cancellation, report capacity, consumption, closure and rejections; authorized recording; refusal; ambiguity; modified identity/configuration/sequence/prefix/command/canonical state/length/digest; a replayed prior decision; failed preparation; allocation failure; reentrancy during allocation and exception cleanup; independently reconstructed prefix continuity; and the 128-operation limit with replay. The test-only request mutations exercise rejection when the authority sees altered bytes; they do not implement or prove a real authority protocol. There is no asynchronous decision, stop or callback interface through which delayed responses or a different authority instance could be delivered. Those future interleavings remain untested. No browser-visible behavior changed.

## Initial performance observation

On WSL2, Intel i7-13620H, GCC 13.3.0, `-O3 -DNDEBUG`, each of three runs used 20 warmups and 300 timed empty-state open operations. Setup and replay verification were outside timing; ordinary `operator new` calls were counted on the measured thread. No filesystem I/O is included. Aligned and external allocations are not counted. The table gives nanoseconds and per-operation ordinary allocations/bytes:

| Run | Direct p50 / p95 / p99 | Gated p50 / p95 / p99 | Direct allocations / bytes | Gated allocations / bytes |
| --- | --- | --- | --- | --- |
| 1 | 1,337 / 2,461 / 2,810 | 20,203 / 41,166 / 91,014 | 16 / 2,007 | 17 / 2,855 |
| 2 | 1,893 / 3,826 / 4,717 | 28,685 / 56,103 / 141,973 | 16 / 2,007 | 17 / 2,855 |
| 3 | 1,882 / 1,927 / 3,314 | 15,035 / 24,557 / 26,322 | 16 / 2,007 | 17 / 2,855 |

The probe separately measured 1,000 SHA-256 calls on 832-byte inputs after 50 warmups: p50/p95/p99 were 6,350/10,518/23,901 ns, 7,864/10,274/28,760 ns, and 4,556/6,652/9,004 ns. The gate hashes the full request three times: for the owner binding, the in-memory decision and revalidation. These standalone measurements are indicative rather than an additive breakdown of the operation distributions. Cloning, execution, encoding, decision validation and publication were not separately timed. Do not remove checks without a separate correctness proof. WSL scheduling and short-interval clock effects limit interpretation; this is not production trading or storage latency.

Reproduce with `c++ -O3 -DNDEBUG -std=c++20 -Iinclude tests/exchange/recorder_authorization_benchmark.cpp build/release/libchronos_core.a -pthread -o /tmp/recorder-authorization-benchmark` and run the executable. Stage-level allocation attribution and real-storage prerequisites remain open.
