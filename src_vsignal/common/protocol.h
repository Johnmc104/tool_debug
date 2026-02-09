/**
 * @file protocol.h
 * @brief JSON protocol definitions for vsignal_server <-> vsignal_cli communication
 */
#ifndef VSIGNAL_PROTOCOL_H
#define VSIGNAL_PROTOCOL_H

#include <string>
#include <vector>
#include <sstream>

namespace vsignal {

/**
 * Lightweight JSON value for protocol messages.
 */
class JsonObject {
public:
    JsonObject() = default;

    void set(const std::string& key, const std::string& val) {
        entries_.push_back({key, quote(val)});
    }
    void set(const std::string& key, int64_t val) {
        entries_.push_back({key, std::to_string(val)});
    }
    void set_raw(const std::string& key, const std::string& raw_json) {
        entries_.push_back({key, raw_json});
    }
    void set_array(const std::string& key, const std::vector<std::string>& arr) {
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) os << ",";
            os << quote(arr[i]);
        }
        os << "]";
        entries_.push_back({key, os.str()});
    }
    void set_bool(const std::string& key, bool val) {
        entries_.push_back({key, val ? "true" : "false"});
    }

    std::string dump() const {
        std::ostringstream os;
        os << "{";
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i) os << ",";
            os << quote(entries_[i].key) << ":" << entries_[i].value;
        }
        os << "}";
        return os.str();
    }

private:
    struct Entry {
        std::string key;
        std::string value;
    };
    std::vector<Entry> entries_;

    static std::string quote(const std::string& s) {
        std::ostringstream os;
        os << "\"";
        for (char c : s) {
            switch (c) {
                case '"':  os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\n': os << "\\n";  break;
                case '\t': os << "\\t";  break;
                default:   os << c;      break;
            }
        }
        os << "\"";
        return os.str();
    }
};

// ─── Protocol commands ───────────────────────────────────────────────────────

namespace cmd {
    constexpr const char* STATUS       = "status";
    constexpr const char* SHUTDOWN     = "shutdown";
    constexpr const char* INFO         = "info";
    constexpr const char* TRACE_DRIVER = "trace_driver";
    constexpr const char* TRACE_LOAD   = "trace_load";
    constexpr const char* FANIN_REG    = "fanin_reg";
    constexpr const char* FANOUT_REG   = "fanout_reg";
    constexpr const char* TRACE_PATH   = "trace_path";
    constexpr const char* INST_CONN    = "inst_conn";
}

// ─── Error codes ─────────────────────────────────────────────────────────────

namespace err {
    constexpr const char* OK                 = "OK";
    constexpr const char* DESIGN_LOAD_FAILED = "DESIGN_LOAD_FAILED";
    constexpr const char* SIGNAL_NOT_FOUND   = "SIGNAL_NOT_FOUND";
    constexpr const char* INSTANCE_NOT_FOUND = "INSTANCE_NOT_FOUND";
    constexpr const char* NO_PATH            = "NO_PATH";
    constexpr const char* INVALID_PARAMS     = "INVALID_PARAMS";
    constexpr const char* INTERNAL_ERROR     = "INTERNAL_ERROR";
}

// ─── Response helpers ────────────────────────────────────────────────────────

inline std::string make_ok_response(int id, const std::string& data_json) {
    std::ostringstream os;
    os << "{\"id\":" << id
       << ",\"status\":\"ok\""
       << ",\"data\":" << data_json
       << "}";
    return os.str();
}

inline std::string make_error_response(int id, const std::string& code,
                                        const std::string& message) {
    JsonObject err_obj;
    err_obj.set("code", code);
    err_obj.set("message", message);

    std::ostringstream os;
    os << "{\"id\":" << id
       << ",\"status\":\"error\""
       << ",\"error\":" << err_obj.dump()
       << "}";
    return os.str();
}

} // namespace vsignal

#endif // VSIGNAL_PROTOCOL_H
