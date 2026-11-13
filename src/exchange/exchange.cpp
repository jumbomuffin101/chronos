#include "chronos/exchange/exchange.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
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
    struct Session {
        std::vector<std::uint64_t> requests;
        std::vector<Report> reports;
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
    std::uint64_t command = 0, event = 0;

    std::unique_ptr<State> clone() const {
        auto out = std::make_unique<State>();
        out->sessions = sessions;
        out->orders = orders;
        out->trades = trades;
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
};

static_assert(std::is_nothrow_move_constructible_v<Report>);
static_assert(std::is_nothrow_destructible_v<Report>);

Exchange::Exchange(std::size_t sessions, std::size_t orders, std::size_t reports,
                   std::size_t requests)
    : state_(std::make_unique<State>()), max_sessions_(sessions), max_orders_(orders),
      max_reports_(reports), max_requests_(requests) {}
Exchange::~Exchange() = default;
bool Exchange::open_session(std::uint64_t session) {
    if (session == 0 || state_->sessions.size() >= max_sessions_ ||
        state_->sessions.contains(session))
        return false;
    return state_->sessions.try_emplace(session).second;
}
void Exchange::close_session(std::uint64_t session) noexcept {
    const auto it = state_->sessions.find(session);
    if (it == state_->sessions.end() || !it->second.reports.empty())
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
    if (s->second.requests.size() >= max_requests_ || state_->orders.size() >= max_orders_)
        return Result::Capacity;
    if (state_->command == std::numeric_limits<std::uint64_t>::max())
        return Result::CounterExhausted;
    std::lock_guard lock{id_mutex};
    if (last_internal_id == std::numeric_limits<std::uint64_t>::max())
        return Result::CounterExhausted;
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
} // namespace chronos::exchange
