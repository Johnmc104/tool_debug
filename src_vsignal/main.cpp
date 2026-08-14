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
#include "tw/daemon.h"

// Server-side code (NPI-dependent, only executes in forked child)
#include "server/server_core.h"

// ─── CLI options ────────────────────────────────────────────────────────────

struct CliOptions {
    std::string command;
    std::string run_dir_override;
    bool json_mode      = false;

    // Output control
    bool compact_mode   = true;
    int64_t limit_val   = 50;

    // Trace options
    bool assign_cell    = false;
    bool pass_mod       = false;
    bool stop_at_pin    = false;
    bool report_primary_port = false;
    std::string scope;
    std::string level   = "high";

    // Signals
    std::vector<std::string> signals;
    std::string positional2;

    // Open-specific
    std::string dbdir;
    std::vector<std::string> source_files;
};


// ─── Usage ───────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cerr <<
R"(vsignal — Trace netlist signal drivers, loads, and register connectivity

Architecture: "vsignal open" starts a daemon that loads a VCS KDB database or
  RTL sources via Verdi NPI. Subsequent trace commands query the loaded netlist.
  Auto-detects running server by searching upward from CWD for
  .vtool/vsignal_run/. All query commands support --json for structured output.

Commands:
  open  -dbdir <kdb_dir>               Load design from VCS KDB (e.g. simv.daidir)
  open  <file.v> [file2.v ...]         Load design from Verilog source files
  close                                Stop server and clean up
  status                               Server uptime, PID, loaded design
  info                                 Loaded design metadata

Trace commands:
  driver  <signal>                     What drives this signal
  load    <signal>                     What this signal drives (fanout)
  fanin   <signal>                     Trace backward to source registers
  fanout  <signal>                     Trace forward to destination registers
  trace   <from_sig> <to_sig>          Combinational path between two signals
  conn    <instance>                   All port connections of an instance

Trace options (parentheses = applicable commands):
  --assign-cell              Follow through assign cells (driver, load, trace)
  --pass-mod                 Cross module boundaries (driver, load)
  --stop-at-pin              Stop at instance pins (fanin, fanout)
  --report-primary-port      Include primary I/O ports (fanin, fanout)
  --scope <name>             Limit search scope (fanin, fanout)
  --level high|low           Connection abstraction for conn (default: high)

Multi-signal (driver, load, fanin, fanout):
  -s, --signal <name>        Signal path (repeatable for batch query)
  -f, --signal-file <file>   Read signal names from file, one per line

Output control:
  --full                     Full output (type, full_name, direction, size)
  --compact, -c              Compact output — DEFAULT (leaf names only)
  --limit <N>, -l <N>        Max results per signal (default: 50, 0=all)

Global options:
  --json                     JSON output (recommended for programmatic use)
  --run-dir <path>           Override runtime directory (.vtool/vsignal_run/)
  -h, --help                 Show this help

Signal paths use dot-separated hierarchy: top.u_sub.sig_a

Examples:
  vsignal open -dbdir simv.daidir
  vsignal driver top.u_sub.sig_a --json
  vsignal load top.u_sub.sig_b --assign-cell --json
  vsignal fanin top.u_sub.q_reg --stop-at-pin --json
  vsignal fanout top.clk --scope top.u_sub --json
  vsignal trace top.data_in top.data_out --json
  vsignal conn top.u_sub --level low --json
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

// ─── LD_LIBRARY_PATH auto-complete ───────────────────────────────────────────

static void ensure_npi_lib_path() {
    const char* verdi_home = std::getenv("VERDI_HOME");
    if (!verdi_home) {
        std::cerr << "Warning: VERDI_HOME not set. NPI libraries may not be found.\n"
                  << "Hint: module load synopsys/verdi/<version>\n";
        return;
    }
    std::string extra = std::string(verdi_home) + "/platform/linux64/bin";
    std::string cur = std::getenv("LD_LIBRARY_PATH") ? std::getenv("LD_LIBRARY_PATH") : "";
    if (cur.find(extra) == std::string::npos) {
        std::string npi_lib = std::string(verdi_home) + "/share/NPI/lib/linux64";
        std::string newpath = npi_lib + ":" + extra;
        if (!cur.empty()) newpath += ":" + cur;
        setenv("LD_LIBRARY_PATH", newpath.c_str(), 1);
    }
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
        std::string stored = tw::RunDir::read_file_content(
            run_dir.design_source_file());
        char resolved[PATH_MAX];
        std::string abs_design = design_source;
        if (realpath(design_source.c_str(), resolved))
            abs_design = resolved;

        if (stored == abs_design) {
            if (json_mode)
                std::cout << "{\"status\":\"ok\",\"message\":\"Server already running\","
                          << "\"pid\":" << run_dir.read_pid() << "}" << std::endl;
            else
                std::cout << "Server already running (PID " << run_dir.read_pid()
                          << ") for " << design_source << "\n";
            return 0;
        }
        if (!json_mode)
            std::cout << "Switching design: closing " << stored << "...\n";
        tw::daemon::shutdown_server(
            run_dir.base(), [](const std::string& s, const std::string& r) { return tw::client::send_request(s, r); },
            vsignal::client::build_request(1, "shutdown"));
        run_dir.cleanup();
    } else {
        run_dir.cleanup();
    }

    run_dir.ensure_dir();
    ensure_npi_lib_path();

    tw::daemon::LaunchConfig cfg;
    cfg.log_tag     = "vsignal";
    cfg.timeout_sec = 30;
    cfg.json_mode   = json_mode;

    return tw::daemon::fork_and_wait(
        run_dir.base(),
        [&]() {
            return vsignal::server::run_server(argc, argv, run_dir,
                                                design_source, design_args);
        },
        [&]() { return vsignal::client::build_request(0, "status"); },
        [](const std::string& s, const std::string& r) { return tw::client::send_request(s, r); },
        cfg);
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

    tw::daemon::shutdown_server(
        run_dir.base(), [](const std::string& s, const std::string& r) { return tw::client::send_request(s, r); },
        vsignal::client::build_request(1, "shutdown"));
    run_dir.cleanup();

    if (json_mode)
        std::cout << "{\"status\":\"ok\",\"message\":\"Design closed\"}" << std::endl;
    else
        std::cout << "Design closed.\n";
    return 0;
}

// ─── Query command dispatcher ────────────────────────────────────────────────

// Build JSON params common to trace commands
static void fill_trace_params(vsignal::JsonObject& p, const CliOptions& opts) {
    if (opts.compact_mode) p.set_bool("compact", true);
    if (opts.limit_val > 0) p.set("limit", opts.limit_val);
}

static int cmd_query(const vsignal::RunDir& run_dir, const CliOptions& opts) {
    if (!run_dir.is_server_alive()) {
        std::cerr << "Error: No active design.\n"
                  << "Use 'vsignal open -dbdir <kdb_dir>' first.\n";
        return 1;
    }

    const auto& cmd = opts.command;
    const auto& sigs = opts.signals;

    auto is_trace_cmd = [](const std::string& c) {
        return c == "driver" || c == "load" || c == "fanin" || c == "fanout";
    };

    // ── Batch mode: multiple signals ──
    if (is_trace_cmd(cmd) && sigs.size() > 1) {
        std::ostringstream agg;
        agg << "{\"id\":1,\"status\":\"ok\",\"data\":{\"results\":[";
        for (size_t si = 0; si < sigs.size(); ++si) {
            if (si) agg << ",";
            vsignal::JsonObject p;
            p.set("signal", sigs[si]);
            fill_trace_params(p, opts);
            if (cmd == "driver" || cmd == "load") {
                if (opts.assign_cell) p.set("assign_cell", (int64_t)1);
                if (opts.pass_mod)    p.set("pass_mod", (int64_t)1);
            } else {
                p.set_bool("stop_at_pin", opts.stop_at_pin);
                p.set_bool("report_primary_port", opts.report_primary_port);
                if (!opts.scope.empty()) p.set("scope", opts.scope);
            }

            std::string cmd_name = (cmd == "driver") ? "trace_driver"
                                 : (cmd == "load")   ? "trace_load"
                                 : (cmd == "fanin")  ? "fanin_reg"
                                 :                     "fanout_reg";
            std::string req = vsignal::client::build_request(
                static_cast<int>(si + 1), cmd_name, p.dump());
            std::string resp = vsignal::client::send_request(
                run_dir.socket_path(), req);

            if (resp.empty()) {
                agg << "{\"signal\":\"" << sigs[si]
                    << "\",\"error\":\"NO_RESPONSE\"}";
            } else {
                vsignal::JsonParser rp;
                if (rp.parse(resp) && rp.get_string("status") == "ok")
                    agg << rp.get_string("data");
                else
                    agg << "{\"signal\":\"" << sigs[si]
                        << "\",\"error\":\"" << rp.get_string("status") << "\"}";
            }
        }
        agg << "]}}";
        vsignal::client::print_response(agg.str(), opts.json_mode);
        return 0;
    }

    // ── Single-signal path ──
    std::string sig1 = sigs.empty() ? "" : sigs[0];
    std::string request;
    int req_id = 1;

    if (cmd == "status") {
        request = vsignal::client::build_request(req_id, "status");
    } else if (cmd == "info") {
        request = vsignal::client::build_request(req_id, "info");

    } else if (cmd == "driver" || cmd == "load") {
        if (sig1.empty()) {
            std::cerr << "Error: Signal name required.\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("signal", sig1);
        if (opts.assign_cell) p.set("assign_cell", (int64_t)1);
        if (opts.pass_mod)    p.set("pass_mod", (int64_t)1);
        fill_trace_params(p, opts);
        request = vsignal::client::build_request(req_id,
            cmd == "driver" ? "trace_driver" : "trace_load", p.dump());

    } else if (cmd == "fanin" || cmd == "fanout") {
        if (sig1.empty()) {
            std::cerr << "Error: Signal name required.\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("signal", sig1);
        p.set_bool("stop_at_pin", opts.stop_at_pin);
        p.set_bool("report_primary_port", opts.report_primary_port);
        if (!opts.scope.empty()) p.set("scope", opts.scope);
        fill_trace_params(p, opts);
        request = vsignal::client::build_request(req_id,
            cmd == "fanin" ? "fanin_reg" : "fanout_reg", p.dump());

    } else if (cmd == "trace") {
        if (sig1.empty() || opts.positional2.empty()) {
            std::cerr << "Error: Two signal names required.\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("from", sig1);
        p.set("to", opts.positional2);
        if (opts.assign_cell) p.set("assign_cell", (int64_t)1);
        fill_trace_params(p, opts);
        request = vsignal::client::build_request(req_id, "trace_path", p.dump());

    } else if (cmd == "conn") {
        if (sig1.empty()) {
            std::cerr << "Error: Instance name required.\n";
            return 1;
        }
        vsignal::JsonObject p;
        p.set("instance", sig1);
        p.set("level", opts.level);
        request = vsignal::client::build_request(req_id, "inst_conn", p.dump());

    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage();
        return 1;
    }

    std::string response = vsignal::client::send_request(run_dir.socket_path(), request);
    if (response.empty()) {
        std::cerr << "Error: No response from server. Is it running?\n";
        return 1;
    }
    vsignal::client::print_response(response, opts.json_mode);
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────────────

static CliOptions parse_args(int argc, char** argv) {
    CliOptions opts;
    std::string positional1;
    std::string signal_name;
    std::vector<std::string> extra_signals;
    std::string signal_file;
    bool in_open = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--run-dir" && i + 1 < argc) {
            opts.run_dir_override = argv[++i];
        } else if (arg == "--json") {
            opts.json_mode = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            std::exit(0);

        } else if (arg == "--assign-cell")       { opts.assign_cell = true;
        } else if (arg == "--pass-mod")          { opts.pass_mod = true;
        } else if (arg == "--stop-at-pin")       { opts.stop_at_pin = true;
        } else if (arg == "--report-primary-port") { opts.report_primary_port = true;
        } else if (arg == "--scope" && i+1 < argc)  { opts.scope = argv[++i];
        } else if (arg == "--level" && i+1 < argc)  { opts.level = argv[++i];
        } else if (arg == "--compact" || arg == "-c") { opts.compact_mode = true;
        } else if (arg == "--full")              { opts.compact_mode = false;
        } else if ((arg == "--limit" || arg == "-l") && i+1 < argc) {
            opts.limit_val = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-s" || arg == "--signal") && i+1 < argc) {
            if (signal_name.empty()) signal_name = argv[++i];
            else extra_signals.push_back(argv[++i]);
        } else if ((arg == "-f" || arg == "--signal-file") && i+1 < argc) {
            signal_file = argv[++i];
        } else if (arg == "-dbdir" && i+1 < argc) {
            opts.dbdir = argv[++i];
        } else if (arg[0] != '-') {
            if (opts.command.empty()) {
                opts.command = arg;
                if (opts.command == "open") in_open = true;
            } else if (in_open) {
                opts.source_files.push_back(arg);
            } else if (positional1.empty()) {
                positional1 = arg;
            } else if (opts.positional2.empty()) {
                opts.positional2 = arg;
            }
        }
    }

    // Collect signals: -s flags → -f file → positional
    if (!signal_name.empty()) opts.signals.push_back(signal_name);
    for (auto& s : extra_signals) opts.signals.push_back(s);
    if (!signal_file.empty()) {
        auto file_sigs = tw::client::read_signal_file(signal_file);
        opts.signals.insert(opts.signals.end(), file_sigs.begin(), file_sigs.end());
    }
    if (opts.signals.empty() && !positional1.empty())
        opts.signals.push_back(positional1);

    return opts;
}

int main(int argc, char** argv) {
    CliOptions opts = parse_args(argc, argv);

    if (opts.command.empty()) {
        print_usage();
        return 1;
    }

    // ── open ──
    if (opts.command == "open") {
        std::string design_source;
        std::vector<std::string> design_args;

        if (!opts.dbdir.empty()) {
            char resolved[PATH_MAX];
            std::string abs_dbdir = opts.dbdir;
            if (realpath(opts.dbdir.c_str(), resolved)) abs_dbdir = resolved;
            design_source = abs_dbdir;
            design_args.push_back("-dbdir");
            design_args.push_back(abs_dbdir);
        } else if (!opts.source_files.empty()) {
            for (auto& f : opts.source_files) {
                char resolved[PATH_MAX];
                if (realpath(f.c_str(), resolved)) f = resolved;
            }
            design_source = opts.source_files[0];
            for (auto& f : opts.source_files) design_args.push_back(f);
        }

        return cmd_open(argc, argv, design_source, design_args,
                        opts.run_dir_override, opts.json_mode);
    }

    // ── RunDir for all other commands ──
    vsignal::RunDir run_dir;
    if (!resolve_run_dir(opts.run_dir_override, run_dir))
        return 1;

    if (opts.command == "close")
        return cmd_close(run_dir, opts.json_mode);

    return cmd_query(run_dir, opts);
}
