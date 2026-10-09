/**
 * @file server_core.h
 * @brief vwave server entry point — dispatch + run_server.
 *
 * Handlers live in handlers.h.
 */
#ifndef WAVE_SERVER_CORE_H
#define WAVE_SERVER_CORE_H

#include <iostream>
#include <string>
#include <chrono>

#include <unistd.h>

#include "backend/backend.h"
#include "common/session.h"

#include "tw/json.h"
#include "tw/protocol.h"
#include "tw/server_loop.h"

#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"

namespace wave {
namespace server {

// ─── Server state ───────────────────────────────────────────────────────────

static WaveBackend*     g_backend = nullptr;
static Session          g_session;
static std::string       g_fsdb_path;
static RunDir*           g_run_dir = nullptr;
static std::chrono::steady_clock::time_point g_start_time;

}  // namespace server
}  // namespace wave

#include "server/handlers.h"

namespace wave {
namespace server {

// ─── Request dispatcher ─────────────────────────────────────────────────────

static std::string dispatch_request(const std::string& request_json) {
    JsonParser req;
    if (!req.parse(request_json))
        return make_error_response(0, err::INVALID_PARAMS, "Invalid JSON");

    int id = static_cast<int>(req.get_int("id", 0));
    std::string cmd_str = req.get_string("cmd");

    JsonParser params;
    std::string params_str = req.get_string("params");
    if (!params_str.empty())
        params.parse(params_str);
    else
        params = req;

    try {
    if (cmd_str == cmd::STATUS)            return handle_status(id);
    if (cmd_str == cmd::SHUTDOWN) {
        tw::server::g_running = 0;
        return make_ok_response(id, "{\"message\":\"shutting down\"}");
    }
    if (cmd_str == cmd::FILE_INFO)         return handle_file_info(id);
    if (cmd_str == cmd::LIST_SCOPES)       return handle_list_scopes(id, params);
    if (cmd_str == cmd::LIST_SIGNALS)      return handle_list_signals(id, params);
    if (cmd_str == cmd::SIGNAL_INFO)       return handle_signal_info(id, params);
    if (cmd_str == cmd::GET_VALUE_AT)      return handle_get_value_at(id, params);
    if (cmd_str == cmd::GET_VALUE_BETWEEN) return handle_get_value_between(id, params);
    if (cmd_str == cmd::FIND_SIGNALS)      return handle_find_signals(id, params);
    if (cmd_str == cmd::NEXT_EDGE)         return handle_next_edge(id, params);
    if (cmd_str == cmd::VC_COUNT)          return handle_vc_count(id, params);

    return make_error_response(id, err::INVALID_PARAMS,
                               "Unknown command: " + cmd_str);
    } catch (const BackendError& e) {
        return make_error_response(id,e.code,e.what());
    } catch (const std::exception& e) {
        return make_error_response(id,err::INTERNAL_ERROR,e.what());
    }
}

// ─── Server entry point ─────────────────────────────────────────────────────

inline int run_server(WaveBackend& backend, const Session& session, RunDir& run_dir) {
    g_backend = &backend;
    g_session = session;
    g_run_dir = &run_dir;
    g_fsdb_path = session.file;
    std::cerr << "[vwave-server] Loading " << session.backend << ": " << session.file << "\n";
    backend.open(session.file);
    auto info = backend.info();
    std::cerr << "[vwave-server] Time range: " << info.min_time << " ~ " << info.max_time << "\n";
    session.write(run_dir.dir());
    if (!run_dir.write_pid(getpid()) || !run_dir.write_fsdb_path())
        throw std::runtime_error("Cannot publish server state");
    g_start_time = std::chrono::steady_clock::now();

    tw::server::install_signal_handlers();

    tw::server::ServerConfig cfg;
    cfg.log_tag           = "vwave-server";
    cfg.idle_timeout_sec  = 3600;
    cfg.client_timeout_sec = 30;

    int rc = tw::server::create_and_run_loop(
        run_dir.socket_path(), dispatch_request, cfg);

    run_dir.cleanup();
    std::cerr << "[vwave-server] Bye.\n";
    return rc;
}

}  // namespace server
}  // namespace wave

#endif  // WAVE_SERVER_CORE_H
