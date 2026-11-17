#include "chronos/simulation/simulation.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronos::simulation {
namespace {
constexpr std::uint64_t offset = 14695981039346656037ULL, prime = 1099511628211ULL;
void hash(std::uint64_t &h, std::uint64_t x) noexcept {
    for (unsigned i = 0; i < 8; ++i) {
        h ^= (x >> (i * 8U)) & 255U;
        h *= prime;
    }
}
std::uint64_t random(std::uint64_t &state) noexcept {
    auto z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}
std::uint64_t uniform(std::uint64_t &state, std::uint64_t bound) noexcept {
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t value;
    do {
        value = random(state);
    } while (value < threshold);
    return value % bound;
}
void fail(SimulationStatus &status, const char *message) noexcept {
    status.state = Lifecycle::Failed;
    try {
        status.error = message;
    } catch (...) {
        status.error.clear();
    }
}
} // namespace

struct SimulationRunner::Impl {
    std::unique_ptr<exchange::Exchange> exchange;
    std::vector<SimulatedParticipant> participants;
    std::vector<CancellationTarget> active;
    std::unique_ptr<CommandGenerator> generator;
    std::vector<std::pair<std::uint64_t, exchange::Report>> reports;
    std::uint64_t h = offset;
    std::size_t seen_trades = 0;
    SimulationObserver *observer = nullptr;
    bool isolated_exchange = false;
};

const char *scenario_name(Scenario s) noexcept {
    switch (s) {
    case Scenario::Balanced:
        return "balanced";
    case Scenario::HighVolume:
        return "high-volume";
    case Scenario::Volatile:
        return "volatile";
    case Scenario::LiquiditySweep:
        return "liquidity-sweep";
    case Scenario::Mixed:
        return "mixed";
    }
    return "unsupported";
}

std::string SimulationRunner::validate(const ScenarioConfig &c) {
    if (c.scenario != Scenario::Balanced && c.scenario != Scenario::HighVolume &&
        c.scenario != Scenario::Volatile && c.scenario != Scenario::LiquiditySweep &&
        c.scenario != Scenario::Mixed)
        return "unsupported scenario";
    if (c.participants < 2 || c.participants > 16)
        return "participants must be between 2 and 16";
    if (c.symbols.empty() || c.symbols.size() > 16)
        return "symbols must contain 1 to 16 entries";
    auto syms = c.symbols;
    std::sort(syms.begin(), syms.end());
    if (std::adjacent_find(syms.begin(), syms.end()) != syms.end())
        return "duplicate symbol";
    if (c.max_commands == 0 || c.max_commands > 1024)
        return "commands must be between 1 and 1024";
    if (c.max_order_history == 0 || c.max_order_history > 1024 || c.max_requests_per_session == 0 ||
        c.max_requests_per_session > 1024)
        return "exchange capacity must be between 1 and 1024";
    if (c.reference_price <= 0 || c.reference_price > 1'000'000'000 || c.price_variation < 0 ||
        c.price_variation >= c.reference_price || c.price_variation > 1'000'000'000)
        return "invalid price range";
    if (!c.min_quantity || c.min_quantity > c.max_quantity ||
        c.max_quantity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return "invalid quantity range";
    if (c.buy_weight > 100 || c.cancel_percent > 100)
        return "weights must be 0 to 100";
    if (c.risk.max_market_price <= 0 || c.risk.allowed_symbols.size() > 1024 ||
        c.risk.max_abs_position >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return "invalid risk configuration";
    return {};
}

SimulationRunner::SimulationRunner(ScenarioConfig c, SimulationObserver *observer,
                                   bool isolated_exchange)
    : impl_(std::make_unique<Impl>()) {
    impl_->observer = observer;
    impl_->isolated_exchange = isolated_exchange;
    result_.config = std::move(c);
}
SimulationRunner::~SimulationRunner() = default;

CommandGenerator::CommandGenerator(ScenarioConfig c)
    : config_(std::move(c)), rng_(config_.seed), reference_(config_.reference_price) {
    const auto error = SimulationRunner::validate(config_);
    if (!error.empty())
        throw std::invalid_argument(error);
    for (std::size_t i = 0; i < config_.participants; ++i)
        participants_.push_back({static_cast<std::uint64_t>(i + 1)});
}
SimCommand CommandGenerator::next(std::span<const CancellationTarget> active) {
    const auto &c = config_;
    if (steps_ >= c.max_commands)
        throw std::out_of_range("generator exhausted");
    for (const auto &target : active)
        if (target.session == 0 || target.session > c.participants)
            throw std::invalid_argument("invalid cancellation owner");
    const auto step = steps_++;
    auto participant = static_cast<std::size_t>(uniform(rng_, c.participants));
    SimCommand cmd;
    cmd.step = static_cast<std::uint64_t>(step + 1);
    const bool sweep = c.scenario == Scenario::LiquiditySweep;
    const auto phase = step % 5;
    const bool cancel = !sweep && !active.empty() && uniform(rng_, 100) < c.cancel_percent &&
                        (c.scenario == Scenario::HighVolume || c.scenario == Scenario::Mixed);
    if (sweep)
        participant = phase == 4 ? 1 : 0;
    else if (c.scenario == Scenario::Mixed)
        participant = step % c.participants;
    if (cancel) {
        const auto target = active[static_cast<std::size_t>(uniform(rng_, active.size()))];
        participant = static_cast<std::size_t>(target.session - 1);
        cmd.kind = CommandKind::Cancel;
        cmd.client_order = target.client;
    }
    auto &owner = participants_[participant];
    cmd.session = owner.session;
    cmd.request = owner.next_request++;
    if (!cancel) {
        cmd.client_order = owner.next_order++;
        const auto symbol_index = sweep ? (step / 5) % c.symbols.size()
                                        : static_cast<std::size_t>(uniform(rng_, c.symbols.size()));
        cmd.symbol = c.symbols[symbol_index];
        cmd.side = sweep ? (phase == 4 ? core::Side::Buy : core::Side::Sell)
                         : (uniform(rng_, 100) < c.buy_weight ? core::Side::Buy : core::Side::Sell);
        cmd.type = sweep && phase == 4 ? core::OrderType::Market : core::OrderType::Limit;
        const auto span = static_cast<std::uint64_t>(c.price_variation) * 2 + 1;
        const auto delta = static_cast<core::Price>(uniform(rng_, span)) - c.price_variation;
        if (c.scenario == Scenario::Volatile && c.price_variation > 0) {
            const auto movement = uniform(rng_, 2) == 0 ? -1 : 1;
            reference_ = std::clamp(reference_ + movement, c.reference_price - c.price_variation,
                                    c.reference_price + c.price_variation);
        }
        cmd.price = cmd.type == core::OrderType::Market ? 0
                    : sweep                             ? c.reference_price +
                                  static_cast<core::Price>(
                                      phase % (static_cast<std::size_t>(c.price_variation) + 1))
                    : c.scenario == Scenario::Volatile ? reference_
                                                       : c.reference_price + delta;
        cmd.quantity = sweep ? (phase == 4 ? c.max_quantity : c.min_quantity)
                             : c.min_quantity + uniform(rng_, c.max_quantity - c.min_quantity + 1);
    }
    return cmd;
}

void SimulationRunner::start() {
    if (result_.status.state != Lifecycle::Idle)
        return;
    try {
        const auto error = validate(result_.config);
        if (!error.empty()) {
            result_.status = {Lifecycle::Failed, 0, error};
            return;
        }
        const auto &c = result_.config;
        result_.outcomes.reserve(c.max_commands);
        result_.books.reserve(c.symbols.size());
        result_.trade_history.reserve(c.max_commands);
        result_.positions.reserve(c.symbols.size() * c.participants);
        impl_->active.reserve(c.max_commands);
        impl_->participants.reserve(c.participants);
        impl_->reports.reserve(2 * c.max_commands + 2);
        impl_->exchange =
            impl_->isolated_exchange
                ? exchange::Exchange::isolated(c.participants, c.max_order_history,
                                               2 * c.max_commands + 2, c.max_requests_per_session)
                : std::make_unique<exchange::Exchange>(c.participants, c.max_order_history,
                                                       2 * c.max_commands + 2,
                                                       c.max_requests_per_session);
        for (std::size_t i = 0; i < c.participants; ++i) {
            const auto id = static_cast<std::uint64_t>(i + 1);
            if (!impl_->exchange->open_session(id) || !impl_->exchange->configure_risk(id, c.risk))
                throw std::runtime_error("cannot initialize participant");
            impl_->participants.push_back({id});
        }
        impl_->generator = std::make_unique<CommandGenerator>(c);
        if (impl_->observer && !impl_->observer->begin(c))
            throw std::runtime_error("log header failed before admission");
        auto &h = impl_->h;
        for (const auto v :
             {static_cast<std::uint64_t>(generator_version), static_cast<std::uint64_t>(c.scenario),
              c.seed, static_cast<std::uint64_t>(c.participants),
              static_cast<std::uint64_t>(c.max_commands),
              static_cast<std::uint64_t>(c.max_order_history),
              static_cast<std::uint64_t>(c.max_requests_per_session),
              static_cast<std::uint64_t>(c.reference_price),
              static_cast<std::uint64_t>(c.price_variation), c.min_quantity, c.max_quantity,
              static_cast<std::uint64_t>(c.buy_weight),
              static_cast<std::uint64_t>(c.cancel_percent),
              static_cast<std::uint64_t>(c.symbols.size())})
            hash(h, v);
        for (const auto symbol : c.symbols)
            hash(h, symbol);
        hash(h, c.risk.max_order_quantity);
        hash(h, c.risk.max_order_notional);
        hash(h, c.risk.max_open_notional);
        hash(h, c.risk.max_open_orders);
        hash(h, c.risk.max_abs_position);
        hash(h, static_cast<std::uint64_t>(c.risk.max_market_price));
        hash(h, c.risk.allowed_symbols.size());
        for (const auto symbol : c.risk.allowed_symbols)
            hash(h, symbol);
        result_.status.state = Lifecycle::Running;
        result_.digest = h;
    } catch (const std::exception &e) {
        fail(result_.status, e.what());
    } catch (...) {
        fail(result_.status, "non-standard exception");
    }
}

void SimulationRunner::run_batch(std::size_t maximum) {
    if (result_.status.state != Lifecycle::Running)
        return;
    try {
        const auto &c = result_.config;
        auto &p = *impl_;
        const auto count = std::min(maximum, c.max_commands - result_.status.steps);
        for (std::size_t k = 0; k < count; ++k) {
            const auto cmd = p.generator->next(p.active);
            const bool cancel = cmd.kind == CommandKind::Cancel;
            ++result_.generated;
            if (p.observer && !p.observer->command(cmd))
                throw std::runtime_error("command log failed before admission");
            ++result_.admitted;
            const auto outcome =
                cancel ? p.exchange->cancel(cmd.session, cmd.request, cmd.client_order)
                       : p.exchange->submit(cmd.session, cmd.request, cmd.client_order,
                                            core::SymbolId{cmd.symbol}, cmd.side, cmd.type,
                                            cmd.price, cmd.quantity);
            result_.trades = static_cast<std::uint64_t>(p.exchange->trade_count());
            result_.outcomes.push_back({cmd, outcome});
            if (cancel) {
                if (outcome == exchange::Result::Accepted)
                    ++result_.cancelled;
                else
                    ++result_.rejected_cancels;
            } else if (outcome == exchange::Result::Accepted) {
                ++result_.accepted_orders;
                if (cmd.type == core::OrderType::Limit)
                    p.active.push_back({cmd.session, cmd.client_order});
            } else
                ++result_.rejected_orders;
            if (p.observer && !p.observer->outcome(cmd, outcome, *p.exchange))
                throw std::runtime_error("outcome log failed after exchange result; committed "
                                         "operations were not rolled back");
            for (const auto v :
                 {cmd.step, cmd.session, cmd.request, cmd.client_order,
                  static_cast<std::uint64_t>(cmd.kind), static_cast<std::uint64_t>(cmd.symbol),
                  static_cast<std::uint64_t>(cmd.side), static_cast<std::uint64_t>(cmd.type),
                  static_cast<std::uint64_t>(cmd.price), cmd.quantity,
                  static_cast<std::uint64_t>(outcome)})
                hash(p.h, v);
            auto &reports = p.reports;
            reports.clear();
            for (const auto &owner2 : p.participants) {
                while (auto r = p.exchange->front(owner2.session)) {
                    reports.emplace_back(owner2.session, *r);
                    p.exchange->pop(owner2.session);
                    if (r->kind == exchange::ReportKind::Filled ||
                        r->kind == exchange::ReportKind::Cancelled)
                        std::erase_if(p.active, [&](const CancellationTarget &a) {
                            return a.session == owner2.session && a.client == r->client_order_id;
                        });
                }
            }
            std::sort(reports.begin(), reports.end(), [](const auto &a, const auto &b) {
                return a.second.sequence < b.second.sequence;
            });
            hash(p.h, reports.size());
            for (const auto &[session, r] : reports) {
                hash(p.h, session);
                for (const auto v :
                     {r.sequence, r.command_sequence, r.request_id, r.client_order_id, r.trade_id,
                      static_cast<std::uint64_t>(r.symbol), static_cast<std::uint64_t>(r.price),
                      r.quantity, r.remaining, static_cast<std::uint64_t>(r.kind)})
                    hash(p.h, v);
            }
            while (p.seen_trades < p.exchange->trade_count()) {
                const auto t = p.exchange->trade(p.seen_trades);
                if (!t || t->quantity >
                              std::numeric_limits<std::uint64_t>::max() - result_.executed_quantity)
                    throw std::overflow_error("executed quantity overflow");
                result_.executed_quantity += t->quantity;
                result_.trade_history.push_back({t->command_sequence, t->buyer_session,
                                                 t->seller_session, t->symbol, t->price,
                                                 t->quantity});
                ++p.seen_trades;
                ++result_.aggregated_trades;
                for (const auto v : {t->command_sequence, t->trade_id, t->buyer_session,
                                     t->seller_session, static_cast<std::uint64_t>(t->symbol),
                                     static_cast<std::uint64_t>(t->price), t->quantity})
                    hash(p.h, v);
            }
            ++result_.status.steps;
            result_.digest = p.h;
        }
        if (result_.status.steps == c.max_commands)
            finish(Lifecycle::Completed);
    } catch (const std::exception &e) {
        fail(result_.status, e.what());
    } catch (...) {
        fail(result_.status, "non-standard exception");
    }
}
void SimulationRunner::finish(Lifecycle state) {
    auto &p = *impl_;
    const auto &c = result_.config;
    for (auto symbol : c.symbols) {
        if (!p.exchange->subscribe(1, core::SymbolId{symbol}))
            throw std::runtime_error("snapshot subscription failed");
        const auto top = p.exchange->market_front(1);
        if (!top || top->symbol != symbol)
            throw std::runtime_error("snapshot unavailable");
        result_.books.push_back(
            {symbol, top->bid_price, top->ask_price, top->bid_quantity, top->ask_quantity});
        for (const auto v :
             {static_cast<std::uint64_t>(symbol), static_cast<std::uint64_t>(top->bid_price),
              static_cast<std::uint64_t>(top->ask_price), top->bid_quantity, top->ask_quantity})
            hash(p.h, v);
        p.exchange->market_pop(1, *top);
    }
    for (const auto &participant : p.participants)
        for (const auto symbol : c.symbols) {
            const auto position = p.exchange->position(participant.session, core::SymbolId{symbol});
            if (!position)
                throw std::runtime_error("participant position unavailable");
            result_.positions.push_back({participant.session, symbol, *position});
            hash(p.h, participant.session);
            hash(p.h, symbol);
            hash(p.h, static_cast<std::uint64_t>(*position));
        }
    result_.digest = p.h;
    if (p.observer && !p.observer->finish(result_, state, *p.exchange))
        throw std::runtime_error("final log failed; committed operations were not rolled back");
    result_.summary_complete = true;
    result_.status.state = state;
}
void SimulationRunner::request_stop() {
    if (result_.status.state != Lifecycle::Running)
        return;
    try {
        finish(Lifecycle::Stopped);
    } catch (const std::exception &e) {
        fail(result_.status, e.what());
    } catch (...) {
        fail(result_.status, "non-standard exception");
    }
}
exchange::Exchange *SimulationRunner::completed_isolated_exchange() noexcept {
    return result_.status.state == Lifecycle::Completed && impl_->isolated_exchange
               ? impl_->exchange.get()
               : nullptr;
}
} // namespace chronos::simulation
