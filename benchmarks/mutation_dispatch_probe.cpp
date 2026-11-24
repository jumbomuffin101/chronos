#include "chronos/exchange/live_owner.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
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
    using Clock = std::chrono::steady_clock;
    std::cout << "mode,operation,sample,latency_ns,scalar_allocations,requested_bytes\n";
    for (int mode = 0; mode < 2; ++mode) {
        for (int operation = 0; operation < 3; ++operation) {
            for (int sample = -10; sample < 200; ++sample) {
                auto direct = mode == 0 ? std::make_unique<exchange::Exchange>() : nullptr;
                auto owner = mode == 1 ? std::make_unique<exchange::LiveExchangeOwner>() : nullptr;
                if (owner)
                    owner->bind();
                const auto open = [&](std::uint64_t id) {
                    return direct ? direct->open_session(id) : owner->open_session(id);
                };
                const auto submit = [&] {
                    return direct ? direct->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                   core::OrderType::Limit, 100, 1)
                                  : owner->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                  core::OrderType::Limit, 100, 1);
                };
                if (!open(1))
                    return 1;
                if (operation != 0 && submit() != exchange::Result::Accepted)
                    return 1;
                const auto prior = operation == 2 ? (direct ? direct->front(1) : owner->front(1))
                                                  : std::optional<exchange::Report>{};
                allocations = bytes = 0;
                counting = true;
                const auto begin = Clock::now();
                bool valid = true;
                if (operation == 0)
                    valid = submit() == exchange::Result::Accepted;
                else if (operation == 1)
                    valid = (direct ? direct->cancel(1, 2, 1) : owner->cancel(1, 2, 1)) ==
                            exchange::Result::Accepted;
                else if (direct)
                    direct->pop(1);
                else
                    owner->pop(1);
                const auto end = Clock::now();
                counting = false;
                if (operation == 2) {
                    const auto next = direct ? direct->front(1) : owner->front(1);
                    valid = prior && (!next || next->sequence > prior->sequence);
                }
                if (!valid || (owner && !owner->finish()))
                    return 1;
                if (sample >= 0)
                    std::cout
                        << (direct ? "direct" : "owner") << ','
                        << (operation == 0   ? "submit"
                            : operation == 1 ? "cancel"
                                             : "pop")
                        << ',' << sample << ','
                        << std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()
                        << ',' << allocations << ',' << bytes << '\n';
            }
        }
    }
}
