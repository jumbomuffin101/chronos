#pragma once

#include "chronos/exchange/exchange.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace chronos::exchange {

struct PreparedScopeResult {
    Result result = Result::InvalidOrder;
    bool installed = false;
};

// Encapsulates a fresh live exchange. Binding happens once on the event-loop
// context; no mutable exchange reference or transfer operation is exposed.
class LiveExchangeOwner final {
    struct Context {};
    enum class Phase { Unbound, Binding, Active, Finishing, Stopped };
    static const std::shared_ptr<const Context> &current_context() {
        static thread_local const auto context = std::make_shared<const Context>();
        return context;
    }

  public:
    explicit LiveExchangeOwner(std::size_t sessions = 16, std::size_t orders = 1024,
                               std::size_t reports = 128, std::size_t requests = 1024,
                               AllocationMode mode = AllocationMode::Shared)
        : exchange_(Exchange::LiveTag{}, sessions, orders, reports, requests, 0, mode) {}
    LiveExchangeOwner(const LiveExchangeOwner &) = delete;
    LiveExchangeOwner &operator=(const LiveExchangeOwner &) = delete;
    LiveExchangeOwner(LiveExchangeOwner &&) = delete;
    LiveExchangeOwner &operator=(LiveExchangeOwner &&) = delete;
#ifdef CHRONOS_TEST_GATE_SEAM
    // Only the separately linked gateway-test executable defines this seam.
    static inline void (*test_before_prepare)(void *) = nullptr;
    static inline bool (*test_before_install)(void *) = nullptr;
    static inline void *test_before_install_context = nullptr;
#endif

    // The surrounding owner must keep this object alive throughout every call.
    // Bind is single-use, including after failed allocation or shutdown.
    void bind() {
        auto expected = Phase::Unbound;
        if (!phase_.compare_exchange_strong(expected, Phase::Binding, std::memory_order_acq_rel))
            throw std::logic_error("live exchange owner already bound or stopped");
        try {
            context_ = current_context();
            phase_.store(Phase::Active, std::memory_order_release);
        } catch (...) {
            exchange_.release_exclusive_registration();
            phase_.store(Phase::Stopped, std::memory_order_release);
            throw;
        }
    }
    // Called by the event loop after its last exchange access. Once stopped,
    // copied read-only inspection is permitted from other contexts.
    [[nodiscard]] bool finish() noexcept {
        if (!is_owner() || frozen_)
            return false;
        // Disable every mutation before releasing exclusive allocation authority.
        phase_.store(Phase::Finishing, std::memory_order_release);
        exchange_.release_exclusive_registration();
        phase_.store(Phase::Stopped, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool open_session(std::uint64_t session) {
        require_mutation();
        return exchange_.open_session(session);
    }
#ifdef CHRONOS_TEST_ID_SEAM
    [[nodiscard]] bool seed_process_id_for_test(std::uint64_t last) {
        require_mutation();
        return exchange_.seed_process_id_for_test(last);
    }
#endif
    [[nodiscard]] bool configure_risk(std::uint64_t session, const RiskLimits &limits) {
        require_mutation();
        return exchange_.configure_risk(session, limits);
    }
    [[nodiscard]] bool set_killed(std::uint64_t session, bool killed) {
        require_mutation();
        return exchange_.set_killed(session, killed);
    }
    [[nodiscard]] bool subscribe(std::uint64_t session, core::SymbolId symbol) {
        require_mutation();
        return exchange_.subscribe(session, symbol);
    }
    [[nodiscard]] bool unsubscribe(std::uint64_t session, core::SymbolId symbol) {
        require_mutation();
        return exchange_.unsubscribe(session, symbol);
    }
    void market_pop(std::uint64_t session, const MarketEvent &event) {
        require_mutation();
        exchange_.market_pop(session, event);
    }
    void close_session(std::uint64_t session) {
        require_mutation();
        exchange_.close_session(session);
    }
    [[nodiscard]] Result submit(std::uint64_t session, std::uint64_t request, std::uint64_t client,
                                core::SymbolId symbol, core::Side side, core::OrderType type,
                                core::Price price, core::Quantity quantity) {
        require_mutation();
        return exchange_.submit(session, request, client, symbol, side, type, price, quantity);
    }
    [[nodiscard]] Result cancel(std::uint64_t session, std::uint64_t request,
                                std::uint64_t client) {
        require_mutation();
        return exchange_.cancel(session, request, client);
    }
    // Gateway-facing non-durable authorization: no caller-supplied callback
    // runs between preparation and installation. Rejections are authoritative
    // results with no candidate installed; no persistence is implied.
    [[nodiscard]] PreparedScopeResult
    submit_prepared_non_durable(std::uint64_t session, std::uint64_t request, std::uint64_t client,
                                core::SymbolId symbol, core::Side side, core::OrderType type,
                                core::Price price, core::Quantity quantity) {
        return prepared_scope(
            [&] {
                return exchange_.prepare_submit(session, request, client, symbol, side, type, price,
                                                quantity);
            },
            authorize_non_durable);
    }
    [[nodiscard]] PreparedScopeResult cancel_prepared_non_durable(std::uint64_t session,
                                                                  std::uint64_t request,
                                                                  std::uint64_t client) {
        return prepared_scope([&] { return exchange_.prepare_cancel(session, request, client); },
                              authorize_non_durable);
    }
    // Builds a bounded partial summary before installation and returns it only
    // afterward. No callback or install capability is exposed to the caller.
    [[nodiscard]] InstalledSummary submit_with_summary(std::uint64_t session, std::uint64_t request,
                                                       std::uint64_t client, core::SymbolId symbol,
                                                       core::Side side, core::OrderType type,
                                                       core::Price price, core::Quantity quantity) {
        return summarized_scope([&] {
            return exchange_.prepare_submit(session, request, client, symbol, side, type, price,
                                            quantity, true);
        });
    }
    [[nodiscard]] InstalledSummary cancel_with_summary(std::uint64_t session, std::uint64_t request,
                                                       std::uint64_t client) {
        return summarized_scope(
            [&] { return exchange_.prepare_cancel(session, request, client, true); });
    }
    void pop(std::uint64_t session) {
        require_mutation();
        exchange_.pop(session);
    }
    [[nodiscard]] std::optional<Report> front(std::uint64_t session) const {
        require_read();
        return exchange_.front(session);
    }
    [[nodiscard]] std::optional<MarketEvent> market_front(std::uint64_t session) const {
        require_read();
        return exchange_.market_front(session);
    }
    [[nodiscard]] std::size_t trade_count() const noexcept {
        return can_read() ? exchange_.trade_count() : 0;
    }
    [[nodiscard]] std::optional<AuditTrade> trade(std::size_t index) const noexcept {
        return can_read() ? exchange_.trade(index) : std::nullopt;
    }
    [[nodiscard]] std::optional<orderbook::OrderSnapshot> find(std::uint64_t session,
                                                               std::uint64_t client) const {
        require_read();
        return exchange_.find(session, client);
    }
    [[nodiscard]] std::vector<std::uint64_t> verification_state() const {
        require_read();
        return exchange_.verification_state();
    }
    // Diagnostic exact words for an active exclusive owner. Prohibited in a
    // frozen scope because it must inspect the process allocator under lock.
    [[nodiscard]] std::vector<std::uint64_t> exact_continuation_state(std::size_t max_words) const {
        require_read();
        if (frozen_)
            throw std::logic_error("exact capture unavailable in a frozen scope");
        return exchange_.live_continuation_state(max_words);
    }

    // The decision callback is synchronous and receives no candidate state or
    // reserved ID. It returns true to install an accepted result; false aborts.
    // No prepared handle can escape the owner-held scope. This is not a
    // durable acknowledgment or gateway publication interface.
    template <class F>
    [[nodiscard]] PreparedScopeResult
    with_prepared_submit(std::uint64_t session, std::uint64_t request, std::uint64_t client,
                         core::SymbolId symbol, core::Side side, core::OrderType type,
                         core::Price price, core::Quantity quantity, F &&decision) {
        return prepared_scope(
            [&] {
                return exchange_.prepare_submit(session, request, client, symbol, side, type, price,
                                                quantity);
            },
            std::forward<F>(decision));
    }
    template <class F>
    [[nodiscard]] PreparedScopeResult with_prepared_cancel(std::uint64_t session,
                                                           std::uint64_t request,
                                                           std::uint64_t client, F &&decision) {
        return prepared_scope([&] { return exchange_.prepare_cancel(session, request, client); },
                              std::forward<F>(decision));
    }

    // A synchronous exclusion primitive only: no prepared state or durable
    // publication exists. The callback must not destroy this owner.
    template <class F> void with_frozen_state(F &&callback) {
        require_mutation();
        frozen_ = true;
        struct Reset {
            bool &frozen;
            ~Reset() { frozen = false; }
        } reset{frozen_};
        std::invoke(std::forward<F>(callback));
    }

  private:
    template <class Prepare> InstalledSummary summarized_scope(Prepare &&prepare) {
        require_mutation();
        if (!exchange_.exclusive_)
            throw std::logic_error("prepared scope requires exclusive registration");
        std::optional<InstalledSummary> summary;
        with_frozen_state([&] {
            auto prepared = std::invoke(std::forward<Prepare>(prepare));
            summary.emplace(exchange_.summarize_prepared(*prepared));
            if (summary->result_ == Result::Accepted) {
                exchange_.install_prepared(*prepared);
                summary->installed_ = true;
            }
        });
        return std::move(*summary);
    }
    [[nodiscard]] static bool authorize_non_durable(Result result) {
#ifdef CHRONOS_TEST_GATE_SEAM
        if (result == Result::Accepted && test_before_install)
            return test_before_install(test_before_install_context);
#else
        (void)result;
#endif
        return true;
    }
    template <class Prepare, class F>
    PreparedScopeResult prepared_scope(Prepare &&prepare, F &&decision) {
        require_mutation();
        if (!exchange_.exclusive_)
            throw std::logic_error("prepared scope requires exclusive registration");
        PreparedScopeResult outcome;
        with_frozen_state([&] {
#ifdef CHRONOS_TEST_GATE_SEAM
            if (test_before_prepare)
                test_before_prepare(test_before_install_context);
#endif
            auto prepared = std::invoke(std::forward<Prepare>(prepare));
            outcome.result = prepared->result;
            const bool install = std::invoke(std::forward<F>(decision), outcome.result);
            if (install && outcome.result == Result::Accepted) {
                exchange_.install_prepared(*prepared);
                outcome.installed = true;
            }
        });
        return outcome;
    }
    [[nodiscard]] bool is_owner() const noexcept {
        if (phase_.load(std::memory_order_acquire) != Phase::Active)
            return false;
        try {
            return context_ == current_context();
        } catch (...) {
            return false;
        }
    }
    [[nodiscard]] bool can_read() const noexcept {
        return phase_.load(std::memory_order_acquire) == Phase::Stopped || is_owner();
    }
    void require_read() const {
        if (!can_read())
            throw std::logic_error("live exchange read requires owner or stopped state");
    }
    void require_mutation() const {
        if (!is_owner() || frozen_)
            throw std::logic_error("live exchange mutation requires active unfrozen owner");
    }
    Exchange exchange_;
    std::shared_ptr<const Context> context_;
    std::atomic<Phase> phase_{Phase::Unbound};
    bool frozen_ = false;
};
} // namespace chronos::exchange
