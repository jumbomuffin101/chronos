#include "chronos/exchange/live_owner.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

using namespace chronos;

TEST(PreparedExchange, ExpectedDequeueChecksRecipientAndLeavesOtherFrontUntouched) {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.open_session(2));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_EQ(
        owner.submit(2, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Limit, 101, 1),
        exchange::Result::Accepted);
    const auto first = owner.front(1);
    const auto second = owner.front(2);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_NE(first->sequence, second->sequence);
    const auto before = owner.exact_continuation_state(4096);
    EXPECT_EQ(owner.consume_expected_report(2, first->sequence).status,
              exchange::DequeueStatus::Mismatch);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_EQ(owner.front(2)->sequence, second->sequence);
    const auto first_index = owner.diagnostic_mutation_index();
    const auto observed = owner.cancel_recorded(1, 2, 999);
    EXPECT_NE(observed.result, exchange::Result::Accepted);
    EXPECT_EQ(observed.diagnostic_index, first_index + 1);
    ASSERT_TRUE(owner.consume_expected_report(2, second->sequence).encode());
    EXPECT_EQ(owner.front(1)->sequence, first->sequence);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, RecordedCommandsAndConditionalDequeueUseActualResults) {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto submitted = owner.submit_recorded(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                 core::OrderType::Limit, 100, 2);
    EXPECT_EQ(submitted.result, exchange::Result::Accepted);
    EXPECT_EQ(submitted.diagnostic_index, 2U);
    ASSERT_TRUE(submitted.encode());
    const auto decoded = exchange::CommandRecord::decode(*submitted.encode());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->command.encode(), submitted.command.encode());
    EXPECT_EQ(decoded->result, submitted.result);
    const auto report = owner.front(1);
    ASSERT_TRUE(report);
    const auto before = owner.exact_continuation_state(4096);
    EXPECT_EQ(owner.consume_expected_report(2, report->sequence).status,
              exchange::DequeueStatus::Empty);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    bool wrong_thread_denied = false;
    std::thread other([&] {
        try {
            static_cast<void>(owner.consume_expected_report(1, report->sequence));
        } catch (const std::logic_error &) {
            wrong_thread_denied = true;
        }
    });
    other.join();
    EXPECT_TRUE(wrong_thread_denied);
    owner.with_frozen_state([&] {
        EXPECT_THROW(static_cast<void>(owner.consume_expected_report(1, report->sequence)),
                     std::logic_error);
    });
    const auto wrong = owner.consume_expected_report(1, report->sequence + 1);
    EXPECT_EQ(wrong.status, exchange::DequeueStatus::Mismatch);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    const auto consumed = owner.consume_expected_report(1, report->sequence);
    EXPECT_EQ(consumed.status, exchange::DequeueStatus::Consumed);
    ASSERT_TRUE(consumed.encode());
    ASSERT_TRUE(exchange::DequeueRecord::decode(*consumed.encode()));
    const auto duplicate = owner.consume_expected_report(1, report->sequence);
    EXPECT_EQ(duplicate.status, exchange::DequeueStatus::Mismatch);
    ASSERT_TRUE(owner.front(1));
    const auto last = owner.front(1)->sequence;
    EXPECT_EQ(owner.consume_expected_report(1, last).status, exchange::DequeueStatus::Consumed);
    EXPECT_EQ(owner.consume_expected_report(1, last).status, exchange::DequeueStatus::Empty);
    EXPECT_EQ(owner.consume_expected_report(0, last).status, exchange::DequeueStatus::Invalid);
    const auto cancelled = owner.cancel_recorded(1, 2, 1);
    EXPECT_EQ(cancelled.result, exchange::Result::Accepted);
    ASSERT_TRUE(cancelled.encode());
    EXPECT_TRUE(exchange::CommandRecord::decode(*cancelled.encode()));
    const auto rejected = owner.cancel_recorded(1, 3, 999);
    EXPECT_NE(rejected.result, exchange::Result::Accepted);
    ASSERT_TRUE(rejected.encode());
    EXPECT_EQ(exchange::CommandRecord::decode(*rejected.encode())->result, rejected.result);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, PartialOperationCodecsRejectNoncanonicalBytes) {
    const exchange::DequeueRecord record{1, 2, 3, 3, exchange::DequeueStatus::Consumed};
    ASSERT_TRUE(record.encode());
    const auto bytes = *record.encode();
    exchange::DequeueRecord::Bytes golden{};
    golden[1] = 1;
    golden[2] = 1;
    golden[3] = 1;
    golden[15] = 1;
    golden[23] = 2;
    golden[31] = 3;
    golden[39] = 3;
    EXPECT_EQ(bytes, golden);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        EXPECT_FALSE(exchange::DequeueRecord::decode(std::span{bytes}.first(n)));
    std::array<std::uint8_t, exchange::DequeueRecord::encoded_size + 1> extra{};
    std::copy(bytes.begin(), bytes.end(), extra.begin());
    EXPECT_FALSE(exchange::DequeueRecord::decode(extra));
    auto bad = bytes;
    bad[4] = 1;
    EXPECT_FALSE(exchange::DequeueRecord::decode(bad));
    bad = bytes;
    bad[1] = 2;
    EXPECT_FALSE(exchange::DequeueRecord::decode(bad));
    bad = bytes;
    bad[39] = 4;
    EXPECT_FALSE(exchange::DequeueRecord::decode(bad));
    const exchange::CommandRecord command{1, exchange::CanonicalCommand::cancel(2, 3, 4),
                                          exchange::Result::Accepted};
    const auto encoded = command.encode();
    ASSERT_TRUE(encoded);
    exchange::CommandRecord::Bytes expected{};
    expected[1] = 1;
    expected[2] = 2;
    expected[3] = 1;
    expected[5] = 1;
    expected[15] = 1;
    expected[17] = 1;
    expected[18] = 2;
    expected[27] = 2;
    expected[35] = 3;
    expected[43] = 4;
    EXPECT_EQ(*encoded, expected);
    for (std::size_t n = 0; n < encoded->size(); ++n)
        EXPECT_FALSE(exchange::CommandRecord::decode(std::span{*encoded}.first(n)));
    std::array<std::uint8_t, exchange::CommandRecord::encoded_size + 1> extra_command{};
    std::copy(encoded->begin(), encoded->end(), extra_command.begin());
    EXPECT_FALSE(exchange::CommandRecord::decode(extra_command));
    auto bad_command = *encoded;
    bad_command[3] = 2;
    EXPECT_FALSE(exchange::CommandRecord::decode(bad_command));
    bad_command = *encoded;
    bad_command[7] = 1;
    EXPECT_FALSE(exchange::CommandRecord::decode(bad_command));
    bad_command = *encoded;
    bad_command[5] = 255;
    EXPECT_FALSE(exchange::CommandRecord::decode(bad_command));
}

TEST(PreparedExchange, DiagnosticIndexCountsRejectionAndNoOpButNotThrowOrAbort) {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    EXPECT_EQ(owner.diagnostic_mutation_index(), 1U);
    EXPECT_FALSE(owner.open_session(1));
    EXPECT_EQ(owner.diagnostic_mutation_index(), 2U);
    owner.pop(1);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 3U);
    const auto before = owner.exact_continuation_state(4096);
    EXPECT_THROW(static_cast<void>(owner.with_prepared_close_session(
                     1, [](exchange::Result) -> bool { throw std::runtime_error("abort"); })),
                 std::runtime_error);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 3U);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    const auto rejected = owner.with_prepared_close_session(999, [](exchange::Result result) {
        EXPECT_EQ(result, exchange::Result::InvalidOrder);
        return true;
    });
    EXPECT_FALSE(rejected.installed);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 4U);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    EXPECT_TRUE(owner.finish());
    EXPECT_THROW(static_cast<void>(owner.open_session(2)), std::logic_error);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 4U);
}

TEST(PreparedExchange, DiagnosticOrderTracksCompletedMutationsAndPreservesAbort) {
    exchange::LiveExchangeOwner owner(3, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    EXPECT_EQ(owner.diagnostic_mutation_index(), 0U);
    ASSERT_TRUE(owner.open_session(1));
    exchange::RiskLimits limits;
    ASSERT_TRUE(owner.configure_risk(1, limits));
    ASSERT_TRUE(owner.subscribe(1, core::SymbolId{1}));
    EXPECT_EQ(owner.diagnostic_mutation_index(), 3U);
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 4U);
    const auto report = owner.front(1);
    ASSERT_TRUE(report);
    owner.pop(1);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 5U);
    const auto before = owner.exact_continuation_state(4096);
    const auto aborted = owner.with_prepared_close_session(1, [&](exchange::Result result) {
        EXPECT_EQ(result, exchange::Result::Accepted);
        EXPECT_EQ(owner.diagnostic_mutation_index(), 5U);
        EXPECT_THROW(owner.pop(1), std::logic_error);
        return false;
    });
    EXPECT_FALSE(aborted.installed);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 5U);
    EXPECT_EQ(owner.exact_continuation_state(4096), before);
    ASSERT_TRUE(
        owner.with_prepared_close_session(1, [](exchange::Result) { return true; }).installed);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 6U);
    ASSERT_TRUE(owner.find(1, 1));
    EXPECT_EQ(owner.find(1, 1)->quantity, 2U);
    EXPECT_FALSE(owner.market_front(1));
    ASSERT_TRUE(owner.open_session(2));
    ASSERT_EQ(
        owner.submit(2, 1, 1, core::SymbolId{1}, core::Side::Sell, core::OrderType::Market, 0, 1),
        exchange::Result::Accepted);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 8U);
    EXPECT_EQ(owner.find(1, 1)->quantity, 1U);
    const auto rejected = owner.cancel(2, 2, 999);
    EXPECT_NE(rejected, exchange::Result::Accepted);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 9U);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, DiagnosticOrderIncludesSubscriberGapConsumption) {
    exchange::LiveExchangeOwner owner(1, 64, 128, 64, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.subscribe(1, core::SymbolId{1}));
    for (std::uint64_t id = 1; id <= 34; ++id)
        ASSERT_EQ(owner.submit(1, id, id, core::SymbolId{1}, core::Side::Buy,
                               core::OrderType::Limit, static_cast<core::Price>(100 + id), 1),
                  exchange::Result::Accepted);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 36U);
    for (int i = 0; i < 32; ++i) {
        const auto event = owner.market_front(1);
        ASSERT_TRUE(event);
        EXPECT_NE(event->kind, exchange::MarketKind::Gap);
        owner.market_pop(1, *event);
    }
    const auto gap = owner.market_front(1);
    ASSERT_TRUE(gap);
    EXPECT_EQ(gap->kind, exchange::MarketKind::Gap);
    owner.market_pop(1, *gap);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 69U);
    EXPECT_FALSE(owner.market_front(1));
    ASSERT_TRUE(owner.subscribe(1, core::SymbolId{1}));
    ASSERT_TRUE(owner.market_front(1));
    EXPECT_EQ(owner.market_front(1)->kind, exchange::MarketKind::Snapshot);
    EXPECT_EQ(owner.diagnostic_mutation_index(), 70U);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, AdministrativeStagingAbortsAndInstallsWithoutExposingCandidates) {
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    const auto initial = owner.exact_continuation_state(4096);
    const auto aborted = owner.with_prepared_open_session(1, [&](exchange::Result result) {
        EXPECT_EQ(result, exchange::Result::Accepted);
        EXPECT_EQ(owner.verification_state()[4], 0U);
        EXPECT_THROW(static_cast<void>(owner.open_session(1)), std::logic_error);
        return false;
    });
    EXPECT_FALSE(aborted.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), initial);
    EXPECT_THROW(static_cast<void>(owner.with_prepared_open_session(
                     1, [](exchange::Result) -> bool { throw std::runtime_error("abort"); })),
                 std::runtime_error);
    EXPECT_EQ(owner.exact_continuation_state(4096), initial);
    ASSERT_TRUE(
        owner.with_prepared_open_session(1, [](exchange::Result) { return true; }).installed);
    const auto opened = owner.exact_continuation_state(4096);
    const auto duplicate =
        owner.with_prepared_open_session(1, [](exchange::Result) { return true; });
    EXPECT_EQ(duplicate.result, exchange::Result::InvalidOrder);
    EXPECT_FALSE(duplicate.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), opened);
    exchange::RiskLimits limits;
    limits.max_order_quantity = 1;
    const auto risk_abort =
        owner.with_prepared_risk_update(1, limits, [](exchange::Result) { return false; });
    EXPECT_FALSE(risk_abort.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), opened);
    ASSERT_TRUE(owner.with_prepared_risk_update(1, limits, [](exchange::Result) { return true; })
                    .installed);
    EXPECT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2),
        exchange::Result::RiskQuantity);
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    const auto resting = owner.exact_continuation_state(4096);
    EXPECT_FALSE(owner.with_prepared_risk_update(1, limits, [](exchange::Result) { return true; })
                     .installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), resting);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, AdministrativeStagingRejectsInvalidContextAndIds) {
    {
        exchange::LiveExchangeOwner shared;
        shared.bind();
        EXPECT_THROW(static_cast<void>(shared.with_prepared_open_session(
                         1, [](exchange::Result) { return true; })),
                     std::logic_error);
        exchange::RiskLimits limits;
        EXPECT_THROW(static_cast<void>(shared.with_prepared_risk_update(
                         1, limits, [](exchange::Result) { return true; })),
                     std::logic_error);
        EXPECT_TRUE(shared.finish());
    }
    exchange::LiveExchangeOwner owner(2, 16, 16, 16, exchange::AllocationMode::Exclusive);
    owner.bind();
    const auto initial = owner.exact_continuation_state(4096);
    const auto invalid = owner.with_prepared_open_session(0, [](exchange::Result) { return true; });
    EXPECT_EQ(invalid.result, exchange::Result::InvalidOrder);
    EXPECT_FALSE(invalid.installed);
    EXPECT_EQ(owner.exact_continuation_state(4096), initial);
    bool denied = false;
    std::thread other([&] {
        try {
            static_cast<void>(
                owner.with_prepared_open_session(1, [](exchange::Result) { return true; }));
        } catch (const std::logic_error &) {
            denied = true;
        }
    });
    other.join();
    EXPECT_TRUE(denied);
    EXPECT_EQ(owner.exact_continuation_state(4096), initial);
    ASSERT_TRUE(owner
                    .with_prepared_open_session(
                        1,
                        [&](exchange::Result result) {
                            EXPECT_EQ(result, exchange::Result::Accepted);
                            EXPECT_THROW(static_cast<void>(owner.with_prepared_open_session(
                                             2, [](exchange::Result) { return true; })),
                                         std::logic_error);
                            return true;
                        })
                    .installed);
    EXPECT_FALSE(
        owner.with_prepared_open_session(1, [](exchange::Result) { return true; }).installed);
    EXPECT_TRUE(
        owner.with_prepared_open_session(2, [](exchange::Result) { return true; }).installed);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, AbortAndExceptionPreserveStateAndReservation) {
    std::uint64_t prior_id = 0;
    {
        exchange::Exchange shared;
        ASSERT_TRUE(shared.open_session(1));
        ASSERT_EQ(shared.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                                100, 1),
                  exchange::Result::Accepted);
        ASSERT_TRUE(shared.front(1));
        prior_id = shared.front(1)->internal_order_id;
    }
    exchange::LiveExchangeOwner owner(4, 64, 64, 64, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    const auto initial = owner.verification_state();
    const auto aborted = owner.with_prepared_submit(
        1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2,
        [&](exchange::Result result) {
            EXPECT_EQ(result, exchange::Result::Accepted);
            EXPECT_EQ(owner.verification_state(), initial);
            EXPECT_THROW(static_cast<void>(owner.exact_continuation_state(4096)), std::logic_error);
            EXPECT_FALSE(owner.front(1));
            EXPECT_THROW(static_cast<void>(owner.open_session(2)), std::logic_error);
            EXPECT_THROW(static_cast<void>(owner.set_killed(1, true)), std::logic_error);
            EXPECT_THROW(static_cast<void>(owner.subscribe(1, core::SymbolId{1})),
                         std::logic_error);
            EXPECT_THROW(owner.pop(1), std::logic_error);
            EXPECT_FALSE(owner.finish());
            EXPECT_THROW(static_cast<void>(owner.with_prepared_cancel(
                             1, 2, 1, [](exchange::Result) { return true; })),
                         std::logic_error);
            bool denied = false, mutation_denied = false;
            std::thread other([&] {
                try {
                    exchange::Exchange shared;
                } catch (const std::runtime_error &) {
                    denied = true;
                }
                try {
                    static_cast<void>(owner.cancel(1, 2, 1));
                } catch (const std::logic_error &) {
                    mutation_denied = true;
                }
            });
            other.join();
            EXPECT_TRUE(denied);
            EXPECT_TRUE(mutation_denied);
            return false;
        });
    EXPECT_EQ(aborted.result, exchange::Result::Accepted);
    EXPECT_FALSE(aborted.installed);
    EXPECT_EQ(owner.verification_state(), initial);
    EXPECT_THROW(static_cast<void>(owner.with_prepared_submit(
                     1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2,
                     [](exchange::Result) -> bool { throw std::runtime_error("abort"); })),
                 std::runtime_error);
    EXPECT_EQ(owner.verification_state(), initial);
    const auto installed = owner.with_prepared_submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                      core::OrderType::Limit, 100, 2,
                                                      [](exchange::Result) { return true; });
    EXPECT_EQ(installed.result, exchange::Result::Accepted);
    EXPECT_TRUE(installed.installed);
    ASSERT_TRUE(owner.front(1));
    const auto first = owner.front(1)->internal_order_id;
    EXPECT_EQ(first, prior_id + 1);
    const auto rejected = owner.with_prepared_submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy,
                                                     core::OrderType::Limit, 100, 0,
                                                     [](exchange::Result) { return true; });
    EXPECT_EQ(rejected.result, exchange::Result::RiskQuantity);
    EXPECT_FALSE(rejected.installed);
    ASSERT_EQ(
        owner.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 99, 1),
        exchange::Result::Accepted);
    owner.pop(1);
    owner.pop(1);
    ASSERT_TRUE(owner.front(1));
    EXPECT_EQ(owner.front(1)->internal_order_id, first + 1);
    const auto before_cancel = owner.verification_state();
    const auto aborted_cancel =
        owner.with_prepared_cancel(1, 3, 2, [](exchange::Result) { return false; });
    EXPECT_EQ(aborted_cancel.result, exchange::Result::Accepted);
    EXPECT_FALSE(aborted_cancel.installed);
    EXPECT_EQ(owner.verification_state(), before_cancel);
    const auto installed_cancel =
        owner.with_prepared_cancel(1, 3, 2, [](exchange::Result) { return true; });
    EXPECT_EQ(installed_cancel.result, exchange::Result::Accepted);
    EXPECT_TRUE(installed_cancel.installed);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, SharedOwnerCannotPrepare) {
    exchange::LiveExchangeOwner owner;
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    bool called = false;
    EXPECT_THROW(static_cast<void>(owner.with_prepared_submit(
                     1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1,
                     [&](exchange::Result) {
                         called = true;
                         return true;
                     })),
                 std::logic_error);
    EXPECT_FALSE(called);
    EXPECT_FALSE(owner.front(1));
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, MatchesReferenceAcrossLevelsFillsFifoAndCancellation) {
    std::uint64_t prior_id = 0;
    {
        exchange::Exchange shared;
        ASSERT_TRUE(shared.open_session(1));
        ASSERT_EQ(shared.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                                100, 1),
                  exchange::Result::Accepted);
        ASSERT_TRUE(shared.front(1));
        prior_id = shared.front(1)->internal_order_id;
    }
    exchange::LiveExchangeOwner owner(4, 64, 64, 64, exchange::AllocationMode::Exclusive);
    auto reference = exchange::Exchange::isolated(4, 64, 64, 64, 0, prior_id);
    owner.bind();
    for (std::uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(owner.open_session(id));
        ASSERT_TRUE(reference->open_session(id));
    }
    ASSERT_TRUE(owner.subscribe(3, core::SymbolId{1}));
    ASSERT_TRUE(reference->subscribe(3, core::SymbolId{1}));
    const auto compare = [&] {
        EXPECT_EQ(owner.verification_state(), reference->verification_state());
        EXPECT_EQ(owner.exact_continuation_state(4096),
                  reference->isolated_continuation_state(4096));
        for (std::uint64_t session = 1; session <= 3; ++session) {
            for (std::uint64_t client = 1; client <= 3; ++client) {
                const auto actual = owner.find(session, client);
                const auto expected = reference->find(session, client);
                ASSERT_EQ(actual.has_value(), expected.has_value());
                if (actual) {
                    EXPECT_EQ(actual->id, expected->id);
                    EXPECT_EQ(actual->symbol, expected->symbol);
                    EXPECT_EQ(actual->side, expected->side);
                    EXPECT_EQ(actual->price, expected->price);
                    EXPECT_EQ(actual->quantity, expected->quantity);
                    EXPECT_EQ(actual->sequence, expected->sequence);
                }
            }
            const auto actual = owner.front(session);
            const auto expected = reference->front(session);
            ASSERT_EQ(actual.has_value(), expected.has_value());
            if (actual) {
                EXPECT_EQ(actual->sequence, expected->sequence);
                EXPECT_EQ(actual->command_sequence, expected->command_sequence);
                EXPECT_EQ(actual->request_id, expected->request_id);
                EXPECT_EQ(actual->client_order_id, expected->client_order_id);
                EXPECT_EQ(actual->internal_order_id, expected->internal_order_id);
                EXPECT_EQ(actual->trade_id, expected->trade_id);
                EXPECT_EQ(actual->kind, expected->kind);
                EXPECT_EQ(actual->price, expected->price);
                EXPECT_EQ(actual->quantity, expected->quantity);
                EXPECT_EQ(actual->remaining, expected->remaining);
            }
        }
        ASSERT_EQ(owner.trade_count(), reference->trade_count());
        for (std::size_t i = 0; i < owner.trade_count(); ++i) {
            const auto actual = owner.trade(i);
            const auto expected = reference->trade(i);
            ASSERT_TRUE(actual);
            ASSERT_TRUE(expected);
            EXPECT_EQ(actual->command_sequence, expected->command_sequence);
            EXPECT_EQ(actual->trade_id, expected->trade_id);
            EXPECT_EQ(actual->buyer_session, expected->buyer_session);
            EXPECT_EQ(actual->seller_session, expected->seller_session);
            EXPECT_EQ(actual->buy_internal_id, expected->buy_internal_id);
            EXPECT_EQ(actual->sell_internal_id, expected->sell_internal_id);
            EXPECT_EQ(actual->symbol, expected->symbol);
            EXPECT_EQ(actual->price, expected->price);
            EXPECT_EQ(actual->quantity, expected->quantity);
        }
    };
    const auto submit = [&](std::uint64_t session, std::uint64_t req, std::uint64_t client,
                            core::Side side, core::OrderType type, core::Price price,
                            core::Quantity quantity) {
        const auto before = owner.verification_state();
        const auto actual =
            owner.with_prepared_submit(session, req, client, core::SymbolId{1}, side, type, price,
                                       quantity, [&](exchange::Result) {
                                           EXPECT_EQ(owner.verification_state(), before);
                                           return true;
                                       });
        const auto expected =
            reference->submit(session, req, client, core::SymbolId{1}, side, type, price, quantity);
        EXPECT_EQ(actual.result, expected);
        EXPECT_EQ(actual.installed, expected == exchange::Result::Accepted);
        compare();
    };
    const auto cancel = [&](std::uint64_t session, std::uint64_t req, std::uint64_t client) {
        const auto before = owner.verification_state();
        const auto actual = owner.with_prepared_cancel(session, req, client, [&](exchange::Result) {
            EXPECT_EQ(owner.verification_state(), before);
            return true;
        });
        const auto expected = reference->cancel(session, req, client);
        EXPECT_EQ(actual.result, expected);
        EXPECT_EQ(actual.installed, expected == exchange::Result::Accepted);
        compare();
    };
    submit(1, 1, 1, core::Side::Sell, core::OrderType::Limit, 100, 2);
    submit(1, 2, 2, core::Side::Sell, core::OrderType::Limit, 101, 3);
    submit(1, 3, 3, core::Side::Sell, core::OrderType::Limit, 101, 2);
    submit(2, 1, 1, core::Side::Buy, core::OrderType::Limit, 101, 4);
    ASSERT_EQ(owner.trade_count(), 2U);
    ASSERT_TRUE(owner.trade(0));
    ASSERT_TRUE(owner.trade(1));
    EXPECT_EQ(owner.trade(0)->price, 100);
    EXPECT_EQ(owner.trade(1)->price, 101);
    submit(3, 1, 1, core::Side::Buy, core::OrderType::Market, 0, 1);
    cancel(2, 2, 3); // another session cannot cancel the seller's order
    cancel(1, 4, 3);
    cancel(1, 5, 2);
    submit(2, 2, 2, core::Side::Buy, core::OrderType::Limit, 100, 0);
    EXPECT_TRUE(owner.finish());
}

TEST(PreparedExchange, CapacityAndWrongContextCannotPublish) {
    exchange::LiveExchangeOwner owner(2, 8, 2, 8, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    const auto state = owner.verification_state();
    const auto rejected = owner.with_prepared_submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy,
                                                     core::OrderType::Limit, 99, 1,
                                                     [](exchange::Result) { return true; });
    EXPECT_EQ(rejected.result, exchange::Result::Capacity);
    EXPECT_FALSE(rejected.installed);
    EXPECT_EQ(owner.verification_state(), state);
    bool denied = false, callback = false;
    std::thread other([&] {
        try {
            static_cast<void>(owner.with_prepared_cancel(1, 2, 1, [&](exchange::Result) {
                callback = true;
                return true;
            }));
        } catch (const std::logic_error &) {
            denied = true;
        }
    });
    other.join();
    EXPECT_TRUE(denied);
    EXPECT_FALSE(callback);
    EXPECT_EQ(owner.verification_state(), state);
    EXPECT_TRUE(owner.finish());
}
