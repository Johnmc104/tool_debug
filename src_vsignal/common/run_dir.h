/**
 * @file run_dir.h
 * @brief Runtime directory management for vsignal.
 *
 * Layout:
 *   <cwd>/.vsignal_run/
 *   ├── vsignal_server.pid      Server PID
 *   ├── vsignal_server.sock     Unix Domain Socket
 *   ├── vsignal_server.log      Server log (daemon stdout/stderr)
 *   └── design_source           Path to loaded design (KDB dir or filelist)
 */
#ifndef VSIGNAL_RUN_DIR_H
#define VSIGNAL_RUN_DIR_H

#include <string>
#include <sys/stat.h>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <libgen.h>
#include <cstring>
#include <climits>
#include <csignal>

namespace vsignal {

class RunDir {
public:
    RunDir() = default;

    /**
     * Initialize run directory for a design source.
     * @param design_src  Path to KDB dir or filelist
     * @param run_dir     Override run directory (empty = auto: <cwd>/.vsignal_run/)
     */
    RunDir(const std::string& design_src, const std::string& run_dir = "") {
        design_src_ = resolve_path(design_src);
        if (!run_dir.empty()) {
            run_dir_ = run_dir;
        } else {
            char cwd_buf[PATH_MAX];
            if (getcwd(cwd_buf, sizeof(cwd_buf))) {
                run_dir_ = std::string(cwd_buf) + "/.vsignal_run";
            } else {
                run_dir_ = "./.vsignal_run";
            }
        }
    }

    static RunDir from_dir(const std::string& run_dir_path) {
        RunDir rd;
        rd.run_dir_ = run_dir_path;
        rd.design_src_ = read_file_content(rd.design_source_file());
        return rd;
    }

    // ─── Path accessors ──────────────────────────────────────────────────────

    std::string socket_path()       const { return run_dir_ + "/vsignal_server.sock"; }
    std::string pid_path()          const { return run_dir_ + "/vsignal_server.pid"; }
    std::string log_path()          const { return run_dir_ + "/vsignal_server.log"; }
    std::string design_source_file()const { return run_dir_ + "/design_source"; }
    std::string dir()               const { return run_dir_; }
    std::string design_source()     const { return design_src_; }

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

    // ─── Design source persistence ──────────────────────────────────────────

    bool write_design_source() const {
        std::ofstream f(design_source_file());
        if (!f) return false;
        f << design_src_ << std::endl;
        return true;
    }

    // ─── Socket management ──────────────────────────────────────────────────

    void remove_socket() const { unlink(socket_path().c_str()); }

    // ─── Cleanup ────────────────────────────────────────────────────────────

    void cleanup() const {
        remove_pid();
        remove_socket();
        unlink(design_source_file().c_str());
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

    static bool auto_detect(RunDir& out) {
        char cwd_buf[PATH_MAX];
        if (!getcwd(cwd_buf, sizeof(cwd_buf))) return false;

        std::string dir = cwd_buf;
        while (!dir.empty() && dir != "/") {
            std::string candidate = dir + "/.vsignal_run";
            struct stat st;
            if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                RunDir rd = RunDir::from_dir(candidate);
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

private:
    std::string run_dir_;
    std::string design_src_;

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

} // namespace vsignal

#endif // VSIGNAL_RUN_DIR_H
