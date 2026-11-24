#include "chronos/exchange/live_owner.hpp"

#include <gtest/gtest.h>

using namespace chronos;

TEST(ParticipantEffects, RepeatedCounterpartyOrdersAndPartialCancellationRemainExact) {
    exchange::LiveExchangeOwner owner(3, 32, 64, 32, exchange::AllocationMode::Exclusive);
    owner.bind();
    for (std::uint64_t id = 1; id <= 3; ++id)
        ASSERT_TRUE(owner.open_session(id));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    ASSERT_EQ(
        owner.submit(2, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_EQ(
        owner.submit(1, 2, 2, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 3),
        exchange::Result::Accepted);
    const auto before = owner.exact_continuation_state(8192);
    const auto unauthorized = owner.cancel_with_summary(3, 1, 1);
    EXPECT_FALSE(unauthorized.installed());
    EXPECT_TRUE(unauthorized.ownership_effects().empty());
    EXPECT_EQ(owner.exact_continuation_state(8192), before);
    const auto sweep = owner.submit_with_summary(3, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Market, 0, 4);
    ASSERT_TRUE(sweep.installed());
    ASSERT_EQ(sweep.trades().size(), 3U);
    ASSERT_EQ(sweep.participant_effects().size(), 3U);
    EXPECT_EQ(sweep.participant_effects()[0].session, 1U);
    EXPECT_EQ(sweep.participant_effects()[0].before.open_notional, 500U);
    EXPECT_EQ(sweep.participant_effects()[0].after.open_notional, 200U);
    EXPECT_EQ(sweep.participant_effects()[0].after.position, -3);
    ASSERT_EQ(sweep.ownership_effects().size(), 4U);
    const auto residual = owner.find(1, 2);
    ASSERT_TRUE(residual);
    EXPECT_EQ(residual->quantity, 2U);
    ASSERT_TRUE(sweep.ownership_effects()[2].after->resting);
    EXPECT_EQ(sweep.ownership_effects()[2].after->resting->quantity, residual->quantity);
    EXPECT_EQ(sweep.ownership_effects()[2].after->resting->sequence, residual->sequence);
    const auto cancelled = owner.cancel_with_summary(1, 3, 2);
    ASSERT_TRUE(cancelled.installed());
    ASSERT_EQ(cancelled.participant_effects().size(), 1U);
    EXPECT_EQ(cancelled.participant_effects()[0].before.open_notional, 200U);
    EXPECT_EQ(cancelled.participant_effects()[0].after.open_notional, 0U);
    EXPECT_FALSE(owner.find(1, 2));
    EXPECT_TRUE(owner.finish());
}

TEST(ParticipantEffects, CrossingAndCancellationCapturePositionsAndOpenExposure) {
    exchange::LiveExchangeOwner owner(4, 32, 64, 32, exchange::AllocationMode::Exclusive);
    owner.bind();
    for (std::uint64_t id = 1; id <= 4; ++id)
        ASSERT_TRUE(owner.open_session(id));
    exchange::RiskLimits seller_limits;
    seller_limits.max_open_notional = 500;
    seller_limits.allowed_symbols = {1};
    ASSERT_TRUE(owner.configure_risk(1, seller_limits));
    for (std::uint64_t id = 1; id <= 3; ++id)
        ASSERT_EQ(owner.submit(id, 1, 1, core::SymbolId{1}, core::Side::Sell,
                               core::OrderType::Limit, static_cast<core::Price>(99 + id),
                               id == 1   ? 3
                               : id == 2 ? 2
                                         : 4),
                  exchange::Result::Accepted);
    ASSERT_TRUE(owner.set_killed(1, true));
    ASSERT_TRUE(owner.find(1, 1));
    ASSERT_TRUE(owner.find(2, 1));
    const auto first_id = owner.find(1, 1)->id.value();
    const auto second_id = owner.find(2, 1)->id.value();
    const auto fill = owner.submit_with_summary(4, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                core::OrderType::Limit, 101, 4);
    ASSERT_TRUE(fill.installed());
    ASSERT_EQ(fill.trades().size(), 2U);
    ASSERT_EQ(fill.participant_effects().size(), 3U);
    const auto &a = fill.participant_effects()[0];
    const auto &b = fill.participant_effects()[1];
    const auto &buyer = fill.participant_effects()[2];
    EXPECT_EQ(a.session, 1U);
    EXPECT_EQ(a.policy.max_open_notional, 500U);
    EXPECT_TRUE(a.policy.symbol_allowed);
    EXPECT_TRUE(a.policy.killed);
    EXPECT_EQ(a.before.open_order_count, 1U);
    EXPECT_EQ(a.before.open_notional, 300U);
    EXPECT_EQ(a.before.symbol_sell_quantity, 3U);
    EXPECT_EQ(a.after.position, -3);
    EXPECT_EQ(a.after.open_order_count, 0U);
    EXPECT_EQ(a.after.open_notional, 0U);
    EXPECT_EQ(b.session, 2U);
    EXPECT_EQ(b.before.open_notional, 202U);
    EXPECT_EQ(b.after.open_notional, 101U);
    EXPECT_EQ(b.after.symbol_sell_quantity, 1U);
    EXPECT_EQ(b.after.position, -1);
    EXPECT_EQ(buyer.session, 4U);
    EXPECT_EQ(buyer.before.position, 0);
    EXPECT_EQ(buyer.after.position, 4);
    EXPECT_EQ(buyer.after.open_notional, 0U);
    ASSERT_EQ(fill.ownership_effects().size(), 3U);
    const auto &first_order = fill.ownership_effects()[0];
    const auto &second_order = fill.ownership_effects()[1];
    const auto &incoming = fill.ownership_effects()[2];
    EXPECT_EQ(first_order.internal_order_id, first_id);
    ASSERT_TRUE(first_order.before);
    ASSERT_TRUE(first_order.after);
    ASSERT_TRUE(first_order.before->resting);
    EXPECT_EQ(first_order.before->resting->quantity, 3U);
    EXPECT_FALSE(first_order.after->open);
    EXPECT_FALSE(first_order.after->resting);
    EXPECT_EQ(second_order.internal_order_id, second_id);
    ASSERT_TRUE(second_order.before);
    ASSERT_TRUE(second_order.after);
    ASSERT_TRUE(second_order.before->resting);
    ASSERT_TRUE(second_order.after->resting);
    EXPECT_EQ(second_order.before->resting->quantity, 2U);
    EXPECT_EQ(second_order.after->resting->quantity, 1U);
    EXPECT_EQ(second_order.before->resting->sequence, second_order.after->resting->sequence);
    EXPECT_FALSE(incoming.before);
    ASSERT_TRUE(incoming.after);
    EXPECT_EQ(incoming.after->session, 4U);
    EXPECT_FALSE(incoming.after->open);
    const auto cancelled = owner.cancel_with_summary(3, 2, 1);
    ASSERT_TRUE(cancelled.installed());
    ASSERT_EQ(cancelled.participant_effects().size(), 1U);
    EXPECT_EQ(cancelled.participant_effects()[0].session, 3U);
    EXPECT_EQ(cancelled.participant_effects()[0].before.open_notional, 408U);
    EXPECT_EQ(cancelled.participant_effects()[0].after.open_notional, 0U);
    EXPECT_EQ(cancelled.participant_effects()[0].after.open_order_count, 0U);
    ASSERT_EQ(cancelled.ownership_effects().size(), 1U);
    ASSERT_TRUE(cancelled.ownership_effects()[0].before);
    ASSERT_TRUE(cancelled.ownership_effects()[0].after);
    EXPECT_TRUE(cancelled.ownership_effects()[0].before->open);
    EXPECT_FALSE(cancelled.ownership_effects()[0].after->open);
    exchange::RiskLimits limits;
    limits.max_order_quantity = 1;
    ASSERT_TRUE(owner.configure_risk(4, limits));
    const auto before = owner.exact_continuation_state(8192);
    const auto rejected = owner.submit_with_summary(4, 2, 2, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 100, 2);
    EXPECT_EQ(rejected.result(), exchange::Result::RiskQuantity);
    EXPECT_FALSE(rejected.installed());
    EXPECT_TRUE(rejected.participant_effects().empty());
    EXPECT_EQ(owner.exact_continuation_state(8192), before);
    EXPECT_TRUE(owner.finish());
}

TEST(ParticipantEffects, ClosedSessionRetainsRestingOwnerEffects) {
    exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    owner.close_session(1);
    const auto result = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                  core::OrderType::Market, 0, 1);
    ASSERT_TRUE(result.installed());
    ASSERT_EQ(result.participant_effects().size(), 2U);
    EXPECT_EQ(result.participant_effects()[0].session, 1U);
    EXPECT_EQ(result.participant_effects()[0].after.position, -1);
    EXPECT_EQ(result.participant_effects()[0].after.open_notional, 100U);
    ASSERT_EQ(result.ownership_effects().size(), 2U);
    EXPECT_EQ(result.ownership_effects()[0].after->session, 1U);
    EXPECT_TRUE(owner.finish());
}

TEST(ParticipantEffects, SelfTradeDoesNotInventPositionChange) {
    exchange::LiveExchangeOwner owner(1, 16, 32, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    const auto fill = owner.submit_with_summary(1, 2, 2, core::SymbolId{1}, core::Side::Buy,
                                                core::OrderType::Market, 0, 1);
    ASSERT_TRUE(fill.installed());
    ASSERT_EQ(fill.participant_effects().size(), 1U);
    EXPECT_EQ(fill.participant_effects()[0].before.position, 0);
    EXPECT_EQ(fill.participant_effects()[0].after.position, 0);
    EXPECT_EQ(fill.participant_effects()[0].before.open_notional, 200U);
    EXPECT_EQ(fill.participant_effects()[0].after.open_notional, 100U);
    EXPECT_TRUE(owner.finish());
}

TEST(ParticipantEffects, ThreeCounterpartiesRemainDistinctAcrossPriceLevels) {
    exchange::LiveExchangeOwner owner(4, 16, 32, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    for (std::uint64_t id = 1; id <= 4; ++id)
        ASSERT_TRUE(owner.open_session(id));
    for (std::uint64_t id = 1; id <= 3; ++id)
        ASSERT_EQ(owner.submit(id, 1, 1, core::SymbolId{1}, core::Side::Sell,
                               core::OrderType::Limit, static_cast<core::Price>(99 + id), 1),
                  exchange::Result::Accepted);
    const auto sweep = owner.submit_with_summary(4, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Market, 0, 3);
    ASSERT_TRUE(sweep.installed());
    ASSERT_EQ(sweep.trades().size(), 3U);
    ASSERT_EQ(sweep.participant_effects().size(), 4U);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(sweep.participant_effects()[i].session, i + 1);
        EXPECT_EQ(sweep.participant_effects()[i].before.position, 0);
        EXPECT_EQ(sweep.participant_effects()[i].after.position, -1);
        EXPECT_EQ(sweep.participant_effects()[i].after.open_notional, 0U);
    }
    EXPECT_EQ(sweep.participant_effects()[3].session, 4U);
    EXPECT_EQ(sweep.participant_effects()[3].after.position, 3);
    EXPECT_TRUE(owner.finish());
}

TEST(ParticipantEffects, OpenNotionalCoversOtherSymbolsButQuantityIsSymbolSpecific) {
    exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    ASSERT_EQ(
        owner.submit(1, 2, 2, core::SymbolId{2}, core::Side::Buy, core::OrderType::Limit, 50, 3),
        exchange::Result::Accepted);
    const auto fill = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                core::OrderType::Market, 0, 1);
    ASSERT_TRUE(fill.installed());
    ASSERT_EQ(fill.participant_effects().size(), 2U);
    const auto &seller = fill.participant_effects()[0];
    EXPECT_EQ(seller.session, 1U);
    EXPECT_EQ(seller.before.open_order_count, 2U);
    EXPECT_EQ(seller.before.open_notional, 350U);
    EXPECT_EQ(seller.before.symbol_sell_quantity, 2U);
    EXPECT_EQ(seller.before.symbol_buy_quantity, 0U);
    EXPECT_EQ(seller.after.open_order_count, 2U);
    EXPECT_EQ(seller.after.open_notional, 250U);
    EXPECT_EQ(seller.after.symbol_sell_quantity, 1U);
    EXPECT_EQ(seller.after.symbol_buy_quantity, 0U);
    EXPECT_TRUE(owner.finish());
}
