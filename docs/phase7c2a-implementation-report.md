# Phase 7C.2A prerequisite review report

The subsequent isolated-only owner prerequisite is described in
[the Phase 7C.2A.1 report](phase7c2a1-implementation-report.md); it does not
implement durability. A later [gateway-specific owner](phase7c2a1b-implementation-report.md)
adds a live boundary but not prepared transactions or durable publication.

This is a deliberately narrower milestone. No durable journal, manifest,
prepare/install API, durable acknowledgment, inspector or crash recovery has
been implemented. The blocker is the missing enforceable exchange-owner and
allocator-lifetime contract described in
[phase7c2a-journal-design.md](phase7c2a-journal-design.md). Implementing a
durable boundary before that contract would risk a stale candidate overwriting
session, risk or report changes, or inconsistent process-wide ID publication.

The regression tests cover unchanged exact state and reusable request IDs
after risk and invalid-cancel rejection; unchanged exact state, no visible
incoming report and no consumed ID when a report-capacity failure occurs
after matching preparation; and changes to exact state from kill-switch and
report-consumption operations outside the order-command path. Existing tests
also cover synchronized allocator uniqueness, private allocator rollback and
allocation failures. These tests characterize current behavior; they do not
prove a durable transaction protocol.

No journal I/O fault injection, journal demo, fsync performance measurements,
or durability claims are possible in this milestone because there is no
journal implementation. Physical power-loss durability is not established.
Phase 7C.2B cold-start recovery, session recovery and private-report delivery
remain unsupported. Validation results are recorded below after execution.

## Validation

In a WSL-native source copy checked against the Windows checkout before the
build, Debug, Release and ASan/UBSan each passed **143/143 CTest cases**.
The new regression suite accounts for three of those cases. The matching CLI
and five-command mixed simulation completed. A separate five-command
liquidity-sweep recording replayed successfully: 12 records, five commands
and four trades. The CHROVIEW comparison and CHRSTATE isolated restoration
demo passed (generation 1, ingress 5, 9,348 bytes), and the direct two-client
gateway fill test passed. The repository formatting check and Git whitespace
check passed. No web contracts changed.

There is no Phase 7C.2A benchmark result or journal inspection demo: these
would require an implementation that this prerequisite review deliberately
does not provide. The tests do not establish fsync behavior or durability.
