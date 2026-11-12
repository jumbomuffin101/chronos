# Phase 5A loopback protocol, version 1

This is a local development protocol for a simulator. It is not authenticated, encrypted, or connected to the matching engine. The server binds **only IPv4 127.0.0.1**. Any local process able to connect can use the service; a session ID is an in-process connection identifier, not a credential. Do not expose or forward this listener to untrusted networks.

## Frame

All multibyte integers use network byte order (big endian). A frame has a 16-byte header followed by a payload, with total length at most **4096 bytes**. Native C++ layout is never transmitted.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | Unsigned total frame length, including header; range 16–4096 |
| 4 | 1 | Protocol version, currently `1` |
| 5 | 1 | Message type |
| 6 | 2 | Flags, must be zero |
| 8 | 8 | Unsigned client request ID, echoed in responses |
| 16 | length − 16 | Payload |

There is no version negotiation: unknown versions are rejected. Request IDs are correlation values only; uniqueness, authentication and idempotency are not enforced. A frame with an invalid length, version, type, flags or type-specific payload length is a protocol error. An incomplete header or payload at EOF is truncated. The streaming decoder has fixed 4096-byte storage and processes arbitrary fragmentation or concatenation without accumulating an unbounded input list. Once it reports an error the decoder is terminal.

## Types

| Value | Name | Payload |
|---:|---|---|
| 1 | Hello | Empty; required as first frame |
| 2 | HelloAck | 8-byte unsigned gateway-instance-local session ID |
| 3 | Health | Empty |
| 4 | HealthAck | Empty |
| 5 | Echo | 0–4080 arbitrary bytes |
| 6 | EchoReply | Exact echo bytes |
| 7 | Error | 2-byte unsigned protocol error code |
| 16 | SubmitOrder | Reserved, 30 bytes: client order ID u64, symbol u32, side u8, order type u8, signed tick price i64, quantity u64 |
| 17 | CancelOrder | Reserved, client order ID u64 |
| 18 | OrderAck | Reserved, client order ID u64, status u8, sequence u64 |
| 19 | Execution | Reserved, client order ID u64, trade ID u64, signed tick price i64, quantity u64, sequence u64 |

Reserved numeric fields have these byte lengths but their enum values and exchange semantics are not finalized. They are not serialized by copying `FutureOrder` or other native structures. The server returns UnsupportedOperation to well-framed SubmitOrder and CancelOrder frames after handshake; it never submits or cancels an order. Client-sent response types or a second Hello are unexpected and close the session after an error response. There is no trade or market-data service.

## Errors and lifecycle

Error codes are: 1 InvalidLength, 2 Oversized, 3 UnsupportedVersion, 4 UnknownType, 5 InvalidFlags, 6 InvalidPayload, 7 Truncated, 8 HandshakeRequired, 9 UnsupportedOperation, 10 UnexpectedMessage. Zero means no error internally and is not sent as a failure. If an invalid header cannot be trusted, the server's error response uses request ID zero and then closes after flushing where possible. A pre-handshake request gets HandshakeRequired and the connection closes; reserved requests get UnsupportedOperation without closing. A truncated EOF is closed without a guaranteed response. Output overflow, timeout, peer disconnect or I/O errors close the connection; delivery of an error on a broken or congested socket is best effort.

States are connected/awaiting Hello → active → closing → closed. IDs increase within one gateway instance, start at 1 and are not reused within that instance; another instance can issue the same IDs. They are not durable across restarts; when the counter is exhausted new sessions are closed. A session is accepted only while below the configured connection limit. Connections beyond the limit are accepted and immediately closed. The built-in client supports one synchronous request at a time and checks the response request ID; it does not retry or provide replay guarantees.

The gateway runs one `poll` thread for accept/read/write and owns all sockets. The matching engine is never accessed. Default maximum is 16 sessions (configurable 1–1024), per-session outgoing pending bytes are capped at 8192, input decoder storage at 4096, and inactivity timeout is 5 seconds. Progress on reads or writes refreshes the idle timeout; it is not an absolute frame deadline, so a slowly progressing local peer can retain a slot. The event loop polls at most 25 ms between stop checks, accepts at most 64 connections per iteration, handles partial writes and does not use unbounded application queues. `stop()` requests the thread to stop, joins it and closes listener and sessions; queued output is discarded, not drained. Call `stop()` and destroy the gateway from its owning thread; concurrent calls and destruction are not supported. The server exposes `running()` for unexpected thread termination. Client operations use a 2-second default deadline for each write and response read (separate deadlines, plus connect); configured client/write timeouts must be positive and no greater than 60 seconds.

This phase has no TLS, production identity, per-account authorization, risk, order routing, durable event sequencing or restart recovery. The future design is in [networking and risk](phase5-networking-risk-design.md).
