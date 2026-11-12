# Benchmarks

See [methodology](../docs/phase4-benchmark-methodology.md) for exact build and run commands, verification, timing boundaries and limitations. The optional `chronos_bench` target uses Google Benchmark v1.8.3 pinned to a commit; `chronos_allocation_probe` separately counts a subset of C++ allocations. Neither is built by default. Running the 10,000- and especially 100,000-order cases can be expensive because populating the engine repeatedly clones its entire book. Results are simulator-internal measurements, not network or production trading latency.
