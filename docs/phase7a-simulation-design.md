# Phase 7A deterministic simulation design

The synchronous, single-owner `SimulationRunner` owns an isolated in-memory
`exchange::Exchange`. It creates distinct local session IDs 1 through N and
submits typed commands through `Exchange::submit` and `cancel`; matching, risk,
process-wide internal order IDs, positions and reports remain exchange-owned.
It does not use TCP or expose a dashboard control. Do not share this isolated
instance with live gateway clients. Sessions are local identifiers, not
authenticated accounts. Runs start with empty books, no positions, default or
explicit configured risk limits, and a fresh exchange instance.

`CommandGenerator` version 2 uses SplitMix64: add `0x9e3779b97f4a7c15`, then
xor-shift/multiply by `0xbf58476d1ce4e5b9` and `0x94d049bb133111eb`, followed
by the final xor-shift, with unsigned 64-bit wrap. Sampling rejects values below
`(0 - bound) % bound` in unsigned arithmetic and then takes `value % bound`.
Every residue has the same number of accepted 64-bit inputs. All prices
and quantities are integers. Random participant and symbol choices, side weight,
and offsets are driven solely by the seed and logical step. Cancellation
selection additionally depends on the ordered set of accepted still-open orders;
the generator's caller must supply authoritative owner/client targets.

Balanced uses weighted buy/sell limits around the reference. High-volume and
mixed add owner-verified cancellation choices; mixed rotates submission owners
across multiple accounts and uses the actual owner for cancellation.
Volatile uses a bounded integer random walk. Liquidity-sweep cycles four sell
limits at consecutive available offsets followed by a market buy from a second
participant, rotating symbols. Quantity and price settings can make a scenario
produce fewer fills; risk and exchange capacity can reject commands. These are
synthetic workloads, not realistic market or profitability models.

Configuration permits 2–16 participants, 1–16 unique uint32 symbols, 1–1024
commands, a positive reference up to 1,000,000,000 ticks, a nonnegative variation
smaller than the reference and at most 1,000,000,000, positive ordered quantities
up to INT64_MAX, and weights from 0 to 100. Order-history and per-session request
capacities are independently bounded 1–1024. Exchange order history is lifetime
history, so cancellations do not free that capacity. Risk limits are not relaxed;
for example, market buys can be rejected by the configured market-price bound.
The CLI defaults to symbol 1; repeat `--symbol N` for multiple symbols.

`start()` validates configuration and initializes sessions; `run_batch(n)` runs
at most n commands, and `request_stop()` finalizes the current book and positions
without undoing accepted commands. Calls must be serialized on the owner thread.
No worker threads or command queues are introduced. The outcome and trade vectors
are bounded by 1024 commands; target tracking is bounded by accepted limits.
Report queues are drained after every operation. A failure stops the run and is
reported as Failed, not Completed. Report storage is reserved before commands
run. If post-commit aggregation overflows or throws, the exchange may already
have committed the last command. `trades` counts committed exchange trades;
`aggregated_trades` and `executed_quantity` describe the successfully aggregated
prefix. `summary_complete` is false for failed or unfinished runs, and a partial
digest is not a replayable transaction log or a completed fingerprint.

## Counters and digest

One logical step generates and synchronously admits one command. These are
simulation-local counts, **not** an exchange ingress sequence. A stop request
generates no command. The exchange advances its own command sequence only on
accepted submit/cancel; rejections do not advance it. Execution report sequences,
per-symbol matching trade IDs and market-feed sequences belong to their existing
exchange domains. The simulator records accepted/rejected outcomes and drains
reports; it does not invent an exchange sequence for a rejection.

The digest is non-cryptographic FNV-1a 64, starting at 14695981039346656037 and
multiplying by 1099511628211 after each byte. Every scalar is serialized as eight
little-endian bytes (signed values converted modulo 2^64). Inputs are, in order:
generator version, scenario/configuration and risk values and symbol lists;
for each step, command fields and result, report count and reports with their
owning session ordered by
exchange report sequence, then newly appended trade audit fields; finally top of
book and positions in configured symbol and participant order. The precise field
order is in `simulation.cpp`. Absolute internal order IDs are deliberately
excluded because the exchange allocator is process-wide; trade counterparties
use local session IDs. This normalizes a meaningful difference between fresh
exchange instances in the same process. Final book hashing covers only best
bid/ask and aggregate quantities, not every resting order or all risk state;
the fingerprint therefore cannot prove complete state equivalence. It is not a
collision-resistant integrity check, durable log or replay format.

Phase 7B must define persisted, versioned command/outcome encodings, ownership,
checksums, failure handling and a consistent commit boundary. Seed and this
digest alone cannot recover a run or reproduce external live-client interleaving.
