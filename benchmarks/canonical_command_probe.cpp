#include "chronos/exchange/canonical_command.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    constexpr std::uint64_t count = 1000000;
    std::uint64_t sink = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto command =
            chronos::exchange::CanonicalCommand::submit(i, i + 1, i + 2, 1, 0, 0, 100, 5);
        const auto encoded = command.encode();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        sink += encoded[static_cast<std::size_t>(i % encoded.size())];
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    std::cout << "commands=" << count
              << " bytes_per_command=" << chronos::exchange::CanonicalCommand::encoded_size
              << " elapsed_ns=" << elapsed
              << " ns_per_command=" << static_cast<double>(elapsed) / static_cast<double>(count)
              << " sink=" << sink << '\n';
}
