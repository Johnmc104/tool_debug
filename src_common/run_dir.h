/**
 * @file run_dir.h
 * @brief Parameterized runtime directory management for tool_wave tools.
 *
 * All tools share a common parent directory `.vtool/` under CWD, with
 * each tool owning a sub-directory inside it:
 *   - vwave   → ".vtool/wave_run",     prefix="wave_server"
 *   - vsignal → ".vtool/vsignal_run",  prefix="vsignal_server"
 *
 * Layout:
 *   <cwd>/.vtool/<sub_dir>/
 *   ├── <prefix>.pid       Server PID
 *   ├── <prefix>.sock      Unix Domain Socket
 *   ├── <prefix>.log       Server log
 *   └── source_info        Tool-specific source path (FSDB / KDB dir / etc.)
 */
#ifndef TW_RUN_DIR_H
#define TW_RUN_DIR_H

#include <string>
#include <fstream>
#include <cstring>
#include <climits>
#include <csignal>

#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

namespace tw {

class RunDir {
public:
    static constexpr const char* VTOOL_DIR = ".vtool";

    RunDir() = default;

    /**
     * Construct a RunDir for a given source path.
     * @param sub_dir_name  Name of the sub-directory under .vtool (e.g. "wave_run")
     * @param file_prefix   Prefix for pid/sock/log files (e.g. "wave_server")
     * @param source_path   Path to the tool-specific source (FSDB / KDB dir)
     * @param run_dir_override  If non-empty, use this directory instead of <cwd>/.vtool/<sub_dir>
     */
    RunDir(const std::string& sub_dir_name,
           const std::string& file_prefix,
           const std::string& source_path,
           const std::string& run_dir_override = "")
        : sub_dir_name_(sub_dir_name)
        , file_prefix_(file_prefix)
        , source_(resolve_path(source_path))
    {
        if (!run_dir_override.empty()) {
            run_dir_ = run_dir_override;
        } else {
            char cwd_buf[PATH_MAX];
            if (getcwd(cwd_buf, sizeof(cwd_buf)))
                run_dir_ = std::string(cwd_buf) + "/" + VTOOL_DIR + "/" + sub_dir_name;
            else
                run_dir_ = std::string("./") + VTOOL_DIR + "/" + sub_dir_name;
        }
    }

    /**
     * Construct from an existing directory (for auto-detect).
     */
    static RunDir from_dir(const std::string& run_dir_path,
                           const std::string& sub_dir_name,
                           const std::string& file_prefix) {
        RunDir rd;
        rd.sub_dir_name_ = sub_dir_name;
        rd.file_prefix_  = file_prefix;
        rd.run_dir_      = run_dir_path;
        rd.source_       = read_file_content(rd.source_info_path());
        return rd;
    }

    // ─── Path accessors ──────────────────────────────────────────────────────

    std::string socket_path()      const { return run_dir_ + "/" + file_prefix_ + ".sock"; }
    std::string pid_path()         const { return run_dir_ + "/" + file_prefix_ + ".pid"; }
    std::string log_path()         const { return run_dir_ + "/" + file_prefix_ + ".log"; }
    std::string source_info_path() const { return run_dir_ + "/source_info"; }
    std::string dir()              const { return run_dir_; }
    std::string source()           const { return source_; }

    // ─── Directory creation ──────────────────────────────────────────────────

    bool ensure_dir() const {
        // Create .vtool/ parent directory first
        std::string::size_type pos = run_dir_.rfind('/');
        if (pos != std::string::npos) {
            std::string parent = run_dir_.substr(0, pos);
            mkdir(parent.c_str(), 0755);  // OK if already exists
        }
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

    // ─── Source info persistence ─────────────────────────────────────────────

    bool write_source_info() const {
        std::ofstream f(source_info_path());
        if (!f) return false;
        f << source_ << std::endl;
        return true;
    }

    // ─── Socket management ──────────────────────────────────────────────────

    void remove_socket() const { unlink(socket_path().c_str()); }

    // ─── Cleanup ────────────────────────────────────────────────────────────

    void cleanup() const {
        remove_pid();
        remove_socket();
        unlink(source_info_path().c_str());
    }

    void cleanup_all() const {
        cleanup();
        unlink(log_path().c_str());
        rmdir(run_dir_.c_str());
    }

    // ─── Server alive check ─────────────────────────────────────────────────

    bool is_server_alive() const {
        pid_t pid = read_pid();
        if (pid <= 0) return false;
        return kill(pid, 0) == 0;
    }

    // ─── Auto-detect ────────────────────────────────────────────────────────

    /**
     * Search upward from CWD for a .vtool/<sub_dir>/ directory with a live server.
     */
    static bool auto_detect(RunDir& out,
                            const std::string& sub_dir_name,
                            const std::string& file_prefix) {
        char cwd_buf[PATH_MAX];
        if (!getcwd(cwd_buf, sizeof(cwd_buf))) return false;

        std::string dir = cwd_buf;
        while (!dir.empty() && dir != "/") {
            std::string candidate = dir + "/" + VTOOL_DIR + "/" + sub_dir_name;
            struct stat st;
            if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                RunDir rd = RunDir::from_dir(candidate, sub_dir_name, file_prefix);
                if (rd.is_server_alive()) {
                    out = rd;
                    return true;
                }
            }
            char* tmp = strdup(dir.c_str());
            std::string parent = dirname(tmp);
            free(tmp);
            if (parent == dir) break;
            dir = parent;
        }
        return false;
    }

    // ─── Utilities ──────────────────────────────────────────────────────────

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
               (content.back() == '\n' || content.back() == '\r' ||
                content.back() == ' '))
            content.pop_back();
        return content;
    }

private:
    std::string sub_dir_name_;
    std::string file_prefix_;
    std::string run_dir_;
    std::string source_;
};

}  // namespace tw

#endif  // TW_RUN_DIR_H
