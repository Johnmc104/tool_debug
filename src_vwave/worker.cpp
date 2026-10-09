#include "server/server_core.h"
#ifdef WAVE_FFR_WORKER
#include "backend/ffr_backend.h"
#else
#include "backend/npi_backend.h"
#endif
#include <memory>

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
    if (argc != 4 || (std::string(argv[1]) != "--serve" && std::string(argv[1]) != "--check")) {
        std::cerr << "ERROR: Internal worker; use vwave open.\n";
        return 1;
    }
    wave::RunDir run_dir;
    bool serving = std::string(argv[1]) == "--serve";
    try {
        auto session = wave::Session::parse(argv[2]);
#ifdef WAVE_FFR_WORKER
        if (session.backend != "ffr") throw std::runtime_error("Expected ffr session");
        std::unique_ptr<wave::WaveBackend> backend(new wave::FfrBackend(session));
#else
        if (session.backend != "npi") throw std::runtime_error("Expected npi session");
        std::unique_ptr<wave::WaveBackend> backend(new wave::NpiBackend);
#endif
        if (!serving) return 0;
        // 不向初始化接口传递 CLI 参数；只传工程会话配置。
        run_dir = wave::RunDir(session.file, argv[3]);
        return wave::server::run_server(*backend, session, run_dir);
    } catch (const std::exception& e) {
        std::cerr << "[vwave-server] ERROR: " << e.what() << '\n';
        if (serving && !run_dir.dir().empty()) run_dir.cleanup();
        return 1;
    }
}
