#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <span>
#include <string>

namespace chronos::journal {
// Experimental read-only envelope validation. This does not authorize
// transactions, validate canonical outcomes, or reconstruct exchange state.
inline constexpr std::uint64_t max_journal_bytes = 64ULL * 1024 * 1024;
inline constexpr std::uint32_t max_record_bytes = 1024 * 1024;
inline constexpr std::size_t journal_header_bytes = 88;
inline constexpr std::size_t manifest_bytes = 112;

[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) noexcept;

struct Inspection {
    bool ok = false;
    std::uint64_t durable_lsn = 0;
    std::uint64_t terminal_offset = 0;
    std::uint64_t tail_bytes = 0;
    std::string error;
};
// Requires seekable input streams. Reads at most one bounded record at a time;
// trailing bytes are classified as opaque, unverified tail and never repaired.
[[nodiscard]] Inspection inspect(std::istream &journal, std::istream &manifest);
} // namespace chronos::journal
