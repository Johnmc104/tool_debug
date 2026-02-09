/**
 * @file protocol.h
 * @brief vsignal protocol — commands, error codes, and response helpers.
 *
 * Imports shared tw:: infrastructure and adds vsignal-specific constants.
 */
#ifndef VSIGNAL_PROTOCOL_H
#define VSIGNAL_PROTOCOL_H

#include "tw/json.h"
#include "tw/protocol.h"

namespace vsignal {

// Re-export shared types
using JsonObject = tw::JsonObject;

// ─── vsignal commands ────────────────────────────────────────────────────────

namespace cmd {
    constexpr const char* STATUS       = "status";
    constexpr const char* SHUTDOWN     = "shutdown";
    constexpr const char* INFO         = "info";
    constexpr const char* TRACE_DRIVER = "trace_driver";
    constexpr const char* TRACE_LOAD   = "trace_load";
    constexpr const char* FANIN_REG    = "fanin_reg";
    constexpr const char* FANOUT_REG   = "fanout_reg";
    constexpr const char* TRACE_PATH   = "trace_path";
    constexpr const char* INST_CONN    = "inst_conn";
}

// ─── vsignal error codes ─────────────────────────────────────────────────────

namespace err {
    // Re-export common
    constexpr const char* OK                 = tw::err::OK;
    constexpr const char* INVALID_PARAMS     = tw::err::INVALID_PARAMS;
    constexpr const char* INTERNAL_ERROR     = tw::err::INTERNAL_ERROR;
    constexpr const char* SIGNAL_NOT_FOUND   = tw::err::SIGNAL_NOT_FOUND;

    // vsignal-specific
    constexpr const char* DESIGN_LOAD_FAILED = "DESIGN_LOAD_FAILED";
    constexpr const char* INSTANCE_NOT_FOUND = "INSTANCE_NOT_FOUND";
    constexpr const char* NO_PATH            = "NO_PATH";
}

// ─── Response helpers (delegate to shared) ──────────────────────────────────

using tw::make_ok_response;
using tw::make_error_response;

}  // namespace vsignal

#endif  // VSIGNAL_PROTOCOL_H
