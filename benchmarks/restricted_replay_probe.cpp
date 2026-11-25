#include "chronos/exchange/restricted_replay.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0, bytes = 0;
} // namespace
void *operator new(std::size_t n) {
    if (void *p = std::malloc(n ? n : 1)) {
        if (counting) {
            ++allocations;
            bytes += n;
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
    using Clock = std::chrono::steady_clock;
    std::cout << "mode,sample,latency_ns,scalar_allocations,requested_bytes,stream_bytes\n";
    for (int mode = 0; mode < 4; ++mode) {
        for (int sample = -5; sample < (mode == 3 ? 30 : 100); ++sample) {
            auto direct =
                mode == 0 ? exchange::Exchange::isolated(16, 128, 128, 128, 0, 0) : nullptr;
            auto owner = mode != 0 ? std::make_unique<R>() : nullptr;
            R::Operation open{R::Kind::Open};
            if (mode >= 2) {
                for (std::uint64_t id = 1; id <= (mode == 3 ? 9U : 8U); ++id) {
                    open.args[0] = id;
                    if (!owner->apply(open).committed)
                        return 1;
                }
            }
            if (mode == 3) {
                for (std::uint64_t id = 1; id <= 8; ++id) {
                    R::Operation sell{R::Kind::Submit};
                    sell.args = {id, 1, 1, 1, 1, 0, 99 + id, 1};
                    const auto result = owner->apply(sell);
                    if (!result.committed || result.outcome != 16)
                        return 1;
                }
            }
            const auto stream = owner ? owner->records() : std::vector<R::Bytes>{};
            std::size_t stream_bytes = 0;
            for (const auto &r : stream)
                stream_bytes += r.size();
            open.args[0] = 1;
            allocations = bytes = 0;
            counting = true;
            const auto start = Clock::now();
            bool ok = false;
            if (mode == 0)
                ok = direct->open_session(1);
            else if (mode == 1)
                ok = owner->apply(open).committed;
            else if (mode == 2)
                ok = R::verify(stream, 8);
            else {
                R::Operation buy{R::Kind::Submit};
                buy.args = {9, 1, 1, 1, 0, 1, 0, 8};
                const auto result = owner->apply(buy);
                ok = result.committed && result.outcome == 16;
            }
            const auto end = Clock::now();
            counting = false;
            if (!ok)
                return 1;
            if (mode == 1)
                stream_bytes = owner->records()[0].size();
            if (mode == 3) {
                const auto final = owner->records();
                stream_bytes = 0;
                for (const auto &r : final)
                    stream_bytes += r.size();
                if (!R::verify(final, 18))
                    return 1;
            }
            if (sample >= 0)
                std::cout
                    << (mode == 0   ? "isolated-open"
                        : mode == 1 ? "record-open"
                        : mode == 2 ? "verify-eight"
                                    : "sweep-eight")
                    << ',' << sample << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                    << ',' << allocations << ',' << bytes << ',' << stream_bytes << '\n';
        }
    }
}
