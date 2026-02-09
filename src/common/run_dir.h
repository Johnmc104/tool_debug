/**
 * @file run_dir.h
 * @brief Runtime directory management for vwave.
 *
 * Layout:
 *   <cwd>/.wave_run/
 *   ├── wave_server.pid      Server PID
 *   ├── wave_server.sock     Unix Domain Socket
 *   ├── wave_server.log      Server log (daemon stdout/stderr)
 *   └── fsdb_path            Absolute path to the loaded FSDB file
 *
 * The .wave_run/ directory is always created under the current working
 * directory (CWD) of the `vwave open` command.  This avoids write-permission
 * issues when the FSDB resides on a shared or read-only filesystem.
 */
#ifndef WAVE_RUN_DIR_H
#define WAVE_RUN_DIR_H

#include <string>
#include <sys/stat.h>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <libgen.h>
#include <cstring>
#include <climits>
#include <csignal>

namespace wave {

class RunDir {
public:
    /**
     * Initialize run directory from an FSDB path.
     * @param fsdb_path  Path to the FSDB file
     * @param run_dir    Override run directory (empty = auto: <cwd>/.wave_run/)
     *
     * Default: .wave_run/ is created under CWD so that users with read-only
     * access to the FSDB directory can still operate without conflicts.
     */
    RunDir(const std::string& fsdb_path, const std::string& run_dir = "") {
        fsdb_abs_ = resolve_path(fsdb_path);
        if (!run_dir.empty()) {
            run_dir_ = run_dir;
        } else {
            char cwd_buf[PATH_MAX];
            if (getcwd(cwd_buf, sizeof(cwd_buf))) {
                run_dir_ = std::string(cwd_buf) + "/.wave_run";
            } else {
                // Fallback to FSDB directory if CWD is unavailable
                char* tmp = strdup(fsdb_abs_.c_str());
                std::string dir = dirname(tmp);
                free(tmp);
                run_dir_ = dir + "/.wave_run";
            }
        }
    }

    /**
     * Initialize from a known .wave_run directory path (used by auto-detect).
     * Reads stored FSDB path from the fsdb_path file inside the directory.
     */
    static RunDir from_dir(const std::string& run_dir_path) {
        RunDir rd;
        rd.run_dir_ = run_dir_path;
        rd.fsdb_abs_ = read_file_content(rd.fsdb_path_file());
        return rd;
    }

    // ─── Path accessors ──────────────────────────────────────────────────────

    std::string socket_path()    const { return run_dir_ + "/wave_server.sock"; }
    std::string pid_path()       const { return run_dir_ + "/wave_server.pid"; }
    std::string log_path()       const { return run_dir_ + "/wave_server.log"; }
    std::string fsdb_path_file() const { return run_dir_ + "/fsdb_path"; }
    std::string dir()            const { return run_dir_; }
    std::string fsdb_path()      const { return fsdb_abs_; }

    // ─── Directory creation ──────────────────────────────────────────────────

    bool ensure_dir() const {
        return mkdir(run_dir_.c_str(), 0755) == 0 || errno == EEXIST;
    }

    // ─── PID management ─────────────────────────────────────────────────────

    bool write_pid(pid_t pid) const {
        std::ofstream f(pid_path());
        if (!f) return false;
        f << pid << std::endl;
        return true;
    }

    pid_t read_pid() const {
        std::ifstream f(pid_path());
        pid_t pid = 0;
        if (f) f >> pid;
        return pid;
    }

    void remove_pid() const { unlink(pid_path().c_str()); }

    // ─── FSDB path persistence ──────────────────────────────────────────────

    bool write_fsdb_path() const {
        std::ofstream f(fsdb_path_file());
        if (!f) return false;
        f << fsdb_abs_ << std::endl;
        return true;
    }

    // ─── Socket management ──────────────────────────────────────────────────

    void remove_socket() const { unlink(socket_path().c_str()); }

    // ─── Cleanup ────────────────────────────────────────────────────────────

    void cleanup() const {
        remove_pid();
        remove_socket();
        unlink(fsdb_path_file().c_str());
        // Don't remove log — keep for debugging
    }

    void cleanup_all() const {
        cleanup();
        unlink(log_path().c_str());
        rmdir(run_dir_.c_str());  // only succeeds if empty
    }

    // ─── Server alive check ─────────────────────────────────────────────────

    bool is_server_alive() const {
        pid_t pid = read_pid();
        if (pid <= 0) return false;
        return kill(pid, 0) == 0;
    }

    // ─── Auto-detect ────────────────────────────────────────────────────────

    /**
     * Search upward from CWD for a `.wave_run/` directory with a live server.
     * Returns true and fills `out` if a running server is found.
     */
    static bool auto_detect(RunDir& out) {
        char cwd_buf[PATH_MAX];
        if (!getcwd(cwd_buf, sizeof(cwd_buf))) return false;

        std::string dir = cwd_buf;
        while (!dir.empty() && dir != "/") {
            std::string candidate = dir + "/.wave_run";
            struct stat st;
            if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                RunDir rd = RunDir::from_dir(candidate);
                if (rd.is_server_alive()) {
                    out = rd;
                    return true;
                }
            }
            // Go up one level
            char* tmp = strdup(dir.c_str());
            std::string parent = dirname(tmp);
            free(tmp);
            if (parent == dir) break;  // reached root
            dir = parent;
        }
        return false;
    }

private:
    RunDir() = default;
    std::string run_dir_;
    std::string fsdb_abs_;

    static std::string resolve_path(const std::string& path) {
        char resolved[PATH_MAX];
        if (realpath(path.c_str(), resolved)) return resolved;
        return path;
    }

    static std::string read_file_content(const std::string& path) {
        std::ifstream f(path);
        std::string content;
        if (f) std::getline(f, content);
        while (!content.empty() &&
               (content.back() == '\n' || content.back() == '\r' || content.back() == ' '))
            content.pop_back();
        return content;
    }
};

} // namespace wave

#endif // WAVE_RUN_DIR_H
