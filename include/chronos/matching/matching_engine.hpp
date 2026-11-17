#pragma once

#include "chronos/core/trade.hpp"
#include "chronos/orderbook/order_book.hpp"

#include <memory>
#include <vector>

namespace chronos::matching {

struct Request {
    core::OrderId id;
    core::SymbolId symbol;
    core::Side side;
    core::OrderType type;
    core::Price price;
    core::Quantity quantity;
};

enum class Error {
    None,
    WrongSymbol,
    InvalidSide,
    InvalidType,
    InvalidPrice,
    InvalidQuantity,
    DuplicateId,
    CounterExhausted,
    BookOverflow,
    InternalError
};
enum class ReportKind {
    Rejected,
    Accepted,
    Resting,
    PartialFill,
    Filled,
    MarketRemainderCancelled
};
enum class Participant { Incoming, Resting };
struct ExecutionReport {
    core::OrderId order_id;
    Participant participant;
    ReportKind kind;
    core::Quantity remaining;
    core::SequenceNumber sequence;
};
struct Outcome {
    Error error = Error::None;
    core::SequenceNumber command_sequence = 0; // Zero on rejection.
    core::Quantity remaining = 0;
    std::vector<core::Trade> trades;
    std::vector<ExecutionReport> reports;
};
// Values are the last issued numbers; useful for restoring counters or boundary tests.
struct Counters {
    std::uint64_t command = 0;
    std::uint64_t event = 0;
    std::uint64_t trade = 0;
};

enum class TransactionMode { Planned, CloneReference };

class MatchingEngine {
  public:
    explicit MatchingEngine(core::SymbolId symbol, Counters counters = {},
                            TransactionMode mode = TransactionMode::Planned);
    [[nodiscard]] Outcome submit(const Request &request);
    // Book maintenance only; these do not emit exchange-level cancel/replace reports.
    [[nodiscard]] orderbook::Result cancel_resting(core::OrderId id);
    [[nodiscard]] orderbook::Result modify_resting(core::OrderId id, core::Price price,
                                                   core::Quantity quantity);
    [[nodiscard]] std::optional<orderbook::OrderSnapshot> find(core::OrderId id) const;
    [[nodiscard]] std::vector<orderbook::LevelSnapshot> depth(core::Side side,
                                                              std::size_t limit) const;
    [[nodiscard]] std::vector<orderbook::OrderSnapshot> orders_at(core::Side side,
                                                                  core::Price price) const;
    [[nodiscard]] Counters counters() const noexcept { return counters_; }
    [[nodiscard]] std::uint64_t book_revision() const noexcept { return book_->revision_; }
    [[nodiscard]] TransactionMode transaction_mode() const noexcept { return mode_; }
    // Read-only book shape for bounded inspection; does not allocate.
    [[nodiscard]] std::size_t resting_order_count() const noexcept { return book_->order_count(); }
    [[nodiscard]] std::size_t price_level_count(core::Side side) const noexcept {
        return side == core::Side::Buy ? book_->bid_levels() : book_->ask_levels();
    }
    // Independent snapshot for an enclosing transaction. Allocation failure leaves
    // the source engine unchanged; the returned book has its own identity.
    [[nodiscard]] std::unique_ptr<MatchingEngine> clone() const;

  private:
    [[nodiscard]] Outcome submit_cloned(const Request &request);
    [[nodiscard]] Outcome submit_planned(const Request &request);
    core::SymbolId symbol_;
    std::unique_ptr<orderbook::OrderBook> book_;
    Counters counters_;
    TransactionMode mode_;
};

} // namespace chronos::matching
