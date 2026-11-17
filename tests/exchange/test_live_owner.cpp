#include "chronos/exchange/live_owner.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace chronos;

template <class T>
concept ExposesExchange = requires(T &owner) { owner.exchange(); };
static_assert(!ExposesExchange<exchange::LiveExchangeOwner>);
static_assert(!std::is_copy_constructible_v<exchange::LiveExchangeOwner>);
static_assert(!std::is_move_constructible_v<exchange::LiveExchangeOwner>);

TEST(LiveOwner, BindingLifecycleAndForeignContextRejection) {
    exchange::LiveExchangeOwner owner;
    EXPECT_THROW(static_cast<void>(owner.open_session(1)), std::logic_error);
    owner.bind();
    EXPECT_THROW(owner.bind(), std::logic_error);
    ASSERT_TRUE(owner.open_session(1));
    bool foreign_rejected = false, foreign_finish = true, foreign_read_rejected = false;
    std::thread other([&] {
        try {
            static_cast<void>(owner.open_session(2));
        } catch (const std::logic_error &) {
            foreign_rejected = true;
        }
        try {
            static_cast<void>(owner.front(1));
        } catch (const std::logic_error &) {
            foreign_read_rejected = true;
        }
        foreign_finish = owner.finish();
    });
    other.join();
    EXPECT_TRUE(foreign_rejected);
    EXPECT_TRUE(foreign_read_rejected);
    EXPECT_FALSE(foreign_finish);
    EXPECT_TRUE(owner.open_session(2));
    EXPECT_TRUE(owner.finish());
    EXPECT_FALSE(owner.finish());
    EXPECT_THROW(static_cast<void>(owner.open_session(3)), std::logic_error);
    EXPECT_THROW(owner.bind(), std::logic_error);
    EXPECT_FALSE(owner.front(1));
}

TEST(LiveOwner, FrozenScopeBlocksAllMutationPathsAndDoesNotAllocateAnId) {
    exchange::LiveExchangeOwner owner;
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_TRUE(owner.subscribe(1, core::SymbolId{1}));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 2),
        exchange::Result::Accepted);
    const auto first = owner.front(1);
    const auto event = owner.market_front(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(event);
    owner.with_frozen_state([&] {
        EXPECT_THROW(static_cast<void>(owner.open_session(2)), std::logic_error);
        EXPECT_THROW(static_cast<void>(owner.configure_risk(1, {})), std::logic_error);
        EXPECT_THROW(static_cast<void>(owner.set_killed(1, true)), std::logic_error);
        EXPECT_THROW(static_cast<void>(owner.subscribe(1, core::SymbolId{2})), std::logic_error);
        EXPECT_THROW(static_cast<void>(owner.unsubscribe(1, core::SymbolId{1})), std::logic_error);
        EXPECT_THROW(owner.market_pop(1, *event), std::logic_error);
        EXPECT_THROW(owner.close_session(1), std::logic_error);
        EXPECT_THROW(static_cast<void>(owner.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy,
                                                    core::OrderType::Limit, 99, 1)),
                     std::logic_error);
        EXPECT_THROW(static_cast<void>(owner.cancel(1, 2, 1)), std::logic_error);
        EXPECT_THROW(owner.pop(1), std::logic_error);
        EXPECT_THROW(owner.with_frozen_state([] {}), std::logic_error);
        EXPECT_FALSE(owner.finish());
        bool foreign_rejected = false;
        std::thread other([&] {
            try {
                static_cast<void>(owner.open_session(3));
            } catch (const std::logic_error &) {
                foreign_rejected = true;
            }
        });
        other.join();
        EXPECT_TRUE(foreign_rejected);
        ASSERT_TRUE(owner.front(1));
        EXPECT_EQ(owner.front(1)->internal_order_id, first->internal_order_id);
    });
    ASSERT_EQ(
        owner.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 99, 1),
        exchange::Result::Accepted);
    owner.pop(1);
    owner.pop(1);
    ASSERT_TRUE(owner.front(1));
    EXPECT_EQ(owner.front(1)->internal_order_id, first->internal_order_id + 1);
    EXPECT_TRUE(owner.finish());
}

TEST(LiveOwner, ExceptionReleasesFreezeAndRejectionIsAtomic) {
    exchange::LiveExchangeOwner owner;
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    EXPECT_THROW(owner.with_frozen_state([] { throw std::runtime_error("test"); }),
                 std::runtime_error);
    EXPECT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 0),
        exchange::Result::RiskQuantity);
    EXPECT_FALSE(owner.front(1));
    EXPECT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    EXPECT_EQ(owner.cancel(1, 2, 1), exchange::Result::Accepted);
    EXPECT_TRUE(owner.finish());
}

TEST(LiveOwner, FinishedContextCannotBeReboundAfterThreadExit) {
    auto owner = std::make_unique<exchange::LiveExchangeOwner>();
    std::thread worker([&] {
        owner->bind();
        EXPECT_TRUE(owner->open_session(1));
        EXPECT_TRUE(owner->finish());
    });
    worker.join();
    EXPECT_THROW(owner->bind(), std::logic_error);
    EXPECT_THROW(static_cast<void>(owner->open_session(2)), std::logic_error);
    EXPECT_FALSE(owner->front(1));
}

TEST(LiveOwner, AbandonedActiveOwnerCannotBeClaimedByAnotherThread) {
    auto owner = std::make_unique<exchange::LiveExchangeOwner>();
    std::thread worker([&] {
        owner->bind();
        EXPECT_TRUE(owner->open_session(1));
    });
    worker.join();
    EXPECT_THROW(owner->bind(), std::logic_error);
    EXPECT_THROW(static_cast<void>(owner->open_session(2)), std::logic_error);
    EXPECT_THROW(static_cast<void>(owner->front(1)), std::logic_error);
    EXPECT_FALSE(owner->finish());
}
