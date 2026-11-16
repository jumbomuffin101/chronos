#include "chronos/snapshot/checkpoint.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>

using namespace chronos;
TEST(Checkpoint, RoundTripAndDetectStateChange) {
    exchange::Exchange book;
    ASSERT_TRUE(book.open_session(1));
    ASSERT_EQ(
        book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 5),
        exchange::Result::Accepted);
    const auto original = snapshot::capture(book, 42);
    EXPECT_EQ(original.exchange_command, 1U);
    std::ostringstream output(std::ios::binary);
    ASSERT_TRUE(snapshot::write(output, original).ok);
    std::istringstream input(output.str(), std::ios::binary);
    snapshot::Checkpoint decoded;
    ASSERT_TRUE(snapshot::read(input, decoded).ok);
    EXPECT_EQ(decoded.generation, 42U);
    EXPECT_EQ(decoded.state, original.state);
    EXPECT_TRUE(snapshot::compare(decoded, book).ok);
    ASSERT_EQ(book.cancel(1, 2, 1), exchange::Result::Accepted);
    EXPECT_FALSE(snapshot::compare(decoded, book).ok);
}
TEST(Checkpoint, TruncationAndCorruptionLeaveDestinationUnchanged) {
    exchange::Exchange book;
    auto checkpoint = snapshot::capture(book, 7);
    std::ostringstream output(std::ios::binary);
    ASSERT_TRUE(snapshot::write(output, checkpoint).ok);
    const auto bytes = output.str();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::istringstream input(bytes.substr(0, i), std::ios::binary);
        auto destination = checkpoint;
        EXPECT_FALSE(snapshot::read(input, destination).ok) << i;
        EXPECT_EQ(destination.state, checkpoint.state);
        EXPECT_EQ(destination.generation, 7U);
    }
    auto corrupt = bytes;
    corrupt[45] ^= 1;
    std::istringstream input(corrupt, std::ios::binary);
    EXPECT_FALSE(snapshot::read(input, checkpoint).ok);
}
TEST(Checkpoint, RejectsUnsupportedVersionAndOversizedLength) {
    exchange::Exchange book;
    const auto checkpoint = snapshot::capture(book, 0);
    std::ostringstream output(std::ios::binary);
    ASSERT_TRUE(snapshot::write(output, checkpoint).ok);
    auto bytes = output.str();
    bytes[8] = 2;
    std::istringstream version(bytes, std::ios::binary);
    snapshot::Checkpoint target;
    EXPECT_FALSE(snapshot::read(version, target).ok);
    bytes = output.str();
    bytes[39] = static_cast<char>(0xff);
    std::istringstream size(bytes, std::ios::binary);
    EXPECT_FALSE(snapshot::read(size, target).ok);
    std::ostringstream bad;
    bad.setstate(std::ios::badbit);
    EXPECT_FALSE(snapshot::write(bad, checkpoint).ok);
}
TEST(Checkpoint, BoundedVerificationRejectsBeforeUnboundedCapture) {
    exchange::Exchange book;
    ASSERT_TRUE(book.open_session(1));
    ASSERT_EQ(
        book.submit(1, 1, 1, core::SymbolId{1}, core::Side::Buy, core::OrderType::Limit, 100, 5),
        exchange::Result::Accepted);
    const auto original = book.verification_state();
    EXPECT_EQ(book.verification_state_bounded(original.size()), original);
    EXPECT_THROW((void)book.verification_state_bounded(original.size() - 1), std::length_error);
    EXPECT_THROW((void)book.verification_state_bounded(0), std::length_error);
    EXPECT_EQ(book.verification_state(), original);
}
