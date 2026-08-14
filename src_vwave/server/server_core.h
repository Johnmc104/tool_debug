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

#include "npi.h"
#include "npi_fsdb.h"

#include "tw/json.h"
#include "tw/protocol.h"
#include "tw/server_loop.h"

#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"

namespace wave {
namespace server {

// ─── Server state ───────────────────────────────────────────────────────────

static npiFsdbFileHandle g_file_hdl = nullptr;
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
}

// ─── Server entry point ─────────────────────────────────────────────────────

inline int run_server(int argc, char** argv,
                      RunDir& run_dir, const std::string& fsdb_path) {
    g_run_dir   = &run_dir;
    g_fsdb_path = fsdb_path;

    std::cerr << "[vwave-server] Initializing NPI...\n";
    npi_init(argc, argv);

    std::cerr << "[vwave-server] Loading FSDB: " << fsdb_path << "\n";
    g_file_hdl = npi_fsdb_open(fsdb_path.c_str());
    if (!g_file_hdl) {
        std::cerr << "[vwave-server] ERROR: Failed to open FSDB: "
                  << fsdb_path << "\n";
        npi_end();
        return 1;
    }

    npiFsdbTime min_t = 0, max_t = 0;
    npi_fsdb_min_time(g_file_hdl, &min_t);
    npi_fsdb_max_time(g_file_hdl, &max_t);
    std::cerr << "[vwave-server] FSDB loaded. Time range: "
              << min_t << " ~ " << max_t << "\n";

    run_dir.write_pid(getpid());
    run_dir.write_fsdb_path();
    g_start_time = std::chrono::steady_clock::now();

    tw::server::install_signal_handlers();

    tw::server::ServerConfig cfg;
    cfg.log_tag           = "vwave-server";
    cfg.idle_timeout_sec  = 3600;
    cfg.client_timeout_sec = 30;

    int rc = tw::server::create_and_run_loop(
        run_dir.socket_path(), dispatch_request, cfg);

    run_dir.cleanup();
    if (g_file_hdl) {
        npi_fsdb_close(g_file_hdl);
        g_file_hdl = nullptr;
    }
    npi_end();
    std::cerr << "[vwave-server] Bye.\n";
    return rc;
}

}  // namespace server
}  // namespace wave

#endif  // WAVE_SERVER_CORE_H
