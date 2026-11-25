#include "chronos/exchange/restricted_replay.hpp"

#include <gtest/gtest.h>
#include <thread>

using namespace chronos;
using Restricted = exchange::RestrictedReplay;

namespace {
Restricted::Operation op(Restricted::Kind kind, std::initializer_list<std::uint64_t> args) {
    Restricted::Operation result{kind};
    std::copy(args.begin(), args.end(), result.args.begin());
    return result;
}
} // namespace

TEST(RestrictedReplay, VerifiesCompleteIsolatedSequenceAndRejectsCorruption) {
    Restricted owner;
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {1})).committed);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {2})).committed);
    const auto sell = op(Restricted::Kind::Submit, {1, 1, 1, 1, 1, 0, 100, 3});
    const auto buy = op(Restricted::Kind::Submit, {2, 1, 1, 1, 0, 0, 100, 2});
    ASSERT_TRUE(owner.apply(sell).committed);
    ASSERT_TRUE(owner.apply(buy).committed);
    const auto before_abort = owner.exact_state();
    EXPECT_FALSE(owner.apply(op(Restricted::Kind::Cancel, {1, 2, 1}), false).committed);
    EXPECT_EQ(owner.exact_state(), before_abort);
    EXPECT_EQ(owner.size(), 4U);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Consume, {1, 1})).committed);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Consume, {1, 1})).committed);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Cancel, {1, 2, 1})).committed);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Close, {1})).committed);
    auto records = owner.records();
    EXPECT_TRUE(Restricted::verify(records, 8));
    EXPECT_FALSE(Restricted::verify(records, 9));
    auto prefix = records;
    prefix.pop_back();
    EXPECT_FALSE(Restricted::verify(prefix, 8));
    EXPECT_TRUE(Restricted::verify(prefix, 7)); // caller accepted a shorter valid prefix
    auto corrupted = records;
    corrupted[3][31] ^= 1;
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted[3] = corrupted[2];
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted.erase(corrupted.begin() + 2);
    EXPECT_FALSE(Restricted::verify(corrupted, 7));
    corrupted = records;
    corrupted[0][23] = 255; // unsupported kind
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted[1].pop_back();
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted[4].back() ^= 1;
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted[4][47] ^= 1; // expected report sequence
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted[0][7] ^= 1; // version/magic
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
    corrupted = records;
    corrupted[0].resize((17 + Restricted::max_state_words + 1) * 8);
    EXPECT_FALSE(Restricted::verify(corrupted, 8));
}

TEST(RestrictedReplay, PrivateReplayNeverAdvancesProcessAllocator) {
    exchange::Exchange live;
    ASSERT_TRUE(live.open_session(1));
    ASSERT_EQ(
        live.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(live.find(1, 1));
    const auto before_id = live.find(1, 1)->id.value();
    Restricted isolated;
    ASSERT_TRUE(isolated.apply(op(Restricted::Kind::Open, {1})).committed);
    ASSERT_TRUE(isolated.apply(op(Restricted::Kind::Submit, {1, 1, 1, 1, 0, 0, 100, 1})).committed);
    EXPECT_TRUE(Restricted::verify(isolated.records(), isolated.size()));
    ASSERT_EQ(
        live.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 99, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(live.find(1, 2));
    EXPECT_EQ(live.find(1, 2)->id.value(), before_id + 1);
}

TEST(RestrictedReplay, RejectsWrongOwnerUnsupportedInputAndCapacityBeforeMutation) {
    Restricted owner(1);
    const auto before = owner.exact_state();
    auto unsupported = op(Restricted::Kind::Open, {1});
    unsupported.args[11] = 1;
    EXPECT_THROW(static_cast<void>(owner.apply(unsupported)), std::invalid_argument);
    EXPECT_EQ(owner.exact_state(), before);
    bool denied = false;
    std::thread other([&] {
        try {
            static_cast<void>(owner.apply(op(Restricted::Kind::Open, {1})));
        } catch (const std::logic_error &) {
            denied = true;
        }
    });
    other.join();
    EXPECT_TRUE(denied);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {1})).committed);
    const auto after = owner.exact_state();
    EXPECT_THROW(static_cast<void>(owner.apply(op(Restricted::Kind::Open, {2}))),
                 std::length_error);
    EXPECT_EQ(owner.exact_state(), after);
    EXPECT_EQ(owner.size(), 1U);
}

TEST(RestrictedReplay, RiskMultiLevelPartialFillAndCancelMatchIndependentExchange) {
    Restricted owner;
    auto independent = exchange::Exchange::isolated(16, 128, 128, 128, 0, 0);
    for (std::uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {id})).committed);
        ASSERT_TRUE(independent->open_session(id));
    }
    exchange::RiskLimits limits;
    limits.max_order_quantity = 10;
    limits.max_order_notional = 100000;
    limits.max_open_notional = 100000;
    limits.max_open_orders = 10;
    limits.max_abs_position = 1000;
    limits.max_market_price = 1000;
    limits.allowed_symbols = {1};
    ASSERT_TRUE(
        owner.apply(op(Restricted::Kind::Risk, {1, 10, 100000, 100000, 10, 1000, 1000, 1, 1}))
            .committed);
    ASSERT_TRUE(independent->configure_risk(1, limits));
    for (std::uint64_t id = 1; id <= 2; ++id) {
        const auto price = 99 + id;
        ASSERT_TRUE(
            owner.apply(op(Restricted::Kind::Submit, {id, 1, 1, 1, 1, 0, price, 2})).committed);
        ASSERT_EQ(independent->submit(id, 1, 1, core::SymbolId{1}, core::Side::Sell,
                                      core::OrderType::Limit, static_cast<core::Price>(price), 2),
                  exchange::Result::Accepted);
    }
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Submit, {3, 1, 1, 1, 0, 0, 101, 3})).committed);
    ASSERT_EQ(independent->submit(3, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                  core::OrderType::Limit, 101, 3),
              exchange::Result::Accepted);
    EXPECT_EQ(owner.exact_state(), independent->isolated_continuation_state(8192));
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Cancel, {2, 2, 1})).committed);
    ASSERT_EQ(independent->cancel(2, 2, 1), exchange::Result::Accepted);
    EXPECT_EQ(owner.exact_state(), independent->isolated_continuation_state(8192));
    const auto unknown = owner.apply(op(Restricted::Kind::Cancel, {3, 2, 999}));
    EXPECT_TRUE(unknown.committed);
    EXPECT_NE(unknown.outcome, 16U);
    EXPECT_NE(independent->cancel(3, 2, 999), exchange::Result::Accepted);
    EXPECT_EQ(owner.exact_state(), independent->isolated_continuation_state(8192));
    EXPECT_TRUE(Restricted::verify(owner.records(), owner.size()));
}

TEST(RestrictedReplay, SamePriceFifoAndWrongOwnerCancellationAreReplayed) {
    Restricted owner;
    auto independent = exchange::Exchange::isolated(16, 128, 128, 128, 0, 0);
    for (std::uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {id})).committed);
        ASSERT_TRUE(independent->open_session(id));
    }
    for (std::uint64_t id = 1; id <= 2; ++id) {
        ASSERT_TRUE(
            owner.apply(op(Restricted::Kind::Submit, {id, 1, 7, 1, 1, 0, 100, 2})).committed);
        ASSERT_EQ(independent->submit(id, 1, 7, core::SymbolId{1}, core::Side::Sell,
                                      core::OrderType::Limit, 100, 2),
                  exchange::Result::Accepted);
    }
    const auto wrong = owner.apply(op(Restricted::Kind::Cancel, {3, 1, 7}));
    EXPECT_TRUE(wrong.committed);
    EXPECT_NE(wrong.outcome, 16U);
    EXPECT_NE(independent->cancel(3, 1, 7), exchange::Result::Accepted);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Submit, {3, 1, 8, 1, 0, 1, 0, 3})).committed);
    ASSERT_EQ(independent->submit(3, 1, 8, core::SymbolId{1}, core::Side::Buy,
                                  core::OrderType::Market, 0, 3),
              exchange::Result::Accepted);
    EXPECT_FALSE(independent->find(1, 7));
    ASSERT_TRUE(independent->find(2, 7));
    EXPECT_EQ(independent->find(2, 7)->quantity, 1U);
    EXPECT_EQ(owner.exact_state(), independent->isolated_continuation_state(8192));
    EXPECT_TRUE(Restricted::verify(owner.records(), owner.size()));
}

TEST(RestrictedReplay, PrivateQueueCapacityRejectionAndConsumptionReplay) {
    Restricted owner;
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {1})).committed);
    for (std::uint64_t id = 1; id <= 64; ++id)
        ASSERT_EQ(owner.apply(op(Restricted::Kind::Submit, {1, id, id, 1, 0, 0, 100, 1})).outcome,
                  16U);
    const auto before = owner.exact_state();
    const auto rejected = owner.apply(op(Restricted::Kind::Submit, {1, 65, 65, 1, 0, 0, 100, 1}));
    EXPECT_EQ(rejected.outcome, 23U); // result-code table: Capacity
    EXPECT_EQ(owner.exact_state(), before);
    EXPECT_EQ(owner.apply(op(Restricted::Kind::Consume, {1, 1})).outcome, 1U);
    EXPECT_EQ(owner.apply(op(Restricted::Kind::Consume, {1, 1})).outcome, 3U);
    EXPECT_EQ(owner.apply(op(Restricted::Kind::Consume, {1, 2})).outcome, 1U);
    EXPECT_EQ(owner.apply(op(Restricted::Kind::Submit, {1, 65, 65, 1, 0, 0, 100, 1})).outcome, 16U);
    EXPECT_TRUE(Restricted::verify(owner.records(), owner.size()));
}
