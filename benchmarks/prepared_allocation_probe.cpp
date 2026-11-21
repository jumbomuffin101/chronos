#include "chronos/exchange/live_owner.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0, bytes = 0;
} // namespace
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) {
        if (counting) {
            ++allocations;
            bytes += size;
        }
        return p;
    }
    throw std::bad_alloc{};
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void operator delete(void *p) noexcept {
    std::free(p);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}

namespace {
using namespace chronos;
template <bool Prepared, bool Cancel> void probe() {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    if (!owner.open_session(1))
        throw std::runtime_error("session failed");
    if constexpr (Cancel) {
        if (owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100,
                         1) != exchange::Result::Accepted)
            throw std::runtime_error("fixture failed");
    }
    allocations = bytes = 0;
    counting = true;
    auto result = exchange::Result::InvalidOrder;
    bool installed = false;
    if constexpr (Prepared) {
        exchange::PreparedScopeResult outcome;
        if constexpr (Cancel)
            outcome = owner.with_prepared_cancel(1, 2, 1, [](exchange::Result) { return true; });
        else
            outcome = owner.with_prepared_submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Limit, 100, 1,
                                                 [](exchange::Result) { return true; });
        result = outcome.result;
        installed = outcome.installed;
    } else {
        if constexpr (Cancel)
            result = owner.cancel(1, 2, 1);
        else
            result = owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                  core::OrderType::Limit, 100, 1);
        installed = result == exchange::Result::Accepted;
    }
    counting = false;
    if (result != exchange::Result::Accepted || !installed || !owner.finish())
        throw std::runtime_error("operation failed");
    std::cout << (Prepared ? "prepared" : "ordinary") << ',' << (Cancel ? "cancel" : "submit")
              << ',' << allocations << ',' << bytes << '\n';
}
} // namespace
int main() {
    try {
        std::cout << "mode,operation,scalar_allocations,requested_bytes\n";
        probe<false, false>();
        probe<true, false>();
        probe<false, true>();
        probe<true, true>();
    } catch (const std::exception &error) {
        counting = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
