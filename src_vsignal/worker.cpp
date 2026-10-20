#include "server/server_core.h"
#include "common/session.h"
#include "common/run_dir.h"
#include "tw/transport.h"

#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    bool stdio_loop = argc == 3 && std::string(argv[1]) == "--stdio-loop";
    bool serving = argc == 4 && std::string(argv[1]) == "--serve";
    if (!stdio_loop && !serving) {
        std::cerr << "ERROR: Internal vsignal worker; use vsignal open.\n";
        return 1;
    }
    try {
        auto session = vsignal::DesignSession::parse(tw::transport::hex_decode(argv[2]));
        tw::npi_env::sync_verdi_home(true);
        tw::npi_env::ensure_npi_lib_path();
        if (stdio_loop)
            return vsignal::server::run_stdio_loop(argc, argv, session.source, session.args);

        vsignal::RunDir run_dir(session.source, argv[3]);
        return vsignal::server::run_server(argc, argv, run_dir, session.source, session.args);
    } catch (const std::exception& error) {
        std::cerr << "[vsignal-worker] ERROR: " << error.what() << "\n";
        return 1;
    }
}
