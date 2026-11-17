#include "chronos/exchange/offline_owner.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace chronos;
using exchange::OwnerAccess;

static_assert(!std::is_copy_constructible_v<exchange::OfflineOwner>);
static_assert(!std::is_move_constructible_v<exchange::OfflineOwner>);

TEST(OfflineOwner, FreezeBlocksEveryExposedMutatorAndPreservesState) {
    exchange::OfflineOwner owner;
    ASSERT_EQ(owner.open_session(1).value, true);
    ASSERT_EQ(owner.subscribe(1, core::SymbolId{1}).value, true);
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1)
            .value,
        exchange::Result::Accepted);
    const auto before = owner.exact_state(4096).value;
    const auto event = owner.market_front(1).value;
    ASSERT_TRUE(event);
    ASSERT_TRUE(*event);
    EXPECT_EQ(owner.with_frozen_state([&] {
        EXPECT_EQ(owner.open_session(2).access, OwnerAccess::Frozen);
        EXPECT_EQ(owner.configure_risk(1, {}).access, OwnerAccess::Frozen);
        EXPECT_EQ(owner.set_killed(1, true).access, OwnerAccess::Frozen);
        EXPECT_EQ(owner.subscribe(1, core::SymbolId{2}).access, OwnerAccess::Frozen);
        EXPECT_EQ(owner.unsubscribe(1, core::SymbolId{1}).access, OwnerAccess::Frozen);
        EXPECT_EQ(owner.market_pop(1, **event), OwnerAccess::Frozen);
        EXPECT_EQ(owner.close_session(1), OwnerAccess::Frozen);
        EXPECT_EQ(
            owner
                .submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1)
                .access,
            OwnerAccess::Frozen);
        EXPECT_EQ(owner.cancel(1, 2, 1).access, OwnerAccess::Frozen);
        EXPECT_EQ(owner.pop(1), OwnerAccess::Frozen);
        EXPECT_EQ(owner.with_frozen_state([] {}), OwnerAccess::Frozen);
        OwnerAccess concurrent = OwnerAccess::Allowed;
        std::thread other([&] { concurrent = owner.open_session(3).access; });
        other.join();
        EXPECT_EQ(concurrent, OwnerAccess::WrongContext);
        EXPECT_EQ(owner.exact_state(4096).value, before);
    }),
              OwnerAccess::Allowed);
    EXPECT_EQ(owner.exact_state(4096).value, before);
    EXPECT_EQ(owner.cancel(1, 2, 1).value, exchange::Result::Accepted);
}

TEST(OfflineOwner, WrongContextCannotReadMutateOrEnterScope) {
    exchange::OfflineOwner owner;
    ASSERT_EQ(owner.open_session(1).value, true);
    const auto empty_report = owner.front(1);
    ASSERT_TRUE(empty_report.value);
    EXPECT_FALSE(*empty_report.value);
    const auto before = owner.exact_state(4096).value;
    OwnerAccess submit = OwnerAccess::Allowed, kill = OwnerAccess::Allowed;
    OwnerAccess read = OwnerAccess::Allowed, scope = OwnerAccess::Allowed;
    bool ran = false, denied_has_value = true;
    std::thread other([&] {
        const auto denied = owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                         core::OrderType::Limit, 100, 1);
        submit = denied.access;
        denied_has_value = denied.value.has_value();
        kill = owner.set_killed(1, true).access;
        read = owner.exact_state(4096).access;
        scope = owner.with_frozen_state([&] { ran = true; });
    });
    other.join();
    EXPECT_EQ(submit, OwnerAccess::WrongContext);
    EXPECT_FALSE(denied_has_value);
    EXPECT_EQ(kill, OwnerAccess::WrongContext);
    EXPECT_EQ(read, OwnerAccess::WrongContext);
    EXPECT_EQ(scope, OwnerAccess::WrongContext);
    EXPECT_FALSE(ran);
    EXPECT_EQ(owner.exact_state(4096).value, before);
}

TEST(OfflineOwner, ExceptionUnfreezesAndRejectionKeepsRequestReusable) {
    exchange::OfflineOwner owner;
    ASSERT_EQ(owner.open_session(1).value, true);
    EXPECT_THROW(
        static_cast<void>(owner.with_frozen_state([] { throw std::runtime_error("test"); })),
        std::runtime_error);
    const auto before = owner.exact_state(4096).value;
    const auto rejected =
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 0);
    EXPECT_EQ(rejected.access, OwnerAccess::Allowed);
    EXPECT_EQ(rejected.value, exchange::Result::RiskQuantity);
    EXPECT_EQ(owner.exact_state(4096).value, before);
    const auto accepted =
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1);
    EXPECT_EQ(accepted.access, OwnerAccess::Allowed);
    EXPECT_EQ(accepted.value, exchange::Result::Accepted);
    const auto report = owner.front(1).value;
    ASSERT_TRUE(report);
    ASSERT_TRUE(*report);
    EXPECT_EQ((*report)->internal_order_id, 1U);
}

TEST(OfflineOwner, FinishedOwnerContextCannotBeReplacedByAnotherThread) {
    std::unique_ptr<exchange::OfflineOwner> owner;
    std::thread creator([&] { owner = std::make_unique<exchange::OfflineOwner>(); });
    creator.join();
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->open_session(1).access, OwnerAccess::WrongContext);
    for (int i = 0; i < 16; ++i) {
        OwnerAccess result = OwnerAccess::Allowed;
        std::thread other([&] { result = owner->open_session(1).access; });
        other.join();
        EXPECT_EQ(result, OwnerAccess::WrongContext);
    }
}
