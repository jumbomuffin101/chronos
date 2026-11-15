#include "chronos/simulation/simulation.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

using namespace chronos::simulation;

namespace {
SimulationResult run(ScenarioConfig c) {
    SimulationRunner runner(std::move(c));
    runner.start();
    runner.run_batch(2000);
    return runner.result();
}
} // namespace
TEST(Simulation, RejectsInvalidConfiguration) {
    ScenarioConfig c;
    c.participants = 1;
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    c = {};
    c.symbols.clear();
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    c = {};
    c.symbols = {1, 1};
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    c = {};
    c.price_variation = c.reference_price;
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    c = {};
    c.min_quantity = 0;
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    c = {};
    c.max_commands = 1025;
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    c = {};
    c.scenario = static_cast<Scenario>(99);
    EXPECT_FALSE(SimulationRunner::validate(c).empty());
    SimulationRunner invalid(c);
    invalid.start();
    EXPECT_EQ(invalid.result().status.state, Lifecycle::Failed);
}
TEST(Simulation, IdenticalSeedAndInitialStateHaveIdenticalNormalizedResults) {
    ScenarioConfig c;
    c.max_commands = 50;
    c.symbols = {1, 2};
    c.scenario = Scenario::Mixed;
    const auto a = run(c);
    const auto b = run(c);
    ASSERT_EQ(a.status.state, Lifecycle::Completed);
    ASSERT_EQ(b.status.state, Lifecycle::Completed);
    EXPECT_TRUE(a.summary_complete);
    EXPECT_EQ(a.outcomes, b.outcomes);
    EXPECT_EQ(a.trade_history, b.trade_history);
    EXPECT_EQ(a.digest, b.digest);
    ++c.seed;
    const auto different = run(c);
    EXPECT_NE(a.outcomes, different.outcomes);
}
TEST(Simulation, LiquiditySweepMatchesAtRestingPricesAcrossLevels) {
    ScenarioConfig c;
    c.scenario = Scenario::LiquiditySweep;
    c.max_commands = 5;
    c.min_quantity = 1;
    c.max_quantity = 3;
    const auto r = run(c);
    ASSERT_EQ(r.status.state, Lifecycle::Completed);
    ASSERT_EQ(r.trade_history.size(), 3);
    EXPECT_EQ(r.trades, 3);
    EXPECT_EQ(r.executed_quantity, 3);
    for (std::size_t i = 0; i < r.trade_history.size(); ++i) {
        EXPECT_EQ(r.trade_history[i].price, 100 + static_cast<chronos::core::Price>(i));
        EXPECT_EQ(r.trade_history[i].seller, 1);
        EXPECT_EQ(r.trade_history[i].buyer, 2);
    }
    ASSERT_EQ(r.books.size(), 1);
    EXPECT_EQ(r.books[0].ask, 103);
    EXPECT_EQ(r.books[0].ask_quantity, 1);
    ASSERT_EQ(r.positions.size(), 4);
    EXPECT_EQ(r.positions[0].position, -3);
    EXPECT_EQ(r.positions[1].position, 3);
}
TEST(Simulation, SweepCanPartiallyFillARestingLevel) {
    ScenarioConfig c;
    c.scenario = Scenario::LiquiditySweep;
    c.max_commands = 5;
    c.min_quantity = 2;
    c.max_quantity = 3;
    const auto r = run(c);
    ASSERT_EQ(r.status.state, Lifecycle::Completed);
    ASSERT_EQ(r.trade_history.size(), 2);
    EXPECT_EQ(r.trade_history[0].price, 100);
    EXPECT_EQ(r.trade_history[0].quantity, 2);
    EXPECT_EQ(r.trade_history[1].price, 101);
    EXPECT_EQ(r.trade_history[1].quantity, 1);
    ASSERT_EQ(r.books.size(), 1);
    EXPECT_EQ(r.books[0].ask, 101);
    EXPECT_EQ(r.books[0].ask_quantity, 1);
}
TEST(Simulation, CancellationsUseOwnerAndRiskRejectionsDoNotMutateBook) {
    ScenarioConfig c;
    c.scenario = Scenario::HighVolume;
    c.max_commands = 100;
    c.cancel_percent = 100;
    const auto a = run(c);
    ASSERT_EQ(a.status.state, Lifecycle::Completed);
    EXPECT_GT(a.cancelled, 0);
    EXPECT_EQ(a.rejected_cancels, 0);
    c = {};
    c.max_commands = 20;
    c.risk.max_order_quantity = 0;
    const auto b = run(c);
    ASSERT_EQ(b.status.state, Lifecycle::Completed);
    EXPECT_EQ(b.accepted_orders, 0);
    EXPECT_EQ(b.rejected_orders, 20);
    EXPECT_EQ(b.trades, 0);
    ASSERT_EQ(b.books.size(), 1);
    EXPECT_EQ(b.books[0].bid_quantity, 0);
    EXPECT_EQ(b.books[0].ask_quantity, 0);
}
TEST(Simulation, BatchesStopAndTerminalStatesAreStable) {
    ScenarioConfig c;
    c.max_commands = 20;
    SimulationRunner runner(c);
    runner.run_batch(3);
    EXPECT_EQ(runner.result().generated, 0);
    runner.start();
    runner.run_batch(7);
    EXPECT_EQ(runner.result().status.steps, 7);
    runner.request_stop();
    EXPECT_EQ(runner.result().status.state, Lifecycle::Stopped);
    EXPECT_TRUE(runner.result().summary_complete);
    EXPECT_EQ(runner.result().books.size(), 1);
    runner.run_batch(20);
    runner.start();
    EXPECT_EQ(runner.result().generated, 7);
}
TEST(Simulation, BatchBoundariesDoNotChangeResults) {
    ScenarioConfig c;
    c.scenario = Scenario::Mixed;
    c.max_commands = 40;
    const auto expected = run(c);
    SimulationRunner runner(c);
    runner.start();
    for (int i = 0; i < 10; ++i)
        runner.run_batch(4);
    EXPECT_EQ(runner.result().status.state, Lifecycle::Completed);
    EXPECT_EQ(runner.result().outcomes, expected.outcomes);
    EXPECT_EQ(runner.result().digest, expected.digest);
}
TEST(Simulation, ExchangeCapacityRejectsWithoutCountingAccepted) {
    ScenarioConfig c;
    c.max_commands = 8;
    c.max_order_history = 3;
    c.buy_weight = 100;
    c.price_variation = 0;
    const auto r = run(c);
    ASSERT_EQ(r.status.state, Lifecycle::Completed);
    EXPECT_EQ(r.accepted_orders, 3);
    EXPECT_EQ(r.rejected_orders, 5);
    for (std::size_t i = 3; i < r.outcomes.size(); ++i)
        EXPECT_EQ(r.outcomes[i].result, chronos::exchange::Result::Capacity);
}
TEST(Simulation, ExecutedQuantityOverflowFailsInsteadOfCompleting) {
    ScenarioConfig c;
    c.scenario = Scenario::LiquiditySweep;
    c.max_commands = 15;
    c.symbols = {1, 2, 3};
    c.reference_price = 1;
    c.price_variation = 0;
    c.min_quantity = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    c.max_quantity = c.min_quantity;
    c.risk.max_market_price = 1;
    const auto r = run(c);
    EXPECT_EQ(r.status.state, Lifecycle::Failed);
    EXPECT_NE(r.status.error.find("overflow"), std::string::npos);
    EXPECT_EQ(r.generated, 15);
    EXPECT_EQ(r.status.steps, 14);
    EXPECT_EQ(r.trades, 3);
    EXPECT_EQ(r.aggregated_trades, 2);
    EXPECT_EQ(r.trade_history.size(), 2);
    EXPECT_FALSE(r.summary_complete);
}
TEST(Simulation, BoundedSamplingRejectsBiasedLowValues) {
    ScenarioConfig c;
    c.seed = 0;
    c.max_commands = 1;
    c.price_variation = 0;
    c.min_quantity = 1;
    c.max_quantity = 4611686018427387905ULL;
    CommandGenerator generator(c);
    EXPECT_EQ(generator.next().quantity, 1426408582835774186ULL);
}
TEST(Simulation, AllScenariosRemainBoundedAndVolatileMoves) {
    ScenarioConfig c;
    c.max_commands = 50;
    for (auto scenario : {Scenario::Balanced, Scenario::HighVolume, Scenario::Volatile,
                          Scenario::LiquiditySweep, Scenario::Mixed}) {
        c.scenario = scenario;
        const auto r = run(c);
        EXPECT_EQ(r.status.state, Lifecycle::Completed);
        EXPECT_EQ(r.generated, 50);
        EXPECT_EQ(r.admitted, 50);
        EXPECT_LE(r.outcomes.size(), 50);
        EXPECT_LE(r.trade_history.size(), 50);
        if (scenario == Scenario::Volatile) {
            bool moved = false;
            for (const auto &o : r.outcomes)
                if (o.command.price != c.reference_price)
                    moved = true;
            EXPECT_TRUE(moved);
        }
    }
}
TEST(Simulation, MixedParticipantsProduceCrossAccountTrades) {
    ScenarioConfig c;
    c.scenario = Scenario::Mixed;
    c.max_commands = 100;
    const auto r = run(c);
    ASSERT_EQ(r.status.state, Lifecycle::Completed);
    bool cross = false;
    for (const auto &trade : r.trade_history)
        if (trade.buyer != trade.seller)
            cross = true;
    EXPECT_TRUE(cross);
    EXPECT_GT(r.cancelled, 0);
}
TEST(Simulation, SweepVersionTwoGoldenDigestAndCommands) {
    ScenarioConfig c;
    c.scenario = Scenario::LiquiditySweep;
    c.max_commands = 10;
    const auto r = run(c);
    ASSERT_EQ(r.status.state, Lifecycle::Completed);
    EXPECT_EQ(r.digest, 0x239e36527e260294ULL);
    ASSERT_EQ(r.outcomes.size(), 10);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(r.outcomes[i].command.session, 1);
        EXPECT_EQ(r.outcomes[i].command.price, 100 + static_cast<chronos::core::Price>(i));
        EXPECT_EQ(r.outcomes[i].command.quantity, 1);
    }
    EXPECT_EQ(r.outcomes[4].command.session, 2);
    EXPECT_EQ(r.outcomes[4].command.type, chronos::core::OrderType::Market);
    EXPECT_EQ(r.outcomes[4].command.quantity, 10);
    CommandGenerator generator(c);
    for (const auto &outcome : r.outcomes)
        EXPECT_EQ(generator.next(), outcome.command);
    EXPECT_THROW((void)generator.next(), std::out_of_range);
}
