#include "chronos/journal/inspection.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

using namespace chronos::journal;
namespace {
using Bytes = std::vector<std::uint8_t>;
void put(Bytes &b, std::size_t pos, std::uint64_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        b[pos + n - 1 - i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::string str(const Bytes &b) { return {b.begin(), b.end()}; }
void seal(Bytes &b) { put(b, b.size() - 4, crc32c(std::span{b}.first(b.size() - 4)), 4); }
Bytes record(std::uint8_t type, std::uint64_t lsn, const Bytes &payload) {
    Bytes b(28 + payload.size());
    put(b, 0, b.size(), 4);
    put(b, 4, 1, 2);
    b[6] = type;
    put(b, 8, lsn, 8);
    put(b, 16, payload.size(), 4);
    std::copy(payload.begin(), payload.end(), b.begin() + 24);
    seal(b);
    return b;
}
struct Fixture {
    Bytes journal = Bytes(journal_header_bytes), manifest = Bytes(manifest_bytes);
    Fixture() {
        std::copy_n("CHRJRNL1", 8, journal.begin());
        put(journal, 8, 1, 2);
        put(journal, 10, journal_header_bytes, 2);
        put(journal, 80, 1, 8);
        std::copy_n("CHRMAN01", 8, manifest.begin());
        put(manifest, 8, 1, 2);
        put(manifest, 10, manifest_bytes, 2);
        for (std::size_t i = 16; i < 80; ++i)
            journal[i] = manifest[i] = static_cast<std::uint8_t>(i);
        boundary(0, journal.size());
    }
    void boundary(std::uint64_t lsn, std::uint64_t offset) {
        put(manifest, 80, lsn, 8);
        put(manifest, 88, offset, 8);
        put(manifest, 96, lsn + 1, 8);
        seal(manifest);
    }
    void append(std::uint64_t lsn = 1) {
        const Bytes prepare = record(1, lsn, {0, 1, 42});
        Bytes payload(44);
        put(payload, 0, journal.size(), 8);
        put(payload, 8, crc32c(std::span{prepare}.first(prepare.size() - 4)), 4);
        const auto digest = sha256(prepare);
        std::copy(digest.begin(), digest.end(), payload.begin() + 12);
        const auto commit = record(2, lsn, payload);
        journal.insert(journal.end(), prepare.begin(), prepare.end());
        journal.insert(journal.end(), commit.begin(), commit.end());
        boundary(lsn, journal.size());
    }
    Inspection run() const {
        std::istringstream j(str(journal), std::ios::binary), m(str(manifest), std::ios::binary);
        return inspect(j, m);
    }
};
} // namespace

TEST(JournalInspection, CryptographicAndChecksumKnownVectors) {
    const std::string input = "123456789";
    EXPECT_EQ(crc32c(std::span{reinterpret_cast<const std::uint8_t *>(input.data()), input.size()}),
              0xe3069283U);
    const std::string abc = "abc";
    const std::array<std::uint8_t, 32> expected{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                                0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                                0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                                0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    EXPECT_EQ(sha256(std::span{reinterpret_cast<const std::uint8_t *>(abc.data()), abc.size()}),
              expected);
}
TEST(JournalInspection, ValidatesBoundPrefixAndReportsOpaqueTail) {
    Fixture f;
    EXPECT_TRUE(f.run().ok);
    f.append();
    const auto valid = f.run();
    ASSERT_TRUE(valid.ok) << valid.error;
    EXPECT_EQ(valid.durable_lsn, 1U);
    f.journal.insert(f.journal.end(), {1, 2, 3});
    EXPECT_EQ(f.run().tail_bytes, 3U);
}
TEST(JournalInspection, RejectsEveryDurableTruncationAndBadIdentity) {
    Fixture f;
    f.append();
    const auto all = f.journal;
    for (std::size_t size = 0; size < all.size(); ++size) {
        f.journal.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_FALSE(f.run().ok) << size;
    }
    f.journal = all;
    f.manifest[16] ^= 1;
    seal(f.manifest);
    EXPECT_FALSE(f.run().ok);
}
TEST(JournalInspection, RejectsCorruptionSequenceAndTerminalMismatch) {
    Fixture f;
    f.append();
    auto original = f.journal;
    f.journal[journal_header_bytes + 26] ^= 1;
    EXPECT_FALSE(f.run().ok);
    f.journal = original;
    put(f.journal, journal_header_bytes + 8, 2, 8);
    EXPECT_FALSE(f.run().ok);
    f.journal = original;
    const auto terminal = journal_header_bytes + 31;
    f.journal[terminal + 36] ^= 1;
    Bytes last(f.journal.begin() + static_cast<std::ptrdiff_t>(terminal), f.journal.end());
    seal(last);
    std::copy(last.begin(), last.end(), f.journal.begin() + static_cast<std::ptrdiff_t>(terminal));
    EXPECT_FALSE(f.run().ok);
    f.journal = original;
    f.manifest[108] ^= 1;
    EXPECT_FALSE(f.run().ok);
}
TEST(JournalInspection, RejectsUnsupportedVersionsOversizeAndInvalidBoundaries) {
    Fixture f;
    f.append();
    const auto original = f.journal;
    put(f.journal, journal_header_bytes, max_record_bytes + 1U, 4);
    EXPECT_FALSE(f.run().ok);
    f.journal = original;
    f.journal[journal_header_bytes + 5] = 2;
    EXPECT_FALSE(f.run().ok);
    f.journal = original;
    f.boundary(1, journal_header_bytes + 31);
    EXPECT_FALSE(f.run().ok);
    f.boundary(2, original.size());
    EXPECT_FALSE(f.run().ok);
}
TEST(JournalInspection, Sha256HandlesPaddingAcrossBlocks) {
    const std::string input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::array<std::uint8_t, 32> expected{0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
                                                0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
                                                0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
                                                0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};
    EXPECT_EQ(sha256(std::span{reinterpret_cast<const std::uint8_t *>(input.data()), input.size()}),
              expected);
}
TEST(JournalInspection, MultiplePairsRejectDuplicateLsnAndUnknownTypes) {
    Fixture f;
    f.append(1);
    f.append(2);
    ASSERT_TRUE(f.run().ok);
    const auto original = f.journal;
    const auto second = journal_header_bytes + 31 + 72;
    put(f.journal, second + 8, 1, 8);
    EXPECT_FALSE(f.run().ok);
    f.journal = original;
    f.journal[second + 6] = 99;
    Bytes modified(f.journal.begin() + static_cast<std::ptrdiff_t>(second),
                   f.journal.begin() + static_cast<std::ptrdiff_t>(second + 31));
    seal(modified);
    std::copy(modified.begin(), modified.end(),
              f.journal.begin() + static_cast<std::ptrdiff_t>(second));
    EXPECT_FALSE(f.run().ok);
}
TEST(JournalInspection, RejectsReservedFieldsAndMalformedManifest) {
    Fixture f;
    f.append();
    f.manifest[104] = 1;
    seal(f.manifest);
    EXPECT_FALSE(f.run().ok);
    f.manifest[104] = 0;
    seal(f.manifest);
    f.manifest.push_back(0);
    EXPECT_FALSE(f.run().ok);
}
