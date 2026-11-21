#include "chronos/exchange/live_owner.hpp"

#include <gtest/gtest.h>
#include <limits>

using namespace chronos;

// This test runs in a dedicated process linked against a test-only core copy.
TEST(PreparedGlobalExhaustion, LastIdCanAbortThenCommitWithoutWrapping) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    exchange::LiveExchangeOwner owner(2, 16, 32, 32, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.seed_process_id_for_test(max - 1));
    const auto before = owner.exact_continuation_state(4096);
    const auto submit = [&](std::uint64_t request, std::uint64_t client, core::Quantity quantity,
                            bool install) {
        return owner.with_prepared_submit(1, request, client, core::SymbolId{1}, core::Side::Buy,
                                          core::OrderType::Limit, 100, quantity,
                                          [install](exchange::Result) { return install; });
    };
    const auto aborted = submit(1, 1, 1, false);
    EXPECT_EQ(aborted.result, exchange::Result::Accepted);
    EXPECT_FALSE(aborted.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    const auto invalid = submit(1, 1, 0, true);
    EXPECT_EQ(invalid.result, exchange::Result::RiskQuantity);
    EXPECT_FALSE(invalid.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    const auto accepted = submit(1, 1, 1, true);
    ASSERT_EQ(accepted.result, exchange::Result::Accepted);
    ASSERT_TRUE(accepted.installed);
    ASSERT_TRUE(owner.front(1));
    EXPECT_EQ(owner.front(1)->internal_order_id, max);
    ASSERT_TRUE(owner.find(1, 1));
    EXPECT_FALSE(owner.seed_process_id_for_test(0));
    const auto exhausted_state = owner.exact_continuation_state(4096);
    const auto exhausted = submit(2, 2, 1, true);
    EXPECT_EQ(exhausted.result, exchange::Result::CounterExhausted);
    EXPECT_FALSE(exhausted.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), exhausted_state);
    const auto cancelled =
        owner.with_prepared_cancel(1, 3, 1, [](exchange::Result) { return true; });
    EXPECT_EQ(cancelled.result, exchange::Result::Accepted);
    EXPECT_TRUE(cancelled.installed);
    EXPECT_FALSE(owner.find(1, 1));
    EXPECT_TRUE(owner.finish());
}
