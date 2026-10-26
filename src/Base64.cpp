#include "Base64.h"
#include <vector>
#include <cstdint>

namespace Base64 {

static const char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < in.size()) {
        uint32_t octet_a = i < in.size() ? static_cast<unsigned char>(in[i++]) : 0;
        uint32_t octet_b = i < in.size() ? static_cast<unsigned char>(in[i++]) : 0;
        uint32_t octet_c = i < in.size() ? static_cast<unsigned char>(in[i++]) : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
        out.push_back(kEncodeTable[(triple >> 6) & 0x3F]);
        out.push_back(kEncodeTable[triple & 0x3F]);
    }

    size_t mod = in.size() % 3;
    if (mod == 1) {
        out[out.size() - 2] = '=';
        out[out.size() - 1] = '=';
    } else if (mod == 2) {
        out[out.size() - 1] = '=';
    }

    return out;
}

static inline int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decode(const std::string& in, std::string& out, std::string& err) {
    out.clear();
    err.clear();

    // Clean whitespace
    std::string clean;
    clean.reserve(in.size());
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        clean.push_back(c);
    }

    if (clean.empty()) return true;
    if (clean.size() % 4 != 0) {
        err = "invalid base64 length";
        return false;
    }

    size_t padding = 0;
    if (clean.back() == '=') {
        padding++;
        if (clean.size() >= 2 && clean[clean.size() - 2] == '=') {
            padding++;
        }
    }

    out.reserve((clean.size() / 4) * 3);

    for (size_t i = 0; i < clean.size(); i += 4) {
        char c0 = clean[i];
        char c1 = clean[i + 1];
        char c2 = clean[i + 2];
        char c3 = clean[i + 3];

        int v0 = decodeChar(c0);
        int v1 = decodeChar(c1);
        int v2 = (c2 == '=') ? 0 : decodeChar(c2);
        int v3 = (c3 == '=') ? 0 : decodeChar(c3);

        if (v0 < 0 || v1 < 0 || (c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) {
            err = "invalid base64 character";
            out.clear();
            return false;
        }

        uint32_t triple = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;

        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (i + 4 < clean.size() || padding < 2) {
            if (c2 != '=') out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (i + 4 < clean.size() || padding < 1) {
            if (c3 != '=') out.push_back(static_cast<char>(triple & 0xFF));
        }
    }

    return true;
}

} // namespace Base64
