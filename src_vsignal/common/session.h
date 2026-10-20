#ifndef VSIGNAL_SESSION_H
#define VSIGNAL_SESSION_H

#include "tw/json.h"
#include "common/json_parser.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace vsignal {

struct DesignSession {
    std::string source;
    std::vector<std::string> args;

    std::string json() const {
        tw::JsonObject object;
        object.set("schema_version", int64_t(1));
        object.set("source", source);
        object.set_array("args", args);
        return object.dump();
    }

    static DesignSession parse(const std::string& text) {
        JsonParser parser;
        if (!parser.parse(text) || parser.get_int("schema_version", 0) != 1)
            throw std::runtime_error("Invalid vsignal worker session");
        DesignSession session;
        session.source = parser.get_string("source");
        session.args = parser.get_array("args");
        if (session.source.empty() || session.args.empty())
            throw std::runtime_error("Incomplete vsignal worker session");
        return session;
    }
};

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
        int hi = digit(hex[i]), lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("Invalid hex worker session");
        result.push_back(static_cast<char>((hi << 4) | lo));
    }
    return result;
}

} // namespace vsignal

#endif
