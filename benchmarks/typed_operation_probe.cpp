#include "chronos/exchange/live_owner.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0, bytes = 0;
} // namespace
void *operator new(std::size_t n) {
    if (void *p = std::malloc(n ? n : 1)) {
        if (counting) {
            ++allocations;
            bytes += n;
        }
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

[[gnu::noinline]] std::optional<chronos::exchange::CommandRecord::Bytes>
encode_record(const chronos::exchange::CommandRecord &record) {
    return record.encode();
}
[[gnu::noinline]] std::optional<chronos::exchange::CommandRecord>
decode_record(const chronos::exchange::CommandRecord::Bytes &bytes) {
    return chronos::exchange::CommandRecord::decode(bytes);
}

int main() {
    using namespace chronos;
    using Clock = std::chrono::steady_clock;
    std::cout << "mode,sample,latency_ns,scalar_allocations,requested_bytes\n";
    for (int mode = 0; mode < 3; ++mode) {
        for (int sample = -10; sample < 200; ++sample) {
            exchange::LiveExchangeOwner owner(2, 16, 32, 16, exchange::AllocationMode::Exclusive);
            owner.bind();
            if (!owner.open_session(1))
                return 1;
            const auto command = owner.submit_recorded(1, 1, 1, core::SymbolId{1}, core::Side::Buy,
                                                       core::OrderType::Limit, 100, 1);
            if (command.result != exchange::Result::Accepted || !owner.front(1))
                return 1;
            const auto sequence = owner.front(1)->sequence;
            const auto encoded = command.encode();
            if (!encoded)
                return 1;
            allocations = bytes = 0;
            counting = true;
            const auto start = Clock::now();
            bool valid = false;
            if (mode == 0) {
                const auto result = encode_record(command);
                valid = result && *result == *encoded;
            } else if (mode == 1) {
                const auto result = decode_record(*encoded);
                valid = result && result->result == exchange::Result::Accepted &&
                        result->diagnostic_index == command.diagnostic_index;
            } else {
                const auto result = owner.consume_expected_report(1, sequence);
                valid = result.status == exchange::DequeueStatus::Consumed &&
                        result.observed_sequence == sequence;
            }
            const auto end = Clock::now();
            counting = false;
            if (!valid || !owner.finish())
                return 1;
            if (sample >= 0)
                std::cout
                    << (mode == 0   ? "encode"
                        : mode == 1 ? "decode"
                                    : "expected-pop")
                    << ',' << sample << ','
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                    << ',' << allocations << ',' << bytes << '\n';
        }
    }
}
