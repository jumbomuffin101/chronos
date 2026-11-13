#include "chronos/matching/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace chronos::matching {

namespace {
// The commit and return path must remain nonthrowing if these types evolve.
static_assert(std::is_nothrow_swappable_v<std::unique_ptr<orderbook::OrderBook>>);
static_assert(std::is_nothrow_copy_assignable_v<Counters>);
static_assert(std::is_nothrow_move_constructible_v<Outcome>);
static_assert(std::is_nothrow_destructible_v<orderbook::OrderBook>);

bool advance(std::uint64_t &value) {
    if (value == std::numeric_limits<std::uint64_t>::max())
        return false;
    ++value;
    return true;
}
} // namespace

MatchingEngine::MatchingEngine(core::SymbolId symbol, Counters counters, TransactionMode mode)
    : symbol_(symbol), book_(std::make_unique<orderbook::OrderBook>(symbol)), counters_(counters),
      mode_(mode) {}

std::unique_ptr<MatchingEngine> MatchingEngine::clone() const {
    auto copy = std::make_unique<MatchingEngine>(symbol_, counters_, mode_);
    copy->book_ = book_->clone();
    return copy;
}

Outcome MatchingEngine::submit(const Request &request) {
    return mode_ == TransactionMode::CloneReference ? submit_cloned(request)
                                                    : submit_planned(request);
}

Outcome MatchingEngine::submit_cloned(const Request &request) {
    using namespace core;
    const auto rejected = [&](Error error) {
        Outcome result;
        result.error = error;
        result.remaining = request.quantity;
        result.reports.push_back(
            {request.id, Participant::Incoming, ReportKind::Rejected, request.quantity, 0});
        return result;
    };
    if (request.symbol != symbol_)
        return rejected(Error::WrongSymbol);
    if (request.side != Side::Buy && request.side != Side::Sell)
        return rejected(Error::InvalidSide);
    if (request.type != OrderType::Limit && request.type != OrderType::Market)
        return rejected(Error::InvalidType);
    if ((request.type == OrderType::Limit && request.price <= 0) ||
        (request.type == OrderType::Market && request.price != 0))
        return rejected(Error::InvalidPrice);
    if (request.quantity == 0)
        return rejected(Error::InvalidQuantity);
    if (book_->find(request.id))
        return rejected(Error::DuplicateId);

    Counters next = counters_;
    if (!advance(next.command))
        return rejected(Error::CounterExhausted);
    auto working = book_->clone();
    Outcome out;
    out.command_sequence = next.command;
    out.remaining = request.quantity;
    auto report = [&](OrderId id, Participant who, ReportKind kind, Quantity remaining) {
        if (!advance(next.event))
            return false;
        out.reports.push_back(ExecutionReport{id, who, kind, remaining, next.event});
        return true;
    };
    if (!report(request.id, Participant::Incoming, ReportKind::Accepted, out.remaining))
        return rejected(Error::CounterExhausted);
    const Side opposite = request.side == Side::Buy ? Side::Sell : Side::Buy;
    while (out.remaining != 0) {
        const auto selected = working->best_resting(opposite);
        if (!selected)
            break;
        const auto resting = selected->order();
        if (request.type == OrderType::Limit &&
            ((request.side == Side::Buy && request.price < resting.price) ||
             (request.side == Side::Sell && request.price > resting.price)))
            break;
        const Quantity quantity = std::min(out.remaining, resting.quantity);
        if (!advance(next.trade) || !advance(next.event))
            return rejected(Error::CounterExhausted);
        const auto buy = request.side == Side::Buy ? request.id : resting.id;
        const auto sell = request.side == Side::Sell ? request.id : resting.id;
        out.trades.push_back(
            Trade{TradeId{next.trade}, symbol_, buy, sell, resting.price, quantity, next.event});
        const auto filled = working->apply_fill(*selected, quantity);
        if (filled != orderbook::Result::Success)
            return rejected(filled == orderbook::Result::Overflow ? Error::CounterExhausted
                                                                  : Error::InternalError);
        out.remaining -= quantity;
        const Quantity resting_remaining = resting.quantity - quantity;
        if (!report(resting.id, Participant::Resting,
                    resting_remaining == 0 ? ReportKind::Filled : ReportKind::PartialFill,
                    resting_remaining) ||
            !report(request.id, Participant::Incoming,
                    out.remaining == 0 ? ReportKind::Filled : ReportKind::PartialFill,
                    out.remaining))
            return rejected(Error::CounterExhausted);
    }
    if (out.remaining != 0 && request.type == OrderType::Limit) {
        Order remainder{request.id,    request.symbol, request.side, OrderType::Limit,
                        request.price, out.remaining,  next.command};
        const auto added = working->add(remainder);
        if (added != orderbook::Result::Success)
            return rejected(added == orderbook::Result::Overflow ? Error::BookOverflow
                                                                 : Error::InternalError);
        if (!report(request.id, Participant::Incoming, ReportKind::Resting, out.remaining))
            return rejected(Error::CounterExhausted);
    } else if (out.remaining != 0) {
        if (!report(request.id, Participant::Incoming, ReportKind::MarketRemainderCancelled,
                    out.remaining))
            return rejected(Error::CounterExhausted);
    }
    book_.swap(working);
    counters_ = next;
    return out;
}

Outcome MatchingEngine::submit_planned(const Request &request) {
    using namespace core;
    const auto rejected = [&](Error error) {
        Outcome result;
        result.error = error;
        result.remaining = request.quantity;
        result.reports.push_back(
            {request.id, Participant::Incoming, ReportKind::Rejected, request.quantity, 0});
        return result;
    };
    if (request.symbol != symbol_)
        return rejected(Error::WrongSymbol);
    if (request.side != Side::Buy && request.side != Side::Sell)
        return rejected(Error::InvalidSide);
    if (request.type != OrderType::Limit && request.type != OrderType::Market)
        return rejected(Error::InvalidType);
    if ((request.type == OrderType::Limit && request.price <= 0) ||
        (request.type == OrderType::Market && request.price != 0))
        return rejected(Error::InvalidPrice);
    if (request.quantity == 0)
        return rejected(Error::InvalidQuantity);
    if (book_->find(request.id))
        return rejected(Error::DuplicateId);

    Counters next = counters_;
    if (!advance(next.command))
        return rejected(Error::CounterExhausted);
    Outcome out;
    out.command_sequence = next.command;
    out.remaining = request.quantity;
    const auto report = [&](OrderId id, Participant who, ReportKind kind, Quantity remaining) {
        if (!advance(next.event))
            return false;
        out.reports.push_back({id, who, kind, remaining, next.event});
        return true;
    };
    if (!report(request.id, Participant::Incoming, ReportKind::Accepted, out.remaining))
        return rejected(Error::CounterExhausted);
    const Side opposite = request.side == Side::Buy ? Side::Sell : Side::Buy;
    bool exhausted = false;
    book_->visit_resting(opposite, [&](const orderbook::OrderSnapshot &resting) {
        if (out.remaining == 0 || (request.type == OrderType::Limit &&
                                   ((request.side == Side::Buy && request.price < resting.price) ||
                                    (request.side == Side::Sell && request.price > resting.price))))
            return false;
        const Quantity quantity = std::min(out.remaining, resting.quantity);
        if (!advance(next.trade) || !advance(next.event)) {
            exhausted = true;
            return false;
        }
        const auto buy = request.side == Side::Buy ? request.id : resting.id;
        const auto sell = request.side == Side::Sell ? request.id : resting.id;
        out.trades.push_back(
            Trade{TradeId{next.trade}, symbol_, buy, sell, resting.price, quantity, next.event});
        out.remaining -= quantity;
        const Quantity resting_remaining = resting.quantity - quantity;
        if (!report(resting.id, Participant::Resting,
                    resting_remaining == 0 ? ReportKind::Filled : ReportKind::PartialFill,
                    resting_remaining) ||
            !report(request.id, Participant::Incoming,
                    out.remaining == 0 ? ReportKind::Filled : ReportKind::PartialFill,
                    out.remaining)) {
            exhausted = true;
            return false;
        }
        return true;
    });
    if (exhausted)
        return rejected(Error::CounterExhausted);
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    const auto fills = static_cast<std::uint64_t>(out.trades.size());
    const auto available = std::numeric_limits<std::uint64_t>::max() - book_->revision_;
    if (fills > available)
        return rejected(Error::CounterExhausted);
    const bool rest = out.remaining != 0 && request.type == OrderType::Limit;
    if (rest) {
        if (fills == available)
            return rejected(Error::BookOverflow);
        const auto level = book_->level(request.side, request.price);
        if (level && out.remaining > std::numeric_limits<Quantity>::max() - level->quantity)
            return rejected(Error::BookOverflow);
        if (!report(request.id, Participant::Incoming, ReportKind::Resting, out.remaining))
            return rejected(Error::CounterExhausted);
    } else if (out.remaining != 0) {
        if (!report(request.id, Participant::Incoming, ReportKind::MarketRemainderCancelled,
                    out.remaining))
            return rejected(Error::CounterExhausted);
    }

    // Insertion is the first mutation and has a strong exception guarantee. All
    // potentially throwing work and all semantic checks precede its success.
    if (rest) {
        const Order remainder{request.id,    request.symbol, request.side, OrderType::Limit,
                              request.price, out.remaining,  next.command};
        const auto result = book_->add(remainder);
        if (result != orderbook::Result::Success)
            return rejected(result == orderbook::Result::Overflow ? Error::BookOverflow
                                                                  : Error::InternalError);
    }
    // From the first successful insertion (or first fill) onward: no allocations,
    // callbacks, failing checks or throwing operations. The private book cannot
    // change between traversal and this loop in the serialized engine.
    for (const auto &trade : out.trades)
        book_->consume_best(opposite, trade.quantity);
    counters_ = next;
    return out;
}

std::optional<orderbook::OrderSnapshot> MatchingEngine::find(core::OrderId id) const {
    return book_->find(id);
}
orderbook::Result MatchingEngine::cancel_resting(core::OrderId id) {
    auto next = counters_;
    if (!advance(next.command))
        return orderbook::Result::Overflow;
    if (mode_ == TransactionMode::CloneReference) {
        auto working = book_->clone();
        const auto result = working->cancel(id);
        if (result == orderbook::Result::Success) {
            book_.swap(working);
            counters_ = next;
        }
        return result;
    }
    const auto result = book_->cancel(id);
    if (result == orderbook::Result::Success)
        counters_ = next;
    return result;
}
orderbook::Result MatchingEngine::modify_resting(core::OrderId id, core::Price price,
                                                 core::Quantity quantity) {
    auto next = counters_;
    if (!advance(next.command))
        return orderbook::Result::Overflow;
    if (mode_ == TransactionMode::CloneReference) {
        auto working = book_->clone();
        const auto result = working->modify(id, price, quantity);
        if (result == orderbook::Result::Success) {
            book_.swap(working);
            counters_ = next;
        }
        return result;
    }
    const auto result = book_->modify(id, price, quantity);
    if (result == orderbook::Result::Success)
        counters_ = next;
    return result;
}
std::vector<orderbook::LevelSnapshot> MatchingEngine::depth(core::Side side,
                                                            std::size_t limit) const {
    return book_->depth(side, limit);
}
std::vector<orderbook::OrderSnapshot> MatchingEngine::orders_at(core::Side side,
                                                                core::Price price) const {
    return book_->orders_at(side, price);
}

} // namespace chronos::matching
