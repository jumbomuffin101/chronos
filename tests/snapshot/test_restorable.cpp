#include "chronos/eventlog/event_log.hpp"
#include "chronos/snapshot/checkpoint.hpp"
#include "chronos/snapshot/restorable.hpp"
#include <gtest/gtest.h>
#include <sstream>

using namespace chronos;
namespace {
struct SimulationFixture {
    simulation::ScenarioConfig config;
    std::ostringstream log{std::ios::binary};
    eventlog::EventRecorder recorder{log};
    simulation::SimulationRunner runner;
    explicit SimulationFixture(simulation::ScenarioConfig c)
        : config(c), runner(config, &recorder, true) {
        runner.start();
        runner.run_batch(config.max_commands);
    }
};
std::uint64_t live_id() {
    exchange::Exchange book;
    EXPECT_TRUE(book.open_session(1));
    EXPECT_EQ(
        book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    const auto report = book.front(1);
    EXPECT_TRUE(report);
    return report ? report->internal_order_id : 0;
}
void put32(std::string &s, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        s[at + i] = static_cast<char>(value >> (8U * i));
}
std::uint64_t get64(const std::string &s, std::size_t at) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[at + i])) << (8U * i);
    return value;
}
std::uint32_t get32(const std::string &s, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + i])) << (8U * i);
    return value;
}
void put64(std::string &s, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        s[at + i] = static_cast<char>(value >> (8U * i));
}
void fix_record(std::string &s, std::size_t at) {
    const auto length = get32(s, at);
    put32(s, at + length - 4,
          eventlog::crc32(reinterpret_cast<const std::uint8_t *>(s.data() + at), length - 4));
}
void fix_checksum(std::string &s) {
    put32(s, s.size() - 4,
          eventlog::crc32(reinterpret_cast<const std::uint8_t *>(s.data()), s.size() - 4));
}
} // namespace
TEST(Restorable, PrivateRunRoundTripAndFollowOnCommands) {
    simulation::ScenarioConfig c;
    c.scenario = simulation::Scenario::LiquiditySweep;
    c.max_commands = 10;
    c.symbols = {1, 2};
    c.min_quantity = 2;
    c.max_quantity = 3;
    SimulationFixture run(c);
    ASSERT_EQ(run.runner.result().status.state, simulation::Lifecycle::Completed);
    auto *original = run.runner.completed_isolated_exchange();
    ASSERT_NE(original, nullptr);
    const auto before = live_id();
    std::ostringstream image(std::ios::binary);
    const auto written = snapshot::write_image(image, run.log.str(), *original, 17);
    ASSERT_TRUE(written.ok) << written.error;
    EXPECT_EQ(written.ingress, 10U);
    std::istringstream input(image.str(), std::ios::binary);
    std::unique_ptr<exchange::Exchange> restored;
    const auto loaded = snapshot::restore_image(input, restored);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    ASSERT_TRUE(restored);
    EXPECT_EQ(loaded.generation, 17U);
    EXPECT_EQ(original->isolated_continuation_state(1024 * 1024),
              restored->isolated_continuation_state(1024 * 1024));
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::istringstream retry(image.str(), std::ios::binary);
        std::unique_ptr<exchange::Exchange> candidate;
        const auto result = snapshot::restore_image(retry, candidate);
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_TRUE(candidate);
        EXPECT_EQ(restored->isolated_continuation_state(1024 * 1024),
                  candidate->isolated_continuation_state(1024 * 1024));
    }
    std::ostringstream second_image(std::ios::binary);
    ASSERT_TRUE(snapshot::write_image(second_image, run.log.str(), *restored, 18).ok);
    std::istringstream second_input(second_image.str(), std::ios::binary);
    std::unique_ptr<exchange::Exchange> second_candidate;
    ASSERT_TRUE(snapshot::restore_image(second_input, second_candidate).ok);
    EXPECT_EQ(original->isolated_continuation_state(1024 * 1024),
              second_candidate->isolated_continuation_state(1024 * 1024));
    EXPECT_EQ(live_id(), before + 1);
    for (auto *book : {original, restored.get()}) {
        EXPECT_EQ(book->submit(1, 1000, 1000, core::SymbolId{2}, core::Side::Sell,
                               core::OrderType::Limit, 110, 5),
                  exchange::Result::Accepted);
        EXPECT_EQ(book->submit(2, 1000, 1000, core::SymbolId{2}, core::Side::Buy,
                               core::OrderType::Limit, 110, 2),
                  exchange::Result::Accepted);
        EXPECT_EQ(book->cancel(1, 1001, 1000), exchange::Result::Accepted);
        EXPECT_EQ(book->submit(2, 1001, 1001, core::SymbolId{2}, core::Side::Buy,
                               core::OrderType::Limit, 110, 0),
                  exchange::Result::RiskQuantity);
    }
    EXPECT_EQ(original->isolated_continuation_state(1024 * 1024),
              restored->isolated_continuation_state(1024 * 1024));
    const auto both = [&](auto command) {
        const auto a = command(*original);
        const auto b = command(*restored);
        EXPECT_EQ(a, b);
        EXPECT_EQ(original->isolated_continuation_state(1024 * 1024),
                  restored->isolated_continuation_state(1024 * 1024));
        return a;
    };
    EXPECT_EQ(both([](auto &book) {
                  return book.submit(1, 1002, 1002, core::SymbolId{3}, core::Side::Sell,
                                     core::OrderType::Limit, 100, 2);
              }),
              exchange::Result::Accepted);
    ASSERT_TRUE(original->find(1, 1002));
    ASSERT_TRUE(restored->find(1, 1002));
    EXPECT_EQ(original->find(1, 1002)->id, restored->find(1, 1002)->id);
    EXPECT_EQ(both([](auto &book) {
                  return book.submit(1, 1003, 1003, core::SymbolId{3}, core::Side::Sell,
                                     core::OrderType::Limit, 101, 3);
              }),
              exchange::Result::Accepted);
    EXPECT_EQ(both([](auto &book) {
                  return book.submit(2, 1002, 1002, core::SymbolId{3}, core::Side::Buy,
                                     core::OrderType::Limit, 101, 4);
              }),
              exchange::Result::Accepted);
    ASSERT_TRUE(original->find(1, 1003));
    EXPECT_EQ(original->find(1, 1003)->quantity, 1U);
    EXPECT_EQ(both([](auto &book) {
                  return book.submit(3, 1000, 1000, core::SymbolId{3}, core::Side::Buy,
                                     core::OrderType::Market, 0, 1);
              }),
              exchange::Result::Accepted);
    EXPECT_FALSE(original->find(1, 1003));
    EXPECT_EQ(both([](auto &book) { return book.cancel(1, 1004, 1003); }),
              exchange::Result::NotFound);
}
TEST(Restorable, RejectsRechecksummedAlteredOrIncompleteHistory) {
    simulation::ScenarioConfig c;
    c.max_commands = 2;
    SimulationFixture run(c);
    auto *source = run.runner.completed_isolated_exchange();
    ASSERT_NE(source, nullptr);
    std::ostringstream out(std::ios::binary);
    ASSERT_TRUE(snapshot::write_image(out, run.log.str(), *source, 1).ok);
    const auto original = out.str();
    const auto log_header = std::size_t{48} + 8;
    const auto first_command = log_header + get32(original, log_header);
    const auto first_outcome = first_command + get32(original, first_command);
    const auto second_command = first_outcome + get32(original, first_outcome);
    const auto second_outcome = second_command + get32(original, second_command);
    const auto terminal = second_outcome + get32(original, second_outcome);
    const auto before = live_id();
    auto altered = original;
    altered[first_command + 20 + 9 * 8] ^= 1; // encoded quantity
    fix_record(altered, first_command);
    fix_checksum(altered);
    std::istringstream altered_in(altered, std::ios::binary);
    std::unique_ptr<exchange::Exchange> result;
    const auto mismatch = snapshot::restore_image(altered_in, result);
    EXPECT_FALSE(mismatch.ok);
    EXPECT_NE(mismatch.error.find("mismatch"), std::string::npos);
    EXPECT_FALSE(result);
    for (const std::uint64_t seq : {1U, 3U}) {
        auto discontinuous = original;
        put64(discontinuous, second_command + 8, seq);
        fix_record(discontinuous, second_command);
        fix_checksum(discontinuous);
        std::istringstream input(discontinuous, std::ios::binary);
        const auto error = snapshot::restore_image(input, result);
        EXPECT_FALSE(error.ok);
        EXPECT_NE(error.error.find("sequence discontinuity"), std::string::npos);
        EXPECT_FALSE(result);
    }
    auto incomplete = original;
    const auto terminal_size = get32(incomplete, terminal);
    incomplete.erase(terminal, terminal_size);
    put64(incomplete, 32, get64(original, 32) - terminal_size);
    fix_checksum(incomplete);
    std::istringstream incomplete_in(incomplete, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(incomplete_in, result).ok);
    EXPECT_FALSE(result);
    EXPECT_EQ(live_id(), before + 1);
}
TEST(Restorable, EmptyBookAfterRiskRejectionRoundTrips) {
    simulation::ScenarioConfig config;
    config.max_commands = 1;
    config.risk.max_order_quantity = 0;
    SimulationFixture run(config);
    ASSERT_EQ(run.runner.result().rejected_orders, 1U);
    auto *source = run.runner.completed_isolated_exchange();
    ASSERT_NE(source, nullptr);
    std::ostringstream output(std::ios::binary);
    ASSERT_TRUE(snapshot::write_image(output, run.log.str(), *source, 1).ok);
    std::istringstream input(output.str(), std::ios::binary);
    std::unique_ptr<exchange::Exchange> restored;
    ASSERT_TRUE(snapshot::restore_image(input, restored).ok);
    EXPECT_EQ(source->isolated_continuation_state(1024 * 1024),
              restored->isolated_continuation_state(1024 * 1024));
    EXPECT_EQ(restored->trade_count(), 0U);
}
TEST(Restorable, RejectsTruncatedCorruptedAndInconsistentImages) {
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    SimulationFixture run(c);
    ASSERT_NE(run.runner.completed_isolated_exchange(), nullptr);
    std::ostringstream image(std::ios::binary);
    ASSERT_TRUE(
        snapshot::write_image(image, run.log.str(), *run.runner.completed_isolated_exchange(), 2)
            .ok);
    const auto bytes = image.str();
    const auto live_before = live_id();
    auto existing = exchange::Exchange::isolated();
    ASSERT_TRUE(existing->open_session(999));
    auto *identity = existing.get();
    std::istringstream occupied(bytes, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(occupied, existing).ok);
    EXPECT_EQ(existing.get(), identity);
    std::unique_ptr<exchange::Exchange> destination;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::istringstream in(bytes.substr(0, i), std::ios::binary);
        ASSERT_FALSE(snapshot::restore_image(in, destination).ok) << i;
        ASSERT_FALSE(destination);
    }
    auto corrupt = bytes;
    corrupt[0] ^= 1;
    std::istringstream invalid_magic(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(invalid_magic, destination).ok);
    corrupt = bytes;
    corrupt[8] = 2;
    std::istringstream invalid_version(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(invalid_version, destination).ok);
    corrupt = bytes;
    corrupt.back() ^= 1;
    std::istringstream invalid_crc(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(invalid_crc, destination).ok);
    corrupt = bytes;
    corrupt[corrupt.size() - 5] ^= 1;
    put32(corrupt, corrupt.size() - 4,
          eventlog::crc32(reinterpret_cast<const std::uint8_t *>(corrupt.data()),
                          corrupt.size() - 4));
    std::istringstream bad_state(corrupt, std::ios::binary);
    const auto result = snapshot::restore_image(bad_state, destination);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("state mismatch"), std::string::npos);
    EXPECT_FALSE(destination);
    corrupt = bytes;
    corrupt[24] ^= 1;
    fix_checksum(corrupt);
    std::istringstream bad_boundary(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(bad_boundary, destination).ok);
    corrupt = bytes;
    put32(corrupt, 32, 0xffffffffU);
    std::istringstream oversized(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(oversized, destination).ok);
    std::istringstream failed_reader(bytes, std::ios::binary);
    failed_reader.setstate(std::ios::badbit);
    EXPECT_FALSE(snapshot::restore_image(failed_reader, destination).ok);
    corrupt = bytes;
    corrupt[48] ^= 1;
    fix_checksum(corrupt);
    std::istringstream corrupt_inner_log(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(corrupt_inner_log, destination).ok);
    corrupt = bytes;
    const auto state_offset = 48U + static_cast<std::size_t>(get64(corrupt, 32));
    corrupt[state_offset + 8] ^= 1; // exact accepted-command counter
    fix_checksum(corrupt);
    std::istringstream corrupted_counter(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(corrupted_counter, destination).ok);
    corrupt = bytes;
    corrupt[corrupt.size() - 4 - 16] ^= 1; // final engine book revision
    fix_checksum(corrupt);
    std::istringstream corrupted_revision(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(corrupted_revision, destination).ok);
    corrupt = bytes;
    for (std::size_t i = 0; i < 8; ++i)
        corrupt[corrupt.size() - 4 - 32 + i] = static_cast<char>(0xff); // impossible high-water
    fix_checksum(corrupt);
    std::istringstream corrupted_highwater(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(corrupted_highwater, destination).ok);
    corrupt = bytes;
    corrupt[corrupt.size() - 4 - 64] = 0; // zero session capacity, with existing sessions
    fix_checksum(corrupt);
    std::istringstream corrupted_capacity(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::restore_image(corrupted_capacity, destination).ok);
    EXPECT_EQ(live_id(), live_before + 1);
}
TEST(Restorable, RejectsLiveOrChangedSourceAndStreamFailure) {
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    SimulationFixture run(c);
    exchange::Exchange live;
    std::ostringstream out(std::ios::binary);
    EXPECT_FALSE(snapshot::write_image(out, run.log.str(), live, 1).ok);
    auto *source = run.runner.completed_isolated_exchange();
    ASSERT_NE(source, nullptr);
    std::ostringstream bad;
    bad.setstate(std::ios::badbit);
    EXPECT_FALSE(snapshot::write_image(bad, run.log.str(), *source, 1).ok);
    class ShortWriter final : public std::streambuf {
        std::streamsize xsputn(const char *, std::streamsize n) override { return n / 2; }
    } short_buffer;
    std::ostream short_output(&short_buffer);
    EXPECT_FALSE(snapshot::write_image(short_output, run.log.str(), *source, 1).ok);
    ASSERT_EQ(source->submit(1, 999, 999, core::SymbolId{1}, core::Side::Buy,
                             core::OrderType::Limit, 100, 1),
              exchange::Result::Accepted);
    EXPECT_FALSE(snapshot::write_image(out, run.log.str(), *source, 1).ok);
}
TEST(Restorable, RejectsChecksumPreservingIdentityRiskAndFifoTampering) {
    simulation::ScenarioConfig c;
    c.max_commands = 5;
    c.buy_weight = 100;
    c.price_variation = 0;
    SimulationFixture run(c);
    auto *source = run.runner.completed_isolated_exchange();
    ASSERT_NE(source, nullptr);
    std::ostringstream output(std::ios::binary);
    ASSERT_TRUE(snapshot::write_image(output, run.log.str(), *source, 1).ok);
    const auto bytes = output.str();
    const auto base = 48U + static_cast<std::size_t>(get64(bytes, 32));
    const auto word = [&](std::size_t index) { return get64(bytes, base + 8 * index); };
    const auto rejects = [&](std::string corrupt) {
        fix_checksum(corrupt);
        std::istringstream input(corrupt, std::ios::binary);
        std::unique_ptr<exchange::Exchange> result;
        const auto status = snapshot::restore_image(input, result);
        EXPECT_FALSE(status.ok);
        EXPECT_NE(status.error.find("state mismatch"), std::string::npos);
        EXPECT_FALSE(result);
    };
    auto corrupt = bytes;
    corrupt[base + 8 * 7] ^= 1; // first session's risk limit
    rejects(corrupt);
    std::size_t cursor = 5;
    for (std::uint64_t i = 0; i < word(4); ++i) {
        cursor += 8; // session, kill and six risk fields
        auto count = word(cursor++);
        cursor += static_cast<std::size_t>(count);
        count = word(cursor++);
        cursor += static_cast<std::size_t>(count);
        count = word(cursor++);
        cursor += static_cast<std::size_t>(count) * 2;
        count = word(cursor++);
        cursor += static_cast<std::size_t>(count) * 11;
        ++cursor; // subscription generation
        const auto subscriptions = word(cursor++);
        for (std::uint64_t j = 0; j < subscriptions; ++j) {
            cursor += 5;
            count = word(cursor++);
            cursor += static_cast<std::size_t>(count) * 10;
        }
    }
    const auto orders = word(cursor++);
    ASSERT_GE(orders, 2U);
    const auto order_base = cursor;
    corrupt = bytes;
    for (std::size_t i = 0; i < 8; ++i)
        corrupt[base + 8 * (order_base + 6) + i] = corrupt[base + 8 * order_base + i];
    rejects(corrupt); // duplicate exact internal ID
    corrupt = bytes;
    corrupt[base + 8 * (order_base + 1)] = static_cast<char>(0xff);
    rejects(corrupt); // missing order owner
    cursor += static_cast<std::size_t>(orders) * 6;
    auto count = word(cursor++);
    cursor += static_cast<std::size_t>(count) * 9;
    count = word(cursor++);
    cursor += static_cast<std::size_t>(count) * 2;
    ASSERT_GE(word(cursor++), 1U); // engines
    cursor += 4;                   // symbol and matching counters
    ASSERT_GE(word(cursor++), 1U); // buy levels
    corrupt = bytes;
    corrupt[base + 8 * (cursor + 1)] ^= 1; // inconsistent aggregate / reservation input
    rejects(corrupt);
    cursor += 3;                   // price, quantity, order count
    ASSERT_GE(word(cursor++), 2U); // FIFO order array
    corrupt = bytes;
    for (std::size_t i = 0; i < 24; ++i)
        std::swap(corrupt[base + 8 * cursor + i], corrupt[base + 8 * (cursor + 3) + i]);
    rejects(corrupt);
}
TEST(Restorable, RejectsChecksumPreservingPositionTampering) {
    simulation::ScenarioConfig c;
    c.scenario = simulation::Scenario::LiquiditySweep;
    c.max_commands = 5;
    c.min_quantity = 2;
    c.max_quantity = 3;
    SimulationFixture run(c);
    auto *source = run.runner.completed_isolated_exchange();
    ASSERT_NE(source, nullptr);
    std::ostringstream output(std::ios::binary);
    ASSERT_TRUE(snapshot::write_image(output, run.log.str(), *source, 1).ok);
    auto bytes = output.str();
    const auto base = 48U + static_cast<std::size_t>(get64(bytes, 32));
    const auto word = [&](std::size_t index) { return get64(bytes, base + 8 * index); };
    std::size_t cursor = 5;
    bool changed = false;
    for (std::uint64_t i = 0; i < word(4) && !changed; ++i) {
        cursor += 8;
        auto count = word(cursor++);
        cursor += static_cast<std::size_t>(count);
        count = word(cursor++);
        cursor += static_cast<std::size_t>(count);
        count = word(cursor++);
        if (count != 0) {
            bytes[base + 8 * (cursor + 1)] ^= 1;
            changed = true;
            break;
        }
        cursor += static_cast<std::size_t>(count) * 2;
        count = word(cursor++);
        cursor += static_cast<std::size_t>(count) * 11;
        ++cursor;
        const auto subscriptions = word(cursor++);
        for (std::uint64_t j = 0; j < subscriptions; ++j) {
            cursor += 5;
            count = word(cursor++);
            cursor += static_cast<std::size_t>(count) * 10;
        }
    }
    ASSERT_TRUE(changed);
    fix_checksum(bytes);
    std::istringstream input(bytes, std::ios::binary);
    std::unique_ptr<exchange::Exchange> candidate;
    const auto result = snapshot::restore_image(input, candidate);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("state mismatch"), std::string::npos);
    EXPECT_FALSE(candidate);
}
