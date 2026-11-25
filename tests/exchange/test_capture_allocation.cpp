#include "authorization_test_harness.hpp"
#include "chronos/exchange/canonical_replay_envelope.hpp"
#include "chronos/exchange/live_owner.hpp"
#include "chronos/exchange/restricted_replay.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <new>
#include <thread>

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
        const auto canonical_before = owner.canonical_state();
        const auto first_canonical = owner.recorded_canonical(0);
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
            EXPECT_EQ(owner.canonical_state(), canonical_before);
            EXPECT_EQ(owner.recorded_canonical(0), first_canonical);
        }
        allocations_until_failure = -1;
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 0);
}

TEST(PublicCaptureAllocation, RestrictedCanonicalLimitFailsBeforeInstallation) {
    using namespace chronos;
    exchange::RestrictedReplay owner;
    exchange::RestrictedReplay::Operation open{exchange::RestrictedReplay::Kind::Open};
    open.args[0] = 1;
    const auto before = owner.exact_state();
    const auto canonical_before = owner.canonical_state();
    owner.set_canonical_limit_for_test(8);
    EXPECT_THROW(static_cast<void>(owner.apply(open)), std::length_error);
    EXPECT_EQ(owner.size(), 0U);
    EXPECT_EQ(owner.exact_state(), before);
    EXPECT_EQ(owner.canonical_state(), canonical_before);
    owner.set_canonical_limit_for_test(65536);
    ASSERT_TRUE(owner.apply(open).committed);
    EXPECT_EQ(owner.size(), 1U);
    ASSERT_EQ(owner.records().size(), 1U);
    EXPECT_EQ(owner.recorded_canonical(0), owner.canonical_state());
}

TEST(PublicCaptureAllocation, NativeApplyAndSealDoNotReplayHistory) {
    using namespace chronos;
    using R = exchange::RestrictedReplay;
    R owner;
    R::execution_count_for_test = 0;
    R::Operation open{R::Kind::Open};
    open.args[0] = 1;
    ASSERT_TRUE(owner.apply(open).committed);
    EXPECT_EQ(R::execution_count_for_test, 1U);
    open.args[0] = 2;
    ASSERT_TRUE(owner.apply(open).committed);
    EXPECT_EQ(R::execution_count_for_test, 2U);
    exchange::CanonicalReplayEnvelope::Identity identity{};
    identity[0] = 1;
    const auto sealed = exchange::CanonicalReplayEnvelope::seal(owner, identity);
    EXPECT_EQ(R::execution_count_for_test, 2U);
    ASSERT_TRUE(exchange::CanonicalReplayEnvelope::verify(sealed.bytes, sealed.anchor));
    EXPECT_EQ(R::execution_count_for_test, 4U);
}

TEST(PublicCaptureAllocation, TestAuthorizationInstallsExactlyOnceAndBindsCandidate) {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    H::Identity id{};
    id[0] = 1;
    H harness(id);
    const auto before = harness.exact_state();
    R::Operation open{R::Kind::Open};
    open.args[0] = 1;
    harness.prepare(open);
    EXPECT_EQ(harness.phase(), H::Phase::Prepared);
    EXPECT_GT(harness.request_size_for_test(), 0U);
    EXPECT_EQ(harness.size(), 0U);
    EXPECT_TRUE(harness.records().empty());
    EXPECT_EQ(harness.exact_state(), before);
    harness.begin_external_decision();
    harness.inject_test_decision(H::TestDecision::Permit);
    EXPECT_EQ(harness.phase(), H::Phase::Authorized);
    EXPECT_EQ(harness.size(), 0U);
    ASSERT_TRUE(harness.install());
    EXPECT_EQ(harness.phase(), H::Phase::Installed);
    EXPECT_EQ(harness.size(), 1U);
    EXPECT_NE(harness.exact_state(), before);
    R reference;
    ASSERT_TRUE(reference.apply(open).committed);
    EXPECT_EQ(harness.canonical_state(), reference.canonical_state());
    EXPECT_TRUE(R::verify(harness.records(), 1));
    EXPECT_THROW(static_cast<void>(harness.install()), std::logic_error);
    EXPECT_THROW(harness.inject_test_decision(H::TestDecision::Permit), std::logic_error);
    harness.reset_after_completion();
    EXPECT_TRUE(harness.shutdown());
}

TEST(PublicCaptureAllocation, TestAuthorizationRejectsMismatchedTokensAndStops) {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    for (const auto fault :
         {H::TestDecision::WrongCandidate, H::TestDecision::WrongSequence,
          H::TestDecision::WrongIdentity, H::TestDecision::WrongConfiguration,
          H::TestDecision::WrongPrefix, H::TestDecision::WrongLength, H::TestDecision::Ambiguous}) {
        H::Identity id{};
        id[0] = 1;
        H harness(id);
        const auto before = harness.exact_state();
        R::Operation open{R::Kind::Open};
        open.args[0] = 1;
        harness.prepare(open);
        harness.begin_external_decision();
        harness.inject_test_decision(fault);
        EXPECT_EQ(harness.phase(), H::Phase::FailedStop);
        EXPECT_EQ(harness.size(), 0U);
        EXPECT_EQ(harness.exact_state(), before);
        EXPECT_THROW(harness.prepare(open), std::logic_error);
        EXPECT_FALSE(harness.shutdown());
        harness.acknowledge_unresolved_for_test();
    }
}

TEST(PublicCaptureAllocation, TestAuthorizationAbortRefusalAndShutdownBoundaries) {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    H::Identity id{};
    id[0] = 1;
    R::Operation open{R::Kind::Open};
    open.args[0] = 1;
    {
        H h(id);
        const auto before = h.exact_state();
        h.prepare(open);
        h.abort_before_attempt();
        EXPECT_EQ(h.phase(), H::Phase::Aborted);
        EXPECT_EQ(h.exact_state(), before);
        EXPECT_EQ(h.size(), 0U);
        h.reset_after_completion();
        h.prepare(open);
        h.begin_external_decision();
        h.inject_test_decision(H::TestDecision::Refuse);
        EXPECT_EQ(h.phase(), H::Phase::Aborted);
        EXPECT_EQ(h.diagnostic(), H::Diagnostic::Refused);
        EXPECT_EQ(h.exact_state(), before);
        EXPECT_EQ(h.size(), 0U);
        EXPECT_TRUE(h.shutdown());
    }
    {
        H h(id);
        h.prepare(open);
        h.begin_external_decision();
        EXPECT_FALSE(h.shutdown());
        EXPECT_EQ(h.phase(), H::Phase::FailedStop);
        EXPECT_EQ(h.diagnostic(), H::Diagnostic::ShutdownPending);
        EXPECT_EQ(h.size(), 0U);
        h.acknowledge_unresolved_for_test();
    }
    {
        H h(id);
        h.prepare(open);
        h.begin_external_decision();
        h.inject_test_decision(H::TestDecision::Permit);
        EXPECT_TRUE(h.shutdown());
        EXPECT_EQ(h.phase(), H::Phase::Stopped);
        EXPECT_EQ(h.size(), 1U);
        EXPECT_THROW(h.prepare(open), std::logic_error);
    }
}

TEST(PublicCaptureAllocation, TestAuthorizationPreparationAllocationFailuresAreAtomic) {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    bool completed = false;
    int failures = 0;
    for (int index = 0; index < 512 && !completed; ++index) {
        H::Identity id{};
        id[0] = 1;
        H h(id);
        const auto before = h.exact_state();
        R::Operation open{R::Kind::Open};
        open.args[0] = 1;
        allocations_until_failure = index;
        try {
            h.prepare(open);
            allocations_until_failure = -1;
            EXPECT_EQ(h.size(), 0U);
            h.abort_before_attempt();
            completed = true;
        } catch (const std::bad_alloc &) {
            allocations_until_failure = -1;
            ++failures;
            EXPECT_EQ(h.phase(), H::Phase::Idle);
            EXPECT_EQ(h.size(), 0U);
            EXPECT_EQ(h.exact_state(), before);
        }
        allocations_until_failure = -1;
        EXPECT_TRUE(h.shutdown());
    }
    EXPECT_TRUE(completed);
    EXPECT_GT(failures, 0);
}

TEST(PublicCaptureAllocation, TestAuthorizationUnresolvedOwnerCannotBeSilentlyDestroyed) {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    EXPECT_DEATH(
        {
            H::Identity id{};
            id[0] = 1;
            H h(id);
            R::Operation open{R::Kind::Open};
            open.args[0] = 1;
            h.prepare(open);
            h.begin_external_decision();
        },
        "terminate");
}

TEST(PublicCaptureAllocation, TestAuthorizationRejectsWrongThreadAndPriorDecisionReuse) {
    using H = chronos::exchange::AuthorizationTestHarness;
    using R = chronos::exchange::RestrictedReplay;
    H::Identity id{};
    id[0] = 1;
    H h(id);
    R::Operation op{R::Kind::Open};
    op.args[0] = 1;
    bool rejected = false;
    std::thread other([&] {
        try {
            h.prepare(op);
        } catch (const std::logic_error &) {
            rejected = true;
        }
    });
    other.join();
    EXPECT_TRUE(rejected);
    EXPECT_EQ(h.size(), 0U);
    h.prepare(op);
    h.begin_external_decision();
    h.inject_test_decision(H::TestDecision::Permit);
    ASSERT_TRUE(h.install());
    h.reset_after_completion();
    op.args[0] = 2;
    h.prepare(op);
    EXPECT_THROW(static_cast<void>(h.install()), std::logic_error);
    EXPECT_THROW(h.inject_test_decision(H::TestDecision::Permit), std::logic_error);
    EXPECT_EQ(h.size(), 1U);
    h.abort_before_attempt();
    EXPECT_TRUE(h.shutdown());
}

TEST(PublicCaptureAllocation, CanonicalEnvelopeAllocationFailureLeavesOwnerUnchanged) {
    using namespace chronos;
    exchange::RestrictedReplay owner;
    exchange::RestrictedReplay::Operation open{exchange::RestrictedReplay::Kind::Open};
    open.args[0] = 1;
    ASSERT_TRUE(owner.apply(open).committed);
    const auto before = owner.exact_state();
    exchange::CanonicalReplayEnvelope::Identity identity{};
    identity[0] = 1;
    const auto sealed = exchange::CanonicalReplayEnvelope::seal(owner, identity);
    for (bool verify : {false, true}) {
        bool completed = false;
        int failures = 0;
        for (int index = 0; index < 512 && !completed; ++index) {
            allocations_until_failure = index;
            try {
                bool valid = false;
                if (verify)
                    valid = exchange::CanonicalReplayEnvelope::verify(sealed.bytes, sealed.anchor);
                else
                    valid = exchange::CanonicalReplayEnvelope::seal(owner, identity).bytes ==
                            sealed.bytes;
                allocations_until_failure = -1;
                ASSERT_TRUE(valid);
                completed = true;
            } catch (const std::bad_alloc &) {
                allocations_until_failure = -1;
                ++failures;
                EXPECT_EQ(owner.exact_state(), before);
            }
            allocations_until_failure = -1;
        }
        EXPECT_TRUE(completed);
        EXPECT_GT(failures, 0);
    }
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
