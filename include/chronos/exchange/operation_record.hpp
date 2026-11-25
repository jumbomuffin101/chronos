#pragma once

#include "chronos/exchange/exchange.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace chronos::exchange {

// These are partial, process-local observations, not a journal envelope or LSN.
enum class DequeueStatus : std::uint8_t { Consumed = 1, Empty = 2, Mismatch = 3, Invalid = 4 };

// The explicit table fixes schema numbers independently of enum representation.
inline constexpr std::array<Result, 17> operation_result_codes{
    Result::Accepted,        Result::UnknownSession, Result::DuplicateRequest,
    Result::DuplicateOrder,  Result::NotOwner,       Result::NotFound,
    Result::InvalidOrder,    Result::Capacity,       Result::CounterExhausted,
    Result::RiskKilled,      Result::RiskSymbol,     Result::RiskQuantity,
    Result::RiskNotional,    Result::RiskOpenOrders, Result::RiskPosition,
    Result::RiskMarketBound, Result::RiskOverflow};

struct CommandRecord {
    static constexpr std::size_t encoded_size = 68;
    using Bytes = std::array<std::uint8_t, encoded_size>;
    std::uint64_t diagnostic_index;
    CanonicalCommand command;
    Result result;

    [[nodiscard]] std::optional<Bytes> encode() const noexcept {
        const auto found =
            std::find(operation_result_codes.begin(), operation_result_codes.end(), result);
        if (diagnostic_index == 0 || found == operation_result_codes.end())
            return std::nullopt;
        Bytes b{};
        b[1] = 1;
        b[2] = 2;                                  // canonical submit/cancel result
        b[3] = result == Result::Accepted ? 1 : 2; // installed / business rejection
        b[5] = static_cast<std::uint8_t>(found - operation_result_codes.begin() + 1);
        for (std::size_t i = 0; i < 8; ++i)
            b[15 - i] = static_cast<std::uint8_t>(diagnostic_index >> (8 * i));
        const auto input = command.encode();
        std::copy(input.begin(), input.end(), b.begin() + 16);
        return b;
    }
    [[nodiscard]] static std::optional<CommandRecord>
    decode(std::span<const std::uint8_t> b) noexcept {
        if (b.size() != encoded_size || b[0] != 0 || b[1] != 1 || b[2] != 2 || b[4] != 0 ||
            b[6] != 0 || b[7] != 0 || b[5] == 0 || b[5] > operation_result_codes.size())
            return std::nullopt;
        const auto result = operation_result_codes[b[5] - 1];
        if (b[3] != (result == Result::Accepted ? 1 : 2))
            return std::nullopt;
        std::uint64_t index = 0;
        for (std::size_t i = 8; i < 16; ++i)
            index = (index << 8) | b[i];
        const auto command = CanonicalCommand::decode(b.subspan(16));
        if (index == 0 || !command)
            return std::nullopt;
        return CommandRecord{index, *command, result};
    }
};

struct DequeueRecord {
    static constexpr std::size_t encoded_size = 40;
    using Bytes = std::array<std::uint8_t, encoded_size>;
    std::uint64_t diagnostic_index, session, expected_sequence, observed_sequence;
    DequeueStatus status;

    [[nodiscard]] bool valid() const noexcept {
        if (diagnostic_index == 0)
            return false;
        switch (status) {
        case DequeueStatus::Consumed:
            return session != 0 && expected_sequence != 0 && observed_sequence == expected_sequence;
        case DequeueStatus::Empty:
            return session != 0 && expected_sequence != 0 && observed_sequence == 0;
        case DequeueStatus::Mismatch:
            return session != 0 && expected_sequence != 0 && observed_sequence != 0 &&
                   observed_sequence != expected_sequence;
        case DequeueStatus::Invalid:
            return (session == 0 || expected_sequence == 0) && observed_sequence == 0;
        }
        return false;
    }
    [[nodiscard]] std::optional<Bytes> encode() const noexcept {
        if (!valid())
            return std::nullopt;
        Bytes b{};
        b[1] = 1;
        b[2] = 1; // private-report dequeue
        b[3] = static_cast<std::uint8_t>(status);
        const auto put = [&](std::size_t off, std::uint64_t value) {
            for (std::size_t i = 0; i < 8; ++i)
                b[off + 7 - i] = static_cast<std::uint8_t>(value >> (8 * i));
        };
        put(8, diagnostic_index);
        put(16, session);
        put(24, expected_sequence);
        put(32, observed_sequence);
        return b;
    }
    [[nodiscard]] static std::optional<DequeueRecord>
    decode(std::span<const std::uint8_t> b) noexcept {
        if (b.size() != encoded_size || b[0] != 0 || b[1] != 1 || b[2] != 1 || b[4] != 0 ||
            b[5] != 0 || b[6] != 0 || b[7] != 0)
            return std::nullopt;
        const auto get = [&](std::size_t off) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < 8; ++i)
                value = (value << 8) | b[off + i];
            return value;
        };
        DequeueRecord record{get(8), get(16), get(24), get(32), static_cast<DequeueStatus>(b[3])};
        if (!record.valid())
            return std::nullopt;
        return record;
    }
};

} // namespace chronos::exchange
