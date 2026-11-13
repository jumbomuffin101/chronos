#pragma once

#include "chronos/gateway/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace chronos::gateway::detail {
struct Command {
    std::uint64_t session;
    Frame frame;
};

// One bounded batch per poll iteration. Full batches reject new commands;
// they do not evict commands already admitted.
class Ingress {
  public:
    static constexpr std::size_t capacity = 32;
    Ingress() { commands_.reserve(capacity); }
    [[nodiscard]] bool push(std::uint64_t session, const Frame &frame) {
        if (commands_.size() == capacity)
            return false;
        commands_.push_back({session, frame});
        return true;
    }
    [[nodiscard]] std::span<const Command> commands() const noexcept { return commands_; }

  private:
    std::vector<Command> commands_;
};
} // namespace chronos::gateway::detail
