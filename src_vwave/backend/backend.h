#ifndef WAVE_BACKEND_H
#define WAVE_BACKEND_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace wave {

struct BackendError : std::runtime_error {
    std::string code;
    BackendError(const std::string& c, const std::string& message)
        : std::runtime_error(message), code(c) {}
};
struct FileInfo {
    int64_t min_time = 0, max_time = 0;
    std::string scale, version;
    bool completed = false;
};
struct SignalInfo {
    std::string name, full_name, direction;
    int left = 0, right = 0;
};
struct Value {
    int64_t time = 0;
    std::string value;
};
struct Range {
    std::vector<Value> changes;
    int64_t total = 0;
};
struct Edge {
    bool found = false;
    Value value;
};

inline std::string leaf_name(const std::string& full) {
    auto dot = full.rfind('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}
inline bool wildcard_match(const char* pattern, const char* text) {
    const char *star = nullptr, *retry = nullptr;
    while (*text) {
        if (*pattern == '?' || *pattern == *text) { ++pattern; ++text; }
        else if (*pattern == '*') { star = pattern++; retry = text; }
        else if (star) { pattern = star + 1; text = ++retry; }
        else return false;
    }
    while (*pattern == '*') ++pattern;
    return *pattern == 0;
}

// 工程内部接口；所有厂商句柄仅存在于各自实现中。
class WaveBackend {
public:
    virtual ~WaveBackend() = default;
    virtual void open(const std::string& path) = 0;
    virtual FileInfo info() = 0;
    virtual bool has_scope(const std::string& path) = 0;
    virtual std::vector<std::string> children(const std::string& path) = 0;
    virtual std::vector<SignalInfo> signals(const std::string& scope) = 0;
    virtual bool signal(const std::string& path, SignalInfo& result) = 0;
    virtual Value point(const std::string& path, int64_t time, const std::string& radix) = 0;
    virtual Range range(const std::string& path, int64_t begin, int64_t end,
                        const std::string& radix, size_t limit) = 0;
    virtual Edge edge(const std::string& path, int64_t time,
                      const std::string& kind, bool backward) = 0;
    virtual int64_t count(const std::string& path, int64_t begin, int64_t end) = 0;

    std::vector<std::string> scopes(const std::string& path, int depth) {
        if (!path.empty() && !has_scope(path))
            throw BackendError("SCOPE_NOT_FOUND", "Scope '" + path + "' not found");
        std::vector<std::string> result;
        std::function<void(const std::string&, int)> walk = [&](const std::string& parent, int level) {
            for (const auto& child : children(parent)) {
                result.push_back(child);
                if (level < depth) walk(child, level + 1);
                if (!parent.empty() && result.size() >= 500) break;
            }
        };
        walk(path, 1);
        return result;
    }
    std::vector<std::string> find(const std::string& scope, const std::string& pattern) {
        if (!scope.empty() && !has_scope(scope))
            throw BackendError("SCOPE_NOT_FOUND", "Scope '" + scope + "' not found");
        std::vector<std::string> result;
        std::function<void(const std::string&, int)> walk = [&](const std::string& parent, int level) {
            if (level > 20 || result.size() >= 200) return;
            for (const auto& sig : signals(parent)) {
                if (wildcard_match(pattern.c_str(), sig.name.c_str())) result.push_back(sig.full_name);
                if (result.size() >= 200) return;
            }
            for (const auto& child : children(parent)) walk(child, level + 1);
        };
        if (scope.empty()) {
            for (const auto& top : children("")) walk(top, 0);
        } else walk(scope, 0);
        return result;
    }
};

} // namespace wave
#endif
