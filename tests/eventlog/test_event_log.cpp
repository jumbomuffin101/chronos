#include "chronos/eventlog/event_log.hpp"
#include "chronos/exchange/live_owner.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <streambuf>

using namespace chronos;
namespace {
std::string recording(simulation::ScenarioConfig c) {
    std::ostringstream out(std::ios::binary);
    eventlog::EventRecorder recorder(out);
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(c.max_commands);
    EXPECT_EQ(runner.result().status.state, simulation::Lifecycle::Completed) << recorder.error();
    return out.str();
}
eventlog::ReplayResult replay(const std::string &s) {
    std::istringstream in(s, std::ios::binary);
    return eventlog::verify(in);
}
std::uint32_t read32(const std::string &s, std::size_t at) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + i])) << (i * 8U);
    return result;
}
void put32(std::string &s, std::size_t at, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        s[at + i] = static_cast<char>(v >> (i * 8U));
}
void checksum(std::string &s, std::size_t at) {
    const auto size = read32(s, at);
    put32(s, at + size - 4,
          eventlog::crc32(reinterpret_cast<const std::uint8_t *>(s.data() + at), size - 4));
}
class ShortBuffer final : public std::streambuf {
  public:
    explicit ShortBuffer(int fail_at) : fail_at_(fail_at) {}
    std::string data;

  protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        if (++calls_ == fail_at_) {
            data.append(s, static_cast<std::size_t>(n / 2));
            return n / 2;
        }
        data.append(s, static_cast<std::size_t>(n));
        return n;
    }
    int sync() override { return 0; }

  private:
    int fail_at_, calls_ = 0;
};
class FlushFailureBuffer final : public std::stringbuf {
  protected:
    int sync() override { return ++calls_ == 3 ? -1 : 0; }

  private:
    int calls_ = 0;
};
} // namespace

TEST(EventLog, Crc32KnownVectorAndLittleEndianHeader) {
    const std::string vector = "123456789";
    EXPECT_EQ(eventlog::crc32(reinterpret_cast<const std::uint8_t *>(vector.data()), vector.size()),
              0xcbf43926U);
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    const auto s = recording(c);
    ASSERT_GE(s.size(), 36U);
    EXPECT_EQ(s.substr(0, 8), std::string("CHRONOS\0", 8));
    EXPECT_EQ(static_cast<unsigned char>(s[12]), 1U);
    EXPECT_EQ(static_cast<unsigned char>(s[13]), 0U);
    EXPECT_EQ(static_cast<unsigned char>(s[28]), 1U);
    EXPECT_EQ(static_cast<unsigned char>(s[29]), 0U);
    EXPECT_TRUE(replay(s).verified);
}
TEST(EventLog, CompleteVersionOneGoldenFixture) {
    std::ifstream input(std::string(CHRONOS_TEST_DATA_DIR) + "/eventlog/golden_v1.hex");
    ASSERT_TRUE(input);
    std::string hex, line;
    while (input >> line)
        hex += line;
    ASSERT_EQ(hex.size() % 2, 0U);
    std::string expected;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
        ASSERT_EQ(ec, std::errc{});
        ASSERT_EQ(end, hex.data() + i + 2);
        expected.push_back(static_cast<char>(value));
    }
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    EXPECT_EQ(recording(c), expected);
    EXPECT_TRUE(replay(expected).verified);
}
TEST(EventLog, ReplaySweepsPartialFillsAndMultiSymbol) {
    simulation::ScenarioConfig c;
    c.scenario = simulation::Scenario::LiquiditySweep;
    c.max_commands = 5;
    c.symbols = {1, 2};
    c.min_quantity = 2;
    c.max_quantity = 3;
    const auto result = replay(recording(c));
    EXPECT_TRUE(result.verified) << result.error;
    EXPECT_EQ(result.commands, 5U);
    EXPECT_GT(result.trades, 0U);
}
TEST(EventLog, ReplayRiskRejectionsAndCancellations) {
    simulation::ScenarioConfig c;
    c.max_commands = 20;
    c.risk.max_order_quantity = 0;
    auto result = replay(recording(c));
    ASSERT_TRUE(result.verified) << result.error;
    EXPECT_EQ(result.rejected, 20U);
    c = {};
    c.max_commands = 30;
    c.scenario = simulation::Scenario::HighVolume;
    c.cancel_percent = 100;
    result = replay(recording(c));
    EXPECT_TRUE(result.verified) << result.error;
}
TEST(EventLog, EveryTruncationIsRejected) {
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    const auto s = recording(c);
    for (std::size_t i = 0; i < s.size(); ++i)
        ASSERT_FALSE(replay(s.substr(0, i)).verified) << i;
}
TEST(EventLog, RejectsMalformedLengthChecksumVersionTypeAndSequence) {
    simulation::ScenarioConfig c;
    c.max_commands = 2;
    const auto original = recording(c);
    auto s = original;
    s[0] ^= 1;
    EXPECT_FALSE(replay(s).verified);
    s = original;
    put32(s, 8, 0xffffffffU);
    EXPECT_FALSE(replay(s).verified);
    s = original;
    s.back() ^= 1;
    EXPECT_FALSE(replay(s).verified);
    s = original;
    s[28] = 2;
    checksum(s, 8);
    const auto bad_version = replay(s);
    EXPECT_FALSE(bad_version.verified);
    EXPECT_EQ(bad_version.offset, 8U);
    const auto command = 8U + read32(original, 8);
    s = original;
    s[command + 4] = 99;
    checksum(s, command);
    EXPECT_FALSE(replay(s).verified);
    s = original;
    s[command + 8] = 2;
    checksum(s, command);
    EXPECT_FALSE(replay(s).verified);
}
TEST(EventLog, ReportsFirstAuthoritativeStateDivergence) {
    simulation::ScenarioConfig c;
    c.max_commands = 2;
    auto s = recording(c);
    const auto command = 8U + read32(s, 8);
    const auto outcome = command + read32(s, command);
    s[outcome + 20 + 16] ^= 1;
    checksum(s, outcome);
    const auto result = replay(s);
    EXPECT_FALSE(result.verified);
    EXPECT_EQ(result.sequence, 1U);
    EXPECT_NE(result.error.find("state mismatch"), std::string::npos);
    auto malformed = recording(c);
    put32(malformed, outcome + 28, 0xffffffffU);
    checksum(malformed, outcome);
    const auto invalid_state = replay(malformed);
    EXPECT_FALSE(invalid_state.verified);
    EXPECT_NE(invalid_state.error.find("state length"), std::string::npos);
}
TEST(EventLog, ShortCommandWriteDoesNotAdmit) {
    ShortBuffer buffer(3);
    std::ostream out(&buffer);
    eventlog::EventRecorder recorder(out);
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(1);
    EXPECT_EQ(runner.result().status.state, simulation::Lifecycle::Failed);
    EXPECT_EQ(runner.result().generated, 1U);
    EXPECT_EQ(runner.result().admitted, 0U);
    EXPECT_FALSE(replay(buffer.data).verified);
}
TEST(EventLog, ShortOutcomeWriteDoesNotClaimRollbackOrCompleteness) {
    ShortBuffer buffer(4);
    std::ostream out(&buffer);
    eventlog::EventRecorder recorder(out);
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(1);
    EXPECT_EQ(runner.result().status.state, simulation::Lifecycle::Failed);
    EXPECT_EQ(runner.result().admitted, 1U);
    EXPECT_EQ(runner.result().accepted_orders, 1U);
    EXPECT_FALSE(runner.result().summary_complete);
    EXPECT_FALSE(replay(buffer.data).verified);
}
TEST(EventLog, FlushFailureBeforeAdmissionIsExplicit) {
    FlushFailureBuffer buffer;
    std::ostream out(&buffer);
    eventlog::EventRecorder recorder(out);
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(1);
    EXPECT_EQ(runner.result().status.state, simulation::Lifecycle::Failed);
    EXPECT_EQ(runner.result().admitted, 0U);
    EXPECT_FALSE(replay(buffer.str()).verified);
}
TEST(EventLog, SameConfigurationProducesSameBytesWithoutChangingCommands) {
    simulation::ScenarioConfig c;
    c.max_commands = 20;
    c.scenario = simulation::Scenario::Mixed;
    EXPECT_EQ(recording(c), recording(c));
    simulation::SimulationRunner plain(c);
    plain.start();
    plain.run_batch(c.max_commands);
    std::ostringstream out(std::ios::binary);
    eventlog::EventRecorder recorder(out);
    simulation::SimulationRunner logged(c, &recorder);
    logged.start();
    logged.run_batch(c.max_commands);
    ASSERT_EQ(logged.result().status.state, simulation::Lifecycle::Completed);
    EXPECT_EQ(plain.result().outcomes, logged.result().outcomes);
    EXPECT_EQ(plain.result().digest, logged.result().digest);
}
TEST(EventLog, StoppedRecordingIsNeverReportedComplete) {
    simulation::ScenarioConfig c;
    c.max_commands = 3;
    std::ostringstream out(std::ios::binary);
    eventlog::EventRecorder recorder(out);
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(1);
    runner.request_stop();
    EXPECT_FALSE(runner.result().summary_complete);
    EXPECT_FALSE(replay(out.str()).verified);
}
TEST(EventLog, MalformedLaterRecordIsRejectedBeforeReplayMutatesExchange) {
    simulation::ScenarioConfig c;
    c.max_commands = 2;
    auto log = recording(c);
    const auto first_command = 8U + read32(log, 8);
    const auto first_outcome = first_command + read32(log, first_command);
    const auto second_command = first_outcome + read32(log, first_outcome);
    log[second_command + 8] = 1;
    checksum(log, second_command);
    const auto issue_order = [] {
        exchange::Exchange book;
        EXPECT_TRUE(book.open_session(1));
        EXPECT_EQ(book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                              100, 1),
                  exchange::Result::Accepted);
        const auto report = book.front(1);
        EXPECT_TRUE(report.has_value());
        return report ? report->internal_order_id : 0;
    };
    const auto before = issue_order();
    EXPECT_FALSE(replay(log).verified);
    const auto after = issue_order();
    EXPECT_EQ(after, before + 1);
}
TEST(EventLog, ValidReplayConsumesTheSharedProcessOrderIdAllocator) {
    simulation::ScenarioConfig config;
    config.max_commands = 1;
    const auto log = recording(config);
    const auto issue = [] {
        exchange::Exchange book;
        EXPECT_TRUE(book.open_session(1));
        EXPECT_EQ(book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                              100, 1),
                  exchange::Result::Accepted);
        const auto report = book.front(1);
        EXPECT_TRUE(report.has_value());
        return report ? report->internal_order_id : 0;
    };
    const auto before = issue();
    std::istringstream in(log, std::ios::binary);
    ASSERT_TRUE(eventlog::verify(in, eventlog::ReplayAllocation::SharedProcess).verified);
    const auto after = issue();
    EXPECT_EQ(after, before + 2);
}
TEST(EventLog, ExclusiveOwnerAllowsIsolatedReplayButRejectsSharedReplay) {
    simulation::ScenarioConfig config;
    config.max_commands = 1;
    const auto log = recording(config);
    exchange::LiveExchangeOwner owner(16, 1024, 128, 1024, exchange::AllocationMode::Exclusive);
    owner.bind();
    ASSERT_TRUE(owner.open_session(1));
    ASSERT_EQ(
        owner.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 1),
        exchange::Result::Accepted);
    ASSERT_TRUE(owner.front(1));
    const auto before = owner.front(1)->internal_order_id;
    EXPECT_TRUE(replay(log).verified);
    std::istringstream shared(log, std::ios::binary);
    const auto rejected = eventlog::verify(shared, eventlog::ReplayAllocation::SharedProcess);
    EXPECT_FALSE(rejected.verified);
    EXPECT_NE(rejected.error.find("shared allocation registration unavailable"), std::string::npos);
    ASSERT_EQ(
        owner.submit(1, 2, 2, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 99, 1),
        exchange::Result::Accepted);
    owner.pop(1);
    owner.pop(1);
    ASSERT_TRUE(owner.front(1));
    EXPECT_EQ(owner.front(1)->internal_order_id, before + 1);
    EXPECT_TRUE(owner.finish());
}
TEST(EventLog, DefaultReplayDoesNotConsumeSharedProcessOrderIds) {
    simulation::ScenarioConfig config;
    config.max_commands = 1;
    const auto log = recording(config);
    const auto issue = [] {
        exchange::Exchange book;
        EXPECT_TRUE(book.open_session(1));
        EXPECT_EQ(book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit,
                              100, 1),
                  exchange::Result::Accepted);
        const auto report = book.front(1);
        EXPECT_TRUE(report.has_value());
        return report ? report->internal_order_id : 0;
    };
    const auto before = issue();
    ASSERT_TRUE(replay(log).verified);
    const auto after = issue();
    EXPECT_EQ(after, before + 1);
}
TEST(EventLog, InvalidReplayAllocationModeIsRejected) {
    simulation::ScenarioConfig config;
    config.max_commands = 1;
    const auto log = recording(config);
    std::istringstream input(log, std::ios::binary);
    const auto result = eventlog::verify(input, static_cast<eventlog::ReplayAllocation>(42));
    EXPECT_FALSE(result.verified);
    EXPECT_NE(result.error.find("allocation mode"), std::string::npos);
}
TEST(EventLog, NormalizationPreservesOwnershipAndFifoAcrossDifferentProcessIds) {
    exchange::Exchange first, second, reversed;
    for (auto *book : {&first, &second, &reversed}) {
        ASSERT_TRUE(book->open_session(1));
        ASSERT_TRUE(book->open_session(2));
    }
    const auto place = [](exchange::Exchange &book, std::uint64_t session) {
        return book.submit(session, 1, 1, core::SymbolId{1}, core::Side::Buy,
                           core::OrderType::Limit, 100, 5);
    };
    ASSERT_EQ(place(first, 1), exchange::Result::Accepted);
    ASSERT_EQ(place(first, 2), exchange::Result::Accepted);
    ASSERT_EQ(place(second, 1), exchange::Result::Accepted);
    ASSERT_EQ(place(second, 2), exchange::Result::Accepted);
    ASSERT_EQ(place(reversed, 2), exchange::Result::Accepted);
    ASSERT_EQ(place(reversed, 1), exchange::Result::Accepted);
    EXPECT_NE(first.front(1)->internal_order_id, second.front(1)->internal_order_id);
    EXPECT_EQ(first.verification_state(), second.verification_state());
    EXPECT_NE(first.verification_state(), reversed.verification_state());
}
TEST(EventLog, TerminalFlushFailureMayLeaveCompleteVerifiableBytes) {
    class TerminalFlushFailure final : public std::stringbuf {
      protected:
        int sync() override { return ++calls_ == 5 ? -1 : 0; }

      private:
        int calls_ = 0;
    } buffer;
    std::ostream out(&buffer);
    eventlog::EventRecorder recorder(out);
    simulation::ScenarioConfig c;
    c.max_commands = 1;
    simulation::SimulationRunner runner(c, &recorder);
    runner.start();
    runner.run_batch(1);
    EXPECT_EQ(runner.result().status.state, simulation::Lifecycle::Failed);
    EXPECT_FALSE(runner.result().summary_complete);
    EXPECT_EQ(runner.result().accepted_orders, 1U);
    EXPECT_TRUE(replay(buffer.str()).verified);
}
TEST(EventLog, RejectsMissingTerminalDuplicateAndSkippedIngress) {
    simulation::ScenarioConfig c;
    c.max_commands = 2;
    const auto original = recording(c);
    const auto first = 8U + read32(original, 8);
    const auto second =
        first + read32(original, first) + read32(original, first + read32(original, first));
    const auto terminal =
        second + read32(original, second) + read32(original, second + read32(original, second));
    EXPECT_FALSE(replay(original.substr(0, terminal)).verified);
    for (const int seq : {1, 3}) {
        auto corrupted = original;
        corrupted[second + 8] = static_cast<char>(seq);
        checksum(corrupted, second);
        EXPECT_FALSE(replay(corrupted).verified);
    }
}
