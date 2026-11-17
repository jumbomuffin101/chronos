#pragma once

#include "chronos/exchange/exchange.hpp"
#include "chronos/gateway/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace chronos::gateway {

enum class TracePoint {
    SocketConfigured,
    ClientSendStart,
    ClientSendEnd,
    FrameReceived,
    CommandEnqueued,
    Dispatch,
    TransactionStart,
    TransactionEnd,
    ReportQueued,
    SocketWrite,
    ClientReceived,
    ControlQueued,
    ControlBatchTaken,
    ControlStarted,
    ControlCompleted,
    StopRequested
};
struct TraceEvent {
    TracePoint point;
    std::int64_t monotonic_ns;
    std::uint64_t session_id;
    std::uint64_t request_id;
    std::uint8_t value;
};
// Callback must be nonblocking, noexcept and safe for concurrent client,
// control, stop and gateway threads. The caller owns the context throughout.
struct TraceSink {
    void (*record)(void *, const TraceEvent &) noexcept = nullptr;
    void *context = nullptr;
};
struct TransportOptions {
    // Unspecified preserves the endpoint default: on for gateway, off for client.
    std::optional<bool> tcp_nodelay = std::nullopt;
    TraceSink trace{};
};

// Move-only RAII wrapper; does not take ownership of descriptors passed elsewhere.
class Socket {
  public:
    explicit Socket(int fd = -1) noexcept : fd_(fd) {}
    ~Socket();
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept;

  private:
    int fd_;
};

// Completes partial writes until deadline; fd must be nonblocking. Returns false
// for timeout, disconnect or system error and never raises SIGPIPE.
[[nodiscard]] bool write_all(int fd, std::span<const std::uint8_t> bytes,
                             std::chrono::milliseconds timeout);

class Gateway {
  public:
    explicit Gateway(std::uint16_t port = 0, std::size_t max_sessions = 16,
                     std::chrono::milliseconds idle_timeout = std::chrono::seconds(5),
                     TransportOptions options = {}, exchange::RiskLimits default_risk = {},
                     exchange::AllocationMode allocation = exchange::AllocationMode::Shared);
    ~Gateway();
    Gateway(const Gateway &) = delete;
    Gateway &operator=(const Gateway &) = delete;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    // Synchronous local administration; applied on the owner thread. A false
    // return means the request was rejected or cancelled before application.
    [[nodiscard]] bool configure_risk(std::uint64_t session, const exchange::RiskLimits &limits,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(2));
    [[nodiscard]] bool set_killed(std::uint64_t session, bool killed,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(2));
    void stop() noexcept;
    // Only call after stop() has joined the owner thread. Volatile, bounded audit.
    [[nodiscard]] std::size_t trade_count() const noexcept;
    [[nodiscard]] std::optional<exchange::AuditTrade> trade(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<exchange::Report> pending_report(std::uint64_t session) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Client {
  public:
    explicit Client(std::uint16_t port, std::chrono::milliseconds timeout = std::chrono::seconds(2),
                    TransportOptions options = {});
    [[nodiscard]] bool send(const Frame &request);
    [[nodiscard]] bool receive(Frame &response);
    [[nodiscard]] bool exchange(const Frame &request, Frame &response);

  private:
    Socket socket_;
    std::chrono::milliseconds timeout_;
    FrameDecoder decoder_;
    std::deque<Frame> pending_;
    TransportOptions options_;
};

} // namespace chronos::gateway
