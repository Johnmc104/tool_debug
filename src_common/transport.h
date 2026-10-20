/**
 * @file transport.h
 * @brief Shared framing helpers for worker launch arguments.
 *
 * Worker sessions cross an LSF/shell boundary as one argv token.  Hex keeps
 * JSON quotes and whitespace out of that command line while remaining free of
 * external serialization dependencies.
 */
#ifndef TW_TRANSPORT_H
#define TW_TRANSPORT_H

#include <cctype>
#include <stdexcept>
#include <string>

namespace tw {
namespace transport {

inline std::string hex_encode(const std::string& text) {
    static const char* digits = "0123456789abcdef";
    std::string result("hex:");
    result.reserve(4 + text.size() * 2);
    for (unsigned char value : text) {
        result += digits[value >> 4];
        result += digits[value & 0xf];
    }
    return result;
}

inline std::string hex_decode(const std::string& encoded) {
    if (encoded.compare(0, 4, "hex:") != 0) return encoded;
    const std::string hex = encoded.substr(4);
    if (hex.size() % 2) throw std::runtime_error("Invalid hex worker session");
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = digit(hex[i]);
        int lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("Invalid hex worker session");
        result.push_back(static_cast<char>((hi << 4) | lo));
    }
    return result;
}

}  // namespace transport
}  // namespace tw

#endif  // TW_TRANSPORT_H
