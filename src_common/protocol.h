/**
 * @file protocol.h
 * @brief Shared protocol primitives for tool_wave JSON-over-UDS protocol.
 *
 * Provides:
 *   - make_ok_response  / make_error_response  (response builders)
 *   - Common error codes shared by all tools
 *
 * Tool-specific command names and error codes are defined in each
 * tool's own protocol header.
 */
#ifndef TW_PROTOCOL_H
#define TW_PROTOCOL_H

#include <string>
#include <sstream>

#include "json.h"   // tw::JsonObject

namespace tw {

// ─── Common error codes (shared across tools) ───────────────────────────────

namespace err {
    constexpr const char* OK             = "OK";
    constexpr const char* INVALID_PARAMS = "INVALID_PARAMS";
    constexpr const char* INTERNAL_ERROR = "INTERNAL_ERROR";
    constexpr const char* SIGNAL_NOT_FOUND = "SIGNAL_NOT_FOUND";
}

// ─── Response builders ──────────────────────────────────────────────────────

inline std::string make_ok_response(int id, const std::string& data_json) {
    std::ostringstream os;
    os << "{\"id\":" << id
       << ",\"status\":\"ok\""
       << ",\"data\":" << data_json
       << "}";
    return os.str();
}

inline std::string make_error_response(int id, const std::string& code,
                                       const std::string& message) {
    JsonObject err_obj;
    err_obj.set("code", code);
    err_obj.set("message", message);

    std::ostringstream os;
    os << "{\"id\":" << id
       << ",\"status\":\"error\""
       << ",\"error\":" << err_obj.dump()
       << "}";
    return os.str();
}

}  // namespace tw

#endif  // TW_PROTOCOL_H
