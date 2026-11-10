# ADR 0001: Core architecture

Status: accepted for Phase 1.

## Decision

Use C++20 for the core: its standard types, compile-time facilities, and explicit control over resources support a clear systems implementation. Represent prices as integer ticks to make equality and ordering exact and avoid floating-point rounding; each instrument will eventually define its tick size and range.

The matching engine will initially process sequenced events deterministically on one thread. This makes ordering and replay easier to reason about and test; it is a design direction, not a claim that replay exists. Separate networking from matching so transport concerns cannot dictate core behavior and the engine can be tested without sockets.

Use CMake for portable target-oriented builds and GoogleTest for well-supported C++ unit testing. Pin dependency versions and fetch only what is enabled. Defer performance optimization until behavior is correct and realistic benchmarks reveal bottlenecks, avoiding complexity based on unmeasured assumptions.
