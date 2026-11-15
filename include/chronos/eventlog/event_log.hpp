#pragma once

#include "chronos/simulation/simulation.hpp"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace chronos::eventlog {
inline constexpr std::size_t max_record_size = 1024 * 1024;
inline constexpr std::size_t max_log_size = 64 * 1024 * 1024;
std::uint32_t crc32(const std::uint8_t *data, std::size_t size) noexcept;

// The caller owns the stream. Each record is flushed, but never fsynced.
class EventRecorder final : public simulation::SimulationObserver {
  public:
    explicit EventRecorder(std::ostream &stream) noexcept : stream_(stream) {}
    bool begin(const simulation::ScenarioConfig &config) override;
    bool command(const simulation::SimCommand &command) override;
    bool outcome(const simulation::SimCommand &command, exchange::Result result,
                 const exchange::Exchange &exchange) override;
    bool finish(const simulation::SimulationResult &result, simulation::Lifecycle state,
                const exchange::Exchange &exchange) override;
    [[nodiscard]] const std::string &error() const noexcept { return error_; }

  private:
    bool write_record(std::uint16_t type, std::uint64_t sequence,
                      const std::vector<std::uint8_t> &payload);
    std::ostream &stream_;
    std::string error_;
    std::uint64_t next_ = 1;
    std::size_t bytes_ = 0;
    bool begun_ = false, pending_ = false, failed_ = false;
};

struct ReplayResult {
    bool verified = false;
    std::uint64_t commands = 0, accepted = 0, rejected = 0, trades = 0;
    std::uint64_t offset = 0, sequence = 0;
    std::string error;
};
// Fully validates the bounded file before constructing or mutating an exchange.
[[nodiscard]] ReplayResult verify(std::istream &stream);
} // namespace chronos::eventlog
