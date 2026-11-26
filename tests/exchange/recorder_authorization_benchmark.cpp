#include "chronos/exchange/restricted_replay.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {
thread_local bool measuring = false;
thread_local std::size_t allocations = 0;
thread_local std::size_t bytes = 0;
} // namespace
void *operator new(std::size_t n) {
    if (measuring) {
        ++allocations;
        bytes += n;
    }
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

int main() {
    using R = chronos::exchange::RestrictedReplay;
    using Clock = std::chrono::steady_clock;
    for (int mode = 0; mode != 2; ++mode) {
        std::vector<long long> latencies;
        latencies.reserve(300);
        std::size_t total_allocations = 0, total_bytes = 0;
        for (int i = -20; i < 300; ++i) {
            R::AuthorizationIdentity id{};
            id[0] = 1;
            R direct;
            R gated(R::InMemoryAuthorization{}, id);
            R &owner = mode ? gated : direct;
            R::Operation op{R::Kind::Open};
            op.args[0] = 1;
            allocations = bytes = 0;
            measuring = true;
            const auto start = Clock::now();
            const auto result = owner.apply(op);
            const auto end = Clock::now();
            measuring = false;
            if (!result.committed || owner.size() != 1 || !R::verify(owner.records(), 1))
                return 1;
            if (i >= 0) {
                latencies.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                total_allocations += allocations;
                total_bytes += bytes;
            }
        }
        std::sort(latencies.begin(), latencies.end());
        std::cout << (mode ? "gated" : "direct") << " n=300 p50_ns=" << latencies[150]
                  << " p95_ns=" << latencies[285] << " p99_ns=" << latencies[297]
                  << " min_ns=" << latencies.front() << " max_ns=" << latencies.back()
                  << " mean_allocs=" << total_allocations / 300.0
                  << " mean_bytes=" << total_bytes / 300.0 << '\n';
    }
    std::vector<std::uint8_t> input(832, 0x5a);
    std::vector<long long> hashes;
    hashes.reserve(1000);
    std::uint64_t checksum = 0;
    for (int i = -50; i < 1000; ++i) {
        input[0] = static_cast<std::uint8_t>(i);
        const auto start = Clock::now();
        const auto digest = chronos::journal::sha256(input);
        const auto end = Clock::now();
        checksum += digest[0];
        if (i >= 0)
            hashes.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    std::sort(hashes.begin(), hashes.end());
    std::cout << "sha256 bytes=832 n=1000 p50_ns=" << hashes[500] << " p95_ns=" << hashes[950]
              << " p99_ns=" << hashes[990] << " min_ns=" << hashes.front()
              << " max_ns=" << hashes.back() << " checksum=" << checksum << '\n';
}
