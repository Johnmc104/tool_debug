/**
 * @file main.cpp
 * @brief vwave — unified FSDB waveform reader CLI.
 *
 * CLI with isolated backend workers:
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
#include <poll.h>
#include <chrono>
#include <stdexcept>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

// Client-side code (NPI-free)
#include "tw/daemon.h"
#include "common/protocol.h"
#include "common/json_parser.h"
#include "common/run_dir.h"
#include "client/client_core.h"

// No vendor code or SDK headers in the CLI.
#include "common/session.h"

// ─── CLI options ────────────────────────────────────────────────────────────

static std::string g_executable;

struct CliOptions {
    std::string command;
    std::string fsdb_path;
    std::string run_dir_override;
    std::string backend; // empty on queries means use the open session
    std::string verdi_home;
    std::string error;
    bool json_mode       = false;
    int open_timeout_sec = 30;

    // Output control
    bool compact_mode    = false;
    int64_t limit_val    = 1000;
    int depth            = 1;

    // Positional / signal
    std::string scope_path;
    std::string signal_name;
    std::vector<std::string> extra_signals;
    std::string signal_file;

    // Value query
    int64_t time_val     = -1;
    int64_t begin_time   = -1;
    int64_t end_time     = -1;
    std::string radix    = "bin";

    // Edge
    std::string edge_type = "any";
    std::string edge_dir  = "forward";

    // Find
    std::string find_scope;
};


// ─── Usage ───────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cerr <<
R"(vwave — Read FSDB waveform files via background server

Architecture: "vwave open" starts a daemon; subsequent commands query it via
  Unix socket. Auto-detects running server by searching upward from CWD for
  .vtool/wave_run/. All query commands support --json for structured output.

Commands:
  open   <file.fsdb>                   Load waveform (start server daemon)
  close                                Stop server and clean up
  status                               Server uptime, PID, loaded file
  info                                 FSDB time range and scope count

Hierarchy:
  scopes  [<path>] [--depth N]         List child scopes (default depth: 1)
  signals <scope>                      List signals in a scope
  signal-info <signal>                 Signal metadata (type, size, bit range)
  find <pattern> [--scope <path>]      Glob search (e.g. "tb.*.clk")

Value queries:
  get -s <signal> -t <time>            Value at a single time point
  get -s <sig1> -s <sig2> -t <time>    Multiple signals at same time
  get -s <signal> -b <t0> -e <t1>      All value changes in time range
  get -f <file> -t <time>              Batch read from signal-list file

Signal analysis:
  edge -s <signal> -t <time>           Find next edge from time t
  vc-count -s <signal> [-b <t> -e <t>] Count value changes in range

Value options (get):
  -s, --signal <name>       Signal path (repeatable for multi-signal)
  -f, --signal-file <file>  Read signal names from file, one per line
  -t, --time <t>            Read at time t (integer, simulation time units)
  -b, --begin <t>           Range start time (requires --end)
  -e, --end <t>             Range end time (requires --begin)
  -r, --radix <fmt>         Output format: bin|hex|oct|dec (default: bin)
  --limit <N>               Max samples for range query (default: 1000)

Edge options:
  --rising                  Rising edges only
  --falling                 Falling edges only
  --dir forward|backward    Search direction (default: forward)

Global options:
  --json                    JSON output (recommended for programmatic use)
  --compact, -c             Compact output (shorter keys, fewer tokens)
  --depth <N>               Scope recursion depth (default: 1)
  --fsdb <path>             Explicit FSDB path (skip auto-detect)
  --run-dir <path>          Override runtime directory (.vtool/wave_run/)
  --backend npi|ffr        Reader backend (open default: npi; queries: current session)
  --verdi-home <path>       FsdbReader install (ffr open; default: VERDI_HOME)
  --timeout <sec>           Server start timeout (default: 30, open only)
  -h, --help                Show this help

Signal paths use dot-separated hierarchy: tb.u_cpu.core.clk

Examples:
  vwave open sim/tb_top.fsdb
  vwave scopes tb --depth 2 --json
  vwave signals tb.u_cpu --json
  vwave get -s tb.u_cpu.clk -t 1000 --json
  vwave get -s tb.u_cpu.clk -s tb.u_cpu.rst -t 5000 -r hex --json
  vwave get -s tb.u_cpu.data -b 0 -e 50000 -r hex --json
  vwave edge -s tb.u_cpu.clk -t 1000 --rising --json
  vwave vc-count -s tb.u_cpu.clk -b 0 -e 100000 --json
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
    if (!run_dir_override.empty()) {
        out = wave::RunDir::from_dir(run_dir_override);
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

static std::string worker_path(const std::string& backend) {
    char executable[PATH_MAX];
    auto size = readlink("/proc/self/exe", executable, sizeof(executable)-1);
    std::string path;
    if (size >= 0) {
        executable[size]=0;
        path=executable;
    } else if (g_executable.find('/')!=std::string::npos) {
        // 最小 chroot 中可以没有 /proc；明确的启动路径仍可定位相邻 worker。
        path=wave::absolute_path(g_executable);
    } else throw std::runtime_error("Cannot locate vwave executable; mount /proc or use an explicit executable path");
    return path.substr(0,path.rfind('/')+1)+"vwave-"+backend+"-worker";
}

static int exec_worker(const std::string& worker, const wave::Session& session,
                       const std::string& mode, const std::string& dir) {
    if (session.backend=="ffr") {
        // exec 前设置，仅作用于子进程；同一个 reader 的依赖优先使用配套目录。
        std::string libs=session.sdk_home+"/share/FsdbReader/linux64";
        setenv("LD_LIBRARY_PATH",libs.c_str(),1);
        setenv("VERDI_HOME",session.sdk_home.c_str(),1);
    }
    auto config=session.json();
    if (mode=="--describe") {
        execl(worker.c_str(),worker.c_str(),mode.c_str(),nullptr);
        std::cerr<<"ERROR: Cannot execute worker "<<worker<<": "<<strerror(errno)<<"\n";
        return 1;
    }
    execl(worker.c_str(),worker.c_str(),mode.c_str(),config.c_str(),dir.c_str(),nullptr);
    std::cerr<<"ERROR: Cannot execute worker "<<worker<<": "<<strerror(errno)<<"\n";
    return 1;
}

static bool preflight(const std::string& worker, const wave::Session& session,
                      const std::string& mode="--check", std::string* output=nullptr) {
    int fds[2];
    if (pipe(fds)!=0) throw std::runtime_error("pipe failed");
    pid_t child=fork();
    if (child<0) { close(fds[0]); close(fds[1]); throw std::runtime_error("fork failed"); }
    if (child==0) {
        close(fds[0]); dup2(fds[1],STDERR_FILENO); dup2(fds[1],STDOUT_FILENO); close(fds[1]);
        _exit(exec_worker(worker,session,mode,"-"));
    }
    close(fds[1]); std::string log; char buffer[4096];
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    int status=0;
    while (std::chrono::steady_clock::now()<deadline) {
        pollfd fd{fds[0],POLLIN,0};
        if (poll(&fd,1,100)>0) {
            auto n=read(fds[0],buffer,sizeof(buffer));
            if (n<=0) break;
            log.append(buffer,n);
        }
    }
    close(fds[0]);
    if (std::chrono::steady_clock::now()>=deadline) {
        kill(child,SIGKILL); waitpid(child,&status,0);
        std::cerr<<"Error: Reader preflight timed out.\n"; return false;
    }
    while (waitpid(child,&status,0)<0 && errno==EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status)!=0) {
        std::cerr<<"Error: Backend preflight failed.\n"<<log; return false;
    }
    if (output) *output=log;
    return true;
}

static wave::Session select_session(const CliOptions& opts) {
    wave::Session s;
    s.backend=opts.backend.empty() ? "npi" : opts.backend;
    s.file=wave::absolute_path(opts.fsdb_path);
    s.file_identity=wave::file_stamp(s.file);
    if (s.backend=="ffr") {
        std::string selected=opts.verdi_home;
        if (selected.empty() && getenv("VERDI_HOME")) selected=getenv("VERDI_HOME");
        if (selected.empty()) throw std::runtime_error("FFR requires --verdi-home or VERDI_HOME");
        s.sdk_home=wave::absolute_path(selected);
        auto dir=s.sdk_home+"/share/FsdbReader/linux64/";
        s.reader=wave::absolute_path(dir+"libnffr.so"); s.support=wave::absolute_path(dir+"libnsys.so");
    } else {
        auto worker=worker_path("npi");
        if (access(worker.c_str(),X_OK)!=0) throw std::runtime_error("Missing backend worker: "+worker);
        std::string output;
        if (!preflight(worker,s,"--describe",&output)) throw std::runtime_error("Cannot inspect NPI worker");
        auto actual=wave::Session::parse(output);
        s.sdk_home=actual.sdk_home; s.reader=actual.reader; s.support=actual.support;
    }
    s.reader_hash=wave::sha256_file(s.reader); s.support_hash=wave::sha256_file(s.support);
    return s;
}

static int cmd_open(const CliOptions& opts) {
    if (opts.fsdb_path.empty()) throw std::runtime_error("Missing FSDB file path");
    auto session=select_session(opts);
    auto worker=worker_path(session.backend);
    if (access(worker.c_str(),X_OK)!=0) throw std::runtime_error("Missing backend worker: "+worker);
    wave::RunDir run_dir(session.file,opts.run_dir_override);
    auto send=[](const std::string& s,const std::string& r) { return tw::client::send_request(s,r); };
    if (run_dir.is_server_alive()) {
        auto stored=tw::RunDir::read_file_content(run_dir.dir()+"/session.json");
        wave::JsonParser response, data;
        auto status=send(run_dir.socket_path(),wave::client::build_request(0,"status"));
        bool verified=response.parse(status) && response.get_string("status")=="ok" &&
            data.parse(response.get_string("data")) && data.get_int("pid",0)==run_dir.read_pid();
        if (!verified) throw std::runtime_error("Live PID with unverified server status; check runtime directory: "+run_dir.dir());
        bool live=data.get_string("backend","npi")==session.backend;
        if (live && stored==session.json()) {
            if (opts.json_mode) {
                wave::JsonObject result; result.set("status","ok"); result.set("message","Server already running");
                result.set("pid",int64_t(run_dir.read_pid())); std::cout<<result.dump()<<std::endl;
            } else std::cout<<"Server already running (PID "<<run_dir.read_pid()<<") for "<<session.file<<"\n";
            return 0;
        }
    }
    // 验证候选库后才停止已有会话。
    if (!preflight(worker,session)) return 1;
    if (run_dir.is_server_alive()) {
        tw::daemon::shutdown_server(run_dir.base(),send,wave::client::build_request(1,"shutdown"));
    }
    run_dir.cleanup();
    if (!run_dir.ensure_dir()) throw std::runtime_error("Cannot create runtime directory: "+run_dir.dir());
    tw::daemon::LaunchConfig cfg;
    cfg.log_tag="vwave"; cfg.timeout_sec=opts.open_timeout_sec; cfg.json_mode=opts.json_mode;
    return tw::daemon::fork_and_wait(run_dir.base(),
        [&]() { return exec_worker(worker,session,"--serve",run_dir.dir()); },
        [&]() { return wave::client::build_request(0,"status"); },send,cfg);
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

    wave::JsonParser response, data;
    auto status=wave::client::send_request(run_dir.socket_path(),wave::client::build_request(0,"status"));
    if (!response.parse(status) || response.get_string("status")!="ok" ||
        !data.parse(response.get_string("data")) || data.get_int("pid",0)!=run_dir.read_pid())
        throw std::runtime_error("Live PID with unverified server status; check runtime directory: "+run_dir.dir());

    tw::daemon::shutdown_server(
        run_dir.base(),
        [](const std::string& s, const std::string& r) {
            return tw::client::send_request(s, r);
        },
        wave::client::build_request(1, "shutdown"));
    run_dir.cleanup();

    if (json_mode)
        std::cout << "{\"status\":\"ok\",\"message\":\"Waveform closed\"}" << std::endl;
    else
        std::cout << "Waveform closed.\n";
    return 0;
}

// ─── Query command dispatcher ────────────────────────────────────────────────

// Collect all signal sources into one vector
static std::vector<std::string> collect_signals(const CliOptions& opts) {
    std::vector<std::string> all;
    if (!opts.signal_name.empty()) all.push_back(opts.signal_name);
    for (auto& s : opts.extra_signals) all.push_back(s);
    if (!opts.signal_file.empty()) {
        auto fs = wave::client::read_signal_file(opts.signal_file);
        all.insert(all.end(), fs.begin(), fs.end());
    }
    return all;
}

static int cmd_query(const wave::RunDir& run_dir, const CliOptions& opts) {
    if (!run_dir.is_server_alive()) {
        std::cerr << "Error: No active waveform.\n"
                  << "Use 'vwave open <file.fsdb>' first.\n";
        return 1;
    }

    const auto& cmd = opts.command;
    std::string request;
    int req_id = 1;

    if (cmd == "status") {
        request = wave::client::build_request(req_id, "status");

    } else if (cmd == "info") {
        request = wave::client::build_request(req_id, "file_info");

    } else if (cmd == "scopes") {
        wave::JsonObject p;
        if (!opts.scope_path.empty()) p.set("path", opts.scope_path);
        if (opts.compact_mode) p.set_bool("compact", true);
        if (opts.depth > 1) p.set("depth", static_cast<int64_t>(opts.depth));
        request = wave::client::build_request(req_id, "list_scopes", p.dump());

    } else if (cmd == "signals") {
        if (opts.scope_path.empty()) {
            std::cerr << "Error: Scope path required.\nUsage: vwave signals <path>\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("path", opts.scope_path);
        if (opts.compact_mode) p.set_bool("compact", true);
        request = wave::client::build_request(req_id, "list_signals", p.dump());

    } else if (cmd == "signal-info") {
        if (opts.signal_name.empty()) {
            std::cerr << "Error: Signal name required.\nUsage: vwave signal-info <name>\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("signal", opts.signal_name);
        request = wave::client::build_request(req_id, "signal_info", p.dump());

    } else if (cmd == "find") {
        std::string pattern = opts.scope_path;
        if (pattern.empty() && !opts.signal_name.empty()) pattern = opts.signal_name;
        if (pattern.empty()) {
            std::cerr << "Error: Pattern required.\nUsage: vwave find <pattern>\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("pattern", pattern);
        if (!opts.find_scope.empty()) p.set("scope", opts.find_scope);
        request = wave::client::build_request(req_id, "find_signals", p.dump());

    } else if (cmd == "get-value") {
        auto all_signals = collect_signals(opts);
        if (all_signals.empty()) {
            std::cerr << "Error: No signals specified (use -s or -f)\n";
            return 1;
        }

        if (opts.begin_time >= 0 && opts.end_time >= 0) {
            if (all_signals.size() > 1) {
                std::ostringstream agg;
                agg << "{\"id\":1,\"status\":\"ok\",\"data\":{\"begin\":"
                    << opts.begin_time << ",\"end\":" << opts.end_time
                    << ",\"signals\":[";
                for (size_t si = 0; si < all_signals.size(); ++si) {
                    if (si) agg << ",";
                    wave::JsonObject p;
                    p.set("signal", all_signals[si]);
                    p.set("begin", opts.begin_time);
                    p.set("end", opts.end_time);
                    p.set("radix", opts.radix);
                    if (opts.limit_val != 1000) p.set("limit", opts.limit_val);
                    std::string req = wave::client::build_request(
                        static_cast<int>(si + 1), "get_value_between", p.dump());
                    std::string resp = wave::client::send_request(
                        run_dir.socket_path(), req);
                    wave::JsonParser rp;
                    if (!resp.empty() && rp.parse(resp)
                        && rp.get_string("status") == "ok")
                        agg << rp.get_string("data");
                    else
                        agg << "{\"signal\":\"" << all_signals[si]
                            << "\",\"error\":\"QUERY_FAILED\"}";
                }
                agg << "]}}";
                wave::client::print_response(agg.str(), opts.json_mode);
                return 0;
            }
            wave::JsonObject p;
            p.set("signal", all_signals[0]);
            p.set("begin", opts.begin_time);
            p.set("end", opts.end_time);
            p.set("radix", opts.radix);
            if (opts.limit_val != 1000) p.set("limit", opts.limit_val);
            request = wave::client::build_request(req_id, "get_value_between", p.dump());

        } else if (opts.time_val >= 0) {
            wave::JsonObject p;
            p.set_array("signals", all_signals);
            p.set("time", opts.time_val);
            p.set("radix", opts.radix);
            request = wave::client::build_request(req_id, "get_value_at", p.dump());
        } else {
            std::cerr << "Error: --time or (--begin + --end) required for get-value\n";
            return 1;
        }

    } else if (cmd == "edge") {
        std::string sig = opts.signal_name;
        if (sig.empty()) sig = opts.scope_path;
        if (sig.empty() || opts.time_val < 0) {
            std::cerr << "Error: -s <sig> and -t <time> required for edge\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("signal", sig);
        p.set("time", opts.time_val);
        p.set("edge", opts.edge_type);
        p.set("dir", opts.edge_dir);
        request = wave::client::build_request(req_id, "next_edge", p.dump());

    } else if (cmd == "vc-count") {
        std::string sig = opts.signal_name;
        if (sig.empty()) sig = opts.scope_path;
        if (sig.empty()) {
            std::cerr << "Error: Signal required for vc-count\n";
            return 1;
        }
        wave::JsonObject p;
        p.set("signal", sig);
        if (opts.begin_time >= 0) p.set("begin", opts.begin_time);
        if (opts.end_time >= 0) p.set("end", opts.end_time);
        request = wave::client::build_request(req_id, "vc_count", p.dump());

    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage();
        return 1;
    }

    std::string response = wave::client::send_request(run_dir.socket_path(), request);
    if (response.empty()) {
        std::cerr << "Error: No response from server. Is it running?\n";
        return 1;
    }
    wave::client::print_response(response, opts.json_mode);
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────────────

static CliOptions parse_args(int argc, char** argv) {
    CliOptions opts;
    std::string scope_or_positional;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--fsdb" && i+1 < argc)        { opts.fsdb_path = argv[++i];
        } else if (arg == "--backend" && i+1 < argc) { opts.backend = argv[++i];
        } else if (arg == "--verdi-home" && i+1 < argc) { opts.verdi_home = argv[++i];
        } else if (arg == "--run-dir" && i+1 < argc) { opts.run_dir_override = argv[++i];
        } else if (arg == "--timeout" && i+1 < argc) {
            opts.open_timeout_sec = std::atoi(argv[++i]);
            if (opts.open_timeout_sec < 1) opts.open_timeout_sec = 30;
        } else if (arg == "--json")              { opts.json_mode = true;
        } else if (arg == "--compact" || arg == "-c") { opts.compact_mode = true;
        } else if (arg == "-h" || arg == "--help") { print_usage(); std::exit(0);

        } else if ((arg == "-s" || arg == "--signal") && i+1 < argc) {
            if (opts.signal_name.empty()) opts.signal_name = argv[++i];
            else opts.extra_signals.push_back(argv[++i]);
        } else if ((arg == "-f" || arg == "--signal-file") && i+1 < argc) {
            opts.signal_file = argv[++i];
        } else if ((arg == "-t" || arg == "--time") && i+1 < argc) {
            opts.time_val = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-b" || arg == "--begin") && i+1 < argc) {
            opts.begin_time = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-e" || arg == "--end") && i+1 < argc) {
            opts.end_time = std::strtoll(argv[++i], nullptr, 10);
        } else if ((arg == "-r" || arg == "--radix") && i+1 < argc) {
            opts.radix = argv[++i];
        } else if ((arg == "-l" || arg == "--limit") && i+1 < argc) {
            opts.limit_val = std::strtoll(argv[++i], nullptr, 10);
        } else if (arg == "--scope" && i+1 < argc)  { opts.find_scope = argv[++i];
        } else if (arg == "--depth" && i+1 < argc) {
            opts.depth = std::atoi(argv[++i]);
            if (opts.depth < 1) opts.depth = 1;
        } else if (arg == "--rising")             { opts.edge_type = "rising";
        } else if (arg == "--falling")            { opts.edge_type = "falling";
        } else if (arg == "--dir" && i+1 < argc)  { opts.edge_dir = argv[++i];
        } else if (arg == "--path" && i+1 < argc) { scope_or_positional = argv[++i];

        } else if (!arg.empty() && arg[0] != '-') {
            if (opts.command.empty()) opts.command = arg;
            else if (scope_or_positional.empty()) scope_or_positional = arg;
            else opts.error = "Unexpected argument: " + arg;
        } else opts.error = "Unknown option or missing value: " + arg;
    }

    if (!opts.backend.empty() && opts.backend != "npi" && opts.backend != "ffr")
        opts.error = "--backend must be npi or ffr";
    if (!opts.verdi_home.empty() && (opts.command != "open" || opts.backend != "ffr"))
        opts.error = "--verdi-home is only supported with open --backend ffr";

    // Normalize
    if (opts.command == "get") opts.command = "get-value";

    // Route positional arg to scope_path or signal_name
    if (opts.command == "signal-info" && opts.signal_name.empty()
        && !scope_or_positional.empty()) {
        opts.signal_name = scope_or_positional;
    } else {
        opts.scope_path = scope_or_positional;
    }

    return opts;
}

int main(int argc, char** argv) {
    g_executable = argv[0];
    try {
    CliOptions opts = parse_args(argc, argv);
    if (!opts.error.empty()) throw std::runtime_error(opts.error);

    if (opts.command.empty()) {
        print_usage();
        return 1;
    }

    // ── open ──
    if (opts.command == "open") {
        if (opts.fsdb_path.empty() && !opts.scope_path.empty())
            opts.fsdb_path = opts.scope_path;
        return cmd_open(opts);
    }

    // ── RunDir for all other commands ──
    wave::RunDir run_dir;
    if (!resolve_run_dir(opts.fsdb_path, opts.run_dir_override, run_dir))
        return 1;

    if (!opts.backend.empty() && run_dir.is_server_alive()) {
        auto response = wave::client::send_request(run_dir.socket_path(),wave::client::build_request(0,"status"));
        wave::JsonParser status, data;
        if (!status.parse(response) || status.get_string("status")!="ok" || !data.parse(status.get_string("data")))
            throw std::runtime_error("Cannot inspect active backend");
        auto actual=data.get_string("backend","npi");
        if (actual!=opts.backend) throw std::runtime_error("Active backend is "+actual+"; use open --backend "+opts.backend+" to switch");
    }
    if (opts.command == "close")
        return cmd_close(run_dir, opts.json_mode);

    return cmd_query(run_dir, opts);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
