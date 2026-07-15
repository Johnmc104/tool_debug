/**
 * @file main.cpp
 * @brief vsignal — netlist signal driver/load trace CLI.
 *
 * Single binary that:
 *   1. `vsignal open -dbdir <kdb_dir>`  — forks a background server daemon
 *      `vsignal open <source.v ...>`    — forks using RTL source files
 *   2. `vsignal <trace-command>`        — auto-detects running server and queries it
 *   3. `vsignal close`                  — stops the server
 *
 * The server socket / PID / log are managed under  <cwd>/.vtool/vsignal_run/
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <sstream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

// Client-side code (NPI-free)
#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"
#include "client/client_core.h"

// Server-side code (NPI-dependent, only executes in forked child)
#include "server/server_core.h"


// ─── Usage ───────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cerr <<
R"(vsignal — Netlist Signal Trace Tool v1.0
  by zhhe - Johnmc104@qq.com
Usage:
  vsignal open  -dbdir <kdb_dir>             Load design from VCS KDB database
  vsignal open  <source.v> [source2.v ...]   Load design from RTL source files
  vsignal close                              Close design (stop server)
  vsignal status                             Show server status
  vsignal info                               Show design info

  vsignal driver  <signal>                   Trace signal drivers
  vsignal load    <signal>                   Trace signal loads
  vsignal fanin   <signal>                   FanIn register connections
  vsignal fanout  <signal>                   FanOut register connections
  vsignal trace   <from_sig> <to_sig>        Signal-to-signal path trace
  vsignal conn    <instance>                 Instance port connections

Trace options:
  --assign-cell                Pass through assign cells (driver/load)
  --pass-mod                   Pass through module boundaries (driver/load)
  --stop-at-pin                Stop at pin (fanin/fanout)
  --report-primary-port        Report primary ports (fanin/fanout)
  --scope <name>               Limit scope for fanin/fanout
  --level high|low             Connection level for conn (default: high)

Global options:
  --run-dir <path>             Override runtime directory
  --json                       JSON output mode
  -h, --help                   Show this help

Examples:
  vsignal open -dbdir simv.daidir
  vsignal open rtl/top.v rtl/sub.v
  vsignal driver top.u_sub.sig_a
  vsignal load   top.u_sub.sig_b
  vsignal fanin  top.u_sub.q_reg
  vsignal fanout top.clk
  vsignal trace  top.data_in top.data_out
  vsignal conn   top.u_sub
  vsignal close
)";
}

// ─── resolve_run_dir: find or create RunDir ─────────────────────────────────

static bool resolve_run_dir(const std::string& /*run_dir_override*/,
                            vsignal::RunDir& out) {
    // Auto-detect: search upward from CWD
    if (vsignal::RunDir::auto_detect(out)) {
        return true;
    }
    std::cerr << "Error: No active design found.\n"
              << "Use 'vsignal open -dbdir <kdb_dir>' or 'vsignal open <sources>' first.\n";
    return false;
}

// ─── Command: open ───────────────────────────────────────────────────────────

static int cmd_open(int argc, char** argv,
                    const std::string& design_source,
                    const std::vector<std::string>& design_args,
                    const std::string& run_dir_override,
                    bool json_mode) {
    if (design_args.empty()) {
        std::cerr << "Error: No design source specified.\n"
                  << "Usage: vsignal open -dbdir <kdb_dir>\n"
                  << "       vsignal open <source.v> [...]\n";
        return 1;
    }

    vsignal::RunDir run_dir(design_source, run_dir_override);

    // Check if server already running
    if (run_dir.is_server_alive()) {
        std::string stored = run_dir.design_source();
        if (stored == run_dir.design_source()) {
            if (json_mode)
                std::cout << "{\"status\":\"ok\",\"message\":\"Server already running\","
                          << "\"pid\":" << run_dir.read_pid() << "}" << std::endl;
            else
                std::cout << "Server already running (PID " << run_dir.read_pid()
                          << ") for " << design_source << "\n";
            return 0;
        } else {
            // Different design → close old, open new
            if (!json_mode)
                std::cout << "Switching design: closing " << stored << "...\n";
            std::string req = vsignal::client::build_request(1, "shutdown");
            vsignal::client::send_request(run_dir.socket_path(), req);
            for (int i = 0; i < 30; ++i) {
                usleep(100000);
                if (!run_dir.is_server_alive()) break;
            }
            run_dir.cleanup();
        }
    } else {
        run_dir.cleanup();
    }

    // Ensure run directory exists
    run_dir.ensure_dir();

    // Fork server process
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        // ── Child: become daemon, then run server ──
        setsid();

        // Redirect stdout/stderr to log file
        int log_fd = open(run_dir.log_path().c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        close(STDIN_FILENO);

        // Run server (initializes NPI, loads design, listens)
        int rc = vsignal::server::run_server(argc, argv, run_dir,
                                              design_source, design_args);
        _exit(rc);
    }

    // ── Parent: wait for server to become ready ──
    bool ready = false;
    for (int i = 0; i < 300; ++i) {  // up to 30s (KDB load can be slow)
        usleep(100000);
        // Check child didn't crash
        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w > 0) {
            std::cerr << "Error: Server process exited unexpectedly.\n";
            std::cerr << "Check log: " << run_dir.log_path() << "\n";
            return 1;
        }
        // Check if socket exists and server is reachable
        struct stat sst;
        if (stat(run_dir.socket_path().c_str(), &sst) == 0) {
            std::string test_req = vsignal::client::build_request(0, "status");
            std::string resp = vsignal::client::send_request(run_dir.socket_path(), test_req);
            if (!resp.empty() && resp.find("\"ok\"") != std::string::npos) {
                ready = true;
                break;
            }
        }
    }

    if (!ready) {
        std::cerr << "Error: Server did not become ready within 30s.\n"
                  << "Check log: " << run_dir.log_path() << "\n";
        return 1;
    }

    if (json_mode) {
        std::cout << "{\"status\":\"ok\",\"message\":\"Design loaded\","
                  << "\"pid\":" << pid
                  << ",\"design\":\"" << design_source << "\""
                  << ",\"socket\":\"" << run_dir.socket_path() << "\""
                  << ",\"log\":\"" << run_dir.log_path() << "\"}" << std::endl;
    } else {
        std::cout << "Design loaded (PID " << pid << ")\n"
                  << "  Design: " << design_source << "\n"
                  << "  Socket: " << run_dir.socket_path() << "\n"
                  << "  Log:    " << run_dir.log_path() << "\n";
    }
    return 0;
}

// ─── Command: close ──────────────────────────────────────────────────────────

static int cmd_close(const vsignal::RunDir& run_dir, bool json_mode) {
    if (!run_dir.is_server_alive()) {
        if (json_mode)
            std::cout << "{\"status\":\"ok\",\"message\":\"No server running\"}" << std::endl;
        else
            std::cout << "No server running.\n";
        return 0;
    }

    std::string req = vsignal::client::build_request(1, "shutdown");
    std::string resp = vsignal::client::send_request(run_dir.socket_path(), req);

    for (int i = 0; i < 30; ++i) {
        usleep(100000);
        if (!run_dir.is_server_alive()) break;
    }

    if (run_dir.is_server_alive()) {
        pid_t pid = run_dir.read_pid();
        if (pid > 0) kill(pid, SIGKILL);
        usleep(200000);
    }

    run_dir.cleanup();

    if (json_mode) {
        vsignal::client::print_response(resp, true);
    } else {
        std::cout << "Design closed.\n";
    }
    return 0;
}

// ─── Query command dispatcher ────────────────────────────────────────────────

static int cmd_query(const vsignal::RunDir& run_dir, bool json_mode,
                     const std::string& command,
                     const std::string& positional1,
                     const std::string& positional2,
                     bool assign_cell, bool pass_mod,
                     bool stop_at_pin, bool report_primary_port,
                     const std::string& scope,
                     const std::string& level) {
    if (!run_dir.is_server_alive()) {
        std::cerr << "Error: No active design.\n"
                  << "Use 'vsignal open -dbdir <kdb_dir>' first.\n";
        return 1;
    }

    std::string request;
    int req_id = 1;

    if (command == "status") {
        request = vsignal::client::build_request(req_id, "status");

    } else if (command == "info") {
        request = vsignal::client::build_request(req_id, "info");

    } else if (command == "driver") {
        if (positional1.empty()) {
            std::cerr << "Error: Signal name required.\nUsage: vsignal driver <signal>\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("signal", positional1);
        if (assign_cell) p.set("assign_cell", (int64_t)1);
        if (pass_mod)    p.set("pass_mod", (int64_t)1);
        request = vsignal::client::build_request(req_id, "trace_driver", p.dump());

    } else if (command == "load") {
        if (positional1.empty()) {
            std::cerr << "Error: Signal name required.\nUsage: vsignal load <signal>\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("signal", positional1);
        if (assign_cell) p.set("assign_cell", (int64_t)1);
        if (pass_mod)    p.set("pass_mod", (int64_t)1);
        request = vsignal::client::build_request(req_id, "trace_load", p.dump());

    } else if (command == "fanin") {
        if (positional1.empty()) {
            std::cerr << "Error: Signal name required.\nUsage: vsignal fanin <signal>\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("signal", positional1);
        p.set_bool("stop_at_pin", stop_at_pin);
        p.set_bool("report_primary_port", report_primary_port);
        if (!scope.empty()) p.set("scope", scope);
        request = vsignal::client::build_request(req_id, "fanin_reg", p.dump());

    } else if (command == "fanout") {
        if (positional1.empty()) {
            std::cerr << "Error: Signal name required.\nUsage: vsignal fanout <signal>\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("signal", positional1);
        p.set_bool("stop_at_pin", stop_at_pin);
        p.set_bool("report_primary_port", report_primary_port);
        if (!scope.empty()) p.set("scope", scope);
        request = vsignal::client::build_request(req_id, "fanout_reg", p.dump());

    } else if (command == "trace") {
        if (positional1.empty() || positional2.empty()) {
            std::cerr << "Error: Two signal names required.\n"
                      << "Usage: vsignal trace <from_signal> <to_signal>\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("from", positional1);
        p.set("to", positional2);
        if (assign_cell) p.set("assign_cell", (int64_t)1);
        request = vsignal::client::build_request(req_id, "trace_path", p.dump());

    } else if (command == "conn") {
        if (positional1.empty()) {
            std::cerr << "Error: Instance name required.\nUsage: vsignal conn <instance>\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("instance", positional1);
        p.set("level", level);
        request = vsignal::client::build_request(req_id, "inst_conn", p.dump());

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }

    std::string response = vsignal::client::send_request(run_dir.socket_path(), request);
    if (response.empty()) {
        std::cerr << "Error: No response from server. Is it running?\n";
        return 1;
    }

    vsignal::client::print_response(response, json_mode);
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // ── Parse arguments ──
    std::string run_dir_override;
    std::string command;
    std::string positional1;        // first positional after command
    std::string positional2;        // second positional (for trace)
    bool json_mode = false;

    // Trace options
    bool assign_cell = false;
    bool pass_mod = false;
    bool stop_at_pin = false;
    bool report_primary_port = false;
    std::string scope;
    std::string level = "high";

    // Open-specific
    std::string dbdir;
    std::vector<std::string> source_files;

    // Parse all arguments
    bool in_open = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Global options
        if (arg == "--run-dir" && i + 1 < argc) {
            run_dir_override = argv[++i];
        } else if (arg == "--json") {
            json_mode = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;

        // Trace options
        } else if (arg == "--assign-cell") {
            assign_cell = true;
        } else if (arg == "--pass-mod") {
            pass_mod = true;
        } else if (arg == "--stop-at-pin") {
            stop_at_pin = true;
        } else if (arg == "--report-primary-port") {
            report_primary_port = true;
        } else if (arg == "--scope" && i + 1 < argc) {
            scope = argv[++i];
        } else if (arg == "--level" && i + 1 < argc) {
            level = argv[++i];

        // Open: -dbdir
        } else if (arg == "-dbdir" && i + 1 < argc) {
            dbdir = argv[++i];

        // Positional arguments
        } else if (arg[0] != '-') {
            if (command.empty()) {
                command = arg;
                if (command == "open") in_open = true;
            } else if (in_open) {
                // After "open", collect source files
                source_files.push_back(arg);
            } else if (positional1.empty()) {
                positional1 = arg;
            } else if (positional2.empty()) {
                positional2 = arg;
            }
        }
    }

    if (command.empty()) {
        print_usage();
        return 1;
    }

    // ── open: special handling ──
    if (command == "open") {
        std::string design_source;
        std::vector<std::string> design_args;

        if (!dbdir.empty()) {
            // KDB mode: -dbdir <path>
            design_source = dbdir;
            design_args.push_back("-dbdir");
            design_args.push_back(dbdir);
        } else if (!source_files.empty()) {
            // RTL source mode
            design_source = source_files[0];
            for (auto& f : source_files) design_args.push_back(f);
        }

        return cmd_open(argc, argv, design_source, design_args,
                        run_dir_override, json_mode);
    }

    // ── All other commands need a RunDir ──
    vsignal::RunDir run_dir;
    if (!resolve_run_dir(run_dir_override, run_dir))
        return 1;

    // ── close ──
    if (command == "close") {
        return cmd_close(run_dir, json_mode);
    }

    // ── Query commands ──
    return cmd_query(run_dir, json_mode, command,
                     positional1, positional2,
                     assign_cell, pass_mod,
                     stop_at_pin, report_primary_port,
                     scope, level);
}
