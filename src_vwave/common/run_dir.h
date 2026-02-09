/**
 * @file run_dir.h
 * @brief vwave runtime directory — thin wrapper over tw::RunDir.
 *
 * Configures the shared RunDir for vwave:
 *   dot_dir  = ".wave_run"
 *   prefix   = "wave_server"
 */
#ifndef WAVE_RUN_DIR_H
#define WAVE_RUN_DIR_H

#include "tw/run_dir.h"

namespace wave {

class RunDir {
public:
    static constexpr const char* DOT_DIR = ".wave_run";
    static constexpr const char* PREFIX  = "wave_server";

    RunDir() = default;

    /** Construct from an FSDB path (used by `vwave open`). */
    RunDir(const std::string& fsdb_path, const std::string& run_dir_override = "")
        : base_(DOT_DIR, PREFIX, fsdb_path, run_dir_override)
    {}

    /** Reconstruct from a known .wave_run directory (used by auto-detect). */
    static RunDir from_dir(const std::string& run_dir_path) {
        RunDir rd;
        rd.base_ = tw::RunDir::from_dir(run_dir_path, DOT_DIR, PREFIX);
        return rd;
    }

    // ─── Path accessors ──────────────────────────────────────────────────────

    std::string socket_path()    const { return base_.socket_path(); }
    std::string pid_path()       const { return base_.pid_path(); }
    std::string log_path()       const { return base_.log_path(); }
    std::string dir()            const { return base_.dir(); }
    std::string fsdb_path()      const { return base_.source(); }

    // Legacy alias kept for backward compat with run_dir.write_fsdb_path()
    std::string fsdb_path_file() const { return base_.source_info_path(); }

    // ─── Operations (delegate) ───────────────────────────────────────────────

    bool   ensure_dir()      const { return base_.ensure_dir(); }
    bool   write_pid(pid_t p)const { return base_.write_pid(p); }
    pid_t  read_pid()        const { return base_.read_pid(); }
    void   remove_pid()      const { base_.remove_pid(); }
    bool   write_fsdb_path() const { return base_.write_source_info(); }
    void   remove_socket()   const { base_.remove_socket(); }
    void   cleanup()         const { base_.cleanup(); }
    void   cleanup_all()     const { base_.cleanup_all(); }
    bool   is_server_alive() const { return base_.is_server_alive(); }

    // ─── Auto-detect ─────────────────────────────────────────────────────────

    static bool auto_detect(RunDir& out) {
        tw::RunDir base;
        if (tw::RunDir::auto_detect(base, DOT_DIR, PREFIX)) {
            out.base_ = base;
            return true;
        }
        return false;
    }

private:
    tw::RunDir base_;
};

}  // namespace wave

#endif  // WAVE_RUN_DIR_H
