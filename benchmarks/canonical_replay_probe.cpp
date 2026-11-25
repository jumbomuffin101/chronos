#include "chronos/exchange/canonical_replay_envelope.hpp"

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
[[gnu::noinline]] bool same_bytes(std::span<const std::uint8_t> a,
                                  std::span<const std::uint8_t> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

int main() {
    using namespace chronos;
    using R = exchange::RestrictedReplay;
    using E = exchange::CanonicalReplayEnvelope;
    using Clock = std::chrono::steady_clock;
    std::cout << "mode,sample,latency_ns,scalar_allocations,requested_bytes,encoded_bytes\n";
    for (int mode = 0; mode < 7; ++mode) {
        const int samples = mode == 6 ? 5 : 30;
        for (int sample = -2; sample < samples; ++sample) {
            auto exchange = exchange::Exchange::isolated(16, 128, 128, 128, 0, 0);
            if (!exchange->open_session(1))
                return 1;
            const auto state = exchange->canonical_isolated_state();
            const auto state_copy = state;
            const auto digest = journal::sha256(state);
            R owner;
            R::Operation operation{R::Kind::Open};
            operation.args[0] = 1;
            if (!owner.apply(operation).committed)
                return 1;
            const int count = mode == 5 ? 32 : mode == 6 ? 128 : 8;
            operation.kind = R::Kind::Close;
            operation.args[0] = 999;
            for (int i = 1; i < count; ++i)
                if (!owner.apply(operation).committed)
                    return 1;
            E::Identity identity{};
            identity[0] = 1;
            const auto envelope = E::seal(owner, identity);
            allocations = bytes = 0;
            counting = true;
            const auto start = Clock::now();
            bool valid = false;
            if (mode == 0)
                valid = exchange->canonical_isolated_state() == state;
            else if (mode == 1)
                valid = same_bytes(state, state_copy);
            else if (mode == 2)
                valid = journal::sha256(state) == digest;
            else if (mode == 3)
                valid = E::seal(owner, identity).bytes == envelope.bytes;
            else
                valid = E::verify(envelope.bytes, envelope.anchor);
            const auto end = Clock::now();
            counting = false;
            if (!valid)
                return 1;
            if (sample >= 0)
                std::cout
                    << (mode == 0   ? "state"
                        : mode == 1 ? "compare"
                        : mode == 2 ? "digest"
                        : mode == 3 ? "seal-eight"
                        : mode == 4 ? "verify-eight"
                        : mode == 5 ? "verify-32"
                                    : "verify-128")
                    << ',' << sample << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                    << ',' << allocations << ',' << bytes << ','
                    << (mode <= 2 ? state.size() : envelope.bytes.size()) << '\n';
        }
    }
}
