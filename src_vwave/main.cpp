/**
 * @file main.cpp
 * @brief vwave — unified FSDB waveform reader CLI.
 *
 * Single binary that:
 *   1. `vwave open <file.fsdb>` — forks a background server daemon
 *   2. `vwave <query-command>`  — auto-detects the running server and queries it
 *   3. `vwave close`            — stops the server
 *   4. `vwave open <other.fsdb>`— switches to a different waveform (close + reopen)
 *
 * The server socket / PID / log are managed under  <cwd>/.vtool/wave_run/
 * This ensures the runtime dir is always writable, even when the FSDB
 * resides on a shared or read-only filesystem.
 * Auto-detect searches upward from CWD for a live .vtool/wave_run/ directory.
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

// NPI environment auto-setup (shared)
#include "tw/npi_env.h"

// ─── Usage ───────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cerr <<
R"(vwave — FSDB Waveform Reader v1.2
  by zhhe - Johnmc104@qq.com
Usage:
  vwave open  <file.fsdb> [--timeout <sec>]  Load waveform (start background server)
  vwave close                                Close waveform (stop server)
  vwave status                               Show server status
  vwave info                                 Show FSDB file info
  vwave scopes  [<path>]                     List hierarchy scopes
  vwave signals <path>                       List signals in scope
  vwave signal-info <name>                   Show signal details
  vwave find <pattern> [--scope <path>]      Search signals by wildcard
  vwave get [options]                        Read signal value(s)

Get options:
  -s, --signal <name>       Signal path (repeatable for multi-signal)
  -f, --signal-file <file>  Read signals from file (one per line)
  -t, --time <t>            Read value at time t
  -b, --begin <t>           Range start time (with --end)
  -e, --end <t>             Range end time (with --begin)
  -r, --radix <fmt>         Value format: bin|hex|oct|dec (default: bin)

Global options:
  --fsdb <path>             Explicit FSDB path (skip auto-detect)
  --run-dir <path>          Override runtime directory
  --timeout <sec>           Server start timeout (default: 30, open only)
  --compact, -c             Compact output (short names, less tokens)
  --json                    JSON output mode
  -h, --help                Show this help

Examples:
  vwave open test/tb_top.fsdb
  vwave info
  vwave scopes
  vwave scopes tb
  vwave signals tb.intf
  vwave get -s tb.intf.clk -t 500000
  vwave get -s tb.intf.clk -s tb.intf.paddr -t 1500000 -r hex
  vwave get -s tb.intf.clk -b 0 -e 100000
  vwave get -f signals.txt -t 500000 -r hex
  vwave close
)";
}

// ─── resolve_run_dir: find or create RunDir ─────────────────────────────────

/**
 * Resolve the RunDir either from explicit --fsdb or by auto-detection.
 * Returns false and prints error if not found.
 */
static bool resolve_run_dir(const std::string& fsdb_path,
                            const std::string& run_dir_override,
                            wave::RunDir& out) {
    if (!fsdb_path.empty()) {
        out = wave::RunDir(fsdb_path, run_dir_override);
        return true;
    }
    // Auto-detect: search upward from CWD
    if (wave::RunDir::auto_detect(out)) {
        return true;
    }
    std::cerr << "Error: No active waveform found.\n"
              << "Use 'vwave open <file.fsdb>' to load a waveform first,\n"
              << "or specify '--fsdb <path>' explicitly.\n";
    return false;
}

// ─── Command: open ───────────────────────────────────────────────────────────

static int cmd_open(int argc, char** argv,
                    const std::string& fsdb_path,
                    const std::string& run_dir_override,
                    bool json_mode,
                    int open_timeout_sec) {
    if (fsdb_path.empty()) {
        std::cerr << "Error: Missing FSDB file path.\n"
                  << "Usage: vwave open <file.fsdb>\n";
        return 1;
    }

    // Verify file exists
    struct stat st;
    if (stat(fsdb_path.c_str(), &st) != 0) {
        std::cerr << "Error: File not found: " << fsdb_path << "\n";
        return 1;
    }

    wave::RunDir run_dir(fsdb_path, run_dir_override);

    // Check if server already running for this FSDB
    if (run_dir.is_server_alive()) {
        std::string stored_fsdb = run_dir.fsdb_path();
        // Resolve the incoming path for comparison
        char resolved[PATH_MAX];
        std::string abs_fsdb = fsdb_path;
        if (realpath(fsdb_path.c_str(), resolved)) abs_fsdb = resolved;

        if (stored_fsdb == abs_fsdb) {
            if (json_mode)
                std::cout << "{\"status\":\"ok\",\"message\":\"Server already running\","
                          << "\"pid\":" << run_dir.read_pid() << "}" << std::endl;
            else
                std::cout << "Server already running (PID " << run_dir.read_pid()
                          << ") for " << fsdb_path << "\n";
            return 0;
        } else {
            // Different FSDB → close old, open new
            if (!json_mode)
                std::cout << "Switching waveform: closing " << stored_fsdb << "...\n";
            std::string req = wave::client::build_request(1, "shutdown");
            wave::client::send_request(run_dir.socket_path(), req);
            // Wait for server to exit
            for (int i = 0; i < 30; ++i) {
                usleep(100000);
                if (!run_dir.is_server_alive()) break;
            }
            run_dir.cleanup();
        }
    } else {
        // Clean stale files
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

        // Run server (initializes NPI, opens FSDB, listens)
        int rc = wave::server::run_server(argc, argv, run_dir, run_dir.fsdb_path());
        _exit(rc);
    }

    // ── Parent: wait for server to become ready ──
    // Poll for socket file + verify child is alive
    bool ready = false;
    int max_polls = open_timeout_sec * 10;  // 100ms per poll
    for (int i = 0; i < max_polls; ++i) {
        usleep(100000);
        // Check child didn't crash
        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w > 0) {
            // Child exited prematurely
            std::cerr << "Error: Server process exited unexpectedly.\n";
            std::cerr << "Check log: " << run_dir.log_path() << "\n";
            return 1;
        }
        // Check if socket exists and server is reachable
        struct stat sst;
        if (stat(run_dir.socket_path().c_str(), &sst) == 0) {
            std::string test_req = wave::client::build_request(0, "status");
            std::string resp = wave::client::send_request(run_dir.socket_path(), test_req);
            if (!resp.empty() && resp.find("\"ok\"") != std::string::npos) {
                ready = true;
                break;
            }
        }
    }

    if (!ready) {
        // Check if child is still running (loading, not crashed)
        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == 0) {
            // Process still alive — likely still loading
            if (json_mode) {
                std::cout << "{\"status\":\"loading\",\"message\":\"Server still starting\","
                          << "\"pid\":" << pid
                          << ",\"timeout\":" << open_timeout_sec << "}" << std::endl;
            } else {
                std::cerr << "Warning: Server still loading (PID " << pid
                          << ", waited " << open_timeout_sec << "s).\n"
                          << "Use 'vwave status' to check when ready.\n";
            }
            return 2;  // Distinct from hard failure (exit code 1)
        }
        std::cerr << "Error: Server process exited during startup.\n"
                  << "Check log: " << run_dir.log_path() << "\n";
        return 1;
    }

    if (json_mode) {
        std::cout << "{\"status\":\"ok\",\"message\":\"Waveform loaded\","
                  << "\"pid\":" << pid
                  << ",\"fsdb\":\"" << run_dir.fsdb_path() << "\""
                  << ",\"socket\":\"" << run_dir.socket_path() << "\""
                  << ",\"log\":\"" << run_dir.log_path() << "\"}" << std::endl;
    } else {
        std::cout << "Waveform loaded (PID " << pid << ")\n"
                  << "  FSDB:   " << run_dir.fsdb_path() << "\n"
                  << "  Socket: " << run_dir.socket_path() << "\n"
                  << "  Log:    " << run_dir.log_path() << "\n";
    }
    return 0;
}

// ─── Command: close ──────────────────────────────────────────────────────────

static int cmd_close(const wave::RunDir& run_dir, bool json_mode) {
    if (!run_dir.is_server_alive()) {
        if (json_mode)
            std::cout << "{\"status\":\"ok\",\"message\":\"No server running\"}" << std::endl;
        else
            std::cout << "No server running.\n";
        return 0;
    }

    std::string req = wave::client::build_request(1, "shutdown");
    std::string resp = wave::client::send_request(run_dir.socket_path(), req);

    // Wait for server to exit
    for (int i = 0; i < 30; ++i) {
        usleep(100000);
        if (!run_dir.is_server_alive()) break;
    }

    // Force kill if still alive
    if (run_dir.is_server_alive()) {
        pid_t pid = run_dir.read_pid();
        if (pid > 0) kill(pid, SIGKILL);
        usleep(200000);
    }

    run_dir.cleanup();

    if (json_mode) {
        wave::client::print_response(resp, true);
    } else {
        std::cout << "Waveform closed.\n";
    }
    return 0;
}

// ─── Query command dispatcher ────────────────────────────────────────────────

static int cmd_query(const wave::RunDir& run_dir, bool json_mode,
                     const std::string& command,
                     const std::string& scope_path,
                     const std::string& signal_name,
                     const std::vector<std::string>& extra_signals,
                     const std::string& signal_file,
                     int64_t time_val, int64_t begin_time, int64_t end_time,
                     const std::string& radix,
                     bool compact_mode,
                     const std::string& find_scope) {
    if (!run_dir.is_server_alive()) {
        std::cerr << "Error: No active waveform.\n"
                  << "Use 'vwave open <file.fsdb>' first.\n";
        return 1;
    }

    std::string request;
    int req_id = 1;

    if (command == "status") {
        request = wave::client::build_request(req_id, "status");

    } else if (command == "info") {
        request = wave::client::build_request(req_id, "file_info");

    } else if (command == "scopes") {
        wave::JsonObject p;
        if (!scope_path.empty()) p.set("path", scope_path);
        if (compact_mode) p.set_bool("compact", true);
        request = wave::client::build_request(req_id, "list_scopes", p.dump());

    } else if (command == "signals") {
        if (scope_path.empty()) {
            std::cerr << "Error: Scope path required.\nUsage: vwave signals <path>\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("path", scope_path);
        if (compact_mode) p.set_bool("compact", true);
        request = wave::client::build_request(req_id, "list_signals", p.dump());

    } else if (command == "signal-info") {
        if (signal_name.empty()) {
            std::cerr << "Error: Signal name required.\nUsage: vwave signal-info <name>\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("signal", signal_name);
        request = wave::client::build_request(req_id, "signal_info", p.dump());

    } else if (command == "find") {
        std::string pattern = scope_path;
        if (pattern.empty() && !signal_name.empty()) pattern = signal_name;
        if (pattern.empty()) {
            std::cerr << "Error: Pattern required.\nUsage: vwave find <pattern> [--scope <path>]\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("pattern", pattern);
        if (!find_scope.empty()) p.set("scope", find_scope);
        request = wave::client::build_request(req_id, "find_signals", p.dump());

    } else if (command == "get-value") {
        std::vector<std::string> all_signals;
        if (!signal_name.empty()) all_signals.push_back(signal_name);
        for (auto& s : extra_signals) all_signals.push_back(s);
        if (!signal_file.empty()) {
            auto file_sigs = wave::client::read_signal_file(signal_file);
            all_signals.insert(all_signals.end(), file_sigs.begin(), file_sigs.end());
        }
        if (all_signals.empty()) {
            std::cerr << "Error: No signals specified (use -s or -f)\n";
            return 1;
        }

        if (begin_time >= 0 && end_time >= 0) {
            if (all_signals.size() > 1) {
                std::cerr << "Warning: --begin/--end mode only supports one signal, "
                          << "using '" << all_signals[0] << "'\n";
            }
            wave::JsonObject p;
            p.set("signal", all_signals[0]);
            p.set("begin", begin_time);
            p.set("end", end_time);
            p.set("radix", radix);
            request = wave::client::build_request(req_id, "get_value_between", p.dump());
        } else if (time_val >= 0) {
            wave::JsonObject p;
            p.set_array("signals", all_signals);
            p.set("time", time_val);
            p.set("radix", radix);
            request = wave::client::build_request(req_id, "get_value_at", p.dump());
        } else {
            std::cerr << "Error: --time or (--begin + --end) required for get-value\n";
            return 1;
        }

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }

    std::string response = wave::client::send_request(run_dir.socket_path(), request);
    if (response.empty()) {
        std::cerr << "Error: No response from server. Is it running?\n";
        return 1;
    }

    wave::client::print_response(response, json_mode);
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Auto-configure NPI environment from VERDI_HOME
    tw::ensure_npi_env(argc, argv);

    // ── Parse arguments ──
    std::string fsdb_path;
    std::string run_dir_override;
    std::string command;
    std::string scope_or_positional;   // first positional after command
    std::string signal_name;
    std::string signal_file;
    std::string radix = "bin";
    int64_t time_val = -1;
    int64_t begin_time = -1;
    int64_t end_time = -1;
    bool json_mode = false;
    int open_timeout_sec = 30;
    bool compact_mode = false;
    std::string find_scope;
    std::vector<std::string> extra_signals;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Global options
        if (arg == "--fsdb" && i + 1 < argc) {
            fsdb_path = argv[++i];
        } else if (arg == "--run-dir" && i + 1 < argc) {
            run_dir_override = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            open_timeout_sec = std::atoi(argv[++i]);
            if (open_timeout_sec < 1) open_timeout_sec = 30;
        } else if (arg == "--json") {
            json_mode = true;
        } else if (arg == "--compact" || arg == "-c") {
            compact_mode = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;

        // Get-value options (short + long form)
        } else if ((arg == "-s" || arg == "--signal") && i + 1 < argc) {
            if (signal_name.empty())
                signal_name = argv[++i];
            else
                extra_signals.push_back(argv[++i]);
        } else if ((arg == "-f" || arg == "--signal-file") && i + 1 < argc) {
            signal_file = argv[++i];
        } else if ((arg == "-t" || arg == "--time") && i + 1 < argc) {
            time_val = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-b" || arg == "--begin") && i + 1 < argc) {
            begin_time = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-e" || arg == "--end") && i + 1 < argc) {
            end_time = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-r" || arg == "--radix") && i + 1 < argc) {
            radix = argv[++i];
        } else if (arg == "--scope" && i + 1 < argc) {
            find_scope = argv[++i];
        } else if (arg == "--path" && i + 1 < argc) {
            // backward compatibility
            scope_or_positional = argv[++i];

        // Positional arguments
        } else if (arg[0] != '-') {
            if (command.empty()) {
                command = arg;
            } else if (scope_or_positional.empty()) {
                scope_or_positional = arg;
            }
        }
    }

    if (command.empty()) {
        print_usage();
        return 1;
    }

    // ── open: special handling ──
    if (command == "open") {
        // The positional after "open" is the FSDB path
        if (fsdb_path.empty() && !scope_or_positional.empty())
            fsdb_path = scope_or_positional;
        return cmd_open(argc, argv, fsdb_path, run_dir_override, json_mode,
                        open_timeout_sec);
    }

    // ── Normalize command aliases ──
    if (command == "get") command = "get-value";

    // ── All other commands need a RunDir ──
    wave::RunDir run_dir;
    if (!resolve_run_dir(fsdb_path, run_dir_override, run_dir))
        return 1;

    // ── close ──
    if (command == "close") {
        return cmd_close(run_dir, json_mode);
    }

    // ── For "scopes" and "signals", the positional arg is the path ──
    std::string scope_path = scope_or_positional;
    // "signal-info": positional arg is the signal name
    if (command == "signal-info" && signal_name.empty() && !scope_or_positional.empty()) {
        signal_name = scope_or_positional;
        scope_path.clear();
    }

    // ── Query commands ──
    return cmd_query(run_dir, json_mode, command,
                     scope_path, signal_name, extra_signals, signal_file,
                     time_val, begin_time, end_time, radix, compact_mode,
                     find_scope);
}
