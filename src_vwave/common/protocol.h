/**
 * @file protocol.h
 * @brief vwave protocol — commands, error codes, and response helpers.
 *
 * Imports shared tw:: infrastructure and adds vwave-specific constants.
 */
#ifndef WAVE_PROTOCOL_H
#define WAVE_PROTOCOL_H

#include "tw/json.h"
#include "tw/protocol.h"

namespace wave {

// Re-export shared types so existing code compiles unchanged
using JsonObject = tw::JsonObject;

// ─── vwave commands ──────────────────────────────────────────────────────────

namespace cmd {
    constexpr const char* STATUS            = "status";
    constexpr const char* SHUTDOWN          = "shutdown";
    constexpr const char* FILE_INFO         = "file_info";
    constexpr const char* LIST_SCOPES       = "list_scopes";
    constexpr const char* LIST_SIGNALS      = "list_signals";
    constexpr const char* SIGNAL_INFO       = "signal_info";
    constexpr const char* GET_VALUE_AT      = "get_value_at";
    constexpr const char* GET_VALUE_BETWEEN = "get_value_between";
    constexpr const char* FIND_SIGNALS      = "find_signals";
    constexpr const char* FIND_VALUE        = "find_value";
    constexpr const char* VC_COUNT          = "vc_count";
}

// ─── vwave error codes (tool-specific additions) ─────────────────────────────

namespace err {
    // Re-export common codes
    constexpr const char* OK               = tw::err::OK;
    constexpr const char* INVALID_PARAMS   = tw::err::INVALID_PARAMS;
    constexpr const char* INTERNAL_ERROR   = tw::err::INTERNAL_ERROR;
    constexpr const char* SIGNAL_NOT_FOUND = tw::err::SIGNAL_NOT_FOUND;

    // vwave-specific
    constexpr const char* FSDB_OPEN_FAILED = "FSDB_OPEN_FAILED";
    constexpr const char* SCOPE_NOT_FOUND  = "SCOPE_NOT_FOUND";
    constexpr const char* INVALID_TIME     = "INVALID_TIME";
    constexpr const char* SERVER_BUSY      = "SERVER_BUSY";
    constexpr const char* FILE_READ_ERROR  = "FILE_READ_ERROR";
}

// ─── Response helpers (delegate to shared) ──────────────────────────────────

using tw::make_ok_response;
using tw::make_error_response;

}  // namespace wave

#endif  // WAVE_PROTOCOL_H
