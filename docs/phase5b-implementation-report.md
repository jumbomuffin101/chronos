# Phase 5B implementation report

Chronos routes version 2 loopback submissions and cancellations to simulated
per-symbol matching engines. A single poll thread owns the exchange, assigns
gateway command/report sequences, routes fills to both participants and
enforces session ownership of cancellations. Internal order IDs are unique
across Exchange instances for the process lifetime. Accepted request IDs and
client order IDs are retained per session; rejected requests can be retried.
The protocol layouts and result values are in the [specification](phase5-protocol-spec.md).

Transactions are prepared against an independent snapshot of all exchange
state. The candidate book, ownership records, counters, private reports and
trade audit are published together with a nonthrowing pointer swap only after
all staging succeeds. Allocation failure or report saturation discards the
candidate. Allocation-failure tests exercise both matching and cancellation.
This implementation clones the entire exchange for each accepted command and
holds a process-wide ID mutex during order staging; it can be much slower than
the Phase 4B matching-only path. See the [design](phase5b-exchange-integration-design.md)
and [measured baseline](phase5b-performance-baseline.md).

Sessions retain orders on disconnect. Unsent reports and a bounded in-memory
trade audit remain in the gateway until destruction; the owner may inspect the
first pending report and the trade audit after stopping the gateway. A report is
removed once its bytes are accepted by the local TCP stack; this is not proof
that a client received or processed it. There is no persistence, replay,
reconnect, exactly-once delivery, risk control or production identity. After
shutdown, only the audit and retained pending reports remain in memory while
the Gateway object exists. The fixed limits and their backpressure behavior are
specified in the protocol document. Historical order/session retention can
exhaust limits in a long-running process.

The owner may finish the current poll iteration and its admitted command batch
after `stop()` is requested; it does not drain all pending network input or
output. A full ingress batch receives a protocol error when that error can be
queued; insufficient exchange report capacity rejects a staged command with a
capacity result. Output overflow or a failed socket write closes the affected
connection, and an error cannot be guaranteed to reach a disconnected or slow
peer. Neither TCP writes nor the local audit are application acknowledgments.

The CLI still demonstrates in-process matching. A separate real loopback run
started an ephemeral listener and two independent client processes: the seller
rested five units at 100, and the buyer submitted a market order for three.
The seller received a partial-fill report for three at 100 with two remaining;
the buyer received a filled report for three at 100 with zero remaining.

On WSL Ubuntu 24.04, the final Debug, Release and ASan/UBSan builds and CTest
suites each passed **70/70**. The final review added a cancellation-capacity
regression verifying that rejection leaves the order unchanged and the same
request ID retryable. Compile-time checks assert that the state swap and state
destruction used at commit are nonthrowing.
The existing matching CLI completed with two trades and 30 units remaining.
The formatting check and `git diff --check` passed; untracked source and design
files were also checked for trailing whitespace. GNU make emitted clock-skew
warnings on the Windows-mounted filesystem during incremental builds. Changed
sources were rebuilt and all tests passed, but these warnings are a limitation
of this environment. Tests cover the bounded ingress queue directly; they do
not simulate every possible kernel scheduling and slow-client interleaving.

Phase 5C should define risk checks and cancellation semantics at the same
transaction boundary, then address authenticated identity, application-level
acknowledgments, durable journaling, replay and reconnection as separate designs.
Measure representative workloads before replacing snapshot staging; any
replacement must preserve allocation-failure atomicity and bounded backpressure.
