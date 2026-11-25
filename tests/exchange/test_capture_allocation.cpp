#include "chronos/exchange/live_owner.hpp"
#include "chronos/exchange/restricted_replay.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <new>

namespace {
thread_local bool fail_next = false;
thread_local int allocations_until_failure = -1;
void arm_failure() { fail_next = true; }
} // namespace
void *operator new(std::size_t size) {
    if (allocations_until_failure == 0) {
        allocations_until_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_until_failure > 0)
        --allocations_until_failure;
    if (fail_next) {
        fail_next = false;
        throw std::bad_alloc{};
    }
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc{};
}

TEST(PublicCaptureAllocation, SummaryAllocationSweepNeverPublishesPartialEffects) {
    using namespace chronos;
    bool completed = false;
    int failures = 0;
    for (int index = 0; index < 512 && !completed; ++index) {
        exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
        owner.bind();
        ASSERT_TRUE(owner.open_session(1));
        ASSERT_TRUE(owner.open_session(2));
        ASSERT_EQ(owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit,
                               100, 2),
                  exchange::Result::Accepted);
        const auto before = owner.exact_continuation_state(4096);
        allocations_until_failure = index;
        try {
            const auto summary = owner.submit_with_summary(
                2, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Market, 0, 1);
            allocations_until_failure = -1;
            ASSERT_TRUE(summary.installed());
            ASSERT_EQ(summary.participant_effects().size(), 2U);
            ASSERT_EQ(summary.ownership_effects().size(), 2U);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            ++failures;
            EXPECT_EQ(owner.exact_continuation_state(4096), before);
        }
        allocations_until_failure = -1;
        EXPECT_TRUE(owner.finish());
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 0);
}

TEST(PublicCaptureAllocation, AdministrativePreparationAllocationSweepIsAtomic) {
    using namespace chronos;
    bool completed = false;
    int failures = 0;
    for (int index = 0; index < 512 && !completed; ++index) {
        exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
        owner.bind();
        ASSERT_TRUE(owner.open_session(1));
        const auto before = owner.exact_continuation_state(4096);
        const auto sequence_before = owner.diagnostic_mutation_index();
        exchange::RiskLimits limits;
        limits.max_order_quantity = 1;
        allocations_until_failure = index;
        try {
            const auto result =
                owner.with_prepared_risk_update(1, limits, [](exchange::Result) { return true; });
            allocations_until_failure = -1;
            ASSERT_TRUE(result.installed);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            ++failures;
            EXPECT_EQ(owner.exact_continuation_state(4096), before);
            EXPECT_EQ(owner.diagnostic_mutation_index(), sequence_before);
        }
        allocations_until_failure = -1;
        EXPECT_TRUE(owner.finish());
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 0);
}

TEST(PublicCaptureAllocation, DiagnosticIndexExhaustionFailsBeforeMutation) {
    using namespace chronos;
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto before = owner.exact_continuation_state(4096);
    owner.seed_diagnostic_index_for_test(std::numeric_limits<std::uint64_t>::max());
    EXPECT_THROW(static_cast<void>(owner.open_session(2)), std::overflow_error);
    EXPECT_THROW(owner.pop(1), std::overflow_error);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_TRUE(owner.finish());
}

TEST(PublicCaptureAllocation, RecordedSubmitAllocationFailureReturnsNoRecordOrMutation) {
    using namespace chronos;
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto before = owner.exact_continuation_state(4096);
    const auto index = owner.diagnostic_mutation_index();
    allocations_until_failure = 0;
    EXPECT_THROW(static_cast<void>(owner.submit_recorded(
                     1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1)),
                 std::bad_alloc);
    allocations_until_failure = -1;
    EXPECT_EQ(owner.diagnostic_mutation_index(), index);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_TRUE(owner.finish());
}

TEST(PublicCaptureAllocation, RestrictedRecordingAllocationSweepIsAtomic) {
    using namespace chronos;
    bool completed = false;
    int failures = 0;
    for (int index = 0; index < 512 && !completed; ++index) {
        exchange::RestrictedReplay owner;
        exchange::RestrictedReplay::Operation first{exchange::RestrictedReplay::Kind::Open};
        first.args[0] = 1;
        ASSERT_TRUE(owner.apply(first).committed);
        const auto before = owner.exact_state();
        const auto prior = owner.records();
        auto next = first;
        next.args[0] = 2;
        allocations_until_failure = index;
        try {
            const auto result = owner.apply(next);
            allocations_until_failure = -1;
            ASSERT_TRUE(result.committed);
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            ++failures;
            EXPECT_EQ(owner.exact_state(), before);
            EXPECT_EQ(owner.records(), prior);
        }
        allocations_until_failure = -1;
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 0);
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
