#include "chronos/exchange/canonical_replay_envelope.hpp"
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
void resign(exchange::CanonicalReplayEnvelope::Sealed &sealed) {
    std::fill(sealed.bytes.begin() + 136, sealed.bytes.begin() + 168, 0);
    sealed.anchor.digest = journal::sha256(sealed.bytes);
    std::copy(sealed.anchor.digest.begin(), sealed.anchor.digest.end(), sealed.bytes.begin() + 136);
}
} // namespace

TEST(RestrictedReplay, NativeRecordsCaptureOriginalCandidateAndRemainStable) {
    Restricted owner;
    const auto apply_and_check = [&](const Restricted::Operation &operation) {
        const auto result = owner.apply(operation);
        EXPECT_TRUE(result.committed);
        EXPECT_EQ(owner.recorded_canonical(owner.size() - 1), owner.canonical_state());
        return result;
    };
    apply_and_check(op(Restricted::Kind::Open, {1}));
    const auto first = owner.recorded_canonical(0);
    apply_and_check(op(Restricted::Kind::Risk, {1, 10, 100000, 100000, 10, 1000, 1000, 1, 1}));
    apply_and_check(op(Restricted::Kind::Open, {2}));
    apply_and_check(op(Restricted::Kind::Submit, {1, 1, 1, 1, 1, 0, 100, 3}));
    apply_and_check(op(Restricted::Kind::Submit, {2, 1, 1, 1, 0, 0, 100, 2}));
    apply_and_check(op(Restricted::Kind::Consume, {1, 1}));
    apply_and_check(op(Restricted::Kind::Cancel, {1, 2, 1}));
    apply_and_check(op(Restricted::Kind::Close, {1}));
    EXPECT_EQ(owner.recorded_canonical(0), first);
    const auto before = owner.canonical_state();
    EXPECT_FALSE(owner.apply(op(Restricted::Kind::Open, {3}), false).committed);
    EXPECT_EQ(owner.canonical_state(), before);
    EXPECT_EQ(owner.size(), 8U);
    EXPECT_THROW(static_cast<void>(owner.recorded_canonical(8)), std::out_of_range);
    exchange::CanonicalReplayEnvelope::Identity identity{};
    identity[0] = 7;
    const auto envelope = exchange::CanonicalReplayEnvelope::seal(owner, identity);
    EXPECT_TRUE(exchange::CanonicalReplayEnvelope::verify(envelope.bytes, envelope.anchor));
    EXPECT_TRUE(Restricted::verify(owner.records(), owner.size()));
}

TEST(RestrictedReplay, CanonicalStateAndEnvelopeVerifyIndependently) {
    using Envelope = exchange::CanonicalReplayEnvelope;
    Restricted owner;
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Open, {1})).committed);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Submit, {1, 1, 1, 1, 0, 0, 100, 2})).committed);
    ASSERT_TRUE(owner.apply(op(Restricted::Kind::Consume, {1, 1})).committed);
    Envelope::Identity identity{};
    identity[0] = 42;
    const auto sealed = Envelope::seal(owner, identity);
    ASSERT_TRUE(Envelope::verify(sealed.bytes, sealed.anchor));
    EXPECT_EQ(sealed.anchor.terminal, 3U);
    Restricted shorter;
    ASSERT_TRUE(shorter.apply(op(Restricted::Kind::Open, {1})).committed);
    ASSERT_TRUE(shorter.apply(op(Restricted::Kind::Submit, {1, 1, 1, 1, 0, 0, 100, 2})).committed);
    const auto prefix = Envelope::seal(shorter, identity);
    EXPECT_TRUE(Envelope::verify(prefix.bytes, prefix.anchor));
    EXPECT_FALSE(Envelope::verify(prefix.bytes, sealed.anchor));
    Restricted replacement;
    ASSERT_TRUE(replacement.apply(op(Restricted::Kind::Open, {2})).committed);
    const auto substitute = Envelope::seal(replacement, identity);
    EXPECT_TRUE(Envelope::verify(substitute.bytes, substitute.anchor));
    EXPECT_FALSE(Envelope::verify(substitute.bytes, sealed.anchor));
    auto altered = sealed;
    altered.anchor.identity[0] ^= 1;
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[32] ^= 1; // incompatible configuration even with recomputed digest
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[103] = 2; // wrong allocator scope
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[64] ^= 1; // altered initial state identity
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes.back() ^= 1; // changed state bytes, with internally consistent digest
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[319] = 2; // first canonical state schema version
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[375] ^= 1; // first canonical state exchange command counter
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[135] ^= 1; // terminal offset
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[207] ^= 1; // first record outcome
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[191] = 2; // first record sequence
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[199] = 255; // first record kind
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[176] = 255; // impossible canonical state length
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[15] = 2; // unsupported envelope schema
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    const auto read_word = [](const std::vector<std::uint8_t> &bytes, std::size_t pos) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
            value = (value << 8) | bytes[pos + i];
        return static_cast<std::size_t>(value);
    };
    const auto first_end =
        Envelope::header_size + 16 + read_word(altered.bytes, 168) + read_word(altered.bytes, 176);
    const auto second_end = first_end + 16 + read_word(altered.bytes, first_end) +
                            read_word(altered.bytes, first_end + 8);
    std::rotate(altered.bytes.begin() + static_cast<std::ptrdiff_t>(Envelope::header_size),
                altered.bytes.begin() + static_cast<std::ptrdiff_t>(first_end),
                altered.bytes.begin() + static_cast<std::ptrdiff_t>(second_end));
    resign(altered);
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes[136] ^= 1;
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.bytes.pop_back();
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    altered = sealed;
    altered.anchor.terminal = 2;
    EXPECT_FALSE(Envelope::verify(altered.bytes, altered.anchor));
    EXPECT_THROW(static_cast<void>(Envelope::seal(owner, {})), std::invalid_argument);
}

TEST(RestrictedReplay, CanonicalStateHasExplicitVersionAndBounds) {
    auto first = exchange::Exchange::isolated(16, 128, 128, 128, 0, 0);
    auto second = exchange::Exchange::isolated(16, 128, 128, 128, 0, 0);
    const auto initial = first->canonical_isolated_state();
    EXPECT_EQ(initial, second->canonical_isolated_state());
    ASSERT_GE(initial.size(), 16U);
    const std::array<std::uint8_t, 8> magic{'C', 'H', 'R', 'C', 'S', 'T', '0', '1'};
    EXPECT_TRUE(std::equal(magic.begin(), magic.end(), initial.begin()));
    EXPECT_EQ(initial[15], 1U);
    std::array<std::uint8_t, 120> expected{};
    std::copy(magic.begin(), magic.end(), expected.begin());
    expected[15] = 1;
    expected[23] = 16;
    expected[31] = 128;
    expected[39] = 128;
    expected[47] = 128;
    EXPECT_TRUE(std::equal(initial.begin(), initial.end(), expected.begin(), expected.end()));
    EXPECT_THROW(static_cast<void>(first->canonical_isolated_state(8)), std::length_error);
    EXPECT_THROW(static_cast<void>(first->canonical_isolated_state(65537)), std::length_error);
    ASSERT_TRUE(first->open_session(1));
    EXPECT_NE(first->canonical_isolated_state(), initial);
    ASSERT_TRUE(first->subscribe(1, core::SymbolId{1}));
    EXPECT_THROW(static_cast<void>(first->canonical_isolated_state()), std::logic_error);
    auto exhausted = exchange::Exchange::isolated(16, 128, 128, 128, 0,
                                                  std::numeric_limits<std::uint64_t>::max());
    EXPECT_NE(exhausted->canonical_isolated_state(), initial);
    ASSERT_TRUE(exhausted->open_session(1));
    const auto before = exhausted->canonical_isolated_state();
    EXPECT_EQ(exhausted->submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                                100, 1),
              exchange::Result::CounterExhausted);
    EXPECT_EQ(exhausted->canonical_isolated_state(), before);
}

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
    exchange::CanonicalReplayEnvelope::Identity identity{};
    identity[0] = 9;
    const auto sealed = exchange::CanonicalReplayEnvelope::seal(owner, identity);
    EXPECT_TRUE(exchange::CanonicalReplayEnvelope::verify(sealed.bytes, sealed.anchor));
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
