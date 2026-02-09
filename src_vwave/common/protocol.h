/**
 * @file protocol.h
 * @brief JSON protocol definitions for wave_server <-> wave_cli communication
 */
#ifndef WAVE_PROTOCOL_H
#define WAVE_PROTOCOL_H

#include <string>
#include <vector>
#include <sstream>

// ─── Simple JSON builder/parser (minimal, no external dependency) ────────────

namespace wave {

/**
 * Lightweight JSON value for protocol messages.
 * Supports string, int64, array-of-strings, and nested key-value pairs.
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
    constexpr const char* STATUS      = "status";
    constexpr const char* SHUTDOWN    = "shutdown";
    constexpr const char* FILE_INFO   = "file_info";
    constexpr const char* LIST_SCOPES = "list_scopes";
    constexpr const char* LIST_SIGNALS = "list_signals";
    constexpr const char* SIGNAL_INFO = "signal_info";
    constexpr const char* GET_VALUE_AT = "get_value_at";
    constexpr const char* GET_VALUE_BETWEEN = "get_value_between";
    constexpr const char* FIND_SIGNALS = "find_signals";
    constexpr const char* FIND_VALUE  = "find_value";
    constexpr const char* VC_COUNT    = "vc_count";
}

// ─── Error codes ─────────────────────────────────────────────────────────────

namespace err {
    constexpr const char* OK               = "OK";
    constexpr const char* FSDB_OPEN_FAILED = "FSDB_OPEN_FAILED";
    constexpr const char* SCOPE_NOT_FOUND  = "SCOPE_NOT_FOUND";
    constexpr const char* SIGNAL_NOT_FOUND = "SIGNAL_NOT_FOUND";
    constexpr const char* INVALID_TIME     = "INVALID_TIME";
    constexpr const char* INVALID_PARAMS   = "INVALID_PARAMS";
    constexpr const char* SERVER_BUSY      = "SERVER_BUSY";
    constexpr const char* FILE_READ_ERROR  = "FILE_READ_ERROR";
    constexpr const char* INTERNAL_ERROR   = "INTERNAL_ERROR";
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

} // namespace wave

#endif // WAVE_PROTOCOL_H
