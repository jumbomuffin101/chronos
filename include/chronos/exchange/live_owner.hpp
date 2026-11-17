#pragma once

#include "chronos/exchange/exchange.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace chronos::exchange {

// Encapsulates a fresh live exchange. Binding happens once on the event-loop
// context; no mutable exchange reference or transfer operation is exposed.
class LiveExchangeOwner final {
    struct Context {};
    enum class Phase { Unbound, Binding, Active, Stopped };
    static const std::shared_ptr<const Context> &current_context() {
        static thread_local const auto context = std::make_shared<const Context>();
        return context;
    }

  public:
    explicit LiveExchangeOwner(std::size_t sessions = 16, std::size_t orders = 1024,
                               std::size_t reports = 128, std::size_t requests = 1024)
        : exchange_(sessions, orders, reports, requests) {}
    LiveExchangeOwner(const LiveExchangeOwner &) = delete;
    LiveExchangeOwner &operator=(const LiveExchangeOwner &) = delete;
    LiveExchangeOwner(LiveExchangeOwner &&) = delete;
    LiveExchangeOwner &operator=(LiveExchangeOwner &&) = delete;

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
            phase_.store(Phase::Stopped, std::memory_order_release);
            throw;
        }
    }
    // Called by the event loop after its last exchange access. Once stopped,
    // copied read-only inspection is permitted from other contexts.
    [[nodiscard]] bool finish() noexcept {
        if (!is_owner() || frozen_)
            return false;
        phase_.store(Phase::Stopped, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool open_session(std::uint64_t session) {
        require_mutation();
        return exchange_.open_session(session);
    }
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
