#include "chronos/eventlog/event_log.hpp"

#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--verify") {
        std::cerr << "Usage: chronos_replay --verify FILE\n";
        return 2;
    }
    std::ifstream input(argv[2], std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open log file\n";
        return 1;
    }
    const auto result = chronos::eventlog::verify(input);
    if (!result.verified) {
        std::cerr << "Verification failed at offset " << result.offset << ", sequence "
                  << result.sequence << ": " << result.error << '\n';
        return 1;
    }
    std::cout << "Verified " << (2 + 2 * result.commands) << " records; commands "
              << result.commands << ", accepted " << result.accepted << ", rejected "
              << result.rejected << ", trades " << result.trades << '\n';
    return 0;
}
