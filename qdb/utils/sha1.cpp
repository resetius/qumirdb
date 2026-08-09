#include <qdb/utils/sha1.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace NQdb::NUtils {

namespace {

uint32_t RotateLeft(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

} // namespace

// Adapted from coroio/coroio/ws/utils.cpp.
std::string Sha1Hex(std::string_view data) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    const size_t paddedSize = ((data.size() + 8) / 64 + 1) * 64;
    std::vector<unsigned char> buffer(paddedSize);
    std::memcpy(buffer.data(), data.data(), data.size());
    buffer[data.size()] = 0x80;

    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        buffer[paddedSize - 1 - i] =
            static_cast<unsigned char>((bitLength >> (8 * i)) & 0xFF);
    }

    for (size_t offset = 0; offset < paddedSize; offset += 64) {
        uint32_t words[80];
        for (int i = 0; i < 16; ++i) {
            const size_t pos = offset + i * 4;
            words[i] =
                (static_cast<uint32_t>(buffer[pos]) << 24) |
                (static_cast<uint32_t>(buffer[pos + 1]) << 16) |
                (static_cast<uint32_t>(buffer[pos + 2]) << 8) |
                static_cast<uint32_t>(buffer[pos + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            words[i] = RotateLeft(
                words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t next = RotateLeft(a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = next;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    static constexpr char Hex[] = "0123456789abcdef";
    const uint32_t digest[] = {h0, h1, h2, h3, h4};
    std::string result(40, '0');
    size_t pos = 0;
    for (uint32_t word : digest) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            result[pos++] = Hex[(word >> shift) & 0x0F];
        }
    }
    return result;
}

} // namespace NQdb::NUtils
