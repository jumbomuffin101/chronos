#pragma once

#include "chronos/exchange/exchange.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace chronos::snapshot {
// A diagnostic observation, NOT a restorable snapshot or durable journal.
// Calls to capture must be serialized with every exchange operation.
constexpr std::size_t max_checkpoint_bytes = 8 * 1024 * 1024;
struct Checkpoint {
    std::uint64_t generation = 0;
    // Exchange accepted-command counter, not simulation ingress/log position.
    std::uint64_t exchange_command = 0;
    std::vector<std::uint64_t> state;
};
struct Result {
    bool ok = false;
    std::string error;
    std::size_t mismatch_word = 0;
};
[[nodiscard]] Checkpoint capture(const exchange::Exchange &source, std::uint64_t generation);
// On failure, read leaves destination unchanged. Checks are structural only;
// the words are opaque and are not a proof of reconstructability.
[[nodiscard]] Result read(std::istream &input, Checkpoint &destination);
[[nodiscard]] Result write(std::ostream &output, const Checkpoint &checkpoint);
[[nodiscard]] Result compare(const Checkpoint &checkpoint, const exchange::Exchange &exchange);
} // namespace chronos::snapshot
