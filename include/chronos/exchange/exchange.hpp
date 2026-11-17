#pragma once

#include "chronos/exchange/allocation_registry.hpp"
#include "chronos/exchange/order_id_allocator.hpp"
#include "chronos/matching/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace chronos::exchange {
class LiveExchangeOwner;

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
    CounterExhausted,
    RiskKilled,
    RiskSymbol,
    RiskQuantity,
    RiskNotional,
    RiskOpenOrders,
    RiskPosition,
    RiskMarketBound,
    RiskOverflow
};
struct RiskLimits {
    std::uint64_t max_order_quantity =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t max_order_notional = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_open_notional = std::numeric_limits<std::uint64_t>::max();
    std::size_t max_open_orders = 1024;
    std::uint64_t max_abs_position =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    core::Price max_market_price = 1'000'000;
    // Empty means every symbol in this local development policy.
    std::vector<std::uint32_t> allowed_symbols;
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
enum class MarketKind : std::uint8_t { Snapshot, Book, Trade, Gap };
struct MarketEvent {
    std::uint32_t symbol;
    std::uint64_t sequence;
    std::uint64_t generation; // local transport token, not a public identifier
    MarketKind kind;
    core::Price bid_price, ask_price, trade_price;
    core::Quantity bid_quantity, ask_quantity, trade_quantity;
};

class Exchange {
  public:
    explicit Exchange(std::size_t max_sessions = 16, std::size_t max_orders = 1024,
                      std::size_t max_reports_per_session = 128,
                      std::size_t max_requests_per_session = 1024,
                      std::uint64_t initial_feed_sequence = 0);
    ~Exchange();
    Exchange(const Exchange &) = delete;
    Exchange &operator=(const Exchange &) = delete;
    // Private IDs are unique only within this instance. The returned exchange
    // cannot be attached to Gateway; this is not a publication mechanism.
    [[nodiscard]] static std::unique_ptr<Exchange>
    isolated(std::size_t max_sessions = 16, std::size_t max_orders = 1024,
             std::size_t max_reports_per_session = 128, std::size_t max_requests_per_session = 1024,
             std::uint64_t initial_feed_sequence = 0, std::uint64_t last_private_id = 0);
    [[nodiscard]] bool is_isolated() const noexcept { return isolated_; }
    [[nodiscard]] bool open_session(std::uint64_t session);
    // Configure only when no orders are open; kill state can change at any time.
    [[nodiscard]] bool configure_risk(std::uint64_t session, const RiskLimits &limits);
    [[nodiscard]] bool set_killed(std::uint64_t session, bool killed) noexcept;
    [[nodiscard]] std::optional<std::int64_t> position(std::uint64_t session,
                                                       core::SymbolId symbol) const noexcept;
    [[nodiscard]] bool subscribe(std::uint64_t session, core::SymbolId symbol);
    [[nodiscard]] bool unsubscribe(std::uint64_t session, core::SymbolId symbol);
    [[nodiscard]] std::optional<MarketEvent> market_front(std::uint64_t session) const;
    void market_pop(std::uint64_t session, const MarketEvent &sent) noexcept;
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
    // Owner-thread-only, read-only canonical verification words. Internal order
    // IDs are replaced by the order's 1-based history ordinal. Not a recovery API.
    [[nodiscard]] std::vector<std::uint64_t> verification_state() const;
    // As above, but rejects states exceeding max_words before allocating an
    // unbounded output or ID-normalization table. Not a restoration format.
    [[nodiscard]] std::vector<std::uint64_t>
    verification_state_bounded(std::size_t max_words) const;
    // Exact IDs and continuation metadata, only for owner-thread isolated
    // exchanges. This is not a live exchange import or publication interface.
    [[nodiscard]] std::vector<std::uint64_t>
    isolated_continuation_state(std::size_t max_words) const;

  private:
    friend class LiveExchangeOwner;
    struct IsolatedTag {};
    struct LiveTag {};
    [[nodiscard]] std::vector<std::uint64_t> state_words(std::size_t max_words, bool exact) const;
    Exchange(IsolatedTag, std::size_t sessions, std::size_t orders, std::size_t reports,
             std::size_t requests, std::uint64_t initial_feed_sequence, std::uint64_t last_id);
    Exchange(LiveTag, std::size_t sessions, std::size_t orders, std::size_t reports,
             std::size_t requests, std::uint64_t initial_feed_sequence, AllocationMode mode);
    void release_exclusive_registration() noexcept;
    struct State;
    // Shared leases outlive state_; an exclusive owner may release at terminal stop.
    detail::AllocationRegistry::Lease registration_;
    std::unique_ptr<State> state_;
    std::size_t max_sessions_, max_orders_, max_reports_, max_requests_;
    detail::OrderIdAllocator private_allocator_{false};
    detail::OrderIdAllocator *allocator_;
    bool isolated_ = false;
    bool exclusive_ = false;
};
} // namespace chronos::exchange
