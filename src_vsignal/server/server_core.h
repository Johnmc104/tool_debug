/**
 * @file server_core.h
 * @brief vsignal server core — NPI netlist integration and request handling.
 *
 * Uses shared tw::server event loop infrastructure for:
 *   - Socket I/O, signal handling, idle timeout (1 h)
 *   - RAII, EINTR-safe reads, per-client timeout
 *
 * Tool-specific logic:
 *   - Design load (KDB -dbdir or RTL source files) via NPI
 *   - Netlist trace command handlers (driver, load, fanin, fanout, path, conn)
 */
#ifndef VSIGNAL_SERVER_CORE_H
#define VSIGNAL_SERVER_CORE_H

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <utility>

#include <unistd.h>
#include <fcntl.h>

// NPI headers
#include "npi.h"
#include "npi_nl.h"
#include "npi_L1.h"

// Shared infrastructure
#include "tw/json.h"
#include "tw/protocol.h"
#include "tw/server_loop.h"

// Tool protocol & RunDir
#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"

namespace vsignal {
namespace server {

// ─── Server state (file-scope within server process) ─────────────────────────

static std::string  g_design_source;
static RunDir*      g_run_dir = nullptr;
static std::chrono::steady_clock::time_point g_start_time;

// ─── NPI signal name helper ─────────────────────────────────────────────────
// NPI NL APIs take char* (not const char*).  This helper avoids scattered
// const_cast<> and is safe because NPI only reads the buffer.

static inline char* npi_str(const std::string& s) {
    return const_cast<char*>(s.c_str());
}

// ─── NPI NL helpers ──────────────────────────────────────────────────────────

static const char* nl_obj_type_str(int type) {
    switch (type) {
        case npiNlInst:             return "instance";
        case npiNlPort:             return "port";
        case npiNlInstPort:         return "inst_port";
        case npiNlDeclNet:          return "net";
        case npiNlConcatNet:        return "concat_net";
        case npiNlSliceNet:         return "slice_net";
        case npiNlPseudoPort:       return "pseudo_port";
        case npiNlPseudoInstPort:   return "pseudo_inst_port";
        case npiNlPseudoNet:        return "pseudo_net";
        case npiNlLib:              return "lib";
        case npiNlCell:             return "cell";
        case npiNlCellPin:          return "cell_pin";
        default:                    return "unknown";
    }
}

static const char* nl_direction_str(int dir) {
    switch (dir) {
        case npiNlInput:  return "input";
        case npiNlOutput: return "output";
        case npiNlInout:  return "inout";
        default:          return "none";
    }
}

/**
 * Convert a single NL handle into a JSON object string.
 * Defensively null-checks every NPI return value.
 */
static std::string nl_handle_to_json(npiNlHandle hdl) {
    if (!hdl) return "{}";

    int obj_type = npi_nl_get(npiNlType, hdl);
    const char* name      = npi_nl_get_str(npiNlName, hdl);
    const char* full_name = npi_nl_get_str(npiNlFullName, hdl);

    JsonObject obj;
    obj.set("type", nl_obj_type_str(obj_type));
    obj.set("name",      name      ? name      : "");
    obj.set("full_name", full_name ? full_name : "");

    // Instance extras
    if (obj_type == npiNlInst) {
        const char* def_name = npi_nl_get_str(npiNlDefName, hdl);
        if (def_name) obj.set("def_name", def_name);
        int cell_type = npi_nl_get(npiNlCellType, hdl);
        if (cell_type > 0) obj.set("cell_type", static_cast<int64_t>(cell_type));
    }

    // Port / InstPort extras
    if (obj_type == npiNlPort || obj_type == npiNlInstPort ||
        obj_type == npiNlPseudoPort || obj_type == npiNlPseudoInstPort) {
        int dir  = npi_nl_get(npiNlDirection, hdl);
        int size = npi_nl_get(npiNlSize, hdl);
        obj.set("direction", nl_direction_str(dir));
        if (size > 0) obj.set("size", static_cast<int64_t>(size));
    }

    // Net extras
    if (obj_type == npiNlDeclNet || obj_type == npiNlConcatNet ||
        obj_type == npiNlSliceNet || obj_type == npiNlPseudoNet) {
        int size = npi_nl_get(npiNlSize, hdl);
        if (size > 0) obj.set("size", static_cast<int64_t>(size));
    }

    return obj.dump();
}

/**
 * Convert a vector of NL handles into a JSON array string.
 */
static std::string nl_hdl_vec_to_json(nlHdlVec_t& hdlVec) {
    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < hdlVec.size(); ++i) {
        if (i) arr << ",";
        arr << nl_handle_to_json(hdlVec[i]);
    }
    arr << "]";
    return arr.str();
}

// ─── Command handlers ────────────────────────────────────────────────────────

static std::string handle_status(int id) {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      now - g_start_time).count();

    JsonObject data;
    data.set("design_source", g_design_source);
    data.set("uptime_seconds", uptime);
    data.set("pid", static_cast<int64_t>(getpid()));
    return make_ok_response(id, data.dump());
}

static std::string handle_info(int id) {
    JsonObject data;
    data.set("design_source", g_design_source);
    data.set("pid", static_cast<int64_t>(getpid()));
    return make_ok_response(id, data.dump());
}

static std::string handle_trace_driver(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    int assign_cell = static_cast<int>(params.get_int("assign_cell", 0));
    int pass_mod    = static_cast<int>(params.get_int("pass_mod", 0));

    nlHdlVec_t hdlVec;
    int ret = npi_nl_trace_driver(npi_str(sig), hdlVec, assign_cell, pass_mod);

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace drivers for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", static_cast<int64_t>(hdlVec.size()));
    data.set_raw("drivers", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

static std::string handle_trace_load(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    int assign_cell = static_cast<int>(params.get_int("assign_cell", 0));
    int pass_mod    = static_cast<int>(params.get_int("pass_mod", 0));

    nlHdlVec_t hdlVec;
    int ret = npi_nl_trace_load(npi_str(sig), hdlVec, assign_cell, pass_mod);

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace loads for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", static_cast<int64_t>(hdlVec.size()));
    data.set_raw("loads", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

static std::string handle_fanin_reg(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    bool stop_at_pin = params.get_bool("stop_at_pin", false);
    bool report_port = params.get_bool("report_primary_port", false);
    std::string scope = params.get_string("scope");

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_fanIn_reg_conn(
        npi_str(sig), hdlVec, stop_at_pin, report_port,
        scope.empty() ? nullptr : scope.c_str());

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace fanin for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", static_cast<int64_t>(hdlVec.size()));
    data.set_raw("fanin", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

static std::string handle_fanout_reg(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    bool stop_at_pin = params.get_bool("stop_at_pin", false);
    bool report_port = params.get_bool("report_primary_port", false);
    std::string scope = params.get_string("scope");

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_fanOut_reg_conn(
        npi_str(sig), hdlVec, stop_at_pin, report_port,
        scope.empty() ? nullptr : scope.c_str());

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace fanout for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", static_cast<int64_t>(hdlVec.size()));
    data.set_raw("fanout", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

static std::string handle_trace_path(int id, const JsonParser& params) {
    std::string from_sig = params.get_string("from");
    std::string to_sig   = params.get_string("to");
    if (from_sig.empty() || to_sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'from' and/or 'to' parameters");

    int assign_cell = static_cast<int>(params.get_int("assign_cell", 0));

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_sig_conn(
        npi_str(from_sig), npi_str(to_sig), hdlVec, assign_cell);

    if (ret < 0)
        return make_error_response(id, err::NO_PATH,
                                   "No path found from " + from_sig +
                                   " to " + to_sig);

    JsonObject data;
    data.set("from", from_sig);
    data.set("to", to_sig);
    data.set("count", static_cast<int64_t>(hdlVec.size()));
    data.set_raw("path", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

static std::string handle_inst_conn(int id, const JsonParser& params) {
    std::string inst = params.get_string("instance");
    if (inst.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'instance' parameter");

    std::string level = params.get_string("level", "high");

    hdl2hdlVecPairVec_t pairVec;
    int ret;
    if (level == "low")
        ret = npi_inst_port_2_low_conn_sig(npi_str(inst), pairVec);
    else
        ret = npi_inst_port_2_high_conn_sig(npi_str(inst), pairVec);

    if (ret < 0)
        return make_error_response(id, err::INSTANCE_NOT_FOUND,
                                   "Failed to get connections for: " + inst);

    // Build JSON: [{port: "...", signals: ["..."]}, ...]
    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < pairVec.size(); ++i) {
        if (i) arr << ",";
        JsonObject pair_obj;

        npiHandle port_hdl = pairVec[i].first;
        const char* port_name = npi_get_str(npiFullName, port_hdl);
        pair_obj.set("port", port_name ? port_name : "");

        hdlVec_t& sigs = pairVec[i].second;
        std::ostringstream sig_arr;
        sig_arr << "[";
        for (size_t j = 0; j < sigs.size(); ++j) {
            if (j) sig_arr << ",";
            const char* sig_name = npi_get_str(npiFullName, sigs[j]);
            sig_arr << "\"" << (sig_name ? sig_name : "") << "\"";
        }
        sig_arr << "]";
        pair_obj.set_raw("signals", sig_arr.str());

        arr << pair_obj.dump();
    }
    arr << "]";

    JsonObject data;
    data.set("instance", inst);
    data.set("level", level);
    data.set("count", static_cast<int64_t>(pairVec.size()));
    data.set_raw("connections", arr.str());
    return make_ok_response(id, data.dump());
}

// ─── Request dispatcher ──────────────────────────────────────────────────────

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

/**
 * Run the server process (called AFTER fork, in the daemon child).
 *
 * @param argc/argv      Original args (passed to npi_init / npi_load_design)
 * @param run_dir        Configured RunDir with socket/pid paths
 * @param design_source  Descriptive name (e.g. KDB dir path)
 * @param design_args    Args for npi_load_design (e.g. {"-dbdir", "simv.daidir"})
 * @return exit code
 */
inline int run_server(int argc, char** argv,
                      RunDir& run_dir,
                      const std::string& design_source,
                      const std::vector<std::string>& design_args) {
    g_run_dir        = &run_dir;
    g_design_source  = design_source;

    // ── Initialize NPI ───────────────────────────────────────────────────────
    std::cerr << "[vsignal-server] Initializing NPI...\n";
    npi_init(argc, argv);

    // ── Build argv for npi_load_design ───────────────────────────────────────
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

    // ── Write PID & source ───────────────────────────────────────────────────
    run_dir.write_pid(getpid());
    run_dir.write_design_source();
    g_start_time = std::chrono::steady_clock::now();

    // ── Install signal handlers & run event loop ─────────────────────────────
    tw::server::install_signal_handlers();

    tw::server::ServerConfig cfg;
    cfg.log_tag            = "vsignal-server";
    cfg.idle_timeout_sec   = 3600;        // 1 hour
    cfg.client_timeout_sec = 60;           // netlist tracing can be slow

    int rc = tw::server::create_and_run_loop(
        run_dir.socket_path(), dispatch_request, cfg);

    // ── Cleanup ──────────────────────────────────────────────────────────────
    run_dir.cleanup();
    npi_end();
    std::cerr << "[vsignal-server] Bye.\n";
    return rc;
}

}  // namespace server
}  // namespace vsignal

#endif  // VSIGNAL_SERVER_CORE_H
