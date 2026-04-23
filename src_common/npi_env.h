/**
 * @file npi_env.h
 * @brief Auto-configure NPI runtime environment from VERDI_HOME.
 *
 * Call tw::ensure_npi_env(argc, argv) at the very start of main().
 * It sets NOVAS_HOME, LD_LIBRARY_PATH, and re-execs the process if
 * the library path was changed (the dynamic linker only reads
 * LD_LIBRARY_PATH at process startup).
 *
 * After this call, the NPI shared libraries are guaranteed to be
 * discoverable — users only need to set VERDI_HOME.
 */
#ifndef TW_NPI_ENV_H
#define TW_NPI_ENV_H

#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace tw {

/**
 * Ensure NPI runtime environment is properly configured from VERDI_HOME.
 *
 * 1. Reads VERDI_HOME (falls back to NOVAS_HOME).
 * 2. Sets NOVAS_HOME = VERDI_HOME if not already set.
 * 3. Prepends NPI lib dir to LD_LIBRARY_PATH if not already present.
 * 4. Re-execs the process so the dynamic linker picks up the new path.
 *
 * A sentinel env var (_TW_NPI_ENV_SET) prevents infinite re-exec.
 *
 * @param argc  main()'s argc
 * @param argv  main()'s argv (used by execv on re-exec)
 */
inline void ensure_npi_env(int /*argc*/, char** argv) {
    // ── Guard against infinite re-exec ───────────────────────────────────────
    const char* sentinel = std::getenv("_TW_NPI_ENV_SET");
    if (sentinel && sentinel[0] == '1') return;

    // ── Resolve VERDI_HOME ───────────────────────────────────────────────────
    const char* verdi_home = std::getenv("VERDI_HOME");
    if (!verdi_home || verdi_home[0] == '\0') {
        verdi_home = std::getenv("NOVAS_HOME");
        if (!verdi_home || verdi_home[0] == '\0') return;
    }

    std::string npi_lib = std::string(verdi_home) + "/share/NPI/lib/linux64";

    // ── Set NOVAS_HOME (NPI internals need it) ───────────────────────────────
    setenv("NOVAS_HOME", verdi_home, 0);  // don't overwrite if already set

    // ── Check if LD_LIBRARY_PATH already contains the NPI lib dir ────────────
    const char* ld_path = std::getenv("LD_LIBRARY_PATH");
    std::string ld_str = ld_path ? ld_path : "";

    if (ld_str.find(npi_lib) != std::string::npos) {
        // Already configured — mark sentinel and continue
        setenv("_TW_NPI_ENV_SET", "1", 1);
        return;
    }

    // ── Prepend NPI lib dir ──────────────────────────────────────────────────
    std::string new_ld = npi_lib;
    if (!ld_str.empty()) new_ld += ":" + ld_str;
    setenv("LD_LIBRARY_PATH", new_ld.c_str(), 1);
    setenv("_TW_NPI_ENV_SET", "1", 1);

    // ── Re-exec so the dynamic linker uses the updated LD_LIBRARY_PATH ──────
    // Prefer /proc/self/exe for a reliable absolute path on Linux.
    execv("/proc/self/exe", argv);
    // Fallback: try argv[0] (works if it's an absolute path or on PATH)
    execv(argv[0], argv);
    // If both fail, continue anyway — compile-time rpath may still work.
}

}  // namespace tw

#endif  // TW_NPI_ENV_H
