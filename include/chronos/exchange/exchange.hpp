#pragma once

#include "chronos/exchange/allocation_registry.hpp"
#include "chronos/exchange/canonical_command.hpp"
#include "chronos/exchange/order_id_allocator.hpp"
#include "chronos/matching/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace chronos::exchange {
class LiveExchangeOwner;
class RestrictedReplay;
#ifdef CHRONOS_TEST_ID_SEAM
namespace detail {
// Linked only in the isolated allocator/capture test executable.
inline thread_local void (*before_capture_allocation_for_test)() = nullptr;
} // namespace detail
#endif

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

struct RecipientReport {
    std::uint64_t session;
    Report report;
};

// Derived from open resting orders, not a persisted reservation ledger.
struct ExposureSnapshot {
    std::int64_t position = 0;
    std::uint64_t open_order_count = 0;
    std::uint64_t open_notional = 0;
    std::uint64_t symbol_buy_quantity = 0;
    std::uint64_t symbol_sell_quantity = 0;
};

// Scalar limits and permission for this command's symbol. The full allowed
// symbol list is not included, so this is not a complete policy snapshot.
struct RiskPolicySnapshot {
    std::uint64_t max_order_quantity;
    std::uint64_t max_order_notional;
    std::uint64_t max_open_notional;
    std::size_t max_open_orders;
    std::uint64_t max_abs_position;
    core::Price max_market_price;
    bool killed;
    bool symbol_allowed;
};

struct ParticipantEffect {
    std::uint64_t session;
    ExposureSnapshot before;
    ExposureSnapshot after;
    RiskPolicySnapshot policy;
};

struct OwnershipSnapshot {
    std::uint64_t session;
    std::uint64_t client_order_id;
    std::uint64_t request_id;
    std::uint32_t symbol;
    bool open;
    std::optional<orderbook::OrderSnapshot> resting;
};

struct OwnershipEffect {
    std::uint64_t internal_order_id;
    std::optional<OwnershipSnapshot> before;
    std::optional<OwnershipSnapshot> after;
};

struct SymbolCounterSnapshot {
    std::uint32_t symbol;
    bool engine_present;
    matching::Counters matching;
    std::uint64_t book_revision;
    bool feed_present;
    std::uint64_t feed_sequence;
};

// A post-install snapshot, not a prepared handle or a complete journal outcome.
// Subscriber delivery effects and verification metadata remain unavailable.
class InstalledSummary final {
  public:
    [[nodiscard]] Result result() const noexcept { return result_; }
    [[nodiscard]] bool installed() const noexcept { return installed_; }
    [[nodiscard]] std::optional<std::uint64_t> new_order_id() const noexcept { return new_id_; }
    [[nodiscard]] std::uint64_t command_before() const noexcept { return command_before_; }
    [[nodiscard]] std::uint64_t command_after() const noexcept { return command_after_; }
    [[nodiscard]] std::uint64_t event_before() const noexcept { return event_before_; }
    [[nodiscard]] std::uint64_t event_after() const noexcept { return event_after_; }
    [[nodiscard]] const std::optional<CanonicalCommand> &command() const noexcept {
        return command_;
    }
    [[nodiscard]] const std::optional<SymbolCounterSnapshot> &symbol_before() const noexcept {
        return symbol_before_;
    }
    [[nodiscard]] const std::optional<SymbolCounterSnapshot> &symbol_after() const noexcept {
        return symbol_after_;
    }
    [[nodiscard]] const std::optional<std::uint64_t> &allocator_before() const noexcept {
        return allocator_before_;
    }
    [[nodiscard]] const std::optional<std::uint64_t> &allocator_after() const noexcept {
        return allocator_after_;
    }
    [[nodiscard]] const std::optional<Report> &initiator_final_report() const noexcept {
        return initiator_final_report_;
    }
    [[nodiscard]] const std::optional<std::int64_t> &initiator_position_before() const noexcept {
        return initiator_position_before_;
    }
    [[nodiscard]] const std::optional<std::int64_t> &initiator_position_after() const noexcept {
        return initiator_position_after_;
    }
    [[nodiscard]] std::span<const AuditTrade> trades() const noexcept { return trades_; }
    [[nodiscard]] std::span<const RecipientReport> reports() const noexcept { return reports_; }
    [[nodiscard]] std::span<const MarketEvent> public_events() const noexcept {
        return public_events_;
    }
    // Sorted by session; includes the initiator and every trade participant.
    [[nodiscard]] std::span<const ParticipantEffect> participant_effects() const noexcept {
        return participant_effects_;
    }
    // Sorted by internal ID; only orders referenced by this command's reports.
    [[nodiscard]] std::span<const OwnershipEffect> ownership_effects() const noexcept {
        return ownership_effects_;
    }

  private:
    friend class Exchange;
    friend class LiveExchangeOwner;
    Result result_ = Result::InvalidOrder;
    bool installed_ = false;
    std::optional<std::uint64_t> new_id_;
    std::optional<CanonicalCommand> command_;
    std::optional<SymbolCounterSnapshot> symbol_before_, symbol_after_;
    std::optional<std::uint64_t> allocator_before_, allocator_after_;
    std::optional<Report> initiator_final_report_;
    std::optional<std::int64_t> initiator_position_before_, initiator_position_after_;
    std::uint64_t command_before_ = 0, command_after_ = 0, event_before_ = 0, event_after_ = 0;
    std::vector<AuditTrade> trades_;
    std::vector<RecipientReport> reports_;
    std::vector<MarketEvent> public_events_;
    std::vector<ParticipantEffect> participant_effects_;
    std::vector<OwnershipEffect> ownership_effects_;
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
    // Versioned exact verification bytes for isolated exchanges without public
    // subscriptions. Not a live import or durability interface.
    [[nodiscard]] std::vector<std::uint8_t>
    canonical_isolated_state(std::size_t max_bytes = 65536) const;

  private:
    friend class LiveExchangeOwner;
    friend class RestrictedReplay;
    struct IsolatedTag {};
    struct LiveTag {};
    [[nodiscard]] std::vector<std::uint64_t> state_words(std::size_t max_words, bool exact,
                                                         std::uint64_t exact_last) const;
    [[nodiscard]] std::vector<std::uint64_t> live_continuation_state(std::size_t max_words) const;
#ifdef CHRONOS_TEST_ID_SEAM
    [[nodiscard]] bool seed_process_id_for_test(std::uint64_t last);
#endif
    Exchange(IsolatedTag, std::size_t sessions, std::size_t orders, std::size_t reports,
             std::size_t requests, std::uint64_t initial_feed_sequence, std::uint64_t last_id);
    Exchange(LiveTag, std::size_t sessions, std::size_t orders, std::size_t reports,
             std::size_t requests, std::uint64_t initial_feed_sequence, AllocationMode mode);
    void release_exclusive_registration() noexcept;
    [[nodiscard]] std::unique_ptr<Exchange> clone_restricted_isolated() const;
    struct State;
    struct Prepared {
        Prepared();
        ~Prepared();
        Prepared(const Prepared &) = delete;
        Prepared &operator=(const Prepared &) = delete;
        Result result = Result::InvalidOrder;
        std::unique_ptr<State> candidate;
        std::unique_ptr<detail::OrderIdAllocator::Reservation> reservation;
        std::vector<MarketEvent> public_events;
        std::optional<CanonicalCommand> command;
        std::optional<std::uint32_t> symbol;
        std::optional<std::uint64_t> allocator_before;
        std::uint64_t session = 0, client = 0, request = 0;
    };
    [[nodiscard]] std::unique_ptr<Prepared>
    prepare_submit(std::uint64_t session, std::uint64_t request, std::uint64_t client,
                   core::SymbolId symbol, core::Side side, core::OrderType type, core::Price price,
                   core::Quantity quantity, bool capture_public = false);
    [[nodiscard]] std::unique_ptr<Prepared> prepare_cancel(std::uint64_t session,
                                                           std::uint64_t request,
                                                           std::uint64_t client,
                                                           bool capture_public = false);
    [[nodiscard]] std::unique_ptr<Prepared> prepare_open_session(std::uint64_t session);
    [[nodiscard]] std::unique_ptr<Prepared> prepare_close_session(std::uint64_t session);
    [[nodiscard]] std::unique_ptr<Prepared> prepare_risk_update(std::uint64_t session,
                                                                const RiskLimits &limits);
    [[nodiscard]] InstalledSummary summarize_prepared(Prepared &prepared) const;
    void install_prepared(Prepared &prepared) noexcept;
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
