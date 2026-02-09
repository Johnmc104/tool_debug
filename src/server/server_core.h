/**
 * @file server_core.h
 * @brief FSDB waveform server core — NPI integration and request handling.
 *
 * This module contains all NPI-dependent server logic:
 *   - FSDB file open/close
 *   - Command handlers (status, info, scopes, signals, values)
 *   - Socket event loop
 *
 * Decoupled from main() so the single `vwave` binary can fork a server
 * process internally without a separate executable.
 */
#ifndef WAVE_SERVER_CORE_H
#define WAVE_SERVER_CORE_H

#include <iostream>
#include <string>
#include <cstring>
#include <csignal>
#include <vector>
#include <sstream>
#include <algorithm>
#include <chrono>

// System
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// NPI headers
#include "npi.h"
#include "npi_fsdb.h"
#include "npi_L1.h"

// Project
#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"

namespace wave {
namespace server {

// ─── Server state (file-scope within the server process) ─────────────────────

static npiFsdbFileHandle g_file_hdl = nullptr;
static std::string       g_fsdb_path;
static int               g_listen_fd = -1;
static bool              g_running = true;
static RunDir*           g_run_dir = nullptr;
static std::chrono::steady_clock::time_point g_start_time;

// ─── Signal handler ──────────────────────────────────────────────────────────

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

// ─── NPI helpers ─────────────────────────────────────────────────────────────

static const char* direction_str(int dir) {
    switch (dir) {
        case npiFsdbDirInput:  return "input";
        case npiFsdbDirOutput: return "output";
        case npiFsdbDirInout:  return "inout";
        default:               return "none";
    }
}

// ─── Command handlers ────────────────────────────────────────────────────────

static std::string handle_status(int id) {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - g_start_time).count();

    JsonObject data;
    data.set("fsdb_file", g_fsdb_path);
    data.set("uptime_seconds", uptime);
    data.set("pid", (int64_t)getpid());
    return make_ok_response(id, data.dump());
}

static std::string handle_file_info(int id) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    npiFsdbTime min_t = 0, max_t = 0;
    npi_fsdb_min_time(g_file_hdl, &min_t);
    npi_fsdb_max_time(g_file_hdl, &max_t);

    NPI_INT32 is_completed = 0;
    npi_fsdb_file_property(npiFsdbFileIsCompleted, g_file_hdl, &is_completed);

    const char* scale_unit = npi_fsdb_file_property_str(npiFsdbFileScaleUnit, g_file_hdl);
    const char* version    = npi_fsdb_file_property_str(npiFsdbFileVersion, g_file_hdl);

    JsonObject data;
    data.set("file", g_fsdb_path);
    data.set("min_time", (int64_t)min_t);
    data.set("max_time", (int64_t)max_t);
    data.set("scale_unit", scale_unit ? scale_unit : "");
    data.set("version", version ? version : "");
    data.set("is_completed", (int64_t)is_completed);
    return make_ok_response(id, data.dump());
}

static std::string handle_list_scopes(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string path = params.get_string("path");
    std::vector<std::string> scope_names;

    if (path.empty()) {
        npiFsdbScopeIter iter = npi_fsdb_iter_top_scope(g_file_hdl);
        if (iter) {
            npiFsdbScopeHandle scope;
            while ((scope = npi_fsdb_iter_scope_next(iter)) != nullptr) {
                const char* name = npi_fsdb_scope_property_str(npiFsdbScopeFullName, scope);
                if (name) scope_names.push_back(name);
            }
            npi_fsdb_iter_scope_stop(iter);
        }
    } else {
        npiFsdbScopeHandle parent = npi_fsdb_scope_by_name(g_file_hdl, path.c_str(), nullptr);
        if (!parent)
            return make_error_response(id, err::SCOPE_NOT_FOUND,
                                       "Scope '" + path + "' not found");
        npiFsdbScopeIter iter = npi_fsdb_iter_child_scope(parent);
        if (iter) {
            npiFsdbScopeHandle scope;
            while ((scope = npi_fsdb_iter_scope_next(iter)) != nullptr) {
                const char* name = npi_fsdb_scope_property_str(npiFsdbScopeFullName, scope);
                if (name) scope_names.push_back(name);
            }
            npi_fsdb_iter_scope_stop(iter);
        }
    }

    JsonObject data;
    data.set("path", path.empty() ? "/" : path);
    data.set_array("scopes", scope_names);
    data.set("count", (int64_t)scope_names.size());
    return make_ok_response(id, data.dump());
}

static std::string handle_list_signals(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string path = params.get_string("path");
    if (path.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'path' parameter");

    npiFsdbScopeHandle scope = npi_fsdb_scope_by_name(g_file_hdl, path.c_str(), nullptr);
    if (!scope)
        return make_error_response(id, err::SCOPE_NOT_FOUND,
                                   "Scope '" + path + "' not found");

    std::ostringstream arr;
    arr << "[";
    npiFsdbSigIter iter = npi_fsdb_iter_sig(scope);
    bool first = true;
    if (iter) {
        npiFsdbSigHandle sig;
        while ((sig = npi_fsdb_iter_sig_next(iter)) != nullptr) {
            const char* sig_name = npi_fsdb_sig_property_str(npiFsdbSigName, sig);
            const char* sig_full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
            NPI_INT32 left = 0, right = 0, dir = 0;
            npi_fsdb_sig_property(npiFsdbSigLeftRange, sig, &left);
            npi_fsdb_sig_property(npiFsdbSigRightRange, sig, &right);
            npi_fsdb_sig_property(npiFsdbSigDirection, sig, &dir);

            if (!first) arr << ",";
            first = false;

            JsonObject sig_obj;
            sig_obj.set("name", sig_name ? sig_name : "");
            sig_obj.set("full_name", sig_full ? sig_full : "");
            sig_obj.set("left", (int64_t)left);
            sig_obj.set("right", (int64_t)right);
            sig_obj.set("direction", direction_str(dir));
            arr << sig_obj.dump();
        }
        npi_fsdb_iter_sig_stop(iter);
    }
    arr << "]";

    JsonObject data;
    data.set("path", path);
    data.set_raw("signals", arr.str());
    return make_ok_response(id, data.dump());
}

static std::string handle_signal_info(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string sig_name = params.get_string("signal");
    if (sig_name.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal' parameter");

    npiFsdbSigHandle sig = npi_fsdb_sig_by_name(g_file_hdl, sig_name.c_str(), nullptr);
    if (!sig)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Signal '" + sig_name + "' not found");

    const char* name = npi_fsdb_sig_property_str(npiFsdbSigName, sig);
    const char* full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
    NPI_INT32 left = 0, right = 0, dir = 0;
    npi_fsdb_sig_property(npiFsdbSigLeftRange, sig, &left);
    npi_fsdb_sig_property(npiFsdbSigRightRange, sig, &right);
    npi_fsdb_sig_property(npiFsdbSigDirection, sig, &dir);

    JsonObject data;
    data.set("name", name ? name : "");
    data.set("full_name", full ? full : "");
    data.set("left", (int64_t)left);
    data.set("right", (int64_t)right);
    data.set("direction", direction_str(dir));
    return make_ok_response(id, data.dump());
}

static std::string handle_get_value_at(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    int64_t time = params.get_int("time", -1);
    if (time < 0)
        return make_error_response(id, err::INVALID_TIME, "Missing or invalid 'time'");

    std::string radix_str = params.get_string("radix", "bin");
    npiFsdbValType format = npiFsdbBinStrVal;
    if (radix_str == "hex") format = npiFsdbHexStrVal;
    else if (radix_str == "oct") format = npiFsdbOctStrVal;
    else if (radix_str == "dec") format = npiFsdbDecStrVal;

    std::vector<std::string> signals = params.get_array("signals");
    if (signals.empty()) {
        std::string single = params.get_string("signal");
        if (!single.empty()) signals.push_back(single);
    }

    if (signals.empty())
        return make_error_response(id, err::INVALID_PARAMS, "No signals specified");

    npiFsdbTime fsdb_time = (npiFsdbTime)time;

    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < signals.size(); ++i) {
        if (i) arr << ",";

        npiFsdbSigHandle sig = npi_fsdb_sig_by_name(g_file_hdl, signals[i].c_str(), nullptr);
        JsonObject val_obj;
        val_obj.set("signal", signals[i]);

        if (!sig) {
            val_obj.set("error", "not found");
        } else {
            std::string val_str;
            if (npi_fsdb_sig_value_at(g_file_hdl, signals[i].c_str(),
                                       fsdb_time, val_str, format)) {
                val_obj.set("value", val_str);
            } else {
                val_obj.set("value", "x");
                val_obj.set("error", "read failed");
            }
            val_obj.set("actual_time", (int64_t)fsdb_time);
        }
        arr << val_obj.dump();
    }
    arr << "]";

    JsonObject data;
    data.set("time", (int64_t)time);
    data.set_raw("values", arr.str());
    return make_ok_response(id, data.dump());
}

static std::string handle_get_value_between(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string sig_name = params.get_string("signal");
    if (sig_name.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal'");

    int64_t begin = params.get_int("begin", -1);
    int64_t end   = params.get_int("end", -1);
    if (begin < 0 || end < 0 || end < begin)
        return make_error_response(id, err::INVALID_TIME, "Invalid time range");

    std::string radix_str = params.get_string("radix", "bin");
    npiFsdbValType format = npiFsdbBinStrVal;
    if (radix_str == "hex") format = npiFsdbHexStrVal;
    else if (radix_str == "oct") format = npiFsdbOctStrVal;
    else if (radix_str == "dec") format = npiFsdbDecStrVal;

    npiFsdbSigHandle sig = npi_fsdb_sig_by_name(g_file_hdl, sig_name.c_str(), nullptr);
    if (!sig)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Signal '" + sig_name + "' not found");

    npiFsdbTime begin_t = (npiFsdbTime)begin;
    npiFsdbTime end_t   = (npiFsdbTime)end;

    fsdbTimeValPairVec_t vcVec;
    if (npi_fsdb_sig_value_between(g_file_hdl, sig_name.c_str(),
                                    begin_t, end_t, vcVec, format)) {
        std::ostringstream arr;
        arr << "[";
        for (size_t i = 0; i < vcVec.size(); ++i) {
            if (i) arr << ",";
            JsonObject vc;
            vc.set("time", (int64_t)vcVec[i].first);
            vc.set("value", vcVec[i].second);
            arr << vc.dump();
        }
        arr << "]";

        JsonObject data;
        data.set("signal", sig_name);
        data.set("begin", (int64_t)begin);
        data.set("end", (int64_t)end);
        data.set_raw("changes", arr.str());
        return make_ok_response(id, data.dump());
    } else {
        JsonObject data;
        data.set("signal", sig_name);
        data.set("begin", (int64_t)begin);
        data.set("end", (int64_t)end);
        data.set_raw("changes", "[]");
        return make_ok_response(id, data.dump());
    }
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

    if (cmd_str == cmd::STATUS)            return handle_status(id);
    if (cmd_str == cmd::SHUTDOWN)          { g_running = false; return make_ok_response(id, "{\"message\":\"shutting down\"}"); }
    if (cmd_str == cmd::FILE_INFO)         return handle_file_info(id);
    if (cmd_str == cmd::LIST_SCOPES)       return handle_list_scopes(id, params);
    if (cmd_str == cmd::LIST_SIGNALS)      return handle_list_signals(id, params);
    if (cmd_str == cmd::SIGNAL_INFO)       return handle_signal_info(id, params);
    if (cmd_str == cmd::GET_VALUE_AT)      return handle_get_value_at(id, params);
    if (cmd_str == cmd::GET_VALUE_BETWEEN) return handle_get_value_between(id, params);

    return make_error_response(id, err::INVALID_PARAMS, "Unknown command: " + cmd_str);
}

// ─── Socket I/O ──────────────────────────────────────────────────────────────

static std::string read_line(int fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        line += c;
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
 * @param argc/argv  Original args (passed to npi_init)
 * @param run_dir    Configured RunDir with socket/pid paths
 * @param fsdb_path  Path to FSDB file
 * @return exit code
 */
inline int run_server(int argc, char** argv,
                      RunDir& run_dir, const std::string& fsdb_path) {
    g_run_dir = &run_dir;
    g_fsdb_path = fsdb_path;

    // Initialize NPI (must be after fork)
    std::cerr << "[vwave-server] Initializing NPI...\n";
    npi_init(argc, argv);

    // Open FSDB
    std::cerr << "[vwave-server] Loading FSDB: " << fsdb_path << "\n";
    g_file_hdl = npi_fsdb_open(fsdb_path.c_str());
    if (!g_file_hdl) {
        std::cerr << "[vwave-server] ERROR: Failed to open FSDB: " << fsdb_path << "\n";
        npi_end();
        return 1;
    }

    npiFsdbTime min_t = 0, max_t = 0;
    npi_fsdb_min_time(g_file_hdl, &min_t);
    npi_fsdb_max_time(g_file_hdl, &max_t);
    std::cerr << "[vwave-server] FSDB loaded. Time range: " << min_t << " ~ " << max_t << "\n";

    // Write PID and FSDB path
    run_dir.write_pid(getpid());
    run_dir.write_fsdb_path();
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
        npi_fsdb_close(g_file_hdl);
        npi_end();
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, run_dir.socket_path().c_str(), sizeof(addr.sun_path) - 1);

    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_listen_fd);
        run_dir.cleanup();
        npi_fsdb_close(g_file_hdl);
        npi_end();
        return 1;
    }

    if (listen(g_listen_fd, 5) < 0) {
        perror("listen");
        close(g_listen_fd);
        run_dir.cleanup();
        npi_fsdb_close(g_file_hdl);
        npi_end();
        return 1;
    }

    std::cerr << "[vwave-server] Listening on: " << run_dir.socket_path() << "\n";
    std::cerr << "[vwave-server] PID: " << getpid() << "\n";
    std::cerr << "[vwave-server] Ready.\n";

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
    std::cerr << "[vwave-server] Shutting down...\n";
    close(g_listen_fd);
    run_dir.cleanup();

    if (g_file_hdl) {
        npi_fsdb_close(g_file_hdl);
        g_file_hdl = nullptr;
    }
    npi_end();
    std::cerr << "[vwave-server] Bye.\n";
    return 0;
}

} // namespace server
} // namespace wave

#endif // WAVE_SERVER_CORE_H
