#pragma once
#include "chronos/exchange/exchange.hpp"
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

namespace chronos::snapshot {
// Version 1 stores a complete verified simulation log and an exact isolated
// exchange state, not CHROVIEW. It does not support arbitrary live exchanges.
struct ImageResult {
    bool ok = false;
    std::uint64_t generation = 0, ingress = 0;
    std::size_t mismatch_word = 0;
    std::string error;
    bool allocation_failure = false;
};
// `log` must be a complete Phase 7B run created from a fresh isolated
// simulation; source must still be at the completed boundary. Calls are
// owner-thread-only. Output is flushed, not fsynced.
[[nodiscard]] ImageResult write_image(std::ostream &out, const std::string &log,
                                      const exchange::Exchange &source, std::uint64_t generation);
// Validates and replays privately. Destination must be empty and remains
// unchanged on failure; no existing exchange can be replaced through this API.
[[nodiscard]] ImageResult restore_image(std::istream &in,
                                        std::unique_ptr<exchange::Exchange> &destination);
} // namespace chronos::snapshot
