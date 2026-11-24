#include "chronos/exchange/canonical_command.hpp"

#include <gtest/gtest.h>
#include <limits>

using chronos::exchange::CanonicalCommand;

TEST(CanonicalCommand, SubmitEncodingPreservesExactBitsAndRoundTrips) {
    const auto c = CanonicalCommand::submit(0x0102030405060708ULL, 2, 3, 0xaabbccddU, 255, 254,
                                            std::numeric_limits<std::int64_t>::min(),
                                            std::numeric_limits<std::uint64_t>::max());
    const auto b = c.encode();
    const CanonicalCommand::Bytes golden{
        0,    1, 1, 0, 1, 2, 3, 4, 5,    6,    7,    8,    0,    0,    0,    0,    0, 0,
        0,    2, 0, 0, 0, 0, 0, 0, 0,    3,    0xaa, 0xbb, 0xcc, 0xdd, 0xff, 0xfe, 0, 0,
        0x80, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    EXPECT_EQ(b, golden);
    EXPECT_EQ(b[1], 1);
    EXPECT_EQ(b[2], 1);
    EXPECT_EQ(b[4], 1);
    EXPECT_EQ(b[11], 8);
    EXPECT_EQ(b[28], 0xaa);
    EXPECT_EQ(b[31], 0xdd);
    EXPECT_EQ(b[32], 255);
    EXPECT_EQ(b[36], 0x80);
    EXPECT_EQ(b[44], 0xff);
    const auto decoded = CanonicalCommand::decode(b);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->encode(), b);
    EXPECT_EQ(decoded->kind(), CanonicalCommand::Kind::Submit);
}
TEST(CanonicalCommand, RejectsNoncanonicalAndUnsupportedInputs) {
    auto b = CanonicalCommand::cancel(1, 2, 3).encode();
    ASSERT_TRUE(CanonicalCommand::decode(b));
    EXPECT_FALSE(CanonicalCommand::decode(std::span{b}.first(b.size() - 1)));
    b[1] = 2;
    EXPECT_FALSE(CanonicalCommand::decode(b));
    b[1] = 1;
    b[3] = 1;
    EXPECT_FALSE(CanonicalCommand::decode(b));
    b[3] = 0;
    b[35] = 1;
    EXPECT_FALSE(CanonicalCommand::decode(b));
    b[35] = 0;
    b[44] = 1;
    EXPECT_FALSE(CanonicalCommand::decode(b));
    b[44] = 0;
    b[2] = 99;
    EXPECT_FALSE(CanonicalCommand::decode(b));
}
