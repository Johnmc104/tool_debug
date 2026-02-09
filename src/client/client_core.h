/**
 * @file client_core.h
 * @brief vwave client core — socket communication and request building.
 *
 * This module is NPI-free and handles:
 *   - Unix domain socket connection
 *   - JSON request construction
 *   - Signal list file reading
 *   - Output formatting
 */
#ifndef WAVE_CLIENT_CORE_H
#define WAVE_CLIENT_CORE_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/protocol.h"
#include "common/json_parser.h"

namespace wave {
namespace client {

// ─── Socket communication ────────────────────────────────────────────────────

inline std::string send_request(const std::string& socket_path,
                                const std::string& request) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return "";
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    std::string data = request + "\n";
    send(fd, data.c_str(), data.size(), 0);

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        response += buf;
        if (response.find('\n') != std::string::npos) break;
    }
    close(fd);

    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();
    return response;
}

// ─── Request builder ─────────────────────────────────────────────────────────

inline std::string build_request(int id, const std::string& cmd,
                                  const std::string& params_json = "{}") {
    std::ostringstream os;
    os << "{\"id\":" << id
       << ",\"cmd\":\"" << cmd << "\""
       << ",\"params\":" << params_json
       << "}";
    return os.str();
}

// ─── Signal list file reader ─────────────────────────────────────────────────

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

// ─── Output formatting ──────────────────────────────────────────────────────

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

} // namespace client
} // namespace wave

#endif // WAVE_CLIENT_CORE_H
