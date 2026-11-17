#pragma once

#include "chronos/exchange/exchange.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace chronos::exchange {

// A restricted, isolated-only prerequisite. It does not wrap a live exchange,
// expose its Exchange, install transactions, or provide durable publication.
enum class OwnerAccess { Allowed, WrongContext, Frozen };

template <class T> struct OwnerResult {
    OwnerAccess access;
    std::optional<T> value; // Empty unless access == Allowed.
};

class OfflineOwner final {
    struct Context {};
    static const std::shared_ptr<const Context> &current_context() {
        static thread_local const auto context = std::make_shared<const Context>();
        return context;
    }

  public:
    explicit OfflineOwner(std::size_t sessions = 16, std::size_t orders = 1024,
                          std::size_t reports = 128, std::size_t requests = 1024)
        : context_(current_context()),
          exchange_(Exchange::isolated(sessions, orders, reports, requests)) {}
    OfflineOwner(const OfflineOwner &) = delete;
    OfflineOwner &operator=(const OfflineOwner &) = delete;
    OfflineOwner(OfflineOwner &&) = delete;
    OfflineOwner &operator=(OfflineOwner &&) = delete;

    [[nodiscard]] OwnerResult<bool> open_session(std::uint64_t session) {
        return mutate([&] { return exchange_->open_session(session); });
    }
    [[nodiscard]] OwnerResult<bool> configure_risk(std::uint64_t session,
                                                   const RiskLimits &limits) {
        return mutate([&] { return exchange_->configure_risk(session, limits); });
    }
    [[nodiscard]] OwnerResult<bool> set_killed(std::uint64_t session, bool killed) {
        return mutate([&] { return exchange_->set_killed(session, killed); });
    }
    [[nodiscard]] OwnerResult<bool> subscribe(std::uint64_t session, core::SymbolId symbol) {
        return mutate([&] { return exchange_->subscribe(session, symbol); });
    }
    [[nodiscard]] OwnerResult<bool> unsubscribe(std::uint64_t session, core::SymbolId symbol) {
        return mutate([&] { return exchange_->unsubscribe(session, symbol); });
    }
    [[nodiscard]] OwnerAccess market_pop(std::uint64_t session, const MarketEvent &event) {
        return mutate_void([&] { exchange_->market_pop(session, event); });
    }
    [[nodiscard]] OwnerAccess close_session(std::uint64_t session) {
        return mutate_void([&] { exchange_->close_session(session); });
    }
    [[nodiscard]] OwnerResult<Result> submit(std::uint64_t session, std::uint64_t request,
                                             std::uint64_t client, core::SymbolId symbol,
                                             core::Side side, core::OrderType type,
                                             core::Price price, core::Quantity quantity) {
        return mutate([&] {
            return exchange_->submit(session, request, client, symbol, side, type, price, quantity);
        });
    }
    [[nodiscard]] OwnerResult<Result> cancel(std::uint64_t session, std::uint64_t request,
                                             std::uint64_t client) {
        return mutate([&] { return exchange_->cancel(session, request, client); });
    }
    [[nodiscard]] OwnerAccess pop(std::uint64_t session) {
        return mutate_void([&] { exchange_->pop(session); });
    }

    // Read-only access is also confined to the construction context. Values
    // are copied; no internal reference or pointer escapes.
    [[nodiscard]] OwnerResult<std::optional<Report>> front(std::uint64_t session) const {
        if (!on_context())
            return {OwnerAccess::WrongContext, {}};
        return {OwnerAccess::Allowed,
                std::optional<std::optional<Report>>{std::in_place, exchange_->front(session)}};
    }
    [[nodiscard]] OwnerResult<std::optional<MarketEvent>>
    market_front(std::uint64_t session) const {
        if (!on_context())
            return {OwnerAccess::WrongContext, {}};
        return {OwnerAccess::Allowed, std::optional<std::optional<MarketEvent>>{
                                          std::in_place, exchange_->market_front(session)}};
    }
    [[nodiscard]] OwnerResult<std::vector<std::uint64_t>> exact_state(std::size_t max_words) const {
        if (!on_context())
            return {OwnerAccess::WrongContext, {}};
        return {OwnerAccess::Allowed, exchange_->isolated_continuation_state(max_words)};
    }

    // Synchronous, non-escaping scope. It freezes every wrapper mutator,
    // including reentrant calls on the owner context. No candidate is staged.
    // Exceptions from the callback propagate after the freeze is released.
    template <class F> [[nodiscard]] OwnerAccess with_frozen_state(F &&callback) {
        const auto access = mutation_access();
        if (access != OwnerAccess::Allowed)
            return access;
        frozen_ = true;
        struct Reset {
            bool &frozen;
            ~Reset() { frozen = false; }
        } reset{frozen_};
        std::invoke(std::forward<F>(callback));
        return OwnerAccess::Allowed;
    }

  private:
    [[nodiscard]] bool on_context() const noexcept {
        try {
            return current_context() == context_;
        } catch (...) {
            // Failure to identify a context must never grant access.
            return false;
        }
    }
    [[nodiscard]] OwnerAccess mutation_access() const noexcept {
        if (!on_context())
            return OwnerAccess::WrongContext;
        return frozen_ ? OwnerAccess::Frozen : OwnerAccess::Allowed;
    }
    template <class F> auto mutate(F &&f) -> OwnerResult<std::invoke_result_t<F>> {
        const auto access = mutation_access();
        if (access != OwnerAccess::Allowed)
            return {access, {}};
        return {access, std::invoke(std::forward<F>(f))};
    }
    template <class F> OwnerAccess mutate_void(F &&f) {
        const auto access = mutation_access();
        if (access == OwnerAccess::Allowed)
            std::invoke(std::forward<F>(f));
        return access;
    }
    const std::shared_ptr<const Context> context_;
    std::unique_ptr<Exchange> exchange_;
    bool frozen_ = false;
};
} // namespace chronos::exchange
