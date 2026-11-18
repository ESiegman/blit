#pragma once
// Base64: kitty's pictures come in it (term.cpp), and the debugger's memory
// (appdebug.cpp).
#include <array>
#include <string>
#include <string_view>

#include "common.hpp"

namespace blit {

// (Anything else in it is passed over.)
inline std::string from_base64(std::string_view in) {
    static const auto kValue = [] {
        std::array<signed char, 256> v{};
        v.fill(-1);
        const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int k = 0; k < 64; ++k) v[static_cast<u8>(abc[k])] = static_cast<signed char>(k);
        return v;
    }();
    std::string out;
    out.reserve(in.size() / 4 * 3);
    u32 acc = 0;
    int bits = 0;
    for (const char c : in) {
        const signed char v = kValue[static_cast<u8>(c)];
        if (v < 0) continue;
        acc = acc << 6 | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>(acc >> bits & 0xFF);
        }
    }
    return out;
}

inline std::string to_base64(std::string_view in) {
    static constexpr char kAbc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const size_t n = std::min<size_t>(3, in.size() - i);
        u32 v = static_cast<u32>(static_cast<u8>(in[i])) << 16;
        if (n > 1) v |= static_cast<u32>(static_cast<u8>(in[i + 1])) << 8;
        if (n > 2) v |= static_cast<u32>(static_cast<u8>(in[i + 2]));
        out += kAbc[v >> 18 & 63];
        out += kAbc[v >> 12 & 63];
        out += n > 1 ? kAbc[v >> 6 & 63] : '=';
        out += n > 2 ? kAbc[v & 63] : '=';
    }
    return out;
}

}  // namespace blit
