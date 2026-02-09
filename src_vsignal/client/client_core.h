/**
 * @file client_core.h
 * @brief vsignal client core — socket communication and output formatting.
 *
 * This module is NPI-free and handles:
 *   - Unix domain socket connection
 *   - JSON request construction
 *   - Output formatting for trace results
 */
#ifndef VSIGNAL_CLIENT_CORE_H
#define VSIGNAL_CLIENT_CORE_H

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/protocol.h"
#include "common/json_parser.h"

namespace vsignal {
namespace client {

// ─── Socket communication ────────────────────────────────────────────────────

inline std::string send_request(const std::string& socket_path,
                                const std::string& request) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return "";
    }

    // Set socket timeout to avoid blocking indefinitely
    struct timeval tv;
    tv.tv_sec = 30;  // netlist tracing can be slow for large designs
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return "";
    }
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    std::string data = request + "\n";
    send(fd, data.c_str(), data.size(), 0);

    // Read response up to first '\n'
    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') goto done;
            response += buf[i];
        }
    }
done:
    close(fd);
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
} // namespace vsignal

#endif // VSIGNAL_CLIENT_CORE_H
