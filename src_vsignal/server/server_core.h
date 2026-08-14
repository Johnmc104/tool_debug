/**
 * @file server_core.h
 * @brief vsignal server entry point — dispatch + run_server.
 *
 * Handlers live in handlers.h; NPI helpers in npi_helpers.h.
 */
#ifndef VSIGNAL_SERVER_CORE_H
#define VSIGNAL_SERVER_CORE_H

#include <iostream>
#include <string>
#include <vector>
#include <chrono>

#include <unistd.h>

#include "npi.h"
#include "npi_nl.h"

#include "tw/json.h"
#include "tw/protocol.h"
#include "tw/server_loop.h"

#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"

namespace vsignal {
namespace server {

// ─── Server state ───────────────────────────────────────────────────────────

static std::string  g_design_source;
static RunDir*      g_run_dir = nullptr;
static std::chrono::steady_clock::time_point g_start_time;

}  // namespace server
}  // namespace vsignal

#include "server/handlers.h"

namespace vsignal {
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

    if (cmd_str == cmd::STATUS)       return handle_status(id);
    if (cmd_str == cmd::SHUTDOWN) {
        tw::server::g_running = 0;
        return make_ok_response(id, "{\"message\":\"shutting down\"}");
    }
    if (cmd_str == cmd::INFO)         return handle_info(id);
    if (cmd_str == cmd::TRACE_DRIVER) return handle_trace_driver(id, params);
    if (cmd_str == cmd::TRACE_LOAD)   return handle_trace_load(id, params);
    if (cmd_str == cmd::FANIN_REG)    return handle_fanin_reg(id, params);
    if (cmd_str == cmd::FANOUT_REG)   return handle_fanout_reg(id, params);
    if (cmd_str == cmd::TRACE_PATH)   return handle_trace_path(id, params);
    if (cmd_str == cmd::INST_CONN)    return handle_inst_conn(id, params);

    return make_error_response(id, err::INVALID_PARAMS,
                               "Unknown command: " + cmd_str);
}

// ─── Server entry point ─────────────────────────────────────────────────────

inline int run_server(int argc, char** argv,
                      RunDir& run_dir,
                      const std::string& design_source,
                      const std::vector<std::string>& design_args) {
    g_run_dir        = &run_dir;
    g_design_source  = design_source;

    std::cerr << "[vsignal-server] Initializing NPI...\n";
    npi_init(argc, argv);

    std::vector<std::string> load_args;
    load_args.push_back("vsignal");
    for (auto& a : design_args) load_args.push_back(a);

    std::vector<char*> c_args;
    c_args.reserve(load_args.size());
    for (auto& s : load_args) c_args.push_back(const_cast<char*>(s.c_str()));

    std::cerr << "[vsignal-server] Loading design:";
    for (auto& a : design_args) std::cerr << " " << a;
    std::cerr << "\n";

    int load_ret = npi_load_design(static_cast<int>(c_args.size()), c_args.data());
    if (load_ret < 0) {
        std::cerr << "[vsignal-server] ERROR: npi_load_design failed (rc="
                  << load_ret << ")\n"
                  << "[vsignal-server] Check: " << run_dir.dir()
                  << "/vsignalLog/compiler.log\n";
        npi_end();
        return 1;
    }
    std::cerr << "[vsignal-server] Design loaded successfully.\n";

    run_dir.write_pid(getpid());
    run_dir.write_design_source();
    g_start_time = std::chrono::steady_clock::now();

    tw::server::install_signal_handlers();

    tw::server::ServerConfig cfg;
    cfg.log_tag            = "vsignal-server";
    cfg.idle_timeout_sec   = 3600;
    cfg.client_timeout_sec = 60;

    int rc = tw::server::create_and_run_loop(
        run_dir.socket_path(), dispatch_request, cfg);

    run_dir.cleanup();
    npi_end();
    std::cerr << "[vsignal-server] Bye.\n";
    return rc;
}

}  // namespace server
}  // namespace vsignal

#endif  // VSIGNAL_SERVER_CORE_H
