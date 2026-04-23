/**
 * @file run_dir.h
 * @brief vsignal runtime directory — thin wrapper over tw::RunDir.
 *
 * Configures the shared RunDir for vsignal:
 *   sub_dir  = "vsignal_run"   (inside .vtool/)
 *   prefix   = "vsignal_server"
 */
#ifndef VSIGNAL_RUN_DIR_H
#define VSIGNAL_RUN_DIR_H

#include "tw/run_dir.h"

namespace vsignal {

class RunDir {
public:
    static constexpr const char* DOT_DIR = "vsignal_run";
    static constexpr const char* PREFIX  = "vsignal_server";

    RunDir() = default;

    /** Construct from a design source (KDB dir or filelist). */
    RunDir(const std::string& design_src, const std::string& run_dir_override = "")
        : base_(DOT_DIR, PREFIX, design_src, run_dir_override)
    {}

    /** Reconstruct from a known .vtool/vsignal_run directory. */
    static RunDir from_dir(const std::string& run_dir_path) {
        RunDir rd;
        rd.base_ = tw::RunDir::from_dir(run_dir_path, DOT_DIR, PREFIX);
        return rd;
    }

    // ─── Path accessors ──────────────────────────────────────────────────────

    std::string socket_path()       const { return base_.socket_path(); }
    std::string pid_path()          const { return base_.pid_path(); }
    std::string log_path()          const { return base_.log_path(); }
    std::string dir()               const { return base_.dir(); }
    std::string design_source()     const { return base_.source(); }
    std::string design_source_file()const { return base_.source_info_path(); }

    // ─── Operations (delegate) ───────────────────────────────────────────────

    bool   ensure_dir()               const { return base_.ensure_dir(); }
    bool   write_pid(pid_t p)         const { return base_.write_pid(p); }
    pid_t  read_pid()                 const { return base_.read_pid(); }
    void   remove_pid()               const { base_.remove_pid(); }
    bool   write_design_source()      const { return base_.write_source_info(); }
    void   remove_socket()            const { base_.remove_socket(); }
    void   cleanup()                  const { base_.cleanup(); }
    void   cleanup_all()              const { base_.cleanup_all(); }
    bool   is_server_alive()          const { return base_.is_server_alive(); }

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

}  // namespace vsignal

#endif  // VSIGNAL_RUN_DIR_H
