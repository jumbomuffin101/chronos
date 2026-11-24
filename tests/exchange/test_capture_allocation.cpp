#include "chronos/exchange/live_owner.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <new>

namespace {
thread_local bool fail_next = false;
void arm_failure() { fail_next = true; }
} // namespace
void *operator new(std::size_t size) {
    if (fail_next) {
        fail_next = false;
        throw std::bad_alloc{};
    }
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }

TEST(PublicCaptureAllocation, VectorAllocationFailurePreservesLiveStateAndId) {
    using namespace chronos;
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto before = owner.exact_continuation_state(4096);
    exchange::detail::before_capture_allocation_for_test = &arm_failure;
    EXPECT_THROW(static_cast<void>(owner.submit_with_summary(
                     1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1)),
                 std::bad_alloc);
    exchange::detail::before_capture_allocation_for_test = nullptr;
    EXPECT_FALSE(fail_next);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_FALSE(owner.front(1));
    const auto retry = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Limit, 100, 1);
    ASSERT_TRUE(retry.installed());
    ASSERT_EQ(retry.public_events().size(), 1U);
    EXPECT_EQ(retry.public_events()[0].sequence, 1U);
    EXPECT_TRUE(owner.finish());
}
