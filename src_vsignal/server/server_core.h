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
#include <functional>
#include <cstdio>
#include <stdexcept>

#include <unistd.h>

#include "npi.h"
#include "npi_nl.h"

#include "tw/json.h"
#include "tw/protocol.h"
#include "tw/server_loop.h"
#include "tw/npi_env.h"

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

inline bool initialize_design(int argc, char** argv,
                              const std::string& design_source,
                              const std::vector<std::string>& design_args) {
    std::cerr << "[vsignal-server] Initializing NPI...\n";
    if (!npi_init(argc, argv)) {
        tw::npi_env::report_init_failure("vsignal-server");
        npi_end();
        return false;
    }

    std::vector<std::string> load_args;
    load_args.push_back("vsignal");
    for (auto& arg : design_args) load_args.push_back(arg);
    std::vector<char*> c_args;
    c_args.reserve(load_args.size());
    for (auto& arg : load_args) c_args.push_back(const_cast<char*>(arg.c_str()));

    std::cerr << "[vsignal-server] Loading design:";
    for (auto& arg : design_args) std::cerr << " " << arg;
    std::cerr << "\n";
    int load_ret = npi_load_design(static_cast<int>(c_args.size()), c_args.data());
    if (load_ret <= 0) {
        std::cerr << "[vsignal-server] ERROR: npi_load_design failed (rc="
                  << load_ret << ") for " << design_source << "\n";
        npi_end();
        return false;
    }
    std::cerr << "[vsignal-server] Design loaded successfully.\n";
    return true;
}

inline int run_server(int argc, char** argv,
                      RunDir& run_dir,
                      const std::string& design_source,
                      const std::vector<std::string>& design_args) {
    g_run_dir        = &run_dir;
    g_design_source  = design_source;

    if (!initialize_design(argc, argv, design_source, design_args)) return 1;

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

inline int run_stdio_loop(int argc, char** argv,
                          const std::string& design_source,
                          const std::vector<std::string>& design_args) {
    int protocol_fd = dup(STDOUT_FILENO);
    if (protocol_fd < 0) throw std::runtime_error("Cannot reserve stdio protocol descriptor");
    dup2(STDERR_FILENO, STDOUT_FILENO);
    if (!initialize_design(argc, argv, design_source, design_args)) {
        dup2(protocol_fd, STDOUT_FILENO);
        close(protocol_fd);
        return 1;
    }
    g_design_source = design_source;
    g_run_dir = nullptr;
    g_start_time = std::chrono::steady_clock::now();
    tw::server::g_running = 1;
    tw::server::install_signal_handlers();
    dup2(protocol_fd, STDOUT_FILENO);
    close(protocol_fd);

    JsonObject ready;
    ready.set("type", "ready");
    ready.set("protocol", "vsignal-stdio-loop");
    ready.set("version", int64_t(1));
    ready.set("pid", static_cast<int64_t>(getpid()));
    ready.set("backend", "npi");
    std::cout << ready.dump() << std::endl;
    std::string request;
    while (tw::server::g_running && std::getline(std::cin, request)) {
        if (request.empty()) continue;
        std::string response;
        int saved = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        response = dispatch_request(request);
        std::cout.flush(); std::fflush(nullptr);
        dup2(saved, STDOUT_FILENO); close(saved);
        std::cout << response << std::endl;
    }
    npi_end();
    return 0;
}

}  // namespace server
}  // namespace vsignal

#endif  // VSIGNAL_SERVER_CORE_H
