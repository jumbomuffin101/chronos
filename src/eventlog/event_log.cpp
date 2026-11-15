#include "chronos/eventlog/event_log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chronos::eventlog {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> magic{'C', 'H', 'R', 'O', 'N', 'O', 'S', 0};
void u16(Bytes &b, std::uint16_t v) {
    for (unsigned i = 0; i < 2; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}
void u32(Bytes &b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}
void u64(Bytes &b, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}
std::uint16_t r16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8U));
}
std::uint32_t r32(const std::uint8_t *p) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(p[i]) << (8U * i);
    return v;
}
struct Cursor {
    const Bytes &b;
    std::size_t pos = 0;
    std::uint64_t word() {
        if (b.size() - pos < 8)
            throw std::runtime_error("truncated payload");
        std::uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(b[pos++]) << (8U * i);
        return v;
    }
    void end() const {
        if (pos != b.size())
            throw std::runtime_error("trailing payload");
    }
};
void words(Bytes &b, const std::vector<std::uint64_t> &values) {
    u64(b, values.size());
    for (const auto v : values)
        u64(b, v);
}
std::vector<std::uint64_t> read_words(Cursor &c) {
    const auto n = c.word();
    if (n > (c.b.size() - c.pos) / 8)
        throw std::runtime_error("invalid state length");
    std::vector<std::uint64_t> out;
    out.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i)
        out.push_back(c.word());
    return out;
}
Bytes config_bytes(const simulation::ScenarioConfig &c) {
    Bytes b;
    for (const auto v :
         {std::uint64_t{1}, std::uint64_t{simulation::SimulationRunner::generator_version},
          static_cast<std::uint64_t>(c.scenario), c.seed,
          static_cast<std::uint64_t>(c.participants), static_cast<std::uint64_t>(c.max_commands),
          static_cast<std::uint64_t>(c.max_order_history),
          static_cast<std::uint64_t>(c.max_requests_per_session),
          static_cast<std::uint64_t>(c.reference_price),
          static_cast<std::uint64_t>(c.price_variation), c.min_quantity, c.max_quantity,
          static_cast<std::uint64_t>(c.buy_weight), static_cast<std::uint64_t>(c.cancel_percent),
          static_cast<std::uint64_t>(c.symbols.size())})
        u64(b, v);
    for (const auto s : c.symbols)
        u64(b, s);
    const auto &r = c.risk;
    for (const auto v : {r.max_order_quantity, r.max_order_notional, r.max_open_notional,
                         static_cast<std::uint64_t>(r.max_open_orders), r.max_abs_position,
                         static_cast<std::uint64_t>(r.max_market_price),
                         static_cast<std::uint64_t>(r.allowed_symbols.size())})
        u64(b, v);
    for (const auto s : r.allowed_symbols)
        u64(b, s);
    return b;
}
simulation::ScenarioConfig read_config(const Bytes &b) {
    Cursor x{b};
    if (x.word() != 1 || x.word() != simulation::SimulationRunner::generator_version)
        throw std::runtime_error("unsupported exchange or generator version");
    simulation::ScenarioConfig c;
    const auto scenario = x.word();
    if (scenario > 4)
        throw std::runtime_error("invalid scenario");
    c.scenario = static_cast<simulation::Scenario>(scenario);
    c.seed = x.word();
    const auto p = x.word(), commands = x.word(), orders = x.word(), requests = x.word();
    if (p > 16 || commands > 1024 || orders > 1024 || requests > 1024)
        throw std::runtime_error("configuration bounds");
    c.participants = static_cast<std::size_t>(p);
    c.max_commands = static_cast<std::size_t>(commands);
    c.max_order_history = static_cast<std::size_t>(orders);
    c.max_requests_per_session = static_cast<std::size_t>(requests);
    c.reference_price = std::bit_cast<std::int64_t>(x.word());
    c.price_variation = std::bit_cast<std::int64_t>(x.word());
    c.min_quantity = x.word();
    c.max_quantity = x.word();
    const auto buy = x.word(), cancel = x.word(), n = x.word();
    if (buy > 100 || cancel > 100 || n > 16)
        throw std::runtime_error("configuration bounds");
    c.buy_weight = static_cast<std::uint32_t>(buy);
    c.cancel_percent = static_cast<std::uint32_t>(cancel);
    c.symbols.clear();
    for (std::uint64_t i = 0; i < n; ++i) {
        const auto s = x.word();
        if (s > UINT32_MAX)
            throw std::runtime_error("invalid symbol");
        c.symbols.push_back(static_cast<std::uint32_t>(s));
    }
    auto &r = c.risk;
    r.max_order_quantity = x.word();
    r.max_order_notional = x.word();
    r.max_open_notional = x.word();
    const auto open = x.word();
    if (open > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("invalid limit");
    r.max_open_orders = static_cast<std::size_t>(open);
    r.max_abs_position = x.word();
    r.max_market_price = std::bit_cast<std::int64_t>(x.word());
    const auto allowed = x.word();
    if (allowed > 1024)
        throw std::runtime_error("risk symbol limit");
    r.allowed_symbols.clear();
    for (std::uint64_t i = 0; i < allowed; ++i) {
        const auto s = x.word();
        if (s > UINT32_MAX)
            throw std::runtime_error("invalid symbol");
        r.allowed_symbols.push_back(static_cast<std::uint32_t>(s));
    }
    x.end();
    const auto error = simulation::SimulationRunner::validate(c);
    if (!error.empty())
        throw std::runtime_error("invalid configuration: " + error);
    return c;
}
Bytes command_bytes(const simulation::SimCommand &c) {
    Bytes b;
    for (const auto v :
         {c.step, c.session, c.request, c.client_order, static_cast<std::uint64_t>(c.kind),
          static_cast<std::uint64_t>(c.symbol), static_cast<std::uint64_t>(c.side),
          static_cast<std::uint64_t>(c.type), static_cast<std::uint64_t>(c.price), c.quantity})
        u64(b, v);
    return b;
}
simulation::SimCommand read_command(const Bytes &b, std::uint64_t seq, std::size_t participants) {
    Cursor x{b};
    simulation::SimCommand c;
    c.step = x.word();
    c.session = x.word();
    c.request = x.word();
    c.client_order = x.word();
    const auto kind = x.word(), symbol = x.word(), side = x.word(), type = x.word();
    c.price = std::bit_cast<std::int64_t>(x.word());
    c.quantity = x.word();
    x.end();
    if (c.step != seq || c.session == 0 || c.session > participants || c.request == 0 || kind > 1 ||
        symbol > UINT32_MAX || side > 1 || type > 1)
        throw std::runtime_error("invalid command fields");
    c.kind = static_cast<simulation::CommandKind>(kind);
    c.symbol = static_cast<std::uint32_t>(symbol);
    c.side = static_cast<core::Side>(side);
    c.type = static_cast<core::OrderType>(type);
    return c;
}
struct Record {
    std::uint64_t offset, seq;
    std::uint16_t type;
    Bytes payload;
};
bool read_exact(std::istream &in, std::uint8_t *p, std::size_t n) {
    in.read(reinterpret_cast<char *>(p), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in.gcount()) == n;
}
} // namespace

std::uint32_t crc32(const std::uint8_t *data, std::size_t size) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}
bool EventRecorder::write_record(std::uint16_t type, std::uint64_t seq, const Bytes &payload) {
    if (failed_)
        return false;
    try {
        if (payload.size() > max_record_size - 24 || bytes_ > max_log_size - (payload.size() + 24))
            throw std::runtime_error("log size limit");
        Bytes frame;
        frame.reserve(payload.size() + 24);
        u32(frame, static_cast<std::uint32_t>(payload.size() + 24));
        u16(frame, type);
        u16(frame, 0);
        u64(frame, seq);
        u32(frame, static_cast<std::uint32_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());
        u32(frame, crc32(frame.data(), frame.size()));
        stream_.write(reinterpret_cast<const char *>(frame.data()),
                      static_cast<std::streamsize>(frame.size()));
        stream_.flush();
        if (!stream_)
            throw std::runtime_error("log write or flush failed");
        bytes_ += frame.size();
        return true;
    } catch (const std::exception &e) {
        failed_ = true;
        try {
            error_ = e.what();
        } catch (...) {
        }
        return false;
    } catch (...) {
        failed_ = true;
        return false;
    }
}
bool EventRecorder::begin(const simulation::ScenarioConfig &c) {
    if (begun_ || failed_)
        return false;
    try {
        if (!simulation::SimulationRunner::validate(c).empty())
            throw std::runtime_error("invalid config");
        stream_.write(reinterpret_cast<const char *>(magic.data()),
                      static_cast<std::streamsize>(magic.size()));
        stream_.flush();
        if (!stream_)
            throw std::runtime_error("header write failed");
        bytes_ = magic.size();
        Bytes b;
        u64(b, 1);
        u64(b, 0);
        const auto config = config_bytes(c);
        b.insert(b.end(), config.begin(), config.end());
        begun_ = write_record(1, 0, b);
        return begun_;
    } catch (const std::exception &e) {
        failed_ = true;
        try {
            error_ = e.what();
        } catch (...) {
        }
        return false;
    } catch (...) {
        failed_ = true;
        return false;
    }
}
bool EventRecorder::command(const simulation::SimCommand &c) {
    if (!begun_ || failed_ || pending_ || c.step != next_)
        return false;
    try {
        if (!write_record(2, next_, command_bytes(c)))
            return false;
        pending_ = true;
        return true;
    } catch (...) {
        failed_ = true;
        return false;
    }
}
bool EventRecorder::outcome(const simulation::SimCommand &c, exchange::Result result,
                            const exchange::Exchange &exchange) {
    if (!begun_ || failed_ || !pending_ || c.step != next_)
        return false;
    try {
        Bytes b;
        u64(b, static_cast<std::uint64_t>(result));
        words(b, exchange.verification_state());
        if (!write_record(3, next_, b))
            return false;
        pending_ = false;
        ++next_;
        return true;
    } catch (const std::exception &e) {
        failed_ = true;
        try {
            error_ = e.what();
        } catch (...) {
        }
        return false;
    } catch (...) {
        failed_ = true;
        return false;
    }
}
bool EventRecorder::finish(const simulation::SimulationResult &r, simulation::Lifecycle state,
                           const exchange::Exchange &exchange) {
    if (!begun_ || failed_ || pending_ || state != simulation::Lifecycle::Completed ||
        r.admitted + 1 != next_)
        return false;
    try {
        Bytes b;
        for (const auto v :
             {r.generated, r.admitted, r.accepted_orders, r.rejected_orders, r.cancelled,
              r.rejected_cancels, r.trades, r.aggregated_trades, r.executed_quantity, r.digest})
            u64(b, v);
        words(b, exchange.verification_state());
        if (!write_record(4, next_ - 1, b))
            return false;
        begun_ = false;
        return true;
    } catch (const std::exception &e) {
        failed_ = true;
        try {
            error_ = e.what();
        } catch (...) {
        }
        return false;
    } catch (...) {
        failed_ = true;
        return false;
    }
}

ReplayResult verify(std::istream &in) {
    ReplayResult result;
    std::vector<Record> records;
    std::uint64_t offset = 0;
    try {
        std::array<std::uint8_t, 8> m{};
        if (!read_exact(in, m.data(), m.size()) || m != magic)
            throw std::runtime_error("invalid or truncated magic");
        offset = 8;
        while (true) {
            result.offset = offset;
            std::array<std::uint8_t, 4> len{};
            in.read(reinterpret_cast<char *>(len.data()), 4);
            const auto got = in.gcount();
            if (got == 0 && in.eof())
                break;
            if (got != 4)
                throw std::runtime_error("truncated record length");
            const auto length = r32(len.data());
            if (length < 24 || length > max_record_size || offset > max_log_size - length)
                throw std::runtime_error("invalid record length");
            Bytes frame(length);
            std::copy(len.begin(), len.end(), frame.begin());
            if (!read_exact(in, frame.data() + 4, length - 4))
                throw std::runtime_error("truncated record");
            if (r16(frame.data() + 6) != 0 || r32(frame.data() + 16) != length - 24)
                throw std::runtime_error("invalid flags or payload length");
            if (r32(frame.data() + length - 4) != crc32(frame.data(), length - 4))
                throw std::runtime_error("checksum mismatch");
            Cursor s{frame};
            s.pos = 8;
            const auto seq = s.word();
            records.push_back(
                {offset, seq, r16(frame.data() + 4), Bytes(frame.begin() + 20, frame.end() - 4)});
            offset += length;
            if (records.size() > 2050)
                throw std::runtime_error("record count limit");
        }
        if (records.size() < 2 || records[0].type != 1 || records[0].seq != 0)
            throw std::runtime_error("missing header");
        result.offset = records[0].offset;
        Cursor head{records[0].payload};
        if (head.word() != 1 || head.word() != 0)
            throw std::runtime_error("unsupported format version");
        Bytes config(head.b.begin() + static_cast<std::ptrdiff_t>(head.pos), head.b.end());
        const auto c = read_config(config);
        if (records.size() != 2 + 2 * c.max_commands)
            throw std::runtime_error("incomplete run or record count mismatch");
        std::vector<simulation::SimCommand> commands;
        commands.reserve(c.max_commands);
        for (std::size_t i = 0; i < c.max_commands; ++i) {
            const auto &command = records[1 + 2 * i], &outcome = records[2 + 2 * i];
            result.offset = command.offset;
            result.sequence = i + 1;
            if (command.type != 2 || command.seq != i + 1)
                throw std::runtime_error("record type or sequence discontinuity");
            commands.push_back(read_command(command.payload, i + 1, c.participants));
            result.offset = outcome.offset;
            if (outcome.type != 3 || outcome.seq != i + 1)
                throw std::runtime_error("record type or sequence discontinuity");
            Cursor o{outcome.payload};
            if (o.word() > static_cast<std::uint64_t>(exchange::Result::RiskOverflow))
                throw std::runtime_error("invalid outcome");
            (void)read_words(o);
            o.end();
        }
        const auto &last = records.back();
        result.offset = last.offset;
        result.sequence = last.seq;
        if (last.type != 4 || last.seq != c.max_commands)
            throw std::runtime_error("missing terminal record");
        Cursor end{last.payload};
        std::array<std::uint64_t, 10> totals{};
        for (auto &v : totals)
            v = end.word();
        const auto final_state = read_words(end);
        end.end();
        if (totals[0] != c.max_commands || totals[1] != c.max_commands || totals[6] != totals[7])
            throw std::runtime_error("invalid terminal counts");
        exchange::Exchange exchange(c.participants, c.max_order_history, 2 * c.max_commands + 2,
                                    c.max_requests_per_session);
        for (std::size_t i = 1; i <= c.participants; ++i)
            if (!exchange.open_session(i) || !exchange.configure_risk(i, c.risk))
                throw std::runtime_error("session initialization failed");
        std::uint64_t accepted_orders = 0, rejected_orders = 0, cancelled = 0, rejected_cancels = 0;
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const auto &cmd = commands[i];
            const auto &record = records[2 + 2 * i];
            result.offset = record.offset;
            result.sequence = i + 1;
            const auto actual = cmd.kind == simulation::CommandKind::Cancel
                                    ? exchange.cancel(cmd.session, cmd.request, cmd.client_order)
                                    : exchange.submit(cmd.session, cmd.request, cmd.client_order,
                                                      core::SymbolId{cmd.symbol}, cmd.side,
                                                      cmd.type, cmd.price, cmd.quantity);
            Cursor o{record.payload};
            const auto expected = o.word();
            const auto state = read_words(o);
            o.end();
            if (expected != static_cast<std::uint64_t>(actual))
                throw std::runtime_error("outcome mismatch: expected " + std::to_string(expected) +
                                         " actual " +
                                         std::to_string(static_cast<std::uint64_t>(actual)));
            const auto actual_state = exchange.verification_state();
            if (state != actual_state) {
                const auto mismatch = std::mismatch(state.begin(), state.end(),
                                                    actual_state.begin(), actual_state.end());
                const auto index = static_cast<std::size_t>(mismatch.first - state.begin());
                const auto expected_word = mismatch.first == state.end()
                                               ? std::string("end")
                                               : std::to_string(*mismatch.first);
                const auto actual_word = mismatch.second == actual_state.end()
                                             ? std::string("end")
                                             : std::to_string(*mismatch.second);
                throw std::runtime_error("state mismatch at word " + std::to_string(index) +
                                         ": expected " + expected_word + " actual " + actual_word);
            }
            if (cmd.kind == simulation::CommandKind::Cancel) {
                if (actual == exchange::Result::Accepted)
                    ++cancelled;
                else
                    ++rejected_cancels;
            } else {
                if (actual == exchange::Result::Accepted)
                    ++accepted_orders;
                else
                    ++rejected_orders;
            }
            for (std::size_t session = 1; session <= c.participants; ++session)
                while (exchange.front(session))
                    exchange.pop(session);
        }
        result.offset = last.offset;
        result.sequence = last.seq;
        std::uint64_t executed_quantity = 0;
        for (std::size_t i = 0; i < exchange.trade_count(); ++i) {
            const auto trade = exchange.trade(i);
            if (!trade ||
                trade->quantity > std::numeric_limits<std::uint64_t>::max() - executed_quantity)
                throw std::runtime_error("executed quantity overflow");
            executed_quantity += trade->quantity;
        }
        if (totals[2] != accepted_orders || totals[3] != rejected_orders ||
            totals[4] != cancelled || totals[5] != rejected_cancels ||
            totals[6] != exchange.trade_count())
            throw std::runtime_error("terminal count mismatch");
        if (totals[8] != executed_quantity)
            throw std::runtime_error("executed quantity mismatch");
        for (const auto symbol : c.symbols) {
            if (!exchange.subscribe(1, core::SymbolId{symbol}))
                throw std::runtime_error("final snapshot failed");
            const auto top = exchange.market_front(1);
            if (!top || top->symbol != symbol)
                throw std::runtime_error("final snapshot mismatch");
            exchange.market_pop(1, *top);
        }
        if (final_state != exchange.verification_state())
            throw std::runtime_error("final state mismatch");
        result.commands = commands.size();
        result.accepted = accepted_orders + cancelled;
        result.rejected = rejected_orders + rejected_cancels;
        result.trades = exchange.trade_count();
        result.verified = true;
        result.offset = 0;
        result.sequence = 0;
    } catch (const std::exception &e) {
        try {
            result.error = e.what();
        } catch (...) {
        }
    } catch (...) {
        try {
            result.error = "unexpected replay failure";
        } catch (...) {
        }
    }
    return result;
}
} // namespace chronos::eventlog
