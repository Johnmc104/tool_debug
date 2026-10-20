#include "server/server_core.h"
#ifdef WAVE_FFR_WORKER
#include "backend/ffr_backend.h"
#else
#include "backend/npi_backend.h"
#endif
#include <memory>
#include <cstdio>

static std::string decode_hex_session(const std::string& encoded) {
    if (encoded.compare(0, 4, "hex:") != 0) return encoded;
    const std::string hex = encoded.substr(4);
    if (hex.size() % 2) throw std::runtime_error("Invalid hex stdio session");
    std::string decoded;
    decoded.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = digit(hex[i]), lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("Invalid hex stdio session");
        decoded.push_back(static_cast<char>((hi << 4) | lo));
    }
    return decoded;
}

int main(int argc, char** argv) {
#ifndef WAVE_FFR_WORKER
    if (argc == 2 && std::string(argv[1]) == "--describe") {
        // NPI 的编译配套信息来自实际 worker，CLI 可以独立重建或仅构建 FFR。
        tw::npi_env::sync_verdi_home(true);
        wave::Session session;
        session.backend = "npi";
        const char* home = getenv("VERDI_HOME");
        if (!home) { std::cerr << "ERROR: NPI requires VERDI_HOME\n"; return 1; }
        session.sdk_home = wave::absolute_path(home);
        auto lib = session.sdk_home + "/share/NPI/lib/linux64/";
        session.reader = wave::absolute_path(lib + "libNPI.so");
        session.support = wave::absolute_path(lib + "libnpiL1.so");
        std::cout << session.json() << std::endl;
        return 0;
    }
#endif
    bool stdio_loop = argc == 3 && std::string(argv[1]) == "--stdio-loop";
    if (!stdio_loop && (argc != 4 ||
        (std::string(argv[1]) != "--serve" && std::string(argv[1]) != "--check"))) {
        std::cerr << "ERROR: Internal worker; use vwave open.\n";
        return 1;
    }
    wave::RunDir run_dir;
    bool serving = std::string(argv[1]) == "--serve";
    int stdio_protocol_fd = -1;
    if (stdio_loop) {
        // Reader constructors may print to stdout before run_stdio_loop() can
        // install its per-call redirection.  Keep that text off the JSONL
        // channel from the first instruction of backend construction.
        stdio_protocol_fd = dup(STDOUT_FILENO);
        if (stdio_protocol_fd < 0) {
            std::cerr << "ERROR: Cannot reserve stdio protocol descriptor\n";
            return 1;
        }
        dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    try {
        auto session = wave::Session::parse(decode_hex_session(argv[2]));
#ifdef WAVE_FFR_WORKER
        if (session.backend != "ffr") throw std::runtime_error("Expected ffr session");
        std::unique_ptr<wave::WaveBackend> backend(new wave::FfrBackend(session));
#else
        if (session.backend != "npi") throw std::runtime_error("Expected npi session");
        std::unique_ptr<wave::WaveBackend> backend(new wave::NpiBackend);
#endif
        if (stdio_loop) {
            std::cout.flush();
            std::fflush(nullptr);
            dup2(stdio_protocol_fd, STDOUT_FILENO);
            close(stdio_protocol_fd);
            stdio_protocol_fd = -1;
            return wave::server::run_stdio_loop(*backend, session);
        }
        if (!serving) return 0;
        // 不向初始化接口传递 CLI 参数；只传工程会话配置。
        run_dir = wave::RunDir(session.file, argv[3]);
        return wave::server::run_server(*backend, session, run_dir);
    } catch (const std::exception& e) {
        if (stdio_protocol_fd >= 0) {
            dup2(stdio_protocol_fd, STDOUT_FILENO);
            close(stdio_protocol_fd);
        }
        std::cerr << "[vwave-server] ERROR: " << e.what() << '\n';
        if (serving && !run_dir.dir().empty()) run_dir.cleanup();
        return 1;
    }
}
