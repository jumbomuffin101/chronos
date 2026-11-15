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
std::mutex id_mutex;
std::uint64_t last_internal_id = 0;
} // namespace
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

    std::unique_ptr<State> clone() const {
        auto out = std::make_unique<State>();
        out->sessions = sessions;
        out->orders = orders;
        out->trades = trades;
        out->feed_sequences = feed_sequences;
        out->initial_feed_sequence = initial_feed_sequence;
        out->command = command;
        out->event = event;
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

Exchange::Exchange(std::size_t sessions, std::size_t orders, std::size_t reports,
                   std::size_t requests, std::uint64_t initial_feed_sequence)
    : state_(std::make_unique<State>()), max_sessions_(sessions), max_orders_(orders),
      max_reports_(reports), max_requests_(requests) {
    state_->initial_feed_sequence = initial_feed_sequence;
}
Exchange::~Exchange() = default;
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
    std::lock_guard lock{id_mutex};
    if (last_internal_id == std::numeric_limits<std::uint64_t>::max())
        return Result::CounterExhausted;
    const auto before_book = state_->top(symbol.value(), MarketKind::Book, 0);
    auto next = state_->clone();
    auto &engine = next->engines[symbol.value()];
    if (!engine)
        engine = std::make_unique<matching::MatchingEngine>(symbol);
    const auto internal_id = last_internal_id + 1;
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
    last_internal_id = internal_id;
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
    std::vector<std::uint64_t> out;
    const auto add = [&](std::uint64_t v) { out.push_back(v); };
    // Preserve identity relationships while allowing process-wide IDs to differ
    // between recording and replay. Duplicate or zero IDs are not normalizable.
    std::map<std::uint64_t, std::uint64_t> labels;
    for (std::size_t i = 0; i < state_->orders.size(); ++i) {
        const auto id = state_->orders[i].internal;
        if (id == 0 || !labels.emplace(id, static_cast<std::uint64_t>(i) + 1).second)
            throw std::logic_error("invalid or duplicate internal order ID");
    }
    const auto label = [&](std::uint64_t id) -> std::uint64_t {
        if (id == 0)
            return 0;
        const auto it = labels.find(id);
        if (it == labels.end())
            throw std::logic_error("unknown internal order ID");
        return it->second;
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
            const auto levels = engine->depth(side, std::numeric_limits<std::size_t>::max());
            add(levels.size());
            for (const auto &level : levels) {
                add(static_cast<std::uint64_t>(level.price));
                add(level.quantity);
                add(level.order_count);
                const auto orders = engine->orders_at(side, level.price);
                add(orders.size());
                for (const auto &order : orders) {
                    add(label(order.id.value()));
                    add(order.quantity);
                    add(order.sequence);
                }
            }
        }
    }
    return out;
}
} // namespace chronos::exchange
