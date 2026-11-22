#include "chronos/gateway/tcp.hpp"
#include "chronos/exchange/live_owner.hpp"
#include "chronos/gateway/ingress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace chronos::gateway {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_output = 2 * max_frame_size;
void trace(TraceSink sink, TracePoint point, std::uint64_t session, std::uint64_t request,
           std::uint8_t value = 0) noexcept {
    if (sink.record) {
        const auto now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
                .count();
        sink.record(sink.context, {point, now, session, request, value});
    }
}
void configure_tcp(int fd, bool nodelay) {
    const int enabled = nodelay ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0)
        throw std::runtime_error("TCP_NODELAY configuration failed");
    int actual = 0;
    socklen_t length = sizeof(actual);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &actual, &length) < 0 ||
        (actual != 0) != nodelay)
        throw std::runtime_error("TCP_NODELAY verification failed");
}
std::uint64_t wire64(const std::uint8_t *p) {
    return read_u64(std::span<const std::uint8_t, 8>{p, 8});
}
void put64(std::uint8_t *p, std::uint64_t v) { write_u64(std::span<std::uint8_t, 8>{p, 8}, v); }
void nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl failed");
}
bool wait_fd(int fd, short events, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const auto bounded = std::clamp<std::int64_t>(left + 1, 1, std::numeric_limits<int>::max());
        pollfd p{fd, events, 0};
        const int result = ::poll(&p, 1, static_cast<int>(bounded));
        if (result > 0)
            return (p.revents & events) != 0;
        if (result < 0 && errno != EINTR)
            return false;
    }
    return false;
}
Frame error_frame(ProtocolError error, std::uint64_t id) {
    Frame frame;
    frame.type = MessageType::Error;
    frame.request_id = id;
    frame.payload_size = 2;
    const auto value = static_cast<std::uint16_t>(error);
    frame.payload[0] = static_cast<std::uint8_t>(value >> 8);
    frame.payload[1] = static_cast<std::uint8_t>(value);
    return frame;
}
} // namespace

Socket::~Socket() {
    if (fd_ >= 0)
        ::close(fd_);
}
Socket::Socket(Socket &&other) noexcept : fd_(other.release()) {}
Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.release();
    }
    return *this;
}
int Socket::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

bool write_all(int fd, std::span<const std::uint8_t> bytes, std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0 || timeout > std::chrono::seconds(60))
        return false;
    const auto deadline = Clock::now() + timeout;
    while (!bytes.empty()) {
        if (!wait_fd(fd, POLLOUT, deadline))
            return false;
        const auto n = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (n > 0)
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        else if (n == 0)
            return false;
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
    }
    return true;
}

class Gateway::Impl {
    struct Control {
        std::uint64_t session;
        bool configure;
        bool killed;
        exchange::RiskLimits limits;
        std::mutex mutex;
        std::condition_variable completed;
        bool started = false, done = false, cancelled = false, result = false;
    };
    struct Session {
        Socket socket;
        FrameDecoder decoder;
        std::vector<std::uint8_t> output;
        std::size_t sent = 0;
        std::uint64_t id;
        bool active = false;
        bool closing = false;
        bool dead = false;
        bool report_inflight = false;
        std::optional<exchange::MarketEvent> market_inflight;
        std::uint64_t report_request = 0;
        std::uint8_t report_kind = 0;
        Clock::time_point activity = Clock::now();
    };

  public:
    Impl(std::uint16_t port, std::size_t maximum, std::chrono::milliseconds idle,
         TransportOptions options, exchange::RiskLimits default_risk,
         exchange::AllocationMode allocation)
        : listener_(::socket(AF_INET, SOCK_STREAM, 0)), maximum_(maximum), idle_(idle),
          exchange_(maximum, 1024, 128, 1024, allocation), options_(options),
          default_risk_(std::move(default_risk)), allocation_(allocation) {
        if (listener_.get() < 0 || maximum == 0 || maximum > 1024 || idle.count() <= 0 ||
            default_risk_.max_market_price <= 0 || default_risk_.allowed_symbols.size() > 1024 ||
            default_risk_.max_abs_position >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("invalid gateway configuration");
        nonblocking(listener_.get());
        int yes = 1;
        if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
            throw std::runtime_error("setsockopt failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            ::listen(listener_.get(), 64) < 0)
            throw std::runtime_error("loopback bind/listen failed");
        socklen_t length = sizeof(address);
        if (::getsockname(listener_.get(), reinterpret_cast<sockaddr *>(&address), &length) < 0)
            throw std::runtime_error("getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~Impl() { stop(); }
    void stop() noexcept {
        std::lock_guard lock{stop_mutex_};
        if (thread_.joinable()) {
            {
                std::lock_guard controls_lock{controls_mutex_};
                stopping_ = true;
                thread_.request_stop();
            }
            trace(options_.trace, TracePoint::StopRequested, 0, 0);
            thread_.join();
        }
        listener_ = Socket{};
    }
    std::uint16_t port() const noexcept { return port_; }
    bool running() const noexcept { return running_.load(); }
    bool control(std::uint64_t session, bool configure, bool killed,
                 const exchange::RiskLimits &limits, std::chrono::milliseconds timeout) {
        if (timeout.count() <= 0 || timeout > std::chrono::seconds(60) ||
            limits.allowed_symbols.size() > 1024)
            return false;
        auto command = std::make_shared<Control>();
        command->session = session;
        command->configure = configure;
        command->killed = killed;
        command->limits = limits;
        {
            std::lock_guard lock{controls_mutex_};
            if (!running_.load() || stopping_ || controls_.size() >= 32)
                return false;
            controls_.push_back(command);
        }
        trace(options_.trace, TracePoint::ControlQueued, session, 0);
        std::unique_lock lock{command->mutex};
        if (!command->completed.wait_for(lock, timeout, [&] { return command->done; })) {
            if (!command->started) {
                command->cancelled = true;
                return false;
            }
            command->completed.wait(lock, [&] { return command->done; });
        }
        return command->result;
    }
    std::size_t trade_count() const noexcept {
        return running_.load() ? 0 : exchange_.trade_count();
    }
    std::optional<exchange::AuditTrade> trade(std::size_t index) const noexcept {
        return running_.load() ? std::nullopt : exchange_.trade(index);
    }
    std::optional<exchange::Report> pending_report(std::uint64_t session) const {
        return running_.load() ? std::nullopt : exchange_.front(session);
    }

  private:
    void process_controls(std::stop_token stop) {
        std::vector<std::shared_ptr<Control>> commands;
        {
            std::lock_guard lock{controls_mutex_};
            commands.swap(controls_);
        }
        if (!commands.empty())
            trace(options_.trace, TracePoint::ControlBatchTaken, 0, 0,
                  static_cast<std::uint8_t>(commands.size()));
        for (const auto &command : commands) {
            bool stopped = false;
            {
                // This lock also serializes starting a control with a stop request.
                std::lock_guard controls_lock{controls_mutex_};
                std::lock_guard lock{command->mutex};
                if (command->cancelled)
                    continue;
                if (stop.stop_requested()) {
                    command->done = true;
                    command->result = false;
                    stopped = true;
                } else {
                    command->started = true;
                }
            }
            if (stopped) {
                trace(options_.trace, TracePoint::ControlCompleted, command->session, 0, 0);
                command->completed.notify_one();
                continue;
            }
            trace(options_.trace, TracePoint::ControlStarted, command->session, 0);
            bool result = false;
            try {
                result = command->configure
                             ? exchange_.configure_risk(command->session, command->limits)
                             : exchange_.set_killed(command->session, command->killed);
            } catch (...) {
                result = false;
            }
            {
                std::lock_guard lock{command->mutex};
                command->result = result;
                command->done = true;
            }
            trace(options_.trace, TracePoint::ControlCompleted, command->session, 0,
                  result ? 1 : 0);
            command->completed.notify_one();
        }
    }
    bool queue(Session &session, const Frame &frame) {
        std::vector<std::uint8_t> bytes;
        if (!encode_frame(frame, bytes) ||
            bytes.size() > max_output - (session.output.size() - session.sent))
            return false;
        if (session.sent != 0) {
            session.output.erase(session.output.begin(),
                                 session.output.begin() +
                                     static_cast<std::ptrdiff_t>(session.sent));
            session.sent = 0;
        }
        session.output.insert(session.output.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool handle(Session &session, const Frame &frame, detail::Ingress &ingress) {
        trace(options_.trace, TracePoint::FrameReceived, session.id, frame.request_id,
              static_cast<std::uint8_t>(frame.type));
        Frame response;
        response.request_id = frame.request_id;
        if (!session.active) {
            if (frame.type != MessageType::Hello) {
                session.closing = true;
                queue(session, error_frame(ProtocolError::HandshakeRequired, frame.request_id));
                return false;
            }
            session.active = true;
            response.type = MessageType::HelloAck;
            response.payload_size = 8;
            write_u64(std::span<std::uint8_t, 8>{response.payload.data(), 8}, session.id);
        } else if (frame.type == MessageType::Health)
            response.type = MessageType::HealthAck;
        else if (frame.type == MessageType::Echo) {
            response = frame;
            response.type = MessageType::EchoReply;
        } else if (frame.type == MessageType::SubmitOrder ||
                   frame.type == MessageType::CancelOrder || frame.type == MessageType::Subscribe ||
                   frame.type == MessageType::Unsubscribe) {
            if (!ingress.push(session.id, frame))
                response = error_frame(ProtocolError::IngressFull, frame.request_id);
            else {
                trace(options_.trace, TracePoint::CommandEnqueued, session.id, frame.request_id);
                return true;
            }
        } else {
            session.closing = true;
            response = error_frame(ProtocolError::UnexpectedMessage, frame.request_id);
        }
        return queue(session, response) && !session.closing;
    }
    void dispatch(Session &session, const Frame &frame) {
        trace(options_.trace, TracePoint::Dispatch, session.id, frame.request_id);
        const auto *p = frame.payload.data();
        if (frame.type == MessageType::Subscribe || frame.type == MessageType::Unsubscribe) {
            const auto symbol = (static_cast<std::uint32_t>(p[0]) << 24) |
                                (static_cast<std::uint32_t>(p[1]) << 16) |
                                (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
            const bool ok = frame.type == MessageType::Subscribe
                                ? exchange_.subscribe(session.id, core::SymbolId{symbol})
                                : exchange_.unsubscribe(session.id, core::SymbolId{symbol});
            Frame ack;
            ack.type = MessageType::MarketAck;
            ack.request_id = frame.request_id;
            ack.payload_size = 5;
            std::copy_n(p, 4, ack.payload.begin());
            ack.payload[4] = ok ? 0 : 1;
            if (!queue(session, ack))
                session.dead = true;
            return;
        }
        const auto client = wire64(p);
        // Reserve and encode a possible business rejection before preparation.
        // Accepted commands retain their reports in the bounded candidate state.
        std::vector<std::uint8_t> rejection;
        if (allocation_ == exchange::AllocationMode::Exclusive) {
            Frame reply;
            reply.type = MessageType::OrderAck;
            reply.request_id = frame.request_id;
            reply.payload_size = 17;
            put64(reply.payload.data(), client);
#ifdef CHRONOS_TEST_GATE_SEAM
            if (detail::test_before_output_capacity_check)
                detail::test_before_output_capacity_check(detail::test_output_capacity_context,
                                                          session.output);
#endif
            if (!encode_frame(reply, rejection) ||
                rejection.size() > max_output - (session.output.size() - session.sent)) {
                session.dead = true;
                return;
            }
            if (session.sent != 0) {
                session.output.erase(session.output.begin(),
                                     session.output.begin() +
                                         static_cast<std::ptrdiff_t>(session.sent));
                session.sent = 0;
            }
#ifdef CHRONOS_TEST_GATE_SEAM
            if (detail::test_before_output_reservation)
                detail::test_before_output_reservation(detail::test_output_reservation_context);
#endif
            session.output.reserve(session.output.size() + rejection.size());
        }
        exchange::Result result;
        trace(options_.trace, TracePoint::TransactionStart, session.id, frame.request_id);
        if (frame.type == MessageType::CancelOrder) {
            if (allocation_ == exchange::AllocationMode::Exclusive) {
                const auto outcome =
                    exchange_.cancel_prepared_non_durable(session.id, frame.request_id, client);
                result = outcome.result;
                if (result == exchange::Result::Accepted && !outcome.installed) {
                    session.dead = true;
                    return;
                }
            } else
                result = exchange_.cancel(session.id, frame.request_id, client);
        } else {
            const auto symbol = (static_cast<std::uint32_t>(p[8]) << 24) |
                                (static_cast<std::uint32_t>(p[9]) << 16) |
                                (static_cast<std::uint32_t>(p[10]) << 8) | p[11];
            if (allocation_ == exchange::AllocationMode::Exclusive) {
                const auto outcome = exchange_.submit_prepared_non_durable(
                    session.id, frame.request_id, client, core::SymbolId{symbol},
                    static_cast<core::Side>(p[12]), static_cast<core::OrderType>(p[13]),
                    std::bit_cast<std::int64_t>(wire64(p + 14)), wire64(p + 22));
                result = outcome.result;
                if (result == exchange::Result::Accepted && !outcome.installed) {
                    session.dead = true;
                    return;
                }
            } else
                result = exchange_.submit(
                    session.id, frame.request_id, client, core::SymbolId{symbol},
                    static_cast<core::Side>(p[12]), static_cast<core::OrderType>(p[13]),
                    std::bit_cast<std::int64_t>(wire64(p + 14)), wire64(p + 22));
        }
        trace(options_.trace, TracePoint::TransactionEnd, session.id, frame.request_id,
              static_cast<std::uint8_t>(result));
        if (result != exchange::Result::Accepted) {
            if (allocation_ == exchange::AllocationMode::Exclusive) {
                rejection[header_size + 8] = static_cast<std::uint8_t>(result);
                session.output.insert(session.output.end(), rejection.begin(), rejection.end());
                return;
            }
            Frame reply;
            reply.type = MessageType::OrderAck;
            reply.request_id = frame.request_id;
            reply.payload_size = 17;
            put64(reply.payload.data(), client);
            reply.payload[8] = static_cast<std::uint8_t>(result);
            if (!queue(session, reply))
                session.dead = true;
        }
    }
    bool queue_report(Session &session) {
        const auto report = exchange_.front(session.id);
        if (!report || session.report_inflight || !session.output.empty())
            return true;
        Frame frame;
        frame.type = MessageType::Execution;
        frame.request_id = report->request_id;
        frame.payload_size = 69;
        auto *p = frame.payload.data();
        put64(p, report->sequence);
        put64(p + 8, report->command_sequence);
        put64(p + 16, report->client_order_id);
        put64(p + 24, report->internal_order_id);
        put64(p + 32, report->trade_id);
        p[40] = static_cast<std::uint8_t>(report->symbol >> 24);
        p[41] = static_cast<std::uint8_t>(report->symbol >> 16);
        p[42] = static_cast<std::uint8_t>(report->symbol >> 8);
        p[43] = static_cast<std::uint8_t>(report->symbol);
        p[44] = static_cast<std::uint8_t>(report->kind);
        put64(p + 45, std::bit_cast<std::uint64_t>(report->price));
        put64(p + 53, report->quantity);
        put64(p + 61, report->remaining);
        if (!queue(session, frame))
            return false;
        session.report_inflight = true;
        session.report_request = report->request_id;
        session.report_kind = static_cast<std::uint8_t>(report->kind);
        trace(options_.trace, TracePoint::ReportQueued, session.id, report->request_id,
              session.report_kind);
        return true;
    }
    bool queue_market(Session &session) {
        if (session.market_inflight || session.report_inflight || !session.output.empty())
            return true;
        const auto event = exchange_.market_front(session.id);
        if (!event)
            return true;
        Frame frame;
        frame.type = MessageType::MarketData;
        frame.payload_size = 61;
        auto *p = frame.payload.data();
        p[0] = static_cast<std::uint8_t>(event->symbol >> 24);
        p[1] = static_cast<std::uint8_t>(event->symbol >> 16);
        p[2] = static_cast<std::uint8_t>(event->symbol >> 8);
        p[3] = static_cast<std::uint8_t>(event->symbol);
        put64(p + 4, event->sequence);
        p[12] = static_cast<std::uint8_t>(event->kind);
        put64(p + 13, std::bit_cast<std::uint64_t>(event->bid_price));
        put64(p + 21, event->bid_quantity);
        put64(p + 29, std::bit_cast<std::uint64_t>(event->ask_price));
        put64(p + 37, event->ask_quantity);
        put64(p + 45, std::bit_cast<std::uint64_t>(event->trade_price));
        put64(p + 53, event->trade_quantity);
        if (!queue(session, frame))
            return false;
        session.market_inflight = *event;
        return true;
    }
    void run(std::stop_token stop) noexcept {
        try {
            exchange_.bind();
            std::vector<Session> sessions;
            while (!stop.stop_requested()) {
                process_controls(stop);
                detail::Ingress ingress;
                for (auto &s : sessions)
                    if (!s.dead && (!queue_report(s) || !queue_market(s)))
                        s.dead = true;
                const std::size_t count = sessions.size();
                std::vector<pollfd> fds;
                fds.reserve(count + 1);
                fds.push_back({listener_.get(), POLLIN, 0});
                for (const auto &s : sessions)
                    fds.push_back({s.socket.get(),
                                   static_cast<short>((s.closing ? 0 : POLLIN) |
                                                      (s.sent < s.output.size() ? POLLOUT : 0)),
                                   0});
                const int ready = ::poll(fds.data(), fds.size(), 25);
                if (ready < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if ((fds[0].revents & POLLIN) != 0) {
                    for (int accepted = 0; accepted < 64; ++accepted) {
                        Socket incoming{::accept(listener_.get(), nullptr, nullptr)};
                        if (incoming.get() < 0) {
                            if (errno == EINTR)
                                continue;
                            break;
                        }
                        if (sessions.size() >= maximum_ ||
                            next_id_ == std::numeric_limits<std::uint64_t>::max())
                            continue;
                        try {
                            nonblocking(incoming.get());
                            configure_tcp(incoming.get(), options_.tcp_nodelay.value_or(true));
                        } catch (const std::runtime_error &) {
                            continue;
                        }
                        Session session;
                        session.socket = std::move(incoming);
                        session.id = ++next_id_;
                        if (!exchange_.open_session(session.id))
                            continue;
                        if (!exchange_.configure_risk(session.id, default_risk_)) {
                            exchange_.close_session(session.id);
                            continue;
                        }
                        trace(options_.trace, TracePoint::SocketConfigured, session.id, 0,
                              options_.tcp_nodelay.value_or(true) ? 1 : 0);
                        sessions.push_back(std::move(session));
                    }
                }
                for (std::size_t i = 0; i < count; ++i) {
                    auto &s = sessions[i];
                    const auto events = fds[i + 1].revents;
                    if ((events & (POLLERR | POLLNVAL)) != 0) {
                        s.dead = true;
                        continue;
                    }
                    if ((events & POLLIN) != 0 && !s.closing) {
                        std::array<std::uint8_t, max_frame_size> input{};
                        const auto n = ::recv(s.socket.get(), input.data(), input.size(), 0);
                        if (n > 0) {
                            s.activity = Clock::now();
                            const auto error = s.decoder.feed(
                                std::span<const std::uint8_t>{input.data(),
                                                              static_cast<std::size_t>(n)},
                                [&](const Frame &f) { return handle(s, f, ingress); });
                            if (error != ProtocolError::None && !s.closing) {
                                s.closing = true;
                                if (!queue(s, error_frame(error, 0)))
                                    s.dead = true;
                            }
                        } else if (n == 0) {
                            s.closing = true;
                            if (s.decoder.finish() != ProtocolError::None)
                                s.dead = true;
                        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            s.dead = true;
                    }
                    if ((events & POLLOUT) != 0 && s.sent < s.output.size()) {
                        const auto write_start = options_.trace.record && s.report_inflight
                                                     ? Clock::now()
                                                     : Clock::time_point{};
                        const auto n = ::send(s.socket.get(), s.output.data() + s.sent,
                                              s.output.size() - s.sent, MSG_NOSIGNAL);
                        if (n > 0) {
                            if (options_.trace.record && s.report_inflight) {
                                const auto ns =
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        write_start.time_since_epoch())
                                        .count();
                                options_.trace.record(options_.trace.context,
                                                      {TracePoint::SocketWrite, ns, s.id,
                                                       s.report_request, s.report_kind});
                            }
                            s.sent += static_cast<std::size_t>(n);
                            s.activity = Clock::now();
                        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                                   errno != EINTR)
                            s.dead = true;
                        if (s.sent == s.output.size()) {
                            s.output.clear();
                            s.sent = 0;
                            if (s.report_inflight) {
                                exchange_.pop(s.id);
                                s.report_inflight = false;
                            }
                            if (s.market_inflight) {
                                exchange_.market_pop(s.id, *s.market_inflight);
                                s.market_inflight.reset();
                            }
                        }
                    }
                    if ((events & POLLHUP) != 0 && (events & POLLIN) == 0)
                        s.dead = true;
                    if (s.closing && s.output.empty())
                        s.dead = true;
                    if (Clock::now() - s.activity >= idle_)
                        s.dead = true;
                }
                for (const auto &command : ingress.commands()) {
                    const auto it =
                        std::find_if(sessions.begin(), sessions.end(), [&](const Session &s) {
                            return s.id == command.session && !s.dead;
                        });
                    if (it == sessions.end())
                        continue;
                    try {
                        dispatch(*it, command.frame);
                    } catch (const std::bad_alloc &) {
                        it->dead = true;
                    }
                }
                for (const auto &s : sessions)
                    if (s.dead)
                        exchange_.close_session(s.id);
                std::erase_if(sessions, [](const Session &s) { return s.dead; });
            }
        } catch (...) { /* Socket owners close on exit; running() reports service failure. */
        }
        (void)exchange_.finish();
        std::vector<std::shared_ptr<Control>> pending;
        {
            std::lock_guard lock{controls_mutex_};
            running_.store(false);
            pending.swap(controls_);
        }
        for (const auto &command : pending) {
            {
                std::lock_guard lock{command->mutex};
                command->cancelled = true;
                command->done = true;
                command->result = false;
            }
            trace(options_.trace, TracePoint::ControlCompleted, command->session, 0, 0);
            command->completed.notify_one();
        }
    }
    Socket listener_;
    std::size_t maximum_;
    std::chrono::milliseconds idle_;
    std::uint16_t port_ = 0;
    std::uint64_t next_id_ = 0;
    exchange::LiveExchangeOwner exchange_;
    TransportOptions options_;
    exchange::RiskLimits default_risk_;
    exchange::AllocationMode allocation_;
    std::mutex controls_mutex_;
    std::vector<std::shared_ptr<Control>> controls_;
    bool stopping_ = false; // protected by controls_mutex_
    std::atomic_bool running_{true};
    std::mutex stop_mutex_;
    std::jthread thread_;
};
Gateway::Gateway(std::uint16_t port, std::size_t max_sessions, std::chrono::milliseconds idle,
                 TransportOptions options, exchange::RiskLimits default_risk,
                 exchange::AllocationMode allocation)
    : impl_(std::make_unique<Impl>(port, max_sessions, idle, options, std::move(default_risk),
                                   allocation)) {}
Gateway::~Gateway() = default;
std::uint16_t Gateway::port() const noexcept { return impl_->port(); }
bool Gateway::running() const noexcept { return impl_->running(); }
bool Gateway::configure_risk(std::uint64_t session, const exchange::RiskLimits &limits,
                             std::chrono::milliseconds timeout) {
    return impl_->control(session, true, false, limits, timeout);
}
bool Gateway::set_killed(std::uint64_t session, bool killed, std::chrono::milliseconds timeout) {
    return impl_->control(session, false, killed, {}, timeout);
}
void Gateway::stop() noexcept { impl_->stop(); }
std::size_t Gateway::trade_count() const noexcept { return impl_->trade_count(); }
std::optional<exchange::AuditTrade> Gateway::trade(std::size_t index) const noexcept {
    return impl_->trade(index);
}
std::optional<exchange::Report> Gateway::pending_report(std::uint64_t session) const {
    return impl_->pending_report(session);
}

Client::Client(std::uint16_t port, std::chrono::milliseconds timeout, TransportOptions options)
    : socket_(::socket(AF_INET, SOCK_STREAM, 0)), timeout_(timeout), options_(options) {
    if (socket_.get() < 0 || port == 0 || timeout.count() <= 0 ||
        timeout > std::chrono::seconds(60))
        throw std::invalid_argument("invalid client configuration");
    nonblocking(socket_.get());
    configure_tcp(socket_.get(), options_.tcp_nodelay.value_or(false));
    trace(options_.trace, TracePoint::SocketConfigured, 0, 0,
          options_.tcp_nodelay.value_or(false) ? 1 : 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int result =
        ::connect(socket_.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (result < 0 && errno != EINPROGRESS)
        throw std::runtime_error("connect failed");
    if (result < 0) {
        if (!wait_fd(socket_.get(), POLLOUT, Clock::now() + timeout_))
            throw std::runtime_error("connect timeout");
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(socket_.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0)
            throw std::runtime_error("connect failed");
    }
}
bool Client::send(const Frame &request) {
    trace(options_.trace, TracePoint::ClientSendStart, 0, request.request_id);
    std::vector<std::uint8_t> encoded;
    const bool result =
        encode_frame(request, encoded) && write_all(socket_.get(), encoded, timeout_);
    trace(options_.trace, TracePoint::ClientSendEnd, 0, request.request_id, result ? 1 : 0);
    return result;
}
bool Client::receive(Frame &response) {
    const auto deadline = Clock::now() + timeout_;
    while (pending_.empty()) {
        if (!wait_fd(socket_.get(), POLLIN, deadline))
            return false;
        std::array<std::uint8_t, max_frame_size> input{};
        const auto n = ::recv(socket_.get(), input.data(), input.size(), 0);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        const auto error =
            decoder_.feed(std::span<const std::uint8_t>{input.data(), static_cast<std::size_t>(n)},
                          [&](const Frame &frame) {
                              if (pending_.size() >= 32)
                                  return false;
                              pending_.push_back(frame);
                              return true;
                          });
        if (error != ProtocolError::None)
            return false;
    }
    response = pending_.front();
    pending_.pop_front();
    trace(options_.trace, TracePoint::ClientReceived, 0, response.request_id,
          response.type == MessageType::Execution ? response.payload[44] : 255);
    return true;
}
bool Client::exchange(const Frame &request, Frame &response) {
    return send(request) && receive(response) && response.request_id == request.request_id;
}
} // namespace chronos::gateway
