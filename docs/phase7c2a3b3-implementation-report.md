# Ordered mutations: prerequisite implementation report

The bounded effect-capture milestone was committed and pushed as `c220f6ae25e4989f0a712f4f30b11e57f110ca7a`. This prepared-administration milestone does not implement a durable journal, operation-sequence dispatcher, crash recovery or live publication. The [ordered mutation design](phase7c2a3b3-ordered-mutation-design.md) inventories owner operations, their admission/rejection behavior, replay/reset choices and current bypasses.

## Implemented scope

Two opt-in exclusive-owner scopes, `with_prepared_open_session` and `with_prepared_risk_update`, build private candidates using existing Exchange operations. The callback sees only `Result`, and a false return or exception discards the candidate; accepted authorization uses the existing nonthrowing install swap. Administrative rejection currently maps to `Result::InvalidOrder` without a detailed reason. There is no allocator reservation and no public or private output from these operations. The freeze prevents owner reentrancy. The normal gateway continues to use its existing direct administrative functions and submit/cancel paths.

Risk preparation performs a state clone, then calls the existing `configure_risk`, which clones again. This avoids a second implementation of the rules but is intentionally not optimized. No operation index was added: it would be unsafe to present an index as authoritative while session closure, report consumption, subscription handling and other mutations still bypass the boundary. The design specifies a future owner-assigned ephemeral identity, business-rejection ordering and distinction from durable LSNs; implementing and enforcing it is outstanding. The existing exact word vector is a bounded diagnostic, not a stable checkpoint or canonical wire format.

## Validation

The WSL-native build used GCC 13.3.0 and locally cached GoogleTest. Debug, Release and ASan/UBSan each passed **221/221** tests, in 0.95 s, 0.82 s and 3.36 s. The new scoped-admin regression checks session creation, duplicate rejection, no early publication, callback abort/exception, risk update, risk rejection and refusal to change risk with a resting order. The allocation-failure sweep checks exact continuation-state equality after every failed risk preparation. Existing suites cover session closure with resting orders, report consumption and queue capacity, subscription and feed-gap behavior, gateway disconnect, and submit/cancel transaction safety; no new operation index or replay coverage is claimed.

The final review added invalid-session, shared-owner, wrong-context and reentrant administrative preparation checks. Debug, Release and ASan/UBSan each passed **222/222** tests, in 0.91 s, 0.85 s and 3.46 s respectively.

Selected protocol, two-client gateway, exclusive gateway and publication-gate tests passed **18/18** in Release. The matching and simulation CLIs ran; Phase 7B verified 12 records, five commands and four trades. CHROVIEW comparison and CHRSTATE isolated restoration with follow-on state comparison passed. The repository clang-format check and Git whitespace check passed. These checks do not establish crash durability or session recovery.

## Performance

Measured on WSL2 Linux 6.6.87.2, Intel i7-13620H, GCC 13.3, Release. [Probe source](../benchmarks/ordered_mutation_probe.cpp) uses 10 warmups and 200 samples per case; fixture creation and population are outside the timed section. `open` stages and installs a session; `risk` stages and installs a risk update for a session without open orders; `exact-state` constructs and destroys the diagnostic vector. The 32-order case has a separate participant with 32 resting orders. Counts cover scalar `operator new` on the benchmark thread only; they do not measure all allocation paths or peak RSS. Durations include result handling and candidate cleanup. Fixed-order, unpinned WSL samples are descriptive and not production latency.

| Operation | Background orders | p50 | p95 | p99 | Median scalar allocations | Median requested bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Prepared open | 0 | 0.410 µs | 0.495 µs | 0.540 µs | 5 | 1,000 |
| Prepared risk | 0 | 0.413 µs | 0.705 µs | 0.801 µs | 7 | 1,496 |
| Exact state | 0 | 0.266 µs | 0.299 µs | 0.368 µs | 1 | 232 |
| Prepared open | 32 | 7.028 µs | 13.227 µs | 15.601 µs | 84 | 13,208 |
| Prepared risk | 32 | 9.564 µs | 18.190 µs | 23.825 µs | 165 | 25,912 |
| Exact state | 32 | 7.935 µs | 15.618 µs | 19.264 µs | 36 | 11,504 |

Reproduce after a Release build with `c++ -O3 -DNDEBUG -std=c++20 -Iinclude benchmarks/ordered_mutation_probe.cpp build/release/libchronos_core.a -pthread -o /tmp/chronos-admin-probe` and `/tmp/chronos-admin-probe`. [Raw CSV](assets/phase7c2a3b3/chronos-admin-probe.csv) contains all 1,200 measured samples. These fixed-order single-run observations are not a controlled comparison with the earlier machine run. Sequence-assignment overhead cannot be measured because assignment is not implemented. Existing submit/cancel code paths were not changed; no new submit/cancel latency comparison was measured. More book sizes, repeated randomized runs and peak memory measurement are needed.

## Remaining blockers

Enforce one boundary for every owner operation and define complete bounded administrative encodings, including expected private report identities; implement and test no-reuse sequencing, failed preparation and authorized rejections. Reconcile private report obligations and startup admission; verify exact allocator ownership/high-water across all users. Specify and validate stable state encoding and storage fault handling before journal writing or any durable acknowledgment. Authenticated reconnect, live publication, crash recovery and exactly-once delivery remain unsupported.
