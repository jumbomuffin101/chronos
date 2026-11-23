#include "chronos/journal/inspection.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <vector>

namespace chronos::journal {
namespace {
constexpr std::array<std::uint32_t, 64> k{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
std::uint64_t number(const std::uint8_t *p, std::size_t n) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
        v = (v << 8) | p[i];
    return v;
}
bool read_exact(std::istream &in, std::span<std::uint8_t> bytes) {
    return static_cast<bool>(in.read(reinterpret_cast<char *>(bytes.data()),
                                     static_cast<std::streamsize>(bytes.size())));
}
void block(std::array<std::uint32_t, 8> &h, const std::uint8_t *p) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
        w[i] = static_cast<std::uint32_t>(number(p + i * 4, 4));
    for (std::size_t i = 16; i < 64; ++i) {
        const auto a = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto b = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + a + w[i - 7] + b;
    }
    auto [a, b, c, d, e, f, g, x] = h;
    for (std::size_t i = 0; i < 64; ++i) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto t1 = x + s1 + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        x = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += x;
}
} // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto original = data.size();
    while (data.size() >= 64) {
        block(h, data.data());
        data = data.subspan(64);
    }
    std::array<std::uint8_t, 128> final{};
    std::copy(data.begin(), data.end(), final.begin());
    final[data.size()] = 0x80;
    const auto len = data.size() < 56 ? 64U : 128U;
    const auto bits = static_cast<std::uint64_t>(original) * 8U;
    for (std::size_t i = 0; i < 8; ++i)
        final[len - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    block(h, final.data());
    if (len == 128)
        block(h, final.data() + 64);
    std::array<std::uint8_t, 32> result{};
    for (std::size_t i = 0; i < h.size(); ++i)
        for (std::size_t j = 0; j < 4; ++j)
            result[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return result;
}

Inspection inspect(std::istream &journal, std::istream &manifest) {
    Inspection result;
    const auto fail = [&](const char *error) {
        result.error = error;
        return result;
    };
    std::array<std::uint8_t, manifest_bytes> m{};
    if (!read_exact(manifest, m) || manifest.peek() != std::char_traits<char>::eof() ||
        manifest.bad())
        return fail("invalid manifest length");
    if (!std::equal(m.begin(), m.begin() + 8, "CHRMAN01") || number(m.data() + 8, 2) != 1 ||
        number(m.data() + 10, 2) != manifest_bytes || number(m.data() + 12, 4) != 0 ||
        number(m.data() + 96, 8) == 0 || number(m.data() + 104, 4) != 0 ||
        crc32c(std::span{m}.first(108)) != number(m.data() + 108, 4))
        return fail("invalid manifest header or checksum");
    result.durable_lsn = number(m.data() + 80, 8);
    result.terminal_offset = number(m.data() + 88, 8);
    if (result.terminal_offset < journal_header_bytes || result.terminal_offset > max_journal_bytes)
        return fail("manifest offset out of range");
    journal.seekg(0, std::ios::end);
    const auto end = journal.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_journal_bytes ||
        static_cast<std::uint64_t>(end) < result.terminal_offset)
        return fail("invalid journal size");
    result.tail_bytes = static_cast<std::uint64_t>(end) - result.terminal_offset;
    journal.seekg(0);
    std::array<std::uint8_t, journal_header_bytes> h{};
    if (!read_exact(journal, h) || !std::equal(h.begin(), h.begin() + 8, "CHRJRNL1") ||
        number(h.data() + 8, 2) != 1 || number(h.data() + 10, 2) != journal_header_bytes ||
        number(h.data() + 12, 4) != 0 ||
        !std::equal(h.begin() + 16, h.begin() + 80, m.begin() + 16) ||
        number(h.data() + 80, 8) != 1)
        return fail("invalid journal header or identity");
    std::uint64_t offset = journal_header_bytes, expected = 1, prepare_offset = 0;
    std::uint32_t prepare_crc = 0;
    std::array<std::uint8_t, 32> prepare_hash{};
    bool pending = false;
    while (offset < result.terminal_offset) {
        std::array<std::uint8_t, 24> header{};
        if (result.terminal_offset - offset < 28 || !read_exact(journal, header))
            return fail("truncated record header");
        const auto length = number(header.data(), 4);
        const auto payload = number(header.data() + 16, 4);
        const auto type = header[6];
        if (length < 28 || length > max_record_bytes || length > result.terminal_offset - offset ||
            payload != length - 28 || number(header.data() + 4, 2) != 1 || header[7] != 0 ||
            number(header.data() + 20, 4) != 0 || number(header.data() + 8, 8) != expected)
            return fail("invalid record header or sequence");
        std::vector<std::uint8_t> record(static_cast<std::size_t>(length));
        std::copy(header.begin(), header.end(), record.begin());
        if (!read_exact(journal, std::span{record}.subspan(header.size())))
            return fail("truncated record");
        const auto checksum = number(record.data() + record.size() - 4, 4);
        if (crc32c(std::span{record}.first(record.size() - 4)) != checksum)
            return fail("record checksum mismatch");
        if (!pending && type == 1) {
            if (payload < 2 || number(record.data() + 24, 2) != 1)
                return fail("unsupported outcome schema");
            pending = true;
            prepare_offset = offset;
            prepare_crc = static_cast<std::uint32_t>(checksum);
            prepare_hash = sha256(record);
        } else if (pending && (type == 2 || type == 3)) {
            if (payload != 44 || number(record.data() + 24, 8) != prepare_offset ||
                number(record.data() + 32, 4) != prepare_crc ||
                !std::equal(prepare_hash.begin(), prepare_hash.end(), record.begin() + 36))
                return fail("terminal record does not bind prepare");
            pending = false;
            if (expected == std::numeric_limits<std::uint64_t>::max())
                return fail("LSN exhaustion unsupported");
            ++expected;
        } else
            return fail("invalid record ordering or type");
        offset += length;
    }
    if (pending || expected - 1 != result.durable_lsn)
        return fail("manifest does not end at matching terminal record");
    result.ok = true;
    return result;
}
} // namespace chronos::journal
