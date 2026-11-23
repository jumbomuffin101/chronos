# Phase 7C.2A.3b implementation report

Status: **incomplete; no durable mode is provided**. A bounded, read-only
structural journal/manifest inspector, CRC-32C and SHA-256 primitives have been
added. The inspector checks lengths, versions, identities, checksums,
PREPARE/terminal binding, contiguous LSNs and the manifest boundary. It leaves
trailing bytes opaque, does not inspect outcome contents or restore state, and
cannot establish that a file was synchronized. See
[the design](phase7c2a3b-journal-design.md) for the provisional layout and
conflict with the earlier proposed layout.

The owner cannot presently extract a complete bounded prepared decision or
perform safe, bounded storage I/O without reworking reservation ownership and
the publication boundary. The writer, manifest publication, journal fault and
crash tests, durable authorization and all durable acknowledgments remain
unimplemented. Ordinary gateway behavior is unchanged. No durability, recovery,
authenticated reconnect or exactly-once claim is made.

The inspector is a prototype; the v1 outcome payload is not a canonical schema
and the format must not be treated as stable. Use
`chronos_journal_inspect JOURNAL MANIFEST` only for experimental fixtures. It
returns nonzero for invalid durable-prefix envelopes and explicitly states that
exchange outcomes and state have not been verified.

On WSL2 Ubuntu with `/tmp` on an ext4 virtual disk, Debug, Release and
ASan/UBSan each passed **196/196** CTest cases in the final inspector review.
Eight inspector cases cover known CRC and SHA-256 vectors, block padding,
structural prefix and opaque tail, every truncation of a small durable prefix,
identity mismatch, checksums, terminal binding, versions, size and boundary
errors, multiple records, duplicate sequence numbers and reserved fields. These
are in-memory fixtures and structural tests, not filesystem I/O
fault injection or crash tests. The full suites include shared, exclusive and
protocol-v3 gateway tests; no gateway behavior was changed.

The matching and simulation CLIs passed. A five-command Phase 7B recording
replayed 12 records and four trades; CHROVIEW comparison and CHRSTATE isolated
restoration passed. A synthetic fixture inspected successfully with LSN 1,
terminal offset 212 and four unverified tail bytes. C++ formatting and Git
whitespace checks passed. No durable write, crash-boundary test or journal
performance measurement exists to report. Web adapter and browser tests were
not rerun because the gateway and wire behavior did not change.
