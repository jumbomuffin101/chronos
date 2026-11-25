// Standalone, test-seam-only microbenchmark. No disk or gateway I/O.
#include "authorization_test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    using Clock = std::chrono::steady_clock;
    constexpr int warmup = 20;
    constexpr int samples = 300;
    std::vector<std::int64_t> direct, prepare, decision, install, total;
    std::size_t request_bytes = 0;
    for (int n = -warmup; n < samples; ++n) {
        H::Identity id{};
        id[0] = 1;
        R::Operation open{R::Kind::Open};
        open.args[0] = 1;
        R reference;
        auto d0 = Clock::now();
        const auto outcome = reference.apply(open);
        auto d1 = Clock::now();
        if (!outcome.committed)
            return 1;
        H h(id);
        auto t0 = Clock::now();
        h.prepare(open);
        auto t1 = Clock::now();
        request_bytes = h.request_size_for_test();
        h.begin_external_decision();
        auto t2 = Clock::now();
        h.inject_test_decision(H::TestDecision::Permit);
        auto t3 = Clock::now();
        const bool installed = h.install();
        auto t4 = Clock::now();
        if (!installed || h.canonical_state() != reference.canonical_state() || !h.shutdown())
            return 2;
        const auto ns = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        };
        if (n >= 0) {
            direct.push_back(ns(d0, d1));
            prepare.push_back(ns(t0, t1));
            decision.push_back(ns(t2, t3));
            install.push_back(ns(t3, t4));
            total.push_back(ns(t0, t4));
        }
    }
    std::cout << "samples=" << samples << " warmup=" << warmup << " request_bytes=" << request_bytes
              << '\n';
    const auto report = [](const char *name, auto &v) {
        std::sort(v.begin(), v.end());
        std::cout << name << " ns min=" << v.front() << " p50=" << v[v.size() / 2]
                  << " p95=" << v[v.size() * 95 / 100] << " p99=" << v[v.size() * 99 / 100]
                  << " max=" << v.back() << '\n';
    };
    report("direct", direct);
    report("prepare_and_hash", prepare);
    report("decision", decision);
    report("install_and_rehash", install);
    report("total", total);
}
