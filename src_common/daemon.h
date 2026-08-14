/**
 * @file daemon.h
 * @brief Fork-based daemon server launcher shared by vwave/vsignal.
 *
 * Handles: fork → setsid → chdir → redirect I/O → run callback → parent poll.
 */
#ifndef TW_DAEMON_H
#define TW_DAEMON_H

#include <string>
#include <functional>
#include <iostream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "run_dir.h"
#include "client.h"

namespace tw {
namespace daemon {

struct LaunchConfig {
    std::string log_tag;
    int timeout_sec = 30;
    bool json_mode  = false;
};

/**
 * Fork a daemon server process, wait for it to become ready.
 *
 * @param run_dir      Configured RunDir (socket/pid/log paths)
 * @param server_fn    Function to run in the child (returns exit code)
 * @param build_status Builds a status request string for the readiness probe
 * @param send_fn      Sends a request to socket_path, returns response string
 * @param cfg          Launch configuration
 * @return 0 on success, 1 on failure, 2 if still loading at timeout
 */
inline int fork_and_wait(
    const RunDir& run_dir,
    std::function<int()> server_fn,
    std::function<std::string()> build_status,
    std::function<std::string(const std::string&, const std::string&)> send_fn,
    const LaunchConfig& cfg)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        // ── Child: become daemon ──
        setsid();

        if (chdir(run_dir.dir().c_str()) != 0)
            perror("chdir to run_dir");

        int log_fd = open(run_dir.log_path().c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        close(STDIN_FILENO);

        _exit(server_fn());
    }

    // ── Parent: poll for readiness ──
    bool ready = false;
    int max_polls = cfg.timeout_sec * 10;
    for (int i = 0; i < max_polls; ++i) {
        usleep(100000);

        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w > 0) {
            std::cerr << "Error: Server process exited unexpectedly.\n"
                      << "Check log: " << run_dir.log_path() << "\n";
            return 1;
        }

        struct stat sst;
        if (stat(run_dir.socket_path().c_str(), &sst) == 0) {
            std::string resp = send_fn(run_dir.socket_path(), build_status());
            if (!resp.empty() && resp.find("\"ok\"") != std::string::npos) {
                ready = true;
                break;
            }
        }
    }

    if (!ready) {
        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == 0) {
            if (cfg.json_mode) {
                std::cout << "{\"status\":\"loading\",\"message\":\"Server still starting\","
                          << "\"pid\":" << pid
                          << ",\"timeout\":" << cfg.timeout_sec << "}" << std::endl;
            } else {
                std::cerr << "Warning: Server still loading (PID " << pid
                          << ", waited " << cfg.timeout_sec << "s).\n"
                          << "Use '" << cfg.log_tag << " status' to check when ready.\n";
            }
            return 2;
        }
        std::cerr << "Error: Server process exited during startup.\n"
                  << "Check log: " << run_dir.log_path() << "\n";
        return 1;
    }

    if (cfg.json_mode) {
        std::cout << "{\"status\":\"ok\",\"message\":\"Loaded\","
                  << "\"pid\":" << pid
                  << ",\"socket\":\"" << run_dir.socket_path() << "\""
                  << ",\"log\":\"" << run_dir.log_path() << "\"}" << std::endl;
    } else {
        std::cout << "Loaded (PID " << pid << ")\n"
                  << "  Socket: " << run_dir.socket_path() << "\n"
                  << "  Log:    " << run_dir.log_path() << "\n";
    }
    return 0;
}

/**
 * Shutdown a running server, with force-kill fallback.
 */
inline void shutdown_server(
    const RunDir& run_dir,
    std::function<std::string(const std::string&, const std::string&)> send_fn,
    const std::string& shutdown_req)
{
    send_fn(run_dir.socket_path(), shutdown_req);
    for (int i = 0; i < 30; ++i) {
        usleep(100000);
        if (!run_dir.is_server_alive()) return;
    }
    pid_t pid = run_dir.read_pid();
    if (pid > 0) kill(pid, SIGKILL);
    usleep(200000);
}

}  // namespace daemon
}  // namespace tw

#endif  // TW_DAEMON_H
