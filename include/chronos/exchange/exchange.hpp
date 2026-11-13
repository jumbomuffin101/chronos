#pragma once

#include "chronos/matching/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace chronos::exchange {

// Prototype owner-thread API. All calls, including reads, must be serialized.
enum class Result {
    Accepted,
    UnknownSession,
    DuplicateRequest,
    DuplicateOrder,
    NotOwner,
    NotFound,
    InvalidOrder,
    Capacity,
    CounterExhausted
};
enum class ReportKind { Accepted, PartialFill, Filled, Resting, Cancelled, MarketRemainder };
struct Report {
    std::uint64_t sequence;
    std::uint64_t command_sequence;
    std::uint64_t request_id;
    std::uint64_t client_order_id;
    std::uint64_t internal_order_id;
    std::uint64_t trade_id;
    std::uint32_t symbol;
    core::Price price;
    core::Quantity quantity;
    core::Quantity remaining;
    ReportKind kind;
};
struct AuditTrade {
    std::uint64_t command_sequence, trade_id, buyer_session, seller_session;
    std::uint64_t buy_internal_id, sell_internal_id;
    std::uint32_t symbol;
    core::Price price;
    core::Quantity quantity;
};

class Exchange {
  public:
    explicit Exchange(std::size_t max_sessions = 16, std::size_t max_orders = 1024,
                      std::size_t max_reports_per_session = 128,
                      std::size_t max_requests_per_session = 1024);
    ~Exchange();
    Exchange(const Exchange &) = delete;
    Exchange &operator=(const Exchange &) = delete;
    [[nodiscard]] bool open_session(std::uint64_t session);
    // Retain sessions with order history or pending reports; their state cannot
    // be safely recycled without a recovery policy.
    void close_session(std::uint64_t session) noexcept;
    [[nodiscard]] Result submit(std::uint64_t session, std::uint64_t request_id,
                                std::uint64_t client_order_id, core::SymbolId symbol,
                                core::Side side, core::OrderType type, core::Price price,
                                core::Quantity quantity);
    [[nodiscard]] Result cancel(std::uint64_t session, std::uint64_t request_id,
                                std::uint64_t client_order_id);
    [[nodiscard]] std::optional<Report> front(std::uint64_t session) const;
    void pop(std::uint64_t session) noexcept;
    [[nodiscard]] std::size_t trade_count() const noexcept;
    [[nodiscard]] std::optional<AuditTrade> trade(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<orderbook::OrderSnapshot> find(std::uint64_t session,
                                                               std::uint64_t client_order_id) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
    std::size_t max_sessions_, max_orders_, max_reports_, max_requests_;
};
} // namespace chronos::exchange
