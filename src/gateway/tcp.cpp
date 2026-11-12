#include "chronos/gateway/tcp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
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
    struct Session {
        Socket socket;
        FrameDecoder decoder;
        std::vector<std::uint8_t> output;
        std::size_t sent = 0;
        std::uint64_t id;
        bool active = false;
        bool closing = false;
        bool dead = false;
        Clock::time_point activity = Clock::now();
    };

  public:
    Impl(std::uint16_t port, std::size_t maximum, std::chrono::milliseconds idle)
        : listener_(::socket(AF_INET, SOCK_STREAM, 0)), maximum_(maximum), idle_(idle) {
        if (listener_.get() < 0 || maximum == 0 || maximum > 1024 || idle.count() <= 0)
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
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        listener_ = Socket{};
    }
    std::uint16_t port() const noexcept { return port_; }
    bool running() const noexcept { return running_.load(); }

  private:
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
    bool handle(Session &session, const Frame &frame) {
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
        } else if (frame.type == MessageType::SubmitOrder || frame.type == MessageType::CancelOrder)
            response = error_frame(ProtocolError::UnsupportedOperation, frame.request_id);
        else {
            session.closing = true;
            response = error_frame(ProtocolError::UnexpectedMessage, frame.request_id);
        }
        return queue(session, response) && !session.closing;
    }
    void run(std::stop_token stop) noexcept {
        try {
            std::vector<Session> sessions;
            while (!stop.stop_requested()) {
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
                        } catch (const std::runtime_error &) {
                            continue;
                        }
                        Session session;
                        session.socket = std::move(incoming);
                        session.id = ++next_id_;
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
                                [&](const Frame &f) { return handle(s, f); });
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
                        const auto n = ::send(s.socket.get(), s.output.data() + s.sent,
                                              s.output.size() - s.sent, MSG_NOSIGNAL);
                        if (n > 0) {
                            s.sent += static_cast<std::size_t>(n);
                            s.activity = Clock::now();
                        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                                   errno != EINTR)
                            s.dead = true;
                        if (s.sent == s.output.size()) {
                            s.output.clear();
                            s.sent = 0;
                        }
                    }
                    if ((events & POLLHUP) != 0 && (events & POLLIN) == 0)
                        s.dead = true;
                    if (s.closing && s.output.empty())
                        s.dead = true;
                    if (Clock::now() - s.activity >= idle_)
                        s.dead = true;
                }
                std::erase_if(sessions, [](const Session &s) { return s.dead; });
            }
        } catch (...) { /* Socket owners close on exit; running() reports service failure. */
        }
        running_.store(false);
    }
    Socket listener_;
    std::size_t maximum_;
    std::chrono::milliseconds idle_;
    std::uint16_t port_ = 0;
    std::uint64_t next_id_ = 0;
    std::atomic_bool running_{true};
    std::jthread thread_;
};
Gateway::Gateway(std::uint16_t port, std::size_t max_sessions, std::chrono::milliseconds idle)
    : impl_(std::make_unique<Impl>(port, max_sessions, idle)) {}
Gateway::~Gateway() = default;
std::uint16_t Gateway::port() const noexcept { return impl_->port(); }
bool Gateway::running() const noexcept { return impl_->running(); }
void Gateway::stop() noexcept { impl_->stop(); }

Client::Client(std::uint16_t port, std::chrono::milliseconds timeout)
    : socket_(::socket(AF_INET, SOCK_STREAM, 0)), timeout_(timeout) {
    if (socket_.get() < 0 || port == 0 || timeout.count() <= 0 ||
        timeout > std::chrono::seconds(60))
        throw std::invalid_argument("invalid client configuration");
    nonblocking(socket_.get());
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
bool Client::exchange(const Frame &request, Frame &response) {
    std::vector<std::uint8_t> encoded;
    if (!encode_frame(request, encoded) || !write_all(socket_.get(), encoded, timeout_))
        return false;
    const auto deadline = Clock::now() + timeout_;
    bool received = false;
    while (!received) {
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
                              if (received)
                                  return false;
                              response = frame;
                              received = true;
                              return true;
                          });
        if (error != ProtocolError::None)
            return false;
    }
    return response.request_id == request.request_id;
}
} // namespace chronos::gateway
