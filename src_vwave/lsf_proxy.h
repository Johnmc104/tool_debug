/**
 * @file lsf_proxy.h
 * @brief Login-host proxy for an LSF stdio-loop worker.
 *
 * The proxy owns the local UDS expected by the existing vwave CLI and keeps a
 * single bsub -I process attached to the worker's stdin/stdout.  This is the
 * small bridge needed for independent local `vwave status/get/...` commands.
 */
#ifndef WAVE_LSF_PROXY_H
#define WAVE_LSF_PROXY_H

#include "common/run_dir.h"
#include "common/session.h"
#include "common/json_parser.h"
#include "common/protocol.h"
#include "tw/lsf.h"
#include "tw/server_loop.h"
#include "tw/transport.h"

#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace wave {
namespace lsf {

struct Config {
    std::string bsub = "bsub";
    std::string bkill = "bkill";
    std::string queue;
    std::string resource;
    std::string job_name;
    int startup_timeout_sec = 180;
    int query_timeout_sec = 360;
};

class RemoteProcess {
public:
    RemoteProcess() = default;
    ~RemoteProcess() { terminate(); }
    RemoteProcess(const RemoteProcess&) = delete;

    void start(const Session& session, const Config& cfg) {
        int in_pipe[2], out_pipe[2], err_pipe[2];
        if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe))
            throw std::runtime_error("pipe failed for LSF worker");
        auto close_all = [&]() {
            close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
        };
        pid_ = fork();
        if (pid_ < 0) { close_all(); throw std::runtime_error("fork failed for LSF worker"); }
        if (pid_ == 0) {
            setpgid(0, 0);
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            // fork_and_wait deliberately closes its own stdin, so the first
            // pipe can legitimately be fd 0.  Do not close a descriptor
            // after dup2() if it is already the standard stream.
            if (in_pipe[0] > STDERR_FILENO) close(in_pipe[0]);
            if (out_pipe[1] > STDERR_FILENO) close(out_pipe[1]);
            if (err_pipe[1] > STDERR_FILENO) close(err_pipe[1]);
            if (in_pipe[1] > STDERR_FILENO) close(in_pipe[1]);
            if (out_pipe[0] > STDERR_FILENO) close(out_pipe[0]);
            if (err_pipe[0] > STDERR_FILENO) close(err_pipe[0]);
            std::vector<std::string> args = tw::lsf::split_command(cfg.bsub.empty() ? "bsub" : cfg.bsub);
            if (args.empty()) args.push_back("bsub");
            bool interactive = false;
            for (const auto& a : args) if (a == "-I" || a == "-Is" || a == "-Ip") interactive = true;
            if (!interactive) args.push_back("-I");
            if (!cfg.job_name.empty()) { args.push_back("-J"); args.push_back(cfg.job_name); }
            if (!cfg.queue.empty()) { args.push_back("-q"); args.push_back(cfg.queue); }
            if (!cfg.resource.empty()) { args.push_back("-R"); args.push_back(cfg.resource); }
            args.push_back(session_worker_);
            args.push_back("--stdio-loop");
            // bsub reconstructs a shell command and strips JSON quotes.  A
            // hex payload keeps the session metadata a single argv token on
            // the execution host while retaining the public stdio-loop mode.
            args.push_back(tw::transport::hex_encode(session.json()));
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
        in_fd_ = in_pipe[1]; out_fd_ = out_pipe[0]; err_fd_ = err_pipe[0];
        std::thread([this]() {
            std::string line;
            char c;
            while (read(err_fd_, &c, 1) == 1) {
                if (c == '\n') { handle_diag(line); line.clear(); }
                else if (line.size() < 65536) line += c;
            }
            if (!line.empty()) handle_diag(line);
        }).detach();
    }

    void set_worker(const std::string& worker) { session_worker_ = worker; }

    void wait_ready(int timeout_sec) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        while (std::chrono::steady_clock::now() < deadline) {
            int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            std::string line = read_line(std::max(1, std::min(remain, 1000)));
            if (line.empty()) continue;
            JsonParser j;
            if (!j.parse(line)) { handle_diag(line); continue; }
            if (j.get_string("type") == "ready" && j.get_string("protocol") == "vwave-stdio-loop") {
                remote_pid_ = static_cast<pid_t>(j.get_int("pid", 0));
                return;
            }
        }
        throw std::runtime_error("Timed out waiting for LSF vwave worker ready");
    }

    std::string request(const std::string& request, int timeout_sec) {
        if (in_fd_ < 0 || out_fd_ < 0) throw std::runtime_error("LSF worker pipe is closed");
        std::string line = request + "\n";
        size_t sent = 0;
        while (sent < line.size()) {
            ssize_t n = write(in_fd_, line.data() + sent, line.size() - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("write to LSF worker failed");
            sent += static_cast<size_t>(n);
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        while (std::chrono::steady_clock::now() < deadline) {
            int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            std::string response = read_line(std::max(1, std::min(remain, 1000)));
            if (response.empty()) continue;
            JsonParser parsed;
            if (!parsed.parse(response)) throw std::runtime_error("LSF worker emitted invalid JSON after ready");
            return response;
        }
        throw std::runtime_error("Timed out waiting for LSF vwave response");
    }

    pid_t remote_pid() const { return remote_pid_; }
    const std::string& job_id() const { return job_id_; }

    void terminate(const std::string& bkill = "") {
        if (in_fd_ >= 0) { close(in_fd_); in_fd_ = -1; }
        if (out_fd_ >= 0) { close(out_fd_); out_fd_ = -1; }
        if (pid_ > 0) {
            kill(-pid_, SIGTERM);
            kill(pid_, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                if (waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; break; }
                usleep(50000);
            }
            if (pid_ > 0) { kill(-pid_, SIGKILL); kill(pid_, SIGKILL); waitpid(pid_, nullptr, 0); pid_ = -1; }
        }
        if (!job_id_.empty() && !bkill.empty()) run_bkill(bkill, job_id_);
    }

private:
    pid_t pid_ = -1, remote_pid_ = 0;
    int in_fd_ = -1, out_fd_ = -1, err_fd_ = -1;
    std::string session_worker_;
    std::string job_id_;
    std::mutex diag_lock_;

    void handle_diag(const std::string& line) {
        std::lock_guard<std::mutex> lock(diag_lock_);
        auto begin = line.find("Job <");
        if (begin != std::string::npos) {
            begin += 5; auto end = line.find('>', begin);
            if (end != std::string::npos) job_id_ = line.substr(begin, end - begin);
        }
        if (!line.empty()) std::cerr << "[vwave-lsf] " << line << "\n";
    }

    std::string read_line(int timeout_ms) {
        std::string result;
        while (true) {
            pollfd p{out_fd_, POLLIN, 0};
            int rc = poll(&p, 1, timeout_ms);
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0) return result;
            char c;
            ssize_t n = read(out_fd_, &c, 1);
            if (n <= 0) return result;
            if (c == '\n') return result;
            if (result.size() >= 4 * 1024 * 1024) throw std::runtime_error("LSF response exceeds 4 MiB");
            result += c;
            timeout_ms = 1000;
        }
    }

    static void run_bkill(const std::string& command, const std::string& job_id) {
        auto args = tw::lsf::split_command(command);
        if (args.empty()) args.push_back("bkill");
        args.push_back(job_id);
        pid_t child = fork();
        if (child == 0) {
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr); execvp(argv[0], argv.data()); _exit(127);
        }
        if (child > 0) waitpid(child, nullptr, 0);
    }
};

inline int run_proxy(const Session& session, RunDir& run_dir, const Config& cfg,
                     const std::string& worker) {
    RemoteProcess remote;
    remote.set_worker(worker);
    if (session.backend == "ffr") {
        std::string libs = session.sdk_home + "/share/FsdbReader/linux64";
        setenv("LD_LIBRARY_PATH", libs.c_str(), 1);
        setenv("VERDI_HOME", session.sdk_home.c_str(), 1);
    }
    try {
        remote.start(session, cfg);
        remote.wait_ready(cfg.startup_timeout_sec);
        if (!run_dir.write_pid(getpid()) || !run_dir.write_fsdb_path())
            throw std::runtime_error("Cannot publish LSF proxy state");
        session.write(run_dir.dir());
        {
            std::ofstream job_name_file(run_dir.dir()+"/lsf_job_name");
            job_name_file << cfg.job_name << "\n";
            std::ofstream job_id_file(run_dir.dir()+"/lsf_job_id");
            job_id_file << remote.job_id() << "\n";
        }
        tw::server::g_running = 1;
        tw::server::install_signal_handlers();
        tw::server::ServerConfig server_cfg;
        server_cfg.log_tag = "vwave-lsf-proxy";
        server_cfg.idle_timeout_sec = 3600;
        server_cfg.client_timeout_sec = cfg.query_timeout_sec;
        auto dispatch = [&](const std::string& request) {
            JsonParser parser;
            if (parser.parse(request) && parser.get_string("cmd") == "status") {
                auto response = remote.request(request, cfg.query_timeout_sec);
                return tw::lsf::add_status_fields(tw::lsf::replace_pid(response, getpid()), remote.job_id(), cfg.job_name);
            }
            auto response = remote.request(request, cfg.query_timeout_sec);
            if (parser.parse(request) &&
                (parser.get_string("cmd") == "shutdown" || parser.get_string("cmd") == "stdio.quit"))
                tw::server::g_running = 0;
            return response;
        };
        int rc = tw::server::create_and_run_loop(run_dir.socket_path(), dispatch, server_cfg);
        remote.terminate(cfg.bkill);
        run_dir.cleanup();
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "[vwave-lsf-proxy] ERROR: " << e.what() << "\n";
        remote.terminate(cfg.bkill);
        run_dir.cleanup();
        return 1;
    }
}

inline int entry(int argc, char** argv) {
    if (argc < 12 || std::string(argv[1]) != "--lsf-proxy") return 2;
    try {
        Session session = Session::parse(tw::transport::hex_decode(argv[2]));
        RunDir run_dir = RunDir::from_dir(argv[3]);
        Config cfg;
        cfg.bsub = argv[5];
        cfg.bkill = argv[6];
        cfg.queue = argv[7];
        cfg.resource = argv[8];
        cfg.job_name = argv[9];
        cfg.startup_timeout_sec = std::max(1, std::atoi(argv[10]));
        cfg.query_timeout_sec = std::max(1, std::atoi(argv[11]));
        return run_proxy(session, run_dir, cfg, argv[4]);
    } catch (const std::exception& e) {
        std::cerr << "[vwave-lsf-proxy] ERROR: " << e.what() << "\n";
        return 1;
    }
}

} // namespace lsf
} // namespace wave

#endif
