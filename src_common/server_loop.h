/**
 * @file server_loop.h
 * @brief Shared server event-loop infrastructure for tool_wave daemons.
 *
 * Provides:
 *   - RAII ScopedFd
 *   - Robust read_line / send_line (EINTR-safe, length-capped)
 *   - ServerConfig: idle timeout (1 h default), log tag, per-client timeout
 *   - run_event_loop(): non-blocking accept loop with idle-timeout auto-shutdown
 *   - signal_handler for clean shutdown
 *
 * Both vwave and vsignal servers delegate their main loop to run_event_loop()
 * and supply a dispatch callback for tool-specific request handling.
 */
#ifndef TW_SERVER_LOOP_H
#define TW_SERVER_LOOP_H

#include <string>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <functional>
#include <iostream>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>

namespace tw {
namespace server {

// ─── RAII fd guard ──────────────────────────────────────────────────────────

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) ::close(fd_); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get()     const { return fd_; }
    int release()       { int f = fd_; fd_ = -1; return f; }
private:
    int fd_;
};

// ─── Shared shutdown flag ───────────────────────────────────────────────────

// Used by the signal handler; the event loop checks this every select() cycle.
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int /*sig*/) {
    g_running = 0;
}

inline void install_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART so select() is interrupted
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Ignore SIGPIPE (broken socket writes)
    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_ign, nullptr);
}

// ─── Robust line I/O ────────────────────────────────────────────────────────

/**
 * Read one newline-terminated line from fd.
 * - Caps at max_len bytes to prevent OOM.
 * - Handles EINTR.
 * - Returns empty string on EOF / error.
 */
inline std::string read_line(int fd, size_t max_len = 4 * 1024 * 1024) {
    std::string line;
    line.reserve(4096);
    char buf[4096];
    while (line.size() < max_len) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') return line;
            line += buf[i];
        }
    }
    return line;
}

/**
 * Send a line (msg + '\n') to fd.  Handles EINTR and partial writes.
 */
inline bool send_line(int fd, const std::string& msg) {
    std::string data = msg + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.c_str() + sent,
                           data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// ─── Server configuration ───────────────────────────────────────────────────

struct ServerConfig {
    std::string log_tag       = "server";      ///< e.g. "vwave-server"
    int  idle_timeout_sec     = 3600;          ///< Auto-shutdown after idle (default 1 h)
    int  client_timeout_sec   = 60;            ///< Per-client recv/send timeout
};

// ─── Event loop ─────────────────────────────────────────────────────────────

using DispatchFn = std::function<std::string(const std::string&)>;

/**
 * Create UDS listener, run select-based event loop until shutdown / idle timeout.
 *
 * @param run_dir_socket  Full path to the UDS socket file (must not exist yet).
 * @param dispatch        Callback: request JSON → response JSON.
 *                        Returning with g_running==0 triggers shutdown.
 * @param cfg             Server configuration.
 * @return 0 on clean exit, 1 on error.
 *
 * The caller is responsible for socket file cleanup.
 */
inline int create_and_run_loop(const std::string& socket_path,
                               DispatchFn dispatch,
                               const ServerConfig& cfg) {
    // ── Create UDS listener ──────────────────────────────────────────────────
    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    ScopedFd guard(listen_fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "[" << cfg.log_tag << "] ERROR: Socket path too long ("
                  << socket_path.size() << " >= " << sizeof(addr.sun_path)
                  << "): " << socket_path << "\n"
                  << "Hint: use --run-dir to specify a shorter path.\n";
        return 1;
    }
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (::listen(listen_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    // Restrict socket to owner
    chmod(socket_path.c_str(), 0600);

    std::cerr << "[" << cfg.log_tag << "] Listening on: " << socket_path << "\n";
    std::cerr << "[" << cfg.log_tag << "] PID: " << getpid() << "\n";
    std::cerr << "[" << cfg.log_tag << "] Idle timeout: "
              << cfg.idle_timeout_sec / 3600 << "h\n";
    std::cerr << "[" << cfg.log_tag << "] Ready.\n";

    // ── Event loop ───────────────────────────────────────────────────────────
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);

    auto last_activity = std::chrono::steady_clock::now();

    while (g_running) {
        // Check idle timeout
        auto now = std::chrono::steady_clock::now();
        auto idle_sec = std::chrono::duration_cast<std::chrono::seconds>(
                            now - last_activity).count();
        if (cfg.idle_timeout_sec > 0 && idle_sec >= cfg.idle_timeout_sec) {
            std::cerr << "[" << cfg.log_tag
                      << "] Idle timeout (" << cfg.idle_timeout_sec / 3600
                      << "h) reached — auto-shutdown.\n";
            break;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);

        struct timeval tv;
        tv.tv_sec  = 1;    // wake every 1 s to check g_running / timeout
        tv.tv_usec = 0;

        int ret = ::select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;  // timeout — loop to check g_running

        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        ScopedFd client_guard(client_fd);

        // Per-client timeout
        struct timeval ctv;
        ctv.tv_sec  = cfg.client_timeout_sec;
        ctv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

        std::string request = read_line(client_fd);
        if (!request.empty()) {
            last_activity = std::chrono::steady_clock::now();
            std::string response = dispatch(request);
            send_line(client_fd, response);
        }
    }

    std::cerr << "[" << cfg.log_tag << "] Shutting down...\n";
    return 0;
}

}  // namespace server
}  // namespace tw

#endif  // TW_SERVER_LOOP_H
