#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

namespace chronos::exchange {
class Exchange;
}
namespace chronos::exchange::detail {
// A reservation keeps its lock until the enclosing transaction is complete.
// Unsynchronized instances are owner-thread-only and must never be shared.
class OrderIdAllocator {
  public:
    constexpr explicit OrderIdAllocator(bool synchronized, std::uint64_t last = 0) noexcept
        : last_(last), synchronized_(synchronized) {}
    OrderIdAllocator(const OrderIdAllocator &) = delete;
    OrderIdAllocator &operator=(const OrderIdAllocator &) = delete;
    class Reservation {
      public:
        explicit Reservation(OrderIdAllocator &allocator)
            : allocator_(allocator), lock_(allocator.mutex_, std::defer_lock) {
            if (allocator.synchronized_)
                lock_.lock();
            if (allocator.last_ != std::numeric_limits<std::uint64_t>::max())
                id_ = allocator.last_ + 1;
        }
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
        // Called once, immediately after the exchange's nonthrowing state swap.
        void commit() noexcept {
            if (id_ != 0)
                allocator_.last_ = id_;
        }

      private:
        OrderIdAllocator &allocator_;
        std::unique_lock<std::mutex> lock_;
        std::uint64_t id_ = 0;
    };

  private:
    friend class ::chronos::exchange::Exchange;
    // Exchange calls this only for its owned, unsynchronized private context.
    [[nodiscard]] std::uint64_t private_last() const noexcept { return last_; }
    std::mutex mutex_;
    std::uint64_t last_;
    bool synchronized_;
};
} // namespace chronos::exchange::detail
