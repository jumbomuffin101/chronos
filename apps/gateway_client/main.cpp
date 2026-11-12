#include "chronos/gateway/tcp.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    unsigned int port = 8088;
    if (argc > 2) {
        std::cerr << "Usage: chronos_gateway_client [port]\n";
        return 1;
    }
    if (argc == 2) {
        const std::string_view text{argv[1]};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port == 0 ||
            port > 65535)
            return 1;
    }
    try {
        using namespace chronos::gateway;
        Client client{static_cast<std::uint16_t>(port)};
        Frame request;
        Frame response;
        request.type = MessageType::Hello;
        request.request_id = 1;
        if (!client.exchange(request, response) || response.type != MessageType::HelloAck)
            return 1;
        const auto id = read_u64(std::span<const std::uint8_t, 8>{response.payload.data(), 8});
        request = {};
        request.type = MessageType::Echo;
        request.request_id = 2;
        constexpr std::string_view message{"chronos loopback"};
        request.payload_size = message.size();
        std::copy(message.begin(), message.end(), request.payload.begin());
        if (!client.exchange(request, response) || response.type != MessageType::EchoReply ||
            response.payload_size != message.size() ||
            !std::equal(message.begin(), message.end(), response.payload.begin()))
            return 1;
        std::cout << "Session " << id << " echo: " << message << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
