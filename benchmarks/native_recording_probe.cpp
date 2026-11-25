#include "chronos/exchange/canonical_replay_envelope.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <sys/resource.h>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0, requested = 0;
} // namespace
void *operator new(std::size_t n) {
    if (void *p = std::malloc(n ? n : 1)) {
        if (counting) {
            ++allocations;
            requested += n;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

int main() {
    using namespace chronos;
    using R = exchange::RestrictedReplay;
    using E = exchange::CanonicalReplayEnvelope;
    using Clock = std::chrono::steady_clock;
    std::cout << "size,run,phase,step,latency_ns,scalar_allocations,requested_bytes,stream_bytes,"
                 "peak_rss_kib,digest\n";
    for (std::size_t n : {8U, 32U, 128U}) {
        for (int run = -1; run < 3; ++run) {
            R owner;
            std::array<std::uint64_t, 4> ids{0, 0, 0, 0};
            for (std::size_t step = 0; step < n; ++step) {
                R::Operation op{R::Kind::Open};
                if (step < 3)
                    op.args[0] = step + 1;
                else if (step == 3) {
                    op.kind = R::Kind::Risk;
                    op.args = {1, 100, 1000000, 1000000, 128, 1000, 1000, 1, 1};
                } else {
                    const auto index = (step - 4) % 3;
                    const std::uint64_t session = index == 2 ? 3 : index + 1;
                    const auto id = ++ids[session];
                    op.kind = R::Kind::Submit;
                    op.args = {session,
                               id,
                               id,
                               1,
                               index == 2 ? 0U : 1U,
                               index == 2 ? 1U : 0U,
                               index == 2 ? 0U : 100U + index,
                               index == 2 ? 1U : 2U};
                }
                allocations = requested = 0;
                counting = true;
                const auto start = Clock::now();
                const auto result = owner.apply(op);
                const auto end = Clock::now();
                counting = false;
                const bool accepted = step < 4 ? result.outcome == 1 : result.outcome == 16;
                if (!result.committed || !accepted)
                    return 1;
                rusage usage{};
                if (getrusage(RUSAGE_SELF, &usage) != 0)
                    return 1;
                if (run >= 0)
                    std::cout
                        << n << ',' << run << ",apply," << step + 1 << ','
                        << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                        << ',' << allocations << ',' << requested << ",0," << usage.ru_maxrss
                        << ",-\n";
            }
            E::Identity identity{};
            identity[0] = 3;
            allocations = requested = 0;
            counting = true;
            const auto start = Clock::now();
            const auto sealed = E::seal(owner, identity);
            const auto end = Clock::now();
            counting = false;
            std::string digest;
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : sealed.anchor.digest) {
                digest.push_back(hex[byte >> 4]);
                digest.push_back(hex[byte & 15]);
            }
            rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) != 0)
                return 1;
            if (run >= 0)
                std::cout
                    << n << ',' << run << ",seal,0,"
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                    << ',' << allocations << ',' << requested << ',' << sealed.bytes.size() << ','
                    << usage.ru_maxrss << ',' << digest << '\n';
            allocations = requested = 0;
            counting = true;
            const auto verify_start = Clock::now();
            const bool valid = E::verify(sealed.bytes, sealed.anchor);
            const auto verify_end = Clock::now();
            counting = false;
            if (!valid || getrusage(RUSAGE_SELF, &usage) != 0)
                return 1;
            if (run >= 0)
                std::cout << n << ',' << run << ",verify,0,"
                          << std::chrono::duration_cast<std::chrono::nanoseconds>(verify_end -
                                                                                  verify_start)
                                 .count()
                          << ',' << allocations << ',' << requested << ',' << sealed.bytes.size()
                          << ',' << usage.ru_maxrss << ',' << digest << '\n';
        }
    }
}
