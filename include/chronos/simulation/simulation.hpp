#pragma once

#include "chronos/exchange/exchange.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace chronos::simulation {
enum class Scenario : std::uint8_t { Balanced, HighVolume, Volatile, LiquiditySweep, Mixed };
enum class Lifecycle : std::uint8_t { Idle, Running, Completed, Stopped, Failed };
enum class CommandKind : std::uint8_t { Submit, Cancel };

struct ScenarioConfig {
    Scenario scenario = Scenario::Balanced;
    std::uint64_t seed = 42;
    std::size_t participants = 4;
    std::vector<std::uint32_t> symbols{1};
    std::size_t max_commands = 1000;
    std::size_t max_order_history = 1024, max_requests_per_session = 1024;
    core::Price reference_price = 100;
    core::Price price_variation = 5;
    core::Quantity min_quantity = 1, max_quantity = 10;
    std::uint32_t buy_weight = 50, cancel_percent = 20;
    exchange::RiskLimits risk{};
};

struct SimulatedParticipant {
    std::uint64_t session;
    std::uint64_t next_request = 1;
    std::uint64_t next_order = 1;
};
struct SimCommand {
    std::uint64_t step = 0, session = 0, request = 0, client_order = 0;
    CommandKind kind = CommandKind::Submit;
    std::uint32_t symbol = 0;
    core::Side side = core::Side::Buy;
    core::OrderType type = core::OrderType::Limit;
    core::Price price = 0;
    core::Quantity quantity = 0;
    bool operator==(const SimCommand &) const = default;
};
struct CancellationTarget {
    std::uint64_t session, client;
};
class CommandGenerator {
  public:
    explicit CommandGenerator(ScenarioConfig config);
    [[nodiscard]] SimCommand next(std::span<const CancellationTarget> active = {});

  private:
    ScenarioConfig config_;
    std::vector<SimulatedParticipant> participants_;
    std::uint64_t rng_;
    core::Price reference_;
    std::size_t steps_ = 0;
};
struct Outcome {
    SimCommand command;
    exchange::Result result;
    bool operator==(const Outcome &) const = default;
};
struct BookSummary {
    std::uint32_t symbol;
    core::Price bid, ask;
    core::Quantity bid_quantity, ask_quantity;
};
struct TradeSummary {
    std::uint64_t command_sequence, buyer, seller;
    std::uint32_t symbol;
    core::Price price;
    core::Quantity quantity;
    bool operator==(const TradeSummary &) const = default;
};
struct PositionSummary {
    std::uint64_t session;
    std::uint32_t symbol;
    std::int64_t position;
};
struct SimulationStatus {
    Lifecycle state = Lifecycle::Idle;
    std::size_t steps = 0;
    std::string error;
};
struct SimulationResult {
    ScenarioConfig config;
    SimulationStatus status;
    std::uint64_t generated = 0, admitted = 0, accepted_orders = 0, rejected_orders = 0,
                  cancelled = 0, rejected_cancels = 0, trades = 0, aggregated_trades = 0,
                  executed_quantity = 0;
    bool summary_complete = false;
    std::uint64_t digest = 0;
    std::vector<Outcome> outcomes;
    std::vector<BookSummary> books;
    std::vector<TradeSummary> trade_history;
    std::vector<PositionSummary> positions;
};
class SimulationObserver {
  public:
    virtual ~SimulationObserver() = default;
    virtual bool begin(const ScenarioConfig &config) = 0;
    virtual bool command(const SimCommand &command) = 0;
    // Called after the exchange result but before private reports are drained.
    virtual bool outcome(const SimCommand &command, exchange::Result result,
                         const exchange::Exchange &exchange) = 0;
    virtual bool finish(const SimulationResult &result, Lifecycle state,
                        const exchange::Exchange &exchange) = 0;
};

class SimulationRunner {
  public:
    static constexpr std::uint32_t generator_version = 2;
    explicit SimulationRunner(ScenarioConfig config, SimulationObserver *observer = nullptr,
                              bool isolated_exchange = false);
    ~SimulationRunner();
    SimulationRunner(const SimulationRunner &) = delete;
    SimulationRunner &operator=(const SimulationRunner &) = delete;
    // All operations are synchronous and must be called by one owner thread.
    void start();
    void run_batch(std::size_t maximum);
    void request_stop();
    [[nodiscard]] const SimulationResult &result() const noexcept { return result_; }
    // Borrowed pointer, invalidated by runner destruction. Only available for
    // a completed isolated run. The caller must serialize access and cannot
    // attach it to a gateway through this API.
    [[nodiscard]] exchange::Exchange *completed_isolated_exchange() noexcept;
    [[nodiscard]] static std::string validate(const ScenarioConfig &config);

  private:
    struct Impl;
    void finish(Lifecycle state);
    std::unique_ptr<Impl> impl_;
    SimulationResult result_;
};
[[nodiscard]] const char *scenario_name(Scenario scenario) noexcept;
} // namespace chronos::simulation
