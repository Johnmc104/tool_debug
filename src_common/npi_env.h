/**
 * @file npi_env.h
 * @brief Verdi/NPI runtime environment checks shared by vwave/vsignal.
 *
 * - Keeps VERDI_HOME consistent with the Verdi version the binary was built
 *   against (NPI resource files must match the loaded libNPI.so).
 * - Prints actionable hints when npi_init() fails (usually license checkout).
 */
#ifndef TW_NPI_ENV_H
#define TW_NPI_ENV_H

#include <string>
#include <iostream>
#include <cstdlib>

#include <sys/stat.h>

// Injected by Makefile: -DTW_BUILD_VERDI_HOME="\"$(VERDI_HOME)\""
#ifndef TW_BUILD_VERDI_HOME
#define TW_BUILD_VERDI_HOME ""
#endif

namespace tw {
namespace npi_env {

inline const char* build_verdi_home() { return TW_BUILD_VERDI_HOME; }

inline bool dir_exists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// Align VERDI_HOME with the build-time Verdi install (call before fork).
/// - Build-time path exists: libNPI.so is pinned to it via RPATH, so force
///   VERDI_HOME to match (warn if the user's value differs).
/// - Otherwise (binary deployed elsewhere): keep the user's VERDI_HOME.
inline void sync_verdi_home(bool quiet) {
    std::string built = build_verdi_home();
    const char* cur_c = std::getenv("VERDI_HOME");
    std::string cur   = cur_c ? cur_c : "";

    if (dir_exists(built)) {
        if (!cur.empty() && cur != built && !quiet) {
            std::cerr << "Warning: VERDI_HOME (" << cur << ") differs from build-time Verdi ("
                      << built << "); using build-time path.\n";
        }
        if (cur != built) setenv("VERDI_HOME", built.c_str(), 1);
        return;
    }
    if (cur.empty() && !quiet) {
        std::cerr << "Warning: VERDI_HOME not set. NPI libraries may not be found.\n"
                  << "Hint: module load synopsys/verdi/<version>\n";
    }
}

/// Prepend $VERDI_HOME NPI/platform lib dirs to LD_LIBRARY_PATH
/// (for helper processes spawned by NPI). Call after sync_verdi_home().
inline void ensure_npi_lib_path() {
    const char* verdi_home = std::getenv("VERDI_HOME");
    if (!verdi_home) return;
    std::string extra = std::string(verdi_home) + "/platform/linux64/bin";
    std::string cur = std::getenv("LD_LIBRARY_PATH") ? std::getenv("LD_LIBRARY_PATH") : "";
    if (cur.find(extra) == std::string::npos) {
        std::string npi_lib = std::string(verdi_home) + "/share/NPI/lib/linux64";
        std::string newpath = npi_lib + ":" + extra;
        if (!cur.empty()) newpath += ":" + cur;
        setenv("LD_LIBRARY_PATH", newpath.c_str(), 1);
    }
}

/// Print diagnostics for npi_init() failure to the server log.
inline void report_init_failure(const std::string& tag) {
    std::cerr << "[" << tag << "] ERROR: npi_init failed (Verdi license checkout failed?)\n"
              << "[" << tag << "] Hint: check SNPSLMD_LICENSE_FILE / LM_LICENSE_FILE\n"
              << "[" << tag << "] Hint: Verdi 2026.03+ checks out feature 'VerdiNPI'; "
                 "if unavailable, export NPI_LICENSE=Verdi\n";
}

}  // namespace npi_env
}  // namespace tw

#endif  // TW_NPI_ENV_H
