#include "chronos/gateway/tcp.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
} // namespace
int main(int argc, char **argv) {
    unsigned int port = 8088;
    if (argc > 2) {
        std::cerr << "Usage: chronos_gateway [port]\n";
        return 1;
    }
    if (argc == 2) {
        const std::string_view text{argv[1]};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port > 65535)
            return 1;
    }
    try {
        chronos::gateway::Gateway gateway{static_cast<std::uint16_t>(port)};
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::cout << "Chronos gateway listening on 127.0.0.1:" << gateway.port() << std::endl;
        while (!interrupted && gateway.running())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!gateway.running()) {
            std::cerr << "Gateway stopped unexpectedly\n";
            return 1;
        }
        gateway.stop();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
