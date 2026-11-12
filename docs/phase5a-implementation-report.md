# Phase 5A implementation report

## Implemented

The Linux-only `chronos_gateway_lib` contains a fixed-capacity streaming frame decoder, explicit big-endian frame encoder and move-only RAII socket wrapper. A single `poll` thread owns a loopback-only TCP listener and bounded per-session input/output; the synchronous client connects only to loopback, handles partial writes and decodes replies. The standalone gateway/client demonstrate handshake and echo. Health and protocol error replies are supported. Reserved order, cancellation, acknowledgment and execution layouts are specified, but order/cancel requests return UnsupportedOperation and no network path accesses the matching engine. The exact format, lifecycle and limits are in the [protocol specification](phase5-protocol-spec.md).

Each accepted connection receives an ID unique within the gateway instance. This local development policy authenticates nobody: any local process able to connect may use it. Default limits are 16 sessions, 4096-byte frames, 8192 pending output bytes per session and a five-second inactivity timeout. The event loop does not create a thread per client. `stop()` joins the worker, closes the listener and sessions, and discards pending output. Client connect/write/read operations have finite deadlines; client/write timeout arguments are bounded to 60 seconds.

## Validation

On WSL Ubuntu 24.04 with GCC 13.3 and CMake 3.28, the final Debug, Release and ASan/UBSan builds each passed **57 of 57** CTest cases, including the 43 pre-existing core cases and 14 protocol/TCP/gateway cases. Tests cover every frame split position, coalescing, invalid headers/lengths, truncated frames, maximum echo, partial writes and write timeout, handshake/health/echo, stopping pipelined requests after a handshake violation, unsupported orders, session limits, idle timeout, peer disconnect, malformed version, multiple clients and shutdown. Each gateway test has a 10-second CTest timeout. Strict project warnings remained enabled.

A separate real loopback demonstration started `chronos_gateway 0`, bound `127.0.0.1:38153`, ran `chronos_gateway_client 38153`, and printed `Session 1 echo: chronos loopback`; the server was then stopped. The existing matching CLI also ran and printed its two trades and remaining quantity. The specific ephemeral port varies. Formatting and Git whitespace checks passed.

## Limitations and future work

The listener and POSIX tests are built only on Linux; other platforms retain the matching core but do not build this gateway. There is no TLS, production authentication, account identity, risk, rate limiting, engine routing, persistent sessions, delivery guarantee, market data, or restart recovery. Idle timeout measures lack of progress, not total frame lifetime; a slowly progressing local peer can retain a session slot. A full output buffer closes the session and shutdown drops queued output. The synchronous client supports one outstanding request and has no reconnect/retry policy. Unexpected worker exceptions stop the service and can be observed with `running()`; there is no automatic restart. Future order field values and risk semantics remain unspecified, and native future structs are not wire encoders. These limitations are deliberate Phase 5A boundaries; see the [networking and risk design](phase5-networking-risk-design.md) for later work.
