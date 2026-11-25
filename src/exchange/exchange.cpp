#include "chronos/exchange/exchange.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace chronos::exchange {
namespace {
// Serialize allocation across Exchange instances, but publish an ID only after
// the staged transaction is ready to commit. The process epoch resets on exit.
constinit detail::OrderIdAllocator process_allocator{true};
} // namespace
detail::AllocationRegistry &detail::AllocationRegistry::process() noexcept {
    static AllocationRegistry registry;
    return registry;
}
struct Exchange::State {
    struct Subscription {
        std::uint64_t generation;
        std::vector<MarketEvent> events;
        std::uint64_t gap_sequence = 0;
        bool gap = false;
        bool gap_sent = false;
    };
    struct Session {
        std::vector<std::uint64_t> requests;
        std::vector<Report> reports;
        RiskLimits limits;
        bool killed = false;
        std::map<std::uint32_t, std::int64_t> positions;
        std::map<std::uint32_t, Subscription> subscriptions;
        std::uint64_t subscription_generation = 0;
    };
    struct Order {
        std::uint64_t session, client, internal, request;
        std::uint32_t symbol;
        bool open;
    };
    std::map<std::uint64_t, Session> sessions;
    std::map<std::uint32_t, std::unique_ptr<matching::MatchingEngine>> engines;
    std::vector<Order> orders;
    std::vector<AuditTrade> trades;
    std::map<std::uint32_t, std::uint64_t> feed_sequences;
    std::uint64_t initial_feed_sequence = 0;
    std::uint64_t command = 0, event = 0;
    // Non-owning, private to a synchronous candidate transaction. Never live.
    std::vector<MarketEvent> *public_capture = nullptr;

    std::unique_ptr<State> clone() const {
        auto out = std::make_unique<State>();
        out->sessions = sessions;
        out->orders = orders;
        out->trades = trades;
        out->feed_sequences = feed_sequences;
        out->initial_feed_sequence = initial_feed_sequence;
        out->command = command;
        out->event = event;
        out->public_capture = public_capture;
        for (const auto &[symbol, engine] : engines)
            out->engines.emplace(symbol, engine->clone());
        return out;
    }
    Order *internal(std::uint64_t id) {
        const auto it = std::find_if(orders.begin(), orders.end(),
                                     [=](const Order &o) { return o.internal == id; });
        return it == orders.end() ? nullptr : &*it;
    }
    bool report(std::size_t maximum, const Order &order, ReportKind kind, std::uint64_t trade,
                core::Price price, core::Quantity quantity, core::Quantity remaining) {
        auto &queue = sessions.at(order.session).reports;
        if (queue.size() >= maximum || event == std::numeric_limits<std::uint64_t>::max())
            return false;
        queue.push_back({++event, command, order.request, order.client, order.internal, trade,
                         order.symbol, price, quantity, remaining, kind});
        return true;
    }
    Result report_failure() const noexcept {
        return event == std::numeric_limits<std::uint64_t>::max() ? Result::CounterExhausted
                                                                  : Result::Capacity;
    }
    bool update_position(std::uint64_t session, std::uint32_t symbol, bool buy,
                         std::uint64_t quantity) {
        if (quantity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return false;
        auto &value = sessions.at(session).positions[symbol];
        const auto q = static_cast<std::int64_t>(quantity);
        if (buy) {
            if (value > std::numeric_limits<std::int64_t>::max() - q)
                return false;
            value += q;
        } else {
            if (value < std::numeric_limits<std::int64_t>::min() + q)
                return false;
            value -= q;
        }
        return true;
    }
    MarketEvent top(std::uint32_t symbol, MarketKind kind, std::uint64_t sequence) const {
        MarketEvent event{symbol, sequence, 0, kind, 0, 0, 0, 0, 0, 0};
        const auto it = engines.find(symbol);
        if (it == engines.end())
            return event;
        const auto bid = it->second->depth(core::Side::Buy, 1);
        const auto ask = it->second->depth(core::Side::Sell, 1);
        if (!bid.empty()) {
            event.bid_price = bid[0].price;
            event.bid_quantity = bid[0].quantity;
        }
        if (!ask.empty()) {
            event.ask_price = ask[0].price;
            event.ask_quantity = ask[0].quantity;
        }
        return event;
    }
    bool publish(MarketEvent event) {
        auto &sequence =
            feed_sequences.try_emplace(event.symbol, initial_feed_sequence).first->second;
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            return false;
        event.sequence = ++sequence;
        if (public_capture) {
            constexpr std::size_t max_public_events = 1025;
            if (public_capture->size() >= max_public_events)
                throw std::length_error("prepared public event limit");
#ifdef CHRONOS_TEST_ID_SEAM
            if (public_capture->size() == public_capture->capacity() &&
                detail::before_capture_allocation_for_test)
                detail::before_capture_allocation_for_test();
#endif
            public_capture->push_back(event);
        }
        for (auto &[id, session] : sessions) {
            (void)id;
            const auto it = session.subscriptions.find(event.symbol);
            if (it == session.subscriptions.end() || it->second.gap)
                continue;
            auto &sub = it->second;
            if (sub.events.size() >= 32) {
                sub.gap = true;
                sub.gap_sequence = event.sequence;
                continue;
            }
            event.generation = sub.generation;
            sub.events.push_back(event);
        }
        return true;
    }
};

static_assert(std::is_nothrow_move_constructible_v<Report>);
static_assert(std::is_nothrow_destructible_v<Report>);
static_assert(std::is_nothrow_move_constructible_v<InstalledSummary>);
static_assert(std::is_nothrow_destructible_v<InstalledSummary>);

Exchange::Exchange(std::size_t sessions, std::size_t orders, std::size_t reports,
                   std::size_t requests, std::uint64_t initial_feed_sequence)
    : Exchange(LiveTag{}, sessions, orders, reports, requests, initial_feed_sequence,
               AllocationMode::Shared) {}
Exchange::Exchange(LiveTag, std::size_t sessions, std::size_t orders, std::size_t reports,
                   std::size_t requests, std::uint64_t initial_feed_sequence, AllocationMode mode)
    : registration_(detail::AllocationRegistry::process().acquire(mode)),
      state_(std::make_unique<State>()), max_sessions_(sessions), max_orders_(orders),
      max_reports_(reports), max_requests_(requests), allocator_(&process_allocator) {
    state_->initial_feed_sequence = initial_feed_sequence;
    exclusive_ = mode == AllocationMode::Exclusive;
}
Exchange::Exchange(IsolatedTag, std::size_t sessions, std::size_t orders, std::size_t reports,
                   std::size_t requests, std::uint64_t initial_feed_sequence, std::uint64_t last_id)
    : state_(std::make_unique<State>()), max_sessions_(sessions), max_orders_(orders),
      max_reports_(reports), max_requests_(requests), private_allocator_(false, last_id),
      allocator_(&private_allocator_), isolated_(true) {
    state_->initial_feed_sequence = initial_feed_sequence;
}
std::unique_ptr<Exchange> Exchange::isolated(std::size_t sessions, std::size_t orders,
                                             std::size_t reports, std::size_t requests,
                                             std::uint64_t initial_feed_sequence,
                                             std::uint64_t last_id) {
    return std::unique_ptr<Exchange>(new Exchange(IsolatedTag{}, sessions, orders, reports,
                                                  requests, initial_feed_sequence, last_id));
}
Exchange::~Exchange() = default;
Exchange::Prepared::Prepared() = default;
Exchange::Prepared::~Prepared() = default;

std::unique_ptr<Exchange::Prepared> Exchange::prepare_open_session(std::uint64_t session) {
    if (!exclusive_)
        throw std::logic_error("prepared session requires exclusive registration");
    auto prepared = std::make_unique<Prepared>();
    auto work = Exchange::isolated(max_sessions_, max_orders_, max_reports_, max_requests_,
                                   state_->initial_feed_sequence);
    work->state_ = state_->clone();
    if (work->open_session(session)) {
        prepared->result = Result::Accepted;
        prepared->candidate = std::move(work->state_);
    }
    return prepared;
}

std::unique_ptr<Exchange::Prepared> Exchange::prepare_risk_update(std::uint64_t session,
                                                                  const RiskLimits &limits) {
    if (!exclusive_)
        throw std::logic_error("prepared risk update requires exclusive registration");
    auto prepared = std::make_unique<Prepared>();
    auto work = Exchange::isolated(max_sessions_, max_orders_, max_reports_, max_requests_,
                                   state_->initial_feed_sequence);
    work->state_ = state_->clone();
    if (work->configure_risk(session, limits)) {
        prepared->result = Result::Accepted;
        prepared->candidate = std::move(work->state_);
    }
    return prepared;
}

std::unique_ptr<Exchange::Prepared> Exchange::prepare_close_session(std::uint64_t session) {
    if (!exclusive_)
        throw std::logic_error("prepared session requires exclusive registration");
    auto prepared = std::make_unique<Prepared>();
    auto work = Exchange::isolated(max_sessions_, max_orders_, max_reports_, max_requests_,
                                   state_->initial_feed_sequence);
    work->state_ = state_->clone();
    if (work->state_->sessions.contains(session)) {
        work->close_session(session);
        prepared->result = Result::Accepted;
        prepared->candidate = std::move(work->state_);
    }
    return prepared;
}

std::unique_ptr<Exchange::Prepared>
Exchange::prepare_submit(std::uint64_t session, std::uint64_t request, std::uint64_t client,
                         core::SymbolId symbol, core::Side side, core::OrderType type,
                         core::Price price, core::Quantity quantity, bool capture_public) {
    if (!exclusive_)
        throw std::logic_error("prepared submission requires exclusive registration");
    auto prepared = std::make_unique<Prepared>();
    prepared->reservation = std::make_unique<detail::OrderIdAllocator::Reservation>(*allocator_);
    const auto reserved_id = prepared->reservation->id();
    const auto previous_id =
        reserved_id == 0 ? std::numeric_limits<std::uint64_t>::max() : reserved_id - 1;
    if (capture_public) {
        prepared->command.emplace(CanonicalCommand::submit(
            session, request, client, symbol.value(), static_cast<std::uint8_t>(side),
            static_cast<std::uint8_t>(type), price, quantity));
        prepared->symbol = symbol.value();
        prepared->allocator_before = previous_id;
        prepared->session = session;
        prepared->request = request;
        prepared->client = client;
    }
    auto work = Exchange::isolated(max_sessions_, max_orders_, max_reports_, max_requests_,
                                   state_->initial_feed_sequence, previous_id);
    work->state_ = state_->clone();
    if (capture_public)
        work->state_->public_capture = &prepared->public_events;
    prepared->result = work->submit(session, request, client, symbol, side, type, price, quantity);
    work->state_->public_capture = nullptr;
    if (prepared->result == Result::Accepted) {
        if (reserved_id == 0 || work->state_->orders.empty() ||
            work->state_->orders.back().internal != reserved_id)
            throw std::logic_error("prepared order ID does not match reservation");
        prepared->candidate = std::move(work->state_);
    }
    return prepared;
}

std::unique_ptr<Exchange::Prepared> Exchange::prepare_cancel(std::uint64_t session,
                                                             std::uint64_t request,
                                                             std::uint64_t client,
                                                             bool capture_public) {
    if (!exclusive_)
        throw std::logic_error("prepared cancellation requires exclusive registration");
    auto prepared = std::make_unique<Prepared>();
    if (capture_public) {
        prepared->command.emplace(CanonicalCommand::cancel(session, request, client));
        prepared->session = session;
        prepared->request = request;
        prepared->client = client;
        const auto order =
            std::find_if(state_->orders.begin(), state_->orders.end(), [=](const State::Order &o) {
                return o.session == session && o.client == client;
            });
        if (order != state_->orders.end())
            prepared->symbol = order->symbol;
        // Exclusive registration prevents any competing live allocator use.
        detail::OrderIdAllocator::Reservation sample(*allocator_);
        prepared->allocator_before =
            sample.id() == 0 ? std::numeric_limits<std::uint64_t>::max() : sample.id() - 1;
    }
    auto work = Exchange::isolated(max_sessions_, max_orders_, max_reports_, max_requests_,
                                   state_->initial_feed_sequence);
    work->state_ = state_->clone();
    if (capture_public)
        work->state_->public_capture = &prepared->public_events;
    prepared->result = work->cancel(session, request, client);
    work->state_->public_capture = nullptr;
    if (prepared->result == Result::Accepted)
        prepared->candidate = std::move(work->state_);
    return prepared;
}

InstalledSummary Exchange::summarize_prepared(Prepared &prepared) const {
    constexpr std::size_t max_trades = 1024, max_reports = 4096;
    InstalledSummary summary;
    summary.result_ = prepared.result;
    summary.command_before_ = summary.command_after_ = state_->command;
    summary.event_before_ = summary.event_after_ = state_->event;
    summary.command_ = prepared.command;
    summary.allocator_before_ = summary.allocator_after_ = prepared.allocator_before;
    const auto symbol_snapshot = [](const State &state, std::uint32_t symbol) {
        SymbolCounterSnapshot snapshot{symbol, false, {}, 0, false, state.initial_feed_sequence};
        const auto engine = state.engines.find(symbol);
        if (engine != state.engines.end()) {
            snapshot.engine_present = true;
            snapshot.matching = engine->second->counters();
            snapshot.book_revision = engine->second->book_revision();
        }
        const auto feed = state.feed_sequences.find(symbol);
        if (feed != state.feed_sequences.end()) {
            snapshot.feed_present = true;
            snapshot.feed_sequence = feed->second;
        }
        return snapshot;
    };
    const auto position_snapshot = [](const State &state, std::uint64_t session,
                                      std::uint32_t symbol) -> std::optional<std::int64_t> {
        const auto account = state.sessions.find(session);
        if (account == state.sessions.end())
            return std::nullopt;
        const auto position = account->second.positions.find(symbol);
        return position == account->second.positions.end() ? 0 : position->second;
    };
    if (prepared.symbol) {
        summary.symbol_before_ = symbol_snapshot(*state_, *prepared.symbol);
        summary.symbol_after_ = summary.symbol_before_;
        summary.initiator_position_before_ =
            position_snapshot(*state_, prepared.session, *prepared.symbol);
        summary.initiator_position_after_ = summary.initiator_position_before_;
    }
    if (prepared.result != Result::Accepted)
        return summary;
    if (!prepared.candidate)
        throw std::logic_error("accepted preparation has no candidate");
    const auto &candidate = *prepared.candidate;
    if (candidate.public_capture != nullptr)
        throw std::logic_error("prepared candidate still has a capture sink");
    if (candidate.trades.size() < state_->trades.size() || candidate.command <= state_->command ||
        candidate.command - state_->command != 1 || candidate.event < state_->event)
        throw std::logic_error("invalid prepared state counters");
    const auto trade_count = candidate.trades.size() - state_->trades.size();
    if (trade_count > max_trades)
        throw std::length_error("prepared trade summary limit");
    summary.trades_.assign(candidate.trades.begin() +
                               static_cast<std::ptrdiff_t>(state_->trades.size()),
                           candidate.trades.end());
    std::size_t report_count = 0;
    for (const auto &[id, session] : candidate.sessions) {
        const auto old = state_->sessions.find(id);
        if (old == state_->sessions.end() || session.reports.size() < old->second.reports.size())
            throw std::logic_error("invalid prepared report queues");
        const auto added = session.reports.size() - old->second.reports.size();
        if (added > max_reports - report_count)
            throw std::length_error("prepared report summary limit");
        report_count += added;
    }
    if (candidate.sessions.size() != state_->sessions.size() ||
        candidate.event - state_->event != report_count)
        throw std::logic_error("prepared report count mismatch");
    summary.reports_.reserve(report_count);
    for (const auto &[id, session] : candidate.sessions) {
        const auto previous = state_->sessions.at(id).reports.size();
        for (auto i = previous; i < session.reports.size(); ++i)
            summary.reports_.push_back({id, session.reports[i]});
    }
    std::sort(summary.reports_.begin(), summary.reports_.end(),
              [](const RecipientReport &a, const RecipientReport &b) {
                  return a.report.sequence < b.report.sequence;
              });
    for (std::size_t i = 0; i < summary.reports_.size(); ++i)
        if (summary.reports_[i].report.sequence != state_->event + i + 1)
            throw std::logic_error("prepared report sequence mismatch");
    // At most two participants per trade plus the initiator. All work and
    // checked accumulation happen on private state before installation.
    if (!prepared.symbol)
        throw std::logic_error("accepted preparation has no symbol");
    std::vector<std::uint64_t> participants;
    participants.reserve(1 + 2 * trade_count);
    participants.push_back(prepared.session);
    for (const auto &trade : summary.trades_) {
        participants.push_back(trade.buyer_session);
        participants.push_back(trade.seller_session);
    }
    std::sort(participants.begin(), participants.end());
    participants.erase(std::unique(participants.begin(), participants.end()), participants.end());
    summary.participant_effects_.reserve(participants.size());
    for (const auto id : participants) {
        const auto before = position_snapshot(*state_, id, *prepared.symbol);
        const auto after = position_snapshot(candidate, id, *prepared.symbol);
        if (!before || !after)
            throw std::logic_error("prepared trade participant missing session");
        const auto &account = state_->sessions.at(id);
        const auto &next_account = candidate.sessions.at(id);
        const auto &limits = account.limits;
        const auto &next_limits = next_account.limits;
        if (account.killed != next_account.killed ||
            limits.max_order_quantity != next_limits.max_order_quantity ||
            limits.max_order_notional != next_limits.max_order_notional ||
            limits.max_open_notional != next_limits.max_open_notional ||
            limits.max_open_orders != next_limits.max_open_orders ||
            limits.max_abs_position != next_limits.max_abs_position ||
            limits.max_market_price != next_limits.max_market_price ||
            limits.allowed_symbols != next_limits.allowed_symbols)
            throw std::logic_error("prepared command changed risk policy");
        const bool allowed = limits.allowed_symbols.empty() ||
                             std::find(limits.allowed_symbols.begin(), limits.allowed_symbols.end(),
                                       *prepared.symbol) != limits.allowed_symbols.end();
        summary.participant_effects_.push_back(
            {id, ExposureSnapshot{*before}, ExposureSnapshot{*after},
             RiskPolicySnapshot{limits.max_order_quantity, limits.max_order_notional,
                                limits.max_open_notional, limits.max_open_orders,
                                limits.max_abs_position, limits.max_market_price, account.killed,
                                allowed}});
    }
    const auto accumulate_exposure = [&](const State &source, bool after) {
        for (const auto &order : source.orders) {
            if (!order.open)
                continue;
            const auto found = std::lower_bound(
                summary.participant_effects_.begin(), summary.participant_effects_.end(),
                order.session, [](const ParticipantEffect &effect, std::uint64_t id) {
                    return effect.session < id;
                });
            if (found == summary.participant_effects_.end() || found->session != order.session)
                continue;
            const auto engine = source.engines.find(order.symbol);
            if (engine == source.engines.end())
                throw std::logic_error("open order missing engine");
            const auto live = engine->second->find(core::OrderId{order.internal});
            if (!live || live->price <= 0 || live->quantity == 0)
                throw std::logic_error("invalid open order exposure");
            auto &exposure = after ? found->after : found->before;
            const auto price = static_cast<std::uint64_t>(live->price);
            if (price > std::numeric_limits<std::uint64_t>::max() / live->quantity)
                throw std::overflow_error("open order notional overflow");
            const auto notional = price * live->quantity;
            if (notional > std::numeric_limits<std::uint64_t>::max() - exposure.open_notional ||
                exposure.open_order_count == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("aggregate open exposure overflow");
            exposure.open_notional += notional;
            ++exposure.open_order_count;
            if (order.symbol == *prepared.symbol) {
                auto &quantity = live->side == core::Side::Buy ? exposure.symbol_buy_quantity
                                                               : exposure.symbol_sell_quantity;
                if (live->quantity > std::numeric_limits<std::uint64_t>::max() - quantity)
                    throw std::overflow_error("symbol exposure overflow");
                quantity += live->quantity;
            }
        }
    };
    accumulate_exposure(*state_, false);
    accumulate_exposure(candidate, true);
    std::vector<std::uint64_t> touched_orders;
    touched_orders.reserve(summary.reports_.size());
    for (const auto &report : summary.reports_) {
        if (report.report.internal_order_id == 0)
            throw std::logic_error("prepared report missing order identity");
        touched_orders.push_back(report.report.internal_order_id);
    }
    std::sort(touched_orders.begin(), touched_orders.end());
    touched_orders.erase(std::unique(touched_orders.begin(), touched_orders.end()),
                         touched_orders.end());
    summary.ownership_effects_.reserve(touched_orders.size());
    for (const auto id : touched_orders)
        summary.ownership_effects_.push_back({id, std::nullopt, std::nullopt});
    const auto capture_ownership = [&](const State &source, bool after) {
        for (const auto &order : source.orders) {
            const auto found = std::lower_bound(
                summary.ownership_effects_.begin(), summary.ownership_effects_.end(),
                order.internal, [](const OwnershipEffect &effect, std::uint64_t id) {
                    return effect.internal_order_id < id;
                });
            if (found == summary.ownership_effects_.end() ||
                found->internal_order_id != order.internal)
                continue;
            std::optional<orderbook::OrderSnapshot> resting;
            if (order.open) {
                const auto engine = source.engines.find(order.symbol);
                if (engine == source.engines.end() ||
                    !(resting = engine->second->find(core::OrderId{order.internal})))
                    throw std::logic_error("open ownership missing resting order");
            }
            auto &slot = after ? found->after : found->before;
            if (slot)
                throw std::logic_error("duplicate ownership identity");
            slot = OwnershipSnapshot{order.session, order.client, order.request,
                                     order.symbol,  order.open,   resting};
        }
    };
    capture_ownership(*state_, false);
    capture_ownership(candidate, true);
    for (const auto &effect : summary.ownership_effects_) {
        if (!effect.after ||
            (effect.before && (effect.before->session != effect.after->session ||
                               effect.before->client_order_id != effect.after->client_order_id ||
                               effect.before->symbol != effect.after->symbol)))
            throw std::logic_error("prepared ownership identity mismatch");
    }
    if (prepared.reservation)
        summary.new_id_ = prepared.reservation->id();
    if (summary.new_id_)
        summary.allocator_after_ = *summary.new_id_;
    if (prepared.public_events.size() < trade_count ||
        prepared.public_events.size() - trade_count > 1)
        throw std::logic_error("prepared public event count mismatch");
    if (!prepared.public_events.empty()) {
        const auto symbol = prepared.public_events.front().symbol;
        const auto old = state_->feed_sequences.find(symbol);
        auto sequence =
            old == state_->feed_sequences.end() ? state_->initial_feed_sequence : old->second;
        for (std::size_t i = 0; i < prepared.public_events.size(); ++i) {
            const auto &event = prepared.public_events[i];
            if (sequence == std::numeric_limits<std::uint64_t>::max() || event.symbol != symbol ||
                event.sequence != ++sequence || event.generation != 0)
                throw std::logic_error("prepared public event sequence mismatch");
            if (i < trade_count) {
                const auto &trade = summary.trades_[i];
                if (event.kind != MarketKind::Trade || event.trade_price != trade.price ||
                    event.trade_quantity != trade.quantity || event.symbol != trade.symbol)
                    throw std::logic_error("prepared trade event mismatch");
            } else if (event.kind != MarketKind::Book)
                throw std::logic_error("prepared book event mismatch");
        }
        const auto actual = candidate.feed_sequences.find(symbol);
        if (actual == candidate.feed_sequences.end() || actual->second != sequence)
            throw std::logic_error("prepared feed counter mismatch");
    }
    summary.public_events_ = std::move(prepared.public_events);
    if (prepared.symbol) {
        summary.symbol_after_ = symbol_snapshot(candidate, *prepared.symbol);
        summary.initiator_position_after_ =
            position_snapshot(candidate, prepared.session, *prepared.symbol);
    }
    for (const auto &item : summary.reports_)
        if (item.session == prepared.session && item.report.request_id == prepared.request &&
            item.report.client_order_id == prepared.client)
            summary.initiator_final_report_ = item.report;
    summary.command_after_ = candidate.command;
    summary.event_after_ = candidate.event;
    return summary;
}

void Exchange::install_prepared(Prepared &prepared) noexcept {
    static_assert(std::is_nothrow_swappable_v<std::unique_ptr<State>>);
    static_assert(noexcept(std::declval<detail::OrderIdAllocator::Reservation &>().commit()));
    state_.swap(prepared.candidate);
    if (prepared.reservation)
        prepared.reservation->commit();
}

void Exchange::release_exclusive_registration() noexcept {
    if (exclusive_) {
        registration_.release();
        exclusive_ = false;
    }
}
bool Exchange::open_session(std::uint64_t session) {
    if (session == 0 || state_->sessions.size() >= max_sessions_ ||
        state_->sessions.contains(session))
        return false;
    return state_->sessions.try_emplace(session).second;
}
bool Exchange::configure_risk(std::uint64_t session, const RiskLimits &limits) {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end() || limits.max_market_price <= 0 ||
        limits.allowed_symbols.size() > 1024 ||
        limits.max_abs_position >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return false;
    if (std::any_of(state_->orders.begin(), state_->orders.end(), [=](const State::Order &order) {
            return order.session == session && order.open;
        }))
        return false;
    for (const auto &[symbol, pos] : it->second.positions) {
        (void)symbol;
        const auto magnitude =
            pos < 0 ? static_cast<std::uint64_t>(-(pos + 1)) + 1 : static_cast<std::uint64_t>(pos);
        if (magnitude > limits.max_abs_position)
            return false;
    }
    auto next = state_->clone();
    next->sessions.at(session).limits = limits;
    state_.swap(next);
    return true;
}
bool Exchange::set_killed(std::uint64_t session, bool killed) noexcept {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end())
        return false;
    it->second.killed = killed;
    return true;
}
std::optional<std::int64_t> Exchange::position(std::uint64_t session,
                                               core::SymbolId symbol) const noexcept {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end())
        return std::nullopt;
    const auto p = it->second.positions.find(symbol.value());
    return p == it->second.positions.end() ? 0 : p->second;
}
bool Exchange::subscribe(std::uint64_t session, core::SymbolId symbol) {
    const auto found = state_->sessions.find(session);
    if (found == state_->sessions.end() ||
        (found->second.subscriptions.size() >= 32 &&
         !found->second.subscriptions.contains(symbol.value())) ||
        found->second.subscription_generation == std::numeric_limits<std::uint64_t>::max())
        return false;
    auto next = state_->clone();
    auto &account = next->sessions.at(session);
    const auto generation = ++account.subscription_generation;
    auto &subscription = account.subscriptions[symbol.value()];
    subscription = State::Subscription{generation, {}, 0, false, false};
    const auto seq = next->feed_sequences.find(symbol.value());
    auto snapshot =
        next->top(symbol.value(), MarketKind::Snapshot,
                  seq == next->feed_sequences.end() ? next->initial_feed_sequence : seq->second);
    snapshot.generation = generation;
    subscription.events.push_back(snapshot);
    state_.swap(next);
    return true;
}
bool Exchange::unsubscribe(std::uint64_t session, core::SymbolId symbol) {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end() || !it->second.subscriptions.contains(symbol.value()))
        return false;
    auto next = state_->clone();
    next->sessions.at(session).subscriptions.erase(symbol.value());
    state_.swap(next);
    return true;
}
std::optional<MarketEvent> Exchange::market_front(std::uint64_t session) const {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end())
        return std::nullopt;
    for (const auto &[symbol, sub] : it->second.subscriptions) {
        if (!sub.events.empty())
            return sub.events.front();
        if (sub.gap && !sub.gap_sent)
            return MarketEvent{
                symbol, sub.gap_sequence, sub.generation, MarketKind::Gap, 0, 0, 0, 0, 0, 0};
    }
    return std::nullopt;
}
void Exchange::market_pop(std::uint64_t session, const MarketEvent &sent) noexcept {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end())
        return;
    const auto sub = it->second.subscriptions.find(sent.symbol);
    if (sub == it->second.subscriptions.end() || sub->second.generation != sent.generation)
        return;
    if (!sub->second.events.empty()) {
        const auto &first = sub->second.events.front();
        if (first.kind == sent.kind && first.sequence == sent.sequence)
            sub->second.events.erase(sub->second.events.begin());
    } else if (sent.kind == MarketKind::Gap && sub->second.gap &&
               sent.sequence == sub->second.gap_sequence) {
        sub->second.gap_sent = true;
    }
}
void Exchange::close_session(std::uint64_t session) noexcept {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end())
        return;
    // A disconnected participant may retain orders and private reports, but no
    // longer consumes public feed capacity.
    it->second.subscriptions.clear();
    if (!it->second.reports.empty())
        return;
    if (std::any_of(state_->orders.begin(), state_->orders.end(),
                    [=](const State::Order &order) { return order.session == session; }))
        return;
    state_->sessions.erase(it);
}

Result Exchange::submit(std::uint64_t session, std::uint64_t request, std::uint64_t client,
                        core::SymbolId symbol, core::Side side, core::OrderType type,
                        core::Price price, core::Quantity quantity) {
    static_assert(std::is_nothrow_swappable_v<std::unique_ptr<State>>);
    static_assert(std::is_nothrow_destructible_v<State>);
    const auto s = state_->sessions.find(session);
    if (s == state_->sessions.end())
        return Result::UnknownSession;
    if (std::find(s->second.requests.begin(), s->second.requests.end(), request) !=
        s->second.requests.end())
        return Result::DuplicateRequest;
    if (std::any_of(state_->orders.begin(), state_->orders.end(), [=](const State::Order &o) {
            return o.session == session && o.client == client;
        }))
        return Result::DuplicateOrder;
    const auto &account = s->second;
    const auto &limits = account.limits;
    if (account.killed)
        return Result::RiskKilled;
    if (!limits.allowed_symbols.empty() &&
        std::find(limits.allowed_symbols.begin(), limits.allowed_symbols.end(), symbol.value()) ==
            limits.allowed_symbols.end())
        return Result::RiskSymbol;
    if ((side != core::Side::Buy && side != core::Side::Sell) ||
        (type != core::OrderType::Limit && type != core::OrderType::Market) ||
        (type == core::OrderType::Limit && price <= 0) ||
        (type == core::OrderType::Market && price != 0))
        return Result::InvalidOrder;
    if (quantity == 0 || quantity > limits.max_order_quantity ||
        quantity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return Result::RiskQuantity;
    auto risk_price = static_cast<std::uint64_t>(
        type == core::OrderType::Limit ? price : limits.max_market_price);
    // A crossing sell executes at resting bid prices, which can exceed the
    // incoming limit. Reserve the whole quantity at the highest executable
    // price so both order and open-notional checks remain conservative.
    if (type == core::OrderType::Limit && side == core::Side::Sell) {
        const auto book = state_->engines.find(symbol.value());
        if (book != state_->engines.end()) {
            const auto bids = book->second->depth(core::Side::Buy, 1);
            if (!bids.empty() && bids.front().price >= price)
                risk_price = std::max(risk_price, static_cast<std::uint64_t>(bids.front().price));
        }
    }
    if (risk_price > std::numeric_limits<std::uint64_t>::max() / quantity)
        return Result::RiskOverflow;
    const auto notional = risk_price * quantity;
    if (notional > limits.max_order_notional)
        return Result::RiskNotional;
    std::size_t open_count = 0;
    std::uint64_t open_notional = 0, same_side_reserved = 0;
    for (const auto &entry : state_->orders) {
        if (entry.session != session || !entry.open)
            continue;
        ++open_count;
        const auto live = state_->engines.at(entry.symbol)->find(core::OrderId{entry.internal});
        if (!live)
            return Result::InvalidOrder;
        const auto p = static_cast<std::uint64_t>(live->price);
        if (p > std::numeric_limits<std::uint64_t>::max() / live->quantity)
            return Result::RiskOverflow;
        const auto amount = p * live->quantity;
        if (amount > std::numeric_limits<std::uint64_t>::max() - open_notional)
            return Result::RiskOverflow;
        open_notional += amount;
        if (entry.symbol == symbol.value() && live->side == side) {
            if (live->quantity > std::numeric_limits<std::uint64_t>::max() - same_side_reserved)
                return Result::RiskOverflow;
            same_side_reserved += live->quantity;
        }
    }
    if (type == core::OrderType::Limit && open_count >= limits.max_open_orders)
        return Result::RiskOpenOrders;
    if (notional > limits.max_open_notional || open_notional > limits.max_open_notional - notional)
        return Result::RiskNotional;
    const auto pos_it = account.positions.find(symbol.value());
    const auto pos = pos_it == account.positions.end() ? 0 : pos_it->second;
    const auto magnitude =
        pos < 0 ? static_cast<std::uint64_t>(-(pos + 1)) + 1 : static_cast<std::uint64_t>(pos);
    const auto max_pos = limits.max_abs_position;
    if (magnitude > max_pos)
        return Result::RiskPosition;
    const auto capacity = side == core::Side::Buy
                              ? (pos < 0 ? max_pos + magnitude : max_pos - magnitude)
                              : (pos < 0 ? max_pos - magnitude : max_pos + magnitude);
    if (quantity > capacity || same_side_reserved > capacity - quantity)
        return Result::RiskPosition;
    if (type == core::OrderType::Market) {
        const auto book = state_->engines.find(symbol.value());
        if (book != state_->engines.end()) {
            auto remaining = quantity;
            const auto other = side == core::Side::Buy ? core::Side::Sell : core::Side::Buy;
            for (const auto &level :
                 book->second->depth(other, std::numeric_limits<std::size_t>::max())) {
                if (level.price > limits.max_market_price)
                    return Result::RiskMarketBound;
                if (level.quantity >= remaining)
                    break;
                remaining -= level.quantity;
            }
        }
    }
    if (s->second.requests.size() >= max_requests_ || state_->orders.size() >= max_orders_)
        return Result::Capacity;
    if (state_->command == std::numeric_limits<std::uint64_t>::max())
        return Result::CounterExhausted;
    detail::OrderIdAllocator::Reservation reservation(*allocator_);
    if (reservation.id() == 0)
        return Result::CounterExhausted;
    const auto before_book = state_->top(symbol.value(), MarketKind::Book, 0);
    auto next = state_->clone();
    auto &engine = next->engines[symbol.value()];
    if (!engine)
        engine = std::make_unique<matching::MatchingEngine>(symbol);
    const auto internal_id = reservation.id();
    auto outcome =
        engine->submit({core::OrderId{internal_id}, symbol, side, type, price, quantity});
    if (outcome.error != matching::Error::None)
        return outcome.error == matching::Error::CounterExhausted ? Result::CounterExhausted
                                                                  : Result::InvalidOrder;
    constexpr std::size_t max_audit_trades = 8192;
    if (outcome.trades.size() > max_audit_trades - next->trades.size())
        return Result::Capacity;
    ++next->command;
    next->sessions.at(session).requests.push_back(request);
    next->orders.push_back({session, client, internal_id, request, symbol.value(), false});
    auto *incoming = next->internal(internal_id);
    if (!next->report(max_reports_, *incoming, ReportKind::Accepted, 0, 0, 0, quantity))
        return next->report_failure();
    for (std::size_t i = 0; i < outcome.trades.size(); ++i) {
        const auto &trade = outcome.trades[i];
        const auto resting_id = trade.buy_order_id.value() == internal_id
                                    ? trade.sell_order_id.value()
                                    : trade.buy_order_id.value();
        auto *resting = next->internal(resting_id);
        if (!resting || outcome.reports.size() < 3 + 2 * i)
            return Result::InvalidOrder;
        const auto buyer_session = side == core::Side::Buy ? session : resting->session;
        const auto seller_session = side == core::Side::Sell ? session : resting->session;
        if (buyer_session != seller_session &&
            (!next->update_position(buyer_session, symbol.value(), true, trade.quantity) ||
             !next->update_position(seller_session, symbol.value(), false, trade.quantity)))
            return Result::RiskOverflow;
        const auto &resting_report = outcome.reports[1 + 2 * i];
        const auto &incoming_report = outcome.reports[2 + 2 * i];
        next->trades.push_back(
            {next->command, trade.id.value(), side == core::Side::Buy ? session : resting->session,
             side == core::Side::Sell ? session : resting->session, trade.buy_order_id.value(),
             trade.sell_order_id.value(), symbol.value(), trade.price, trade.quantity});
        if (!next->report(max_reports_, *resting,
                          resting_report.remaining ? ReportKind::PartialFill : ReportKind::Filled,
                          trade.id.value(), trade.price, trade.quantity,
                          resting_report.remaining) ||
            !next->report(max_reports_, *incoming,
                          incoming_report.remaining ? ReportKind::PartialFill : ReportKind::Filled,
                          trade.id.value(), trade.price, trade.quantity, incoming_report.remaining))
            return next->report_failure();
        resting->open = resting_report.remaining != 0;
    }
    if (outcome.remaining != 0) {
        const bool is_resting = type == core::OrderType::Limit;
        incoming->open = is_resting;
        if (!next->report(max_reports_, *incoming,
                          is_resting ? ReportKind::Resting : ReportKind::MarketRemainder, 0, 0, 0,
                          outcome.remaining))
            return next->report_failure();
    }
    for (const auto &trade : outcome.trades) {
        MarketEvent event{symbol.value(), 0, 0, MarketKind::Trade, 0, 0,
                          trade.price,    0, 0, trade.quantity};
        if (!next->publish(event))
            return Result::CounterExhausted;
    }
    const auto after_book = next->top(symbol.value(), MarketKind::Book, 0);
    if (before_book.bid_price != after_book.bid_price ||
        before_book.bid_quantity != after_book.bid_quantity ||
        before_book.ask_price != after_book.ask_price ||
        before_book.ask_quantity != after_book.ask_quantity) {
        if (!next->publish(after_book))
            return Result::CounterExhausted;
    }
    state_.swap(next);
    reservation.commit();
    return Result::Accepted;
}

Result Exchange::cancel(std::uint64_t session, std::uint64_t request, std::uint64_t client) {
    const auto s = state_->sessions.find(session);
    if (s == state_->sessions.end())
        return Result::UnknownSession;
    if (std::find(s->second.requests.begin(), s->second.requests.end(), request) !=
        s->second.requests.end())
        return Result::DuplicateRequest;
    const auto found =
        std::find_if(state_->orders.begin(), state_->orders.end(), [=](const State::Order &o) {
            return o.session == session && o.client == client;
        });
    if (found == state_->orders.end())
        return Result::NotFound;
    if (!found->open)
        return Result::NotFound;
    if (s->second.requests.size() >= max_requests_)
        return Result::Capacity;
    if (state_->command == std::numeric_limits<std::uint64_t>::max())
        return Result::CounterExhausted;
    const auto before_book = state_->top(found->symbol, MarketKind::Book, 0);
    auto next = state_->clone();
    auto *order = next->internal(found->internal);
    const auto snapshot = next->engines.at(order->symbol)->find(core::OrderId{order->internal});
    if (!snapshot ||
        next->engines.at(order->symbol)->cancel_resting(core::OrderId{order->internal}) !=
            orderbook::Result::Success)
        return Result::NotFound;
    ++next->command;
    next->sessions.at(session).requests.push_back(request);
    order->request = request;
    order->open = false;
    if (!next->report(max_reports_, *order, ReportKind::Cancelled, 0, 0, snapshot->quantity, 0))
        return next->report_failure();
    const auto after_book = next->top(order->symbol, MarketKind::Book, 0);
    if (before_book.bid_price != after_book.bid_price ||
        before_book.bid_quantity != after_book.bid_quantity ||
        before_book.ask_price != after_book.ask_price ||
        before_book.ask_quantity != after_book.ask_quantity) {
        if (!next->publish(after_book))
            return Result::CounterExhausted;
    }
    state_.swap(next);
    return Result::Accepted;
}
std::optional<Report> Exchange::front(std::uint64_t session) const {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end() || it->second.reports.empty())
        return std::nullopt;
    return it->second.reports.front();
}
void Exchange::pop(std::uint64_t session) noexcept {
    const auto it = state_->sessions.find(session);
    if (it != state_->sessions.end() && !it->second.reports.empty())
        it->second.reports.erase(it->second.reports.begin());
}
std::size_t Exchange::trade_count() const noexcept { return state_->trades.size(); }
std::optional<AuditTrade> Exchange::trade(std::size_t index) const noexcept {
    if (index >= state_->trades.size())
        return std::nullopt;
    return state_->trades[index];
}
std::optional<orderbook::OrderSnapshot> Exchange::find(std::uint64_t session,
                                                       std::uint64_t client) const {
    const auto it =
        std::find_if(state_->orders.begin(), state_->orders.end(), [=](const State::Order &o) {
            return o.session == session && o.client == client && o.open;
        });
    if (it == state_->orders.end())
        return std::nullopt;
    return state_->engines.at(it->symbol)->find(core::OrderId{it->internal});
}
std::vector<std::uint64_t> Exchange::verification_state() const {
    return verification_state_bounded(std::numeric_limits<std::size_t>::max());
}
std::vector<std::uint64_t> Exchange::verification_state_bounded(std::size_t max_words) const {
    return state_words(max_words, false, 0);
}
std::vector<std::uint64_t> Exchange::isolated_continuation_state(std::size_t max_words) const {
    if (!isolated_)
        throw std::logic_error("continuation capture requires an isolated exchange");
    return state_words(max_words, true, private_allocator_.private_last());
}

std::vector<std::uint8_t> Exchange::canonical_isolated_state(std::size_t max_bytes) const {
    if (!isolated_)
        throw std::logic_error("canonical capture requires isolated exchange");
    if (max_bytes > 65536)
        throw std::length_error("canonical state limit");
    std::vector<std::uint8_t> out;
    const auto add = [&](std::uint64_t value) {
        if (max_bytes < 8 || out.size() > max_bytes - 8)
            throw std::length_error("canonical state exceeds byte limit");
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto report_kind = [](ReportKind kind) -> std::uint64_t {
        switch (kind) {
        case ReportKind::Accepted:
            return 1;
        case ReportKind::PartialFill:
            return 2;
        case ReportKind::Filled:
            return 3;
        case ReportKind::Resting:
            return 4;
        case ReportKind::Cancelled:
            return 5;
        case ReportKind::MarketRemainder:
            return 6;
        }
        throw std::logic_error("unknown report kind");
    };
    add(0x4348524353543031ULL); // CHRCST01
    add(1);                     // schema version
    add(max_sessions_);
    add(max_orders_);
    add(max_reports_);
    add(max_requests_);
    add(private_allocator_.private_last());
    add(state_->initial_feed_sequence);
    add(state_->command);
    add(state_->event);
    add(state_->sessions.size());
    for (const auto &[id, account] : state_->sessions) {
        if (!account.subscriptions.empty())
            throw std::logic_error("canonical schema excludes public subscriptions");
        add(id);
        add(account.killed ? 1 : 0);
        const auto &limits = account.limits;
        add(limits.max_order_quantity);
        add(limits.max_order_notional);
        add(limits.max_open_notional);
        add(limits.max_open_orders);
        add(limits.max_abs_position);
        add(static_cast<std::uint64_t>(limits.max_market_price));
        add(limits.allowed_symbols.size());
        for (auto symbol : limits.allowed_symbols)
            add(symbol);
        add(account.requests.size());
        for (auto request : account.requests)
            add(request);
        add(account.positions.size());
        for (const auto &[symbol, position] : account.positions) {
            add(symbol);
            add(static_cast<std::uint64_t>(position));
        }
        add(account.reports.size());
        for (const auto &report : account.reports) {
            add(report.sequence);
            add(report.command_sequence);
            add(report.request_id);
            add(report.client_order_id);
            add(report.internal_order_id);
            add(report.trade_id);
            add(report.symbol);
            add(static_cast<std::uint64_t>(report.price));
            add(report.quantity);
            add(report.remaining);
            add(report_kind(report.kind));
        }
        add(account.subscription_generation);
    }
    add(state_->orders.size());
    for (const auto &order : state_->orders) {
        add(order.session);
        add(order.client);
        add(order.internal);
        add(order.request);
        add(order.symbol);
        add(order.open ? 1 : 0);
    }
    add(state_->trades.size());
    for (const auto &trade : state_->trades) {
        add(trade.command_sequence);
        add(trade.trade_id);
        add(trade.buyer_session);
        add(trade.seller_session);
        add(trade.buy_internal_id);
        add(trade.sell_internal_id);
        add(trade.symbol);
        add(static_cast<std::uint64_t>(trade.price));
        add(trade.quantity);
    }
    add(state_->feed_sequences.size());
    for (const auto &[symbol, sequence] : state_->feed_sequences) {
        add(symbol);
        add(sequence);
    }
    add(state_->engines.size());
    for (const auto &[symbol, engine] : state_->engines) {
        add(symbol);
        const auto c = engine->counters();
        add(c.command);
        add(c.event);
        add(c.trade);
        add(engine->book_revision());
        switch (engine->transaction_mode()) {
        case matching::TransactionMode::Planned:
            add(1);
            break;
        case matching::TransactionMode::CloneReference:
            add(2);
            break;
        default:
            throw std::logic_error("unknown transaction mode");
        }
        for (const auto side : {core::Side::Buy, core::Side::Sell}) {
            const auto count = engine->price_level_count(side);
            if (count > max_bytes / 32)
                throw std::length_error("canonical level limit");
            const auto levels = engine->depth(side, count);
            if (levels.size() != count)
                throw std::logic_error("canonical level count mismatch");
            add(count);
            for (const auto &level : levels) {
                if (level.order_count > max_bytes / 24)
                    throw std::length_error("canonical order limit");
                add(static_cast<std::uint64_t>(level.price));
                add(level.quantity);
                add(level.order_count);
                const auto orders = engine->orders_at(side, level.price);
                if (orders.size() != level.order_count)
                    throw std::logic_error("canonical order count mismatch");
                for (const auto &order : orders) {
                    add(order.id.value());
                    add(order.quantity);
                    add(order.sequence);
                }
            }
        }
    }
    return out;
}
std::vector<std::uint64_t> Exchange::live_continuation_state(std::size_t max_words) const {
    if (isolated_ || !exclusive_)
        throw std::logic_error("live continuation capture requires exclusive registration");
    detail::OrderIdAllocator::Reservation reservation(*allocator_);
    const auto next = reservation.id();
    const auto last = next == 0 ? std::numeric_limits<std::uint64_t>::max() : next - 1;
    return state_words(max_words, true, last);
}
#ifdef CHRONOS_TEST_ID_SEAM
bool Exchange::seed_process_id_for_test(std::uint64_t last) {
    if (isolated_ || !exclusive_ || !state_->orders.empty() || !state_->trades.empty())
        return false;
    return allocator_->seed_initial_for_test(last);
}
#endif
std::vector<std::uint64_t> Exchange::state_words(std::size_t max_words, bool exact,
                                                 std::uint64_t exact_last) const {
    // Exact no-allocation shape preflight before allocating output or
    // normalization nodes. All arithmetic is checked against the caller's cap.
    std::size_t minimum = 0;
    const auto charge = [&](std::size_t count, std::size_t words) {
        if (words != 0 && count > (max_words - minimum) / words)
            throw std::length_error("verification state exceeds word limit");
        minimum += count * words;
    };
    charge(1, 9); // top-level words and collection lengths
    charge(state_->orders.size(), 5);
    if (exact) {
        charge(state_->orders.size(), 1);
        charge(1, 6); // exact metadata version, capacities and allocator high-water
        charge(state_->engines.size(), 3); // symbol, revision and transaction mode
    }
    charge(state_->trades.size(), 9);
    charge(state_->feed_sequences.size(), 2);
    charge(state_->engines.size(), 6); // symbol/counters and two side lengths
    for (const auto &[id, session] : state_->sessions) {
        (void)id;
        charge(1, 14); // fixed session words
        charge(session.limits.allowed_symbols.size(), 1);
        charge(session.requests.size(), 1);
        charge(session.positions.size(), 2);
        charge(session.reports.size(), 11);
        for (const auto &[symbol, sub] : session.subscriptions) {
            (void)symbol;
            charge(1, 6);
            charge(sub.events.size(), 10);
        }
    }
    for (const auto &[symbol, engine] : state_->engines) {
        (void)symbol;
        charge(engine->price_level_count(core::Side::Buy), 4);
        charge(engine->price_level_count(core::Side::Sell), 4);
        charge(engine->resting_order_count(), 3);
    }
    std::vector<std::uint64_t> out;
    out.reserve(minimum);
    const auto add = [&](std::uint64_t v) {
        if (out.size() == max_words)
            throw std::length_error("verification state exceeds word limit");
        if (out.size() == out.capacity()) {
            const auto grow = out.size() > max_words / 2 ? max_words : out.size() * 2;
            out.reserve(std::min(max_words, std::max<std::size_t>(1, grow)));
        }
        out.push_back(v);
    };
    // Preserve identity relationships while allowing process-wide IDs to differ
    // between recording and replay. Duplicate or zero IDs are not normalizable.
    std::map<std::uint64_t, std::uint64_t> labels;
    std::vector<bool> seen;
    if (exact)
        seen.resize(state_->orders.size());
    for (std::size_t i = 0; i < state_->orders.size(); ++i) {
        const auto &entry = state_->orders[i];
        const auto id = entry.internal;
        if (id == 0 || !labels.emplace(id, static_cast<std::uint64_t>(i) + 1).second)
            throw std::logic_error("invalid or duplicate internal order ID");
        if (exact && (!state_->sessions.contains(entry.session) || id > exact_last))
            throw std::logic_error("invalid order owner or allocator high-water");
    }
    const auto label = [&](std::uint64_t id) -> std::uint64_t {
        if (id == 0)
            return 0;
        const auto it = labels.find(id);
        if (it == labels.end())
            throw std::logic_error("unknown internal order ID");
        return exact ? id : it->second;
    };
    add(1);
    add(state_->command);
    add(state_->event);
    add(state_->initial_feed_sequence);
    add(state_->sessions.size());
    for (const auto &[id, session] : state_->sessions) {
        add(id);
        add(session.killed);
        const auto &r = session.limits;
        add(r.max_order_quantity);
        add(r.max_order_notional);
        add(r.max_open_notional);
        add(r.max_open_orders);
        add(r.max_abs_position);
        add(static_cast<std::uint64_t>(r.max_market_price));
        add(r.allowed_symbols.size());
        for (const auto s : r.allowed_symbols)
            add(s);
        add(session.requests.size());
        for (const auto request : session.requests)
            add(request);
        add(session.positions.size());
        for (const auto &[symbol, pos] : session.positions) {
            add(symbol);
            add(static_cast<std::uint64_t>(pos));
        }
        add(session.reports.size());
        for (const auto &report : session.reports) {
            add(report.sequence);
            add(report.command_sequence);
            add(report.request_id);
            add(report.client_order_id);
            add(label(report.internal_order_id));
            add(report.trade_id);
            add(report.symbol);
            add(static_cast<std::uint64_t>(report.price));
            add(report.quantity);
            add(report.remaining);
            add(static_cast<std::uint64_t>(report.kind));
        }
        add(session.subscription_generation);
        add(session.subscriptions.size());
        for (const auto &[symbol, sub] : session.subscriptions) {
            add(symbol);
            add(sub.generation);
            add(sub.gap_sequence);
            add(sub.gap);
            add(sub.gap_sent);
            add(sub.events.size());
            for (const auto &e : sub.events) {
                add(e.symbol);
                add(e.sequence);
                add(e.generation);
                add(static_cast<std::uint64_t>(e.kind));
                add(static_cast<std::uint64_t>(e.bid_price));
                add(static_cast<std::uint64_t>(e.ask_price));
                add(static_cast<std::uint64_t>(e.trade_price));
                add(e.bid_quantity);
                add(e.ask_quantity);
                add(e.trade_quantity);
            }
        }
    }
    add(state_->orders.size());
    for (const auto &order : state_->orders) {
        if (exact)
            add(order.internal);
        add(order.session);
        add(order.client);
        add(order.request);
        add(order.symbol);
        add(order.open);
    }
    add(state_->trades.size());
    for (const auto &t : state_->trades) {
        add(t.command_sequence);
        add(t.trade_id);
        add(t.buyer_session);
        add(t.seller_session);
        add(label(t.buy_internal_id));
        add(label(t.sell_internal_id));
        add(t.symbol);
        add(static_cast<std::uint64_t>(t.price));
        add(t.quantity);
    }
    add(state_->feed_sequences.size());
    for (const auto &[symbol, sequence] : state_->feed_sequences) {
        add(symbol);
        add(sequence);
    }
    add(state_->engines.size());
    for (const auto &[symbol, engine] : state_->engines) {
        add(symbol);
        const auto counters = engine->counters();
        add(counters.command);
        add(counters.event);
        add(counters.trade);
        for (const auto side : {core::Side::Buy, core::Side::Sell}) {
            const auto levels = engine->depth(side, max_words / 4 + 1);
            if (levels.size() > (max_words - out.size()) / 4)
                throw std::length_error("verification levels exceed word limit");
            add(levels.size());
            for (const auto &level : levels) {
                add(static_cast<std::uint64_t>(level.price));
                add(level.quantity);
                add(level.order_count);
                if (level.order_count > (max_words - out.size()) / 3)
                    throw std::length_error("verification orders exceed word limit");
                const auto orders = engine->orders_at(side, level.price);
                if (exact && orders.size() != level.order_count)
                    throw std::logic_error("invalid level order count");
                add(orders.size());
                core::Quantity total = 0;
                for (const auto &order : orders) {
                    if (exact) {
                        const auto owner = labels.find(order.id.value());
                        if (owner == labels.end())
                            throw std::logic_error("book order missing from ownership history");
                        const auto index = static_cast<std::size_t>(owner->second - 1);
                        const auto &entry = state_->orders[index];
                        const auto indexed = engine->find(order.id);
                        if (seen[index] || !entry.open || entry.symbol != symbol || !indexed ||
                            indexed->id != order.id || indexed->symbol != order.symbol ||
                            indexed->side != side || indexed->price != level.price ||
                            indexed->quantity != order.quantity ||
                            indexed->sequence != order.sequence || order.symbol.value() != symbol ||
                            order.side != side || order.price != level.price ||
                            order.quantity == 0 ||
                            order.quantity > std::numeric_limits<core::Quantity>::max() - total)
                            throw std::logic_error("book ownership or lookup index mismatch");
                        seen[index] = true;
                        total += order.quantity;
                    }
                    add(label(order.id.value()));
                    add(order.quantity);
                    add(order.sequence);
                }
                if (exact && total != level.quantity)
                    throw std::logic_error("book aggregate mismatch");
            }
        }
    }
    if (exact) {
        for (std::size_t i = 0; i < state_->orders.size(); ++i)
            if (state_->orders[i].open != seen[i])
                throw std::logic_error("open order missing from book");
        add(1);
        add(max_sessions_);
        add(max_orders_);
        add(max_reports_);
        add(max_requests_);
        add(exact_last);
        for (const auto &[symbol, engine] : state_->engines) {
            add(symbol);
            add(engine->book_revision());
            add(static_cast<std::uint64_t>(engine->transaction_mode()));
        }
        if (out.size() != minimum)
            throw std::logic_error("book shape or index count mismatch");
    }
    return out;
}
} // namespace chronos::exchange
