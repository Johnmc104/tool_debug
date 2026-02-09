/**
 * @file client.h
 * @brief Shared client-side primitives — UDS send/recv, request builder, output.
 *
 * NPI-free.  Used by both vwave and vsignal CLI front-ends.
 */
#ifndef TW_CLIENT_H
#define TW_CLIENT_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>
#include <cerrno>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "json.h"
#include "protocol.h"

namespace tw {
namespace client {

// ─── RAII socket guard ──────────────────────────────────────────────────────

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) ::close(fd_); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int get() const { return fd_; }
    int release() { int f = fd_; fd_ = -1; return f; }
private:
    int fd_;
};

// ─── Socket communication ───────────────────────────────────────────────────

/**
 * Connect to server, send request, return response.
 * @param socket_path  UDS path
 * @param request      JSON request string (without trailing newline)
 * @param timeout_sec  SO_RCVTIMEO / SO_SNDTIMEO in seconds (default 30)
 * @return             Response string, or empty on failure
 */
inline std::string send_request(const std::string& socket_path,
                                const std::string& request,
                                int timeout_sec = 30) {
    ScopedFd sfd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (sfd.get() < 0) {
        perror("socket");
        return "";
    }

    // Timeout
    struct timeval tv;
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(sfd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sfd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        return "";
    }
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sfd.get(), reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        return "";
    }

    // Send request + newline
    std::string data = request + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(sfd.get(), data.c_str() + sent,
                           data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return "";
        }
        sent += static_cast<size_t>(n);
    }

    // Read response up to first '\n'
    std::string response;
    response.reserve(4096);
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(sfd.get(), buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') return response;
            response += buf[i];
        }
        // Safety: cap at 4 MB to prevent OOM from malformed server
        if (response.size() > 4 * 1024 * 1024) break;
    }
    return response;
}

// ─── Request builder ────────────────────────────────────────────────────────

inline std::string build_request(int id, const std::string& cmd,
                                 const std::string& params_json = "{}") {
    std::ostringstream os;
    os << "{\"id\":" << id
       << ",\"cmd\":\"" << cmd << "\""
       << ",\"params\":" << params_json
       << "}";
    return os.str();
}

// ─── Signal list file reader (used by vwave; harmless to include here) ──────

inline std::vector<std::string> read_signal_file(const std::string& path) {
    std::vector<std::string> signals;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "Error: Cannot open signal list file: " << path << "\n";
        return signals;
    }
    std::string line;
    while (std::getline(f, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#') continue;
        signals.push_back(line);
    }
    return signals;
}

// ─── Output formatting ─────────────────────────────────────────────────────

inline void print_response(const std::string& response, bool json_mode) {
    if (response.empty()) return;

    if (json_mode) {
        std::cout << response << std::endl;
        return;
    }

    JsonParser resp;
    if (!resp.parse(response)) {
        std::cout << response << std::endl;
        return;
    }

    std::string status = resp.get_string("status");
    if (status == "error") {
        std::string err_str = resp.get_string("error");
        JsonParser err_obj;
        if (err_obj.parse(err_str)) {
            std::cerr << "Error [" << err_obj.get_string("code") << "]: "
                      << err_obj.get_string("message") << "\n";
        } else {
            std::cerr << "Error: " << err_str << "\n";
        }
        return;
    }

    std::string data_str = resp.get_string("data");
    if (!data_str.empty()) {
        std::cout << data_str << std::endl;
    } else {
        std::cout << response << std::endl;
    }
}

}  // namespace client
}  // namespace tw

#endif  // TW_CLIENT_H
