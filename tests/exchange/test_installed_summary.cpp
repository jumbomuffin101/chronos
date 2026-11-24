#include "chronos/exchange/live_owner.hpp"

#include <array>
#include <gtest/gtest.h>
#include <thread>

using namespace chronos;

TEST(InstalledSummary, CapturesAuthoritativeTradeAndReportDeltasAfterInstall) {
    exchange::LiveExchangeOwner owner(4, 32, 32, 32, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    const auto resting = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Sell,
                                                   core::OrderType::Limit, 100, 5);
    ASSERT_EQ(resting.result(), exchange::Result::Accepted);
    ASSERT_TRUE(resting.installed());
    ASSERT_TRUE(resting.new_order_id());
    ASSERT_TRUE(resting.command());
    EXPECT_EQ(resting.command()->encode(),
              exchange::CanonicalCommand::submit(1, 1, 1, 1, 1, 0, 100, 5).encode());
    ASSERT_TRUE(resting.allocator_before());
    ASSERT_TRUE(resting.allocator_after());
    EXPECT_EQ(*resting.allocator_before() + 1, *resting.new_order_id());
    EXPECT_EQ(*resting.allocator_after(), *resting.new_order_id());
    ASSERT_TRUE(resting.symbol_before());
    ASSERT_TRUE(resting.symbol_after());
    EXPECT_FALSE(resting.symbol_before()->engine_present);
    EXPECT_FALSE(resting.symbol_before()->feed_present);
    EXPECT_TRUE(resting.symbol_after()->engine_present);
    EXPECT_TRUE(resting.symbol_after()->feed_present);
    EXPECT_EQ(resting.symbol_after()->feed_sequence, 1U);
    EXPECT_EQ(resting.symbol_after()->matching.command, 1U);
    ASSERT_TRUE(resting.initiator_final_report());
    EXPECT_EQ(resting.initiator_final_report()->kind, exchange::ReportKind::Resting);
    EXPECT_EQ(resting.initiator_final_report()->remaining, 5U);
    ASSERT_EQ(resting.reports().size(), 2U);
    EXPECT_EQ(resting.reports()[0].report.kind, exchange::ReportKind::Accepted);
    EXPECT_EQ(resting.reports()[1].report.kind, exchange::ReportKind::Resting);
    EXPECT_EQ(resting.command_before(), 0U);
    EXPECT_EQ(resting.command_after(), 1U);
    EXPECT_EQ(resting.event_after() - resting.event_before(), 2U);
    ASSERT_EQ(resting.public_events().size(), 1U);
    EXPECT_EQ(resting.public_events()[0].kind, exchange::MarketKind::Book);
    EXPECT_EQ(resting.public_events()[0].sequence, 1U);
    EXPECT_EQ(resting.public_events()[0].ask_price, 100);
    EXPECT_EQ(resting.public_events()[0].ask_quantity, 5U);
    const auto fill = owner.submit_with_summary(2, 1, 2, core::SymbolId{1}, core::Side::Buy,
                                                core::OrderType::Market, 0, 3);
    ASSERT_EQ(fill.result(), exchange::Result::Accepted);
    ASSERT_TRUE(fill.installed());
    ASSERT_TRUE(fill.new_order_id());
    ASSERT_TRUE(fill.symbol_before());
    ASSERT_TRUE(fill.symbol_after());
    EXPECT_EQ(fill.symbol_before()->matching.command, 1U);
    EXPECT_EQ(fill.symbol_after()->matching.command, 2U);
    EXPECT_EQ(fill.symbol_after()->matching.trade, 1U);
    EXPECT_EQ(fill.symbol_before()->feed_sequence, 1U);
    EXPECT_EQ(fill.symbol_after()->feed_sequence, 3U);
    ASSERT_TRUE(fill.initiator_position_before());
    ASSERT_TRUE(fill.initiator_position_after());
    EXPECT_EQ(*fill.initiator_position_before(), 0);
    EXPECT_EQ(*fill.initiator_position_after(), 3);
    ASSERT_TRUE(fill.initiator_final_report());
    EXPECT_EQ(fill.initiator_final_report()->kind, exchange::ReportKind::Filled);
    EXPECT_EQ(fill.initiator_final_report()->remaining, 0U);
    ASSERT_EQ(fill.trades().size(), 1U);
    EXPECT_EQ(fill.trades()[0].sell_internal_id, *resting.new_order_id());
    EXPECT_EQ(fill.trades()[0].buy_internal_id, *fill.new_order_id());
    EXPECT_EQ(fill.trades()[0].quantity, 3U);
    EXPECT_EQ(fill.trades()[0].price, 100);
    ASSERT_EQ(fill.reports().size(), 3U);
    EXPECT_EQ(fill.reports()[0].session, 2U);
    EXPECT_EQ(fill.reports()[0].report.kind, exchange::ReportKind::Accepted);
    EXPECT_EQ(fill.reports()[1].session, 1U);
    EXPECT_EQ(fill.reports()[1].report.kind, exchange::ReportKind::PartialFill);
    EXPECT_EQ(fill.reports()[1].report.remaining, 2U);
    EXPECT_EQ(fill.reports()[2].session, 2U);
    EXPECT_EQ(fill.reports()[2].report.kind, exchange::ReportKind::Filled);
    EXPECT_EQ(fill.event_after() - fill.event_before(), 3U);
    ASSERT_EQ(fill.public_events().size(), 2U);
    EXPECT_EQ(fill.public_events()[0].kind, exchange::MarketKind::Trade);
    EXPECT_EQ(fill.public_events()[0].sequence, 2U);
    EXPECT_EQ(fill.public_events()[0].trade_price, 100);
    EXPECT_EQ(fill.public_events()[0].trade_quantity, 3U);
    EXPECT_EQ(fill.public_events()[1].kind, exchange::MarketKind::Book);
    EXPECT_EQ(fill.public_events()[1].sequence, 3U);
    EXPECT_EQ(fill.public_events()[1].ask_quantity, 2U);
    ASSERT_TRUE(owner.find(1, 1));
    EXPECT_EQ(owner.find(1, 1)->quantity, 2U);
    ASSERT_TRUE(owner.trade(0));
    EXPECT_EQ(owner.trade(0)->trade_id, fill.trades()[0].trade_id);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, RejectAndCancellationPreserveExactBookkeeping) {
    exchange::LiveExchangeOwner owner(3, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    const auto before = owner.exact_continuation_state(4096);
    const auto rejected = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 100, 0);
    EXPECT_EQ(rejected.result(), exchange::Result::RiskQuantity);
    EXPECT_FALSE(rejected.installed());
    EXPECT_FALSE(rejected.new_order_id());
    ASSERT_TRUE(rejected.command());
    EXPECT_EQ(rejected.command()->encode(),
              exchange::CanonicalCommand::submit(1, 1, 1, 1, 0, 0, 100, 0).encode());
    ASSERT_TRUE(rejected.allocator_before());
    ASSERT_TRUE(rejected.allocator_after());
    EXPECT_EQ(rejected.allocator_before(), rejected.allocator_after());
    ASSERT_TRUE(rejected.symbol_before());
    ASSERT_TRUE(rejected.symbol_after());
    EXPECT_EQ(rejected.symbol_before()->matching.command,
              rejected.symbol_after()->matching.command);
    EXPECT_FALSE(rejected.initiator_final_report());
    EXPECT_EQ(rejected.initiator_position_before(), rejected.initiator_position_after());
    EXPECT_TRUE(rejected.trades().empty());
    EXPECT_TRUE(rejected.reports().empty());
    EXPECT_TRUE(rejected.public_events().empty());
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    const auto accepted = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 100, 1);
    ASSERT_TRUE(accepted.installed());
    const auto before_wrong = owner.exact_continuation_state(4096);
    const auto wrong = owner.cancel_with_summary(2, 1, 1);
    EXPECT_EQ(wrong.result(), exchange::Result::NotFound);
    EXPECT_FALSE(wrong.installed());
    EXPECT_FALSE(wrong.symbol_before());
    EXPECT_EQ(wrong.allocator_before(), wrong.allocator_after());
    EXPECT_EQ(owner.exact_continuation_state(4096), before_wrong);
    const auto cancelled = owner.cancel_with_summary(1, 2, 1);
    ASSERT_TRUE(cancelled.installed());
    EXPECT_FALSE(cancelled.new_order_id());
    EXPECT_EQ(cancelled.allocator_before(), cancelled.allocator_after());
    ASSERT_TRUE(cancelled.symbol_before());
    ASSERT_TRUE(cancelled.symbol_after());
    EXPECT_GT(cancelled.symbol_after()->book_revision, cancelled.symbol_before()->book_revision);
    EXPECT_EQ(cancelled.symbol_before()->matching.command + 1,
              cancelled.symbol_after()->matching.command);
    ASSERT_TRUE(cancelled.initiator_final_report());
    EXPECT_EQ(cancelled.initiator_final_report()->kind, exchange::ReportKind::Cancelled);
    ASSERT_EQ(cancelled.reports().size(), 1U);
    EXPECT_EQ(cancelled.reports()[0].report.internal_order_id, *accepted.new_order_id());
    EXPECT_EQ(cancelled.reports()[0].report.kind, exchange::ReportKind::Cancelled);
    ASSERT_EQ(cancelled.public_events().size(), 1U);
    EXPECT_EQ(cancelled.public_events()[0].kind, exchange::MarketKind::Book);
    EXPECT_EQ(cancelled.public_events()[0].bid_quantity, 0U);
    EXPECT_FALSE(owner.find(1, 1));
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, OversizedTradeDeltaRejectsBeforeInstallation) {
    exchange::LiveExchangeOwner owner(2, 1100, 4096, 1100, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    exchange::RiskLimits limits;
    limits.max_open_orders = 1100;
    ASSERT_TRUE(owner.configure_risk(1, limits));
    for (std::uint64_t i = 1; i <= 1025; ++i) {
        ASSERT_EQ(owner.submit(1, i, i, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit,
                               100, 1),
                  exchange::Result::Accepted);
        owner.pop(1);
        owner.pop(1);
    }
    const auto before = owner.exact_continuation_state(1024 * 1024);
    EXPECT_THROW(
        static_cast<void>(owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Market, 0, 1025)),
        std::length_error);
    EXPECT_EQ(owner.exact_continuation_state(1024 * 1024), before);
    EXPECT_EQ(owner.trade_count(), 0U);
    ASSERT_EQ(owner.cancel(1, 1026, 1025), exchange::Result::Accepted);
    const auto maximum = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                   core::OrderType::Market, 0, 1024);
    ASSERT_TRUE(maximum.installed());
    ASSERT_EQ(maximum.trades().size(), 1024U);
    ASSERT_EQ(maximum.public_events().size(), 1025U);
    EXPECT_EQ(maximum.public_events().front().sequence, 1027U);
    EXPECT_EQ(maximum.public_events().back().kind, exchange::MarketKind::Book);
    EXPECT_EQ(maximum.public_events().back().sequence, 2051U);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, WrongContextAndSharedOwnerCannotPrepare) {
    {
        exchange::LiveExchangeOwner owner;
        owner.bind();
        ASSERT_TRUE(owner.open_session(1));
        EXPECT_THROW(
            static_cast<void>(owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                        core::OrderType::Limit, 100, 1)),
            std::logic_error);
        EXPECT_TRUE(owner.finish());
    }
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    bool rejected = false;
    std::thread other([&] {
        try {
            static_cast<void>(owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                        core::OrderType::Limit, 100, 1));
        } catch (const std::logic_error &) {
            rejected = true;
        }
    });
    other.join();
    EXPECT_TRUE(rejected);
    EXPECT_FALSE(owner.front(1));
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, MultiLevelSweepRetainsPriceTimeTradeAndReportOrder) {
    exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    std::array<std::uint64_t, 3> ids{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto order =
            owner.submit_with_summary(1, i + 1, i + 1, core::SymbolId{1}, core::Side::Sell,
                                      core::OrderType::Limit, i == 2 ? 101 : 100, 1);
        ASSERT_TRUE(order.installed());
        ASSERT_TRUE(order.new_order_id());
        ids[i] = *order.new_order_id();
    }
    const auto sweep = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Market, 0, 3);
    ASSERT_TRUE(sweep.installed());
    ASSERT_EQ(sweep.trades().size(), 3U);
    ASSERT_EQ(sweep.reports().size(), 7U);
    ASSERT_EQ(sweep.public_events().size(), 4U);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(sweep.trades()[i].sell_internal_id, ids[i]);
        EXPECT_EQ(sweep.trades()[i].price, i == 2 ? 101 : 100);
        ASSERT_TRUE(owner.trade(i));
        EXPECT_EQ(owner.trade(i)->trade_id, sweep.trades()[i].trade_id);
        EXPECT_EQ(sweep.reports()[1 + 2 * i].session, 1U);
        EXPECT_EQ(sweep.reports()[1 + 2 * i].report.trade_id, sweep.trades()[i].trade_id);
        EXPECT_EQ(sweep.reports()[2 + 2 * i].session, 2U);
        EXPECT_EQ(sweep.public_events()[i].kind, exchange::MarketKind::Trade);
        EXPECT_EQ(sweep.public_events()[i].sequence, 3U + i);
        EXPECT_EQ(sweep.public_events()[i].trade_price, i == 2 ? 101 : 100);
        EXPECT_EQ(sweep.public_events()[i].trade_quantity, 1U);
    }
    EXPECT_EQ(sweep.public_events()[3].kind, exchange::MarketKind::Book);
    EXPECT_EQ(sweep.public_events()[3].sequence, 6U);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, AuthoritativeEventsDoNotDependOnSubscriberCount) {
    for (std::uint64_t count = 0; count <= 2; ++count) {
        exchange::LiveExchangeOwner owner(4, 32, 32, 32, exchange::AllocationMode::Exclusive);
        owner.bind();
        for (std::uint64_t id = 1; id <= 4; ++id)
            ASSERT_TRUE(owner.open_session(id));
        for (std::uint64_t id = 3; id < 3 + count; ++id) {
            ASSERT_TRUE(owner.subscribe(id, core::SymbolId{1}));
            const auto snapshot = owner.market_front(id);
            ASSERT_TRUE(snapshot);
            EXPECT_EQ(snapshot->kind, exchange::MarketKind::Snapshot);
            owner.market_pop(id, *snapshot);
        }
        const auto rest = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Sell,
                                                    core::OrderType::Limit, 100, 5);
        ASSERT_TRUE(rest.installed());
        ASSERT_EQ(rest.public_events().size(), 1U);
        EXPECT_EQ(rest.public_events()[0].sequence, 1U);
        EXPECT_EQ(rest.public_events()[0].symbol, 1U);
        EXPECT_EQ(rest.public_events()[0].generation, 0U);
        EXPECT_EQ(rest.public_events()[0].ask_quantity, 5U);
        const auto fill = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Market, 0, 3);
        ASSERT_TRUE(fill.installed());
        ASSERT_EQ(fill.public_events().size(), 2U);
        EXPECT_EQ(fill.public_events()[0].kind, exchange::MarketKind::Trade);
        EXPECT_EQ(fill.public_events()[0].sequence, 2U);
        EXPECT_EQ(fill.public_events()[0].trade_price, 100);
        EXPECT_EQ(fill.public_events()[0].trade_quantity, 3U);
        EXPECT_EQ(fill.public_events()[1].kind, exchange::MarketKind::Book);
        EXPECT_EQ(fill.public_events()[1].sequence, 3U);
        EXPECT_EQ(fill.public_events()[1].ask_quantity, 2U);
        for (std::uint64_t id = 3; id < 3 + count; ++id) {
            for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
                const auto delivered = owner.market_front(id);
                ASSERT_TRUE(delivered);
                EXPECT_EQ(delivered->sequence, sequence);
                owner.market_pop(id, *delivered);
            }
            EXPECT_FALSE(owner.market_front(id));
        }
        EXPECT_TRUE(owner.finish());
    }
}

TEST(InstalledSummary, CapturesEventsWhileSubscriberIsGappedAndAfterResubscription) {
    exchange::LiveExchangeOwner owner(3, 128, 128, 128, exchange::AllocationMode::Exclusive);
    owner.bind();
    for (std::uint64_t id = 1; id <= 3; ++id)
        ASSERT_TRUE(owner.open_session(id));
    ASSERT_TRUE(owner.subscribe(2, core::SymbolId{1}));
    auto event = owner.market_front(2);
    ASSERT_TRUE(event);
    owner.market_pop(2, *event);
    for (std::uint64_t i = 1; i <= 33; ++i) {
        ASSERT_EQ(owner.submit(1, i, i, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                               100, 1),
                  exchange::Result::Accepted);
        owner.pop(1);
        owner.pop(1);
    }
    const auto fill = owner.submit_with_summary(3, 1, 1, core::SymbolId{1}, core::Side::Sell,
                                                core::OrderType::Market, 0, 1);
    ASSERT_TRUE(fill.installed());
    ASSERT_EQ(fill.public_events().size(), 2U);
    EXPECT_EQ(fill.public_events()[0].kind, exchange::MarketKind::Trade);
    EXPECT_EQ(fill.public_events()[0].sequence, 34U);
    EXPECT_EQ(fill.public_events()[0].trade_quantity, 1U);
    EXPECT_EQ(fill.public_events()[1].kind, exchange::MarketKind::Book);
    EXPECT_EQ(fill.public_events()[1].sequence, 35U);
    EXPECT_EQ(fill.public_events()[1].bid_quantity, 32U);
    for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
        event = owner.market_front(2);
        ASSERT_TRUE(event);
        EXPECT_EQ(event->sequence, sequence);
        owner.market_pop(2, *event);
    }
    event = owner.market_front(2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, exchange::MarketKind::Gap);
    EXPECT_EQ(event->sequence, 33U);
    owner.market_pop(2, *event);
    ASSERT_TRUE(owner.subscribe(2, core::SymbolId{1}));
    event = owner.market_front(2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, exchange::MarketKind::Snapshot);
    EXPECT_EQ(event->sequence, 35U);
    EXPECT_EQ(event->bid_quantity, 32U);
    owner.market_pop(2, *event);
    const auto cancellation = owner.cancel_with_summary(1, 34, 2);
    ASSERT_TRUE(cancellation.installed());
    ASSERT_EQ(cancellation.public_events().size(), 1U);
    EXPECT_EQ(cancellation.public_events()[0].sequence, 36U);
    EXPECT_EQ(cancellation.public_events()[0].bid_quantity, 31U);
    event = owner.market_front(2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->sequence, 36U);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, FeedSequencesArePerSymbolAndUnchangedForNoBookChange) {
    exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto first = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Limit, 100, 1);
    ASSERT_EQ(first.public_events().size(), 1U);
    EXPECT_EQ(first.public_events()[0].sequence, 1U);
    const auto second = owner.submit_with_summary(1, 2, 2, core::SymbolId{2}, core::Side::Buy,
                                                  core::OrderType::Limit, 200, 1);
    ASSERT_EQ(second.public_events().size(), 1U);
    EXPECT_EQ(second.public_events()[0].symbol, 2U);
    EXPECT_EQ(second.public_events()[0].sequence, 1U);
    const auto worse = owner.submit_with_summary(1, 3, 3, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Limit, 99, 1);
    EXPECT_TRUE(worse.installed());
    EXPECT_TRUE(worse.public_events().empty());
    const auto cancel = owner.cancel_with_summary(1, 4, 1);
    ASSERT_TRUE(cancel.installed());
    ASSERT_EQ(cancel.public_events().size(), 1U);
    EXPECT_EQ(cancel.public_events()[0].symbol, 1U);
    EXPECT_EQ(cancel.public_events()[0].sequence, 2U);
    EXPECT_EQ(cancel.public_events()[0].bid_price, 99);
    ASSERT_EQ(
        owner.submit(1, 5, 5, core::SymbolId{2}, core::Side::Buy, core::OrderType::Limit, 201, 1),
        exchange::Result::Accepted);
    const auto next = owner.cancel_with_summary(1, 6, 5);
    ASSERT_TRUE(next.installed());
    ASSERT_EQ(next.public_events().size(), 1U);
    EXPECT_EQ(next.public_events()[0].symbol, 2U);
    EXPECT_EQ(next.public_events()[0].sequence, 3U);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, AbortedPreparedCommandProducesNoDeliveryOrSequenceAdvance) {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    ASSERT_TRUE(owner.subscribe(2, core::SymbolId{1}));
    const auto snapshot = owner.market_front(2);
    ASSERT_TRUE(snapshot);
    owner.market_pop(2, *snapshot);
    const auto before = owner.exact_continuation_state(4096);
    const auto aborted = owner.with_prepared_submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 100, 1,
                                                    [](exchange::Result) { return false; });
    EXPECT_EQ(aborted.result, exchange::Result::Accepted);
    EXPECT_FALSE(aborted.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_FALSE(owner.market_front(2));
    const auto installed = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                     core::OrderType::Limit, 100, 1);
    ASSERT_TRUE(installed.installed());
    ASSERT_EQ(installed.public_events().size(), 1U);
    EXPECT_EQ(installed.public_events()[0].sequence, 1U);
    const auto delivery = owner.market_front(2);
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->sequence, 1U);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, DuplicateAndCapacityRejectionsPreserveCounterSnapshots) {
    exchange::LiveExchangeOwner owner(1, 1, 8, 8, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto accepted = owner.submit_with_summary(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 100, 1);
    ASSERT_TRUE(accepted.installed());
    const auto before = owner.exact_continuation_state(4096);
    const auto duplicate_request = owner.submit_with_summary(
        1, 1, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1);
    const auto duplicate_order = owner.submit_with_summary(
        1, 2, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1);
    const auto capacity = owner.submit_with_summary(1, 2, 2, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 100, 1);
    EXPECT_EQ(duplicate_request.result(), exchange::Result::DuplicateRequest);
    EXPECT_EQ(duplicate_order.result(), exchange::Result::DuplicateOrder);
    EXPECT_EQ(capacity.result(), exchange::Result::Capacity);
    for (const auto *rejected : {&duplicate_request, &duplicate_order, &capacity}) {
        EXPECT_FALSE(rejected->installed());
        EXPECT_TRUE(rejected->reports().empty());
        EXPECT_TRUE(rejected->public_events().empty());
        ASSERT_TRUE(rejected->symbol_before());
        ASSERT_TRUE(rejected->symbol_after());
        EXPECT_EQ(rejected->symbol_before()->matching.command,
                  rejected->symbol_after()->matching.command);
        EXPECT_EQ(rejected->symbol_before()->book_revision,
                  rejected->symbol_after()->book_revision);
        EXPECT_EQ(rejected->allocator_before(), rejected->allocator_after());
    }
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_TRUE(owner.finish());
}

TEST(InstalledSummary, CapturesSignedInitiatorPositionForSellFill) {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 3),
        exchange::Result::Accepted);
    const auto sold = owner.submit_with_summary(2, 1, 1, core::SymbolId{1}, core::Side::Sell,
                                                core::OrderType::Market, 0, 2);
    ASSERT_TRUE(sold.installed());
    ASSERT_TRUE(sold.initiator_position_before());
    ASSERT_TRUE(sold.initiator_position_after());
    EXPECT_EQ(*sold.initiator_position_before(), 0);
    EXPECT_EQ(*sold.initiator_position_after(), -2);
    EXPECT_EQ(sold.trades().front().seller_session, 2U);
    EXPECT_TRUE(owner.finish());
}
