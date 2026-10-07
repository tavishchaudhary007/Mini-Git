#include "Hasher.h"
#include <cstdint>
#include <iomanip>
#include <sstream>

static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

std::string Hasher::sha1(const std::string& data) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Pre-processing: append 0x80, pad with zeros, append 64-bit big-endian bit length.
    std::string msg = data;
    uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

    auto byteAt = [&](size_t i) { return static_cast<uint32_t>(static_cast<uint8_t>(msg[i])); };

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (byteAt(off + 4 * i) << 24) | (byteAt(off + 4 * i + 1) << 16) |
                   (byteAt(off + 4 * i + 2) << 8) | byteAt(off + 4 * i + 3);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::ostringstream out;
    for (uint32_t h : {h0, h1, h2, h3, h4}) out << std::hex << std::setw(8) << std::setfill('0') << h;
    return out.str();
}
