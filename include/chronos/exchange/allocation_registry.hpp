#pragma once

#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace chronos::exchange {
class Exchange;
enum class AllocationMode { Shared, Exclusive };
namespace detail {

// Coordinates lifetimes, not IDs. The process registry is private to the
// exchange implementation. Independent instances cannot access that registry.
class AllocationRegistry {
  public:
    class Lease {
      public:
        Lease() noexcept = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept
            : registry_(std::exchange(other.registry_, nullptr)), mode_(other.mode_) {}
        Lease &operator=(Lease &&) = delete;
        ~Lease() { release(); }
        void release() noexcept {
            if (!registry_)
                return;
            std::lock_guard lock{registry_->mutex_};
            if (mode_ == AllocationMode::Exclusive)
                registry_->exclusive_ = false;
            else
                --registry_->shared_;
            registry_ = nullptr;
        }

      private:
        friend class AllocationRegistry;
        Lease(AllocationRegistry &registry, AllocationMode mode) noexcept
            : registry_(&registry), mode_(mode) {}
        AllocationRegistry *registry_ = nullptr;
        AllocationMode mode_ = AllocationMode::Shared;
    };

  private:
    friend class ::chronos::exchange::Exchange;
    AllocationRegistry() = default;
    static AllocationRegistry &process() noexcept;
    [[nodiscard]] Lease acquire(AllocationMode mode) {
        if (mode != AllocationMode::Shared && mode != AllocationMode::Exclusive)
            throw std::invalid_argument("invalid allocation mode");
        bool unavailable = false;
        {
            std::lock_guard lock{mutex_};
            if (mode == AllocationMode::Exclusive) {
                unavailable = exclusive_ || shared_ != 0;
                if (!unavailable)
                    exclusive_ = true;
            } else {
                unavailable = exclusive_ || shared_ == std::numeric_limits<std::size_t>::max();
                if (!unavailable)
                    ++shared_;
            }
        }
        if (unavailable)
            throw std::runtime_error(mode == AllocationMode::Exclusive
                                         ? "exclusive allocation registration unavailable"
                                         : "shared allocation registration unavailable");
        return Lease{*this, mode};
    }

    std::mutex mutex_;
    std::size_t shared_ = 0;
    bool exclusive_ = false;
};
} // namespace detail
} // namespace chronos::exchange
