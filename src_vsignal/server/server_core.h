/**
 * @file server_core.h
 * @brief vsignal server core — NPI netlist integration and request handling.
 *
 * This module contains all NPI-dependent server logic:
 *   - Design load (KDB -dbdir or RTL source files)
 *   - Netlist trace command handlers (driver, load, fanin, fanout, path, conn)
 *   - Socket event loop
 *
 * Decoupled from main() so the single `vsignal` binary can fork a server
 * process internally without a separate executable.
 */
#ifndef VSIGNAL_SERVER_CORE_H
#define VSIGNAL_SERVER_CORE_H

#include <iostream>
#include <string>
#include <cstring>
#include <csignal>
#include <vector>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <utility>

// System
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// NPI headers
#include "npi.h"
#include "npi_nl.h"
#include "npi_L1.h"

// Project
#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"

namespace vsignal {
namespace server {

// ─── Server state (file-scope within the server process) ─────────────────────

static std::string       g_design_source;
static int               g_listen_fd = -1;
static volatile sig_atomic_t g_running = 1;   // signal-safe flag
static RunDir*           g_run_dir = nullptr;
static std::chrono::steady_clock::time_point g_start_time;

// ─── Signal handler ──────────────────────────────────────────────────────────

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ─── NPI NL helpers ──────────────────────────────────────────────────────────

/**
 * Convert an NPI NL object type code to descriptive string.
 */
static const char* nl_obj_type_str(int type) {
    switch (type) {
        case npiNlInst:         return "instance";
        case npiNlPort:         return "port";
        case npiNlInstPort:     return "inst_port";
        case npiNlDeclNet:      return "net";
        case npiNlConcatNet:    return "concat_net";
        case npiNlSliceNet:     return "slice_net";
        case npiNlPseudoPort:   return "pseudo_port";
        case npiNlPseudoInstPort: return "pseudo_inst_port";
        case npiNlPseudoNet:    return "pseudo_net";
        case npiNlLib:          return "lib";
        case npiNlCell:         return "cell";
        case npiNlCellPin:      return "cell_pin";
        default:                return "unknown";
    }
}

/**
 * Convert NPI NL direction code to string.
 */
static const char* nl_direction_str(int dir) {
    switch (dir) {
        case npiNlInput:  return "input";
        case npiNlOutput: return "output";
        case npiNlInout:  return "inout";
        default:          return "none";
    }
}

/**
 * Convert a single npiNlHandle into a JSON object string.
 * Extracts type, name, full_name, and (for ports) direction.
 */
static std::string nl_handle_to_json(npiNlHandle hdl) {
    int obj_type = npi_nl_get(npiNlType, hdl);
    const char* name = npi_nl_get_str(npiNlName, hdl);
    const char* full_name = npi_nl_get_str(npiNlFullName, hdl);

    JsonObject obj;
    obj.set("type", nl_obj_type_str(obj_type));
    obj.set("name", name ? name : "");
    obj.set("full_name", full_name ? full_name : "");

    // For instances, add definition name and cell type
    if (obj_type == npiNlInst) {
        const char* def_name = npi_nl_get_str(npiNlDefName, hdl);
        if (def_name) obj.set("def_name", def_name);
        int cell_type = npi_nl_get(npiNlCellType, hdl);
        if (cell_type > 0) obj.set("cell_type", (int64_t)cell_type);
    }

    // For ports/instports, add direction and size
    if (obj_type == npiNlPort || obj_type == npiNlInstPort ||
        obj_type == npiNlPseudoPort || obj_type == npiNlPseudoInstPort) {
        int dir = npi_nl_get(npiNlDirection, hdl);
        obj.set("direction", nl_direction_str(dir));
        int size = npi_nl_get(npiNlSize, hdl);
        if (size > 0) obj.set("size", (int64_t)size);
    }

    // For nets, add size
    if (obj_type == npiNlDeclNet || obj_type == npiNlConcatNet ||
        obj_type == npiNlSliceNet || obj_type == npiNlPseudoNet) {
        int size = npi_nl_get(npiNlSize, hdl);
        if (size > 0) obj.set("size", (int64_t)size);
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
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - g_start_time).count();

    JsonObject data;
    data.set("design_source", g_design_source);
    data.set("uptime_seconds", uptime);
    data.set("pid", (int64_t)getpid());
    return make_ok_response(id, data.dump());
}

static std::string handle_info(int id) {
    JsonObject data;
    data.set("design_source", g_design_source);
    data.set("pid", (int64_t)getpid());
    return make_ok_response(id, data.dump());
}

/**
 * trace_driver: find netlist drivers of a signal.
 * Params: { "signal": "<hier_name>", "assign_cell": 0|1, "pass_mod": 0|1 }
 */
static std::string handle_trace_driver(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal' parameter");

    int assign_cell = (int)params.get_int("assign_cell", 0);
    int pass_mod    = (int)params.get_int("pass_mod", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_trace_driver(const_cast<char*>(sig.c_str()),
                                   hdlVec, assign_cell, pass_mod);

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace drivers for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", (int64_t)hdlVec.size());
    data.set_raw("drivers", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

/**
 * trace_load: find netlist loads of a signal.
 * Params: { "signal": "<hier_name>", "assign_cell": 0|1, "pass_mod": 0|1 }
 */
static std::string handle_trace_load(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal' parameter");

    int assign_cell = (int)params.get_int("assign_cell", 0);
    int pass_mod    = (int)params.get_int("pass_mod", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_trace_load(const_cast<char*>(sig.c_str()),
                                 hdlVec, assign_cell, pass_mod);

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace loads for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", (int64_t)hdlVec.size());
    data.set_raw("loads", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

/**
 * fanin_reg: find fan-in register connections of a signal.
 * Params: { "signal": "<hier_name>", "stop_at_pin": true|false,
 *           "report_primary_port": true|false, "scope": "<scope>" }
 */
static std::string handle_fanin_reg(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal' parameter");

    bool stop_at_pin = params.get_bool("stop_at_pin", false);
    bool report_port = params.get_bool("report_primary_port", false);
    std::string scope = params.get_string("scope");

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_fanIn_reg_conn(
        const_cast<char*>(sig.c_str()), hdlVec,
        stop_at_pin, report_port,
        scope.empty() ? nullptr : scope.c_str());

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace fanin for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", (int64_t)hdlVec.size());
    data.set_raw("fanin", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

/**
 * fanout_reg: find fan-out register connections of a signal.
 * Params: { "signal": "<hier_name>", "stop_at_pin": true|false,
 *           "report_primary_port": true|false, "scope": "<scope>" }
 */
static std::string handle_fanout_reg(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal' parameter");

    bool stop_at_pin = params.get_bool("stop_at_pin", false);
    bool report_port = params.get_bool("report_primary_port", false);
    std::string scope = params.get_string("scope");

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_fanOut_reg_conn(
        const_cast<char*>(sig.c_str()), hdlVec,
        stop_at_pin, report_port,
        scope.empty() ? nullptr : scope.c_str());

    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace fanout for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("count", (int64_t)hdlVec.size());
    data.set_raw("fanout", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

/**
 * trace_path: find signal-to-signal connection path.
 * Params: { "from": "<sig_hier>", "to": "<sig_hier>", "assign_cell": 0|1 }
 */
static std::string handle_trace_path(int id, const JsonParser& params) {
    std::string from_sig = params.get_string("from");
    std::string to_sig   = params.get_string("to");
    if (from_sig.empty() || to_sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'from' and/or 'to' parameters");

    int assign_cell = (int)params.get_int("assign_cell", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_sig_conn(
        const_cast<char*>(from_sig.c_str()),
        const_cast<char*>(to_sig.c_str()),
        hdlVec, assign_cell);

    if (ret < 0)
        return make_error_response(id, err::NO_PATH,
                                   "No path found from " + from_sig + " to " + to_sig);

    JsonObject data;
    data.set("from", from_sig);
    data.set("to", to_sig);
    data.set("count", (int64_t)hdlVec.size());
    data.set_raw("path", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

/**
 * inst_conn: get instance port connections (high or low).
 * Params: { "instance": "<inst_hier>", "level": "high"|"low" }
 */
static std::string handle_inst_conn(int id, const JsonParser& params) {
    std::string inst = params.get_string("instance");
    if (inst.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'instance' parameter");

    std::string level = params.get_string("level", "high");

    hdl2hdlVecPairVec_t pairVec;
    int ret;
    if (level == "low") {
        ret = npi_inst_port_2_low_conn_sig(
            const_cast<char*>(inst.c_str()), pairVec);
    } else {
        ret = npi_inst_port_2_high_conn_sig(
            const_cast<char*>(inst.c_str()), pairVec);
    }

    if (ret < 0)
        return make_error_response(id, err::INSTANCE_NOT_FOUND,
                                   "Failed to get connections for: " + inst);

    // Build JSON array of {port: <info>, signals: [<info>, ...]}
    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < pairVec.size(); ++i) {
        if (i) arr << ",";
        JsonObject pair_obj;

        // Port handle (npiHandle, use npi_get_str)
        npiHandle port_hdl = pairVec[i].first;
        const char* port_name = npi_get_str(npiFullName, port_hdl);
        pair_obj.set("port", port_name ? port_name : "");

        // Connected signals
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
    data.set("count", (int64_t)pairVec.size());
    data.set_raw("connections", arr.str());
    return make_ok_response(id, data.dump());
}

// ─── Request dispatcher ──────────────────────────────────────────────────────

static std::string dispatch_request(const std::string& request_json) {
    JsonParser req;
    if (!req.parse(request_json))
        return make_error_response(0, err::INVALID_PARAMS, "Invalid JSON");

    int id = (int)req.get_int("id", 0);
    std::string cmd_str = req.get_string("cmd");

    JsonParser params;
    std::string params_str = req.get_string("params");
    if (!params_str.empty()) {
        params.parse(params_str);
    } else {
        params = req;
    }

    if (cmd_str == cmd::STATUS)        return handle_status(id);
    if (cmd_str == cmd::SHUTDOWN)      { g_running = 0; return make_ok_response(id, "{\"message\":\"shutting down\"}"); }
    if (cmd_str == cmd::INFO)          return handle_info(id);
    if (cmd_str == cmd::TRACE_DRIVER)  return handle_trace_driver(id, params);
    if (cmd_str == cmd::TRACE_LOAD)    return handle_trace_load(id, params);
    if (cmd_str == cmd::FANIN_REG)     return handle_fanin_reg(id, params);
    if (cmd_str == cmd::FANOUT_REG)    return handle_fanout_reg(id, params);
    if (cmd_str == cmd::TRACE_PATH)    return handle_trace_path(id, params);
    if (cmd_str == cmd::INST_CONN)     return handle_inst_conn(id, params);

    return make_error_response(id, err::INVALID_PARAMS, "Unknown command: " + cmd_str);
}

// ─── Socket I/O ──────────────────────────────────────────────────────────────

static std::string read_line(int fd) {
    std::string line;
    char buf[4096];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') return line;
            line += buf[i];
        }
    }
    return line;
}

static bool send_line(int fd, const std::string& msg) {
    std::string data = msg + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.c_str() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// ─── Server entry point ─────────────────────────────────────────────────────

/**
 * Run the server process (called AFTER fork if daemonized).
 * @param argc/argv     Original args (passed to npi_init / npi_load_design)
 * @param run_dir       Configured RunDir with socket/pid paths
 * @param design_source Path to design (KDB dbdir or source file)
 * @param design_args   Extra args for npi_load_design (e.g., {"-dbdir", "simv.daidir"})
 * @return exit code
 */
inline int run_server(int argc, char** argv,
                      RunDir& run_dir, const std::string& design_source,
                      const std::vector<std::string>& design_args) {
    g_run_dir = &run_dir;
    g_design_source = design_source;

    // ── Initialize NPI ───────────────────────────────────────────────────────
    std::cerr << "[vsignal-server] Initializing NPI...\n";
    npi_init(argc, argv);

    // ── Build argv for npi_load_design ───────────────────────────────────────
    // npi_load_design expects argc/argv like command-line args:
    //   program_name  <design_args...>
    std::vector<std::string> load_args;
    load_args.push_back("vsignal");
    for (auto& a : design_args) load_args.push_back(a);

    std::vector<char*> c_args;
    for (auto& s : load_args) c_args.push_back(const_cast<char*>(s.c_str()));

    std::cerr << "[vsignal-server] Loading design:";
    for (auto& a : design_args) std::cerr << " " << a;
    std::cerr << "\n";

    int load_ret = npi_load_design((int)c_args.size(), c_args.data());
    // npi_load_design returns non-negative on success (often 1)
    if (load_ret < 0) {
        std::cerr << "[vsignal-server] ERROR: npi_load_design failed (rc="
                  << load_ret << ")\n";
        npi_end();
        return 1;
    }

    std::cerr << "[vsignal-server] Design loaded successfully.\n";

    // Write PID and design source
    run_dir.write_pid(getpid());
    run_dir.write_design_source();
    g_start_time = std::chrono::steady_clock::now();

    // Signals
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Create UDS
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        run_dir.cleanup();
        npi_end();
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (run_dir.socket_path().size() >= sizeof(addr.sun_path)) {
        std::cerr << "[vsignal-server] ERROR: Socket path too long ("
                  << run_dir.socket_path().size() << " >= "
                  << sizeof(addr.sun_path) << "): "
                  << run_dir.socket_path() << "\n"
                  << "Hint: use --run-dir to specify a shorter path.\n";
        close(g_listen_fd);
        run_dir.cleanup();
        npi_end();
        return 1;
    }
    strncpy(addr.sun_path, run_dir.socket_path().c_str(), sizeof(addr.sun_path) - 1);

    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_listen_fd);
        run_dir.cleanup();
        npi_end();
        return 1;
    }

    if (listen(g_listen_fd, 5) < 0) {
        perror("listen");
        close(g_listen_fd);
        run_dir.cleanup();
        npi_end();
        return 1;
    }

    // Restrict socket access to owner only
    chmod(run_dir.socket_path().c_str(), 0600);

    std::cerr << "[vsignal-server] Listening on: " << run_dir.socket_path() << "\n";
    std::cerr << "[vsignal-server] PID: " << getpid() << "\n";
    std::cerr << "[vsignal-server] Ready.\n";

    // Event loop
    fcntl(g_listen_fd, F_SETFL, O_NONBLOCK);
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_listen_fd, &fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(g_listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        int client_fd = accept(g_listen_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        std::string request = read_line(client_fd);
        if (!request.empty()) {
            std::string response = dispatch_request(request);
            send_line(client_fd, response);
        }
        close(client_fd);
    }

    // Cleanup
    std::cerr << "[vsignal-server] Shutting down...\n";
    close(g_listen_fd);
    run_dir.cleanup();

    npi_end();
    std::cerr << "[vsignal-server] Bye.\n";
    return 0;
}

} // namespace server
} // namespace vsignal

#endif // VSIGNAL_SERVER_CORE_H
