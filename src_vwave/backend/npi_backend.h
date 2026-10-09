#ifndef WAVE_NPI_BACKEND_H
#define WAVE_NPI_BACKEND_H
#include "backend/backend.h"
#include "npi.h"
#include "npi_fsdb.h"
#include "npi_L1.h"
#include "tw/npi_env.h"
#include <unordered_map>

namespace wave {
class NpiBackend : public WaveBackend {
    npiFsdbFileHandle file_ = nullptr;
    bool initialized_ = false;
    char executable_[PATH_MAX]{};
    char* args_[2] = {executable_, nullptr};
    int argc_ = 1;
    char** argv_ = args_;
    std::unordered_map<std::string, npiFsdbSigHandle> cache_;
    npiFsdbSigHandle handle(const std::string& name) {
        auto it = cache_.find(name);
        if (it != cache_.end()) return it->second;
        auto h = npi_fsdb_sig_by_name(file_, name.c_str(), nullptr);
        if (h) cache_[name] = h;
        return h;
    }
    static std::string str(const char* s) { return s ? s : ""; }
    static npiFsdbValType format(const std::string& radix) {
        if (radix == "hex") return npiFsdbHexStrVal;
        if (radix == "oct") return npiFsdbOctStrVal;
        if (radix == "dec") return npiFsdbDecStrVal;
        return npiFsdbBinStrVal;
    }
    static SignalInfo describe(npiFsdbSigHandle h) {
        SignalInfo s;
        s.full_name = str(npi_fsdb_sig_property_str(npiFsdbSigFullName, h));
        s.name = str(npi_fsdb_sig_property_str(npiFsdbSigName, h));
        // T-2022 NPI 首信号名字可能包含损坏的非 ASCII 字节。
        if (s.name.empty() || std::any_of(s.name.begin(), s.name.end(),
            [](unsigned char c) { return c > 127; })) s.name = leaf_name(s.full_name);
        NPI_INT32 left = 0, right = 0, dir = 0;
        npi_fsdb_sig_property(npiFsdbSigLeftRange, h, &left);
        npi_fsdb_sig_property(npiFsdbSigRightRange, h, &right);
        npi_fsdb_sig_property(npiFsdbSigDirection, h, &dir);
        s.left = left; s.right = right;
        s.direction = dir == npiFsdbDirInput ? "input" : dir == npiFsdbDirOutput ? "output" :
                      dir == npiFsdbDirInout ? "inout" : "none";
        return s;
    }
public:
    ~NpiBackend() override {
        if (file_) npi_fsdb_close(file_);
        if (initialized_) npi_end();
    }
    void open(const std::string& path) override {
        tw::npi_env::sync_verdi_home(false);
        auto n = readlink("/proc/self/exe", executable_, sizeof(executable_)-1);
        if (n < 0) throw BackendError("FSDB_OPEN_FAILED", "Cannot locate NPI worker");
        executable_[n] = 0;
        if (!npi_init(argc_, argv_)) {
            tw::npi_env::report_init_failure("vwave-server");
            npi_end();
            throw BackendError("FSDB_OPEN_FAILED", "npi_init failed");
        }
        initialized_ = true;
        file_ = npi_fsdb_open(path.c_str());
        if (!file_) throw BackendError("FSDB_OPEN_FAILED", "Failed to open FSDB: " + path);
    }
    FileInfo info() override {
        FileInfo i;
        npiFsdbTime a = 0, b = 0;
        npi_fsdb_min_time(file_, &a); npi_fsdb_max_time(file_, &b);
        i.min_time = a; i.max_time = b;
        i.scale = str(npi_fsdb_file_property_str(npiFsdbFileScaleUnit, file_));
        i.version = str(npi_fsdb_file_property_str(npiFsdbFileVersion, file_));
        NPI_INT32 completed = 0;
        npi_fsdb_file_property(npiFsdbFileIsCompleted, file_, &completed);
        i.completed = completed;
        return i;
    }
    bool has_scope(const std::string& path) override {
        return npi_fsdb_scope_by_name(file_, path.c_str(), nullptr) != nullptr;
    }
    std::vector<std::string> children(const std::string& path) override {
        auto it = path.empty() ? npi_fsdb_iter_top_scope(file_) :
            npi_fsdb_iter_child_scope(npi_fsdb_scope_by_name(file_, path.c_str(), nullptr));
        std::vector<std::string> result;
        if (it) {
            while (auto scope = npi_fsdb_iter_scope_next(it)) {
                const char* name = npi_fsdb_scope_property_str(npiFsdbScopeFullName, scope);
                if (name) result.emplace_back(name);
            }
            npi_fsdb_iter_scope_stop(it);
        }
        return result;
    }
    std::vector<SignalInfo> signals(const std::string& scope) override {
        auto h = npi_fsdb_scope_by_name(file_, scope.c_str(), nullptr);
        if (!h) throw BackendError("SCOPE_NOT_FOUND", "Scope '" + scope + "' not found");
        std::vector<SignalInfo> result;
        auto it = npi_fsdb_iter_sig(h);
        if (it) {
            while (auto s = npi_fsdb_iter_sig_next(it)) result.push_back(describe(s));
            npi_fsdb_iter_sig_stop(it);
        }
        return result;
    }
    bool signal(const std::string& path, SignalInfo& result) override {
        auto h = handle(path);
        if (!h) return false;
        result = describe(h); return true;
    }
    Value point(const std::string& path, int64_t time, const std::string& radix) override {
        npiFsdbTime t = time;
        std::string value;
        if (!npi_fsdb_sig_value_at(file_, path.c_str(), t, value, format(radix)))
            throw BackendError("FILE_READ_ERROR", "read failed");
        return {static_cast<int64_t>(t), value};
    }
    Range range(const std::string& path, int64_t begin, int64_t end,
                const std::string& radix, size_t limit) override {
        npiFsdbTime a = begin, b = end;
        fsdbTimeValPairVec_t values;
        Range r;
        if (npi_fsdb_sig_value_between(file_, path.c_str(), a, b, values, format(radix))) {
            r.total = values.size();
            for (size_t j = 0; j < std::min(limit, values.size()); ++j)
                r.changes.push_back({static_cast<int64_t>(values[j].first), values[j].second});
        }
        return r;
    }
    Edge edge(const std::string& path, int64_t time, const std::string& kind, bool backward) override {
        Edge e;
        npiFsdbTime t = time, found = 0;
        if (kind == "rising" || kind == "falling") {
            const char* target = kind == "rising" ? "1" : "0";
            int ok = backward ? npi_fsdb_sig_find_value_backward(file_, path.c_str(), target, t, found, npiFsdbBinStrVal) :
                npi_fsdb_sig_find_value_forward(file_, path.c_str(), target, t + 1, found, npiFsdbBinStrVal);
            e.found = ok;
            e.value = {static_cast<int64_t>(found), target};
        } else {
            auto h = npi_fsdb_create_vct(handle(path));
            if (h) {
                npi_fsdb_goto_time(h, t);
                e.found = backward ? npi_fsdb_goto_prev(h) : npi_fsdb_goto_next(h);
                if (e.found) {
                    npi_fsdb_vct_time(h, &found);
                    npiFsdbValue v; v.format = npiFsdbBinStrVal;
                    e.value.time = found;
                    if (npi_fsdb_vct_value(h, &v)) e.value.value = str(v.value.str);
                }
                npi_fsdb_release_vct(h);
            }
        }
        return e;
    }
    int64_t count(const std::string& path, int64_t begin, int64_t end) override {
        return npi_fsdb_sig_vc_count(file_, path.c_str(), begin, end);
    }
};
} // namespace wave
#endif
