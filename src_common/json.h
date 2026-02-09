/**
 * @file json.h
 * @brief Unified minimal JSON builder + parser for tool_wave protocol messages.
 *
 * Shared by vwave and vsignal — no external dependencies.
 *
 * JsonObject:  Build JSON objects incrementally (string, int64, bool, raw, array).
 * JsonParser:  Parse flat JSON objects with string/int/bool/array values.
 */
#ifndef TW_JSON_H
#define TW_JSON_H

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdlib>
#include <cctype>
#include <cstdint>

namespace tw {

// ═══════════════════════════════════════════════════════════════════════════════
//  JsonObject — lightweight JSON builder
// ═══════════════════════════════════════════════════════════════════════════════

class JsonObject {
public:
    JsonObject() = default;

    void set(const std::string& key, const std::string& val) {
        entries_.push_back({key, quote(val)});
    }
    void set(const std::string& key, int64_t val) {
        entries_.push_back({key, std::to_string(val)});
    }
    void set_bool(const std::string& key, bool val) {
        entries_.push_back({key, val ? "true" : "false"});
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

// ═══════════════════════════════════════════════════════════════════════════════
//  JsonParser — minimal read-only JSON parser for protocol messages
// ═══════════════════════════════════════════════════════════════════════════════

class JsonParser {
public:
    bool parse(const std::string& json) {
        str_ = json;
        pos_ = 0;
        strings_.clear();
        ints_.clear();
        arrays_.clear();
        return parse_object();
    }

    bool has(const std::string& key) const {
        return strings_.count(key) || ints_.count(key) || arrays_.count(key);
    }

    std::string get_string(const std::string& key,
                           const std::string& def = "") const {
        auto it = strings_.find(key);
        if (it != strings_.end()) return it->second;
        auto it2 = ints_.find(key);
        if (it2 != ints_.end()) return std::to_string(it2->second);
        return def;
    }

    int64_t get_int(const std::string& key, int64_t def = 0) const {
        auto it = ints_.find(key);
        if (it != ints_.end()) return it->second;
        auto it2 = strings_.find(key);
        if (it2 != strings_.end())
            return std::strtoll(it2->second.c_str(), nullptr, 10);
        return def;
    }

    bool get_bool(const std::string& key, bool def = false) const {
        auto it = strings_.find(key);
        if (it != strings_.end()) return it->second == "true";
        auto it2 = ints_.find(key);
        if (it2 != ints_.end()) return it2->second != 0;
        return def;
    }

    std::vector<std::string> get_array(const std::string& key) const {
        auto it = arrays_.find(key);
        if (it != arrays_.end()) return it->second;
        return {};
    }

private:
    std::string str_;
    size_t pos_ = 0;

    std::map<std::string, std::string>              strings_;
    std::map<std::string, int64_t>                  ints_;
    std::map<std::string, std::vector<std::string>> arrays_;

    void skip_ws() {
        while (pos_ < str_.size() &&
               std::isspace(static_cast<unsigned char>(str_[pos_])))
            pos_++;
    }

    bool expect(char c) {
        skip_ws();
        if (pos_ < str_.size() && str_[pos_] == c) { pos_++; return true; }
        return false;
    }

    std::string parse_string_val() {
        skip_ws();
        if (pos_ >= str_.size() || str_[pos_] != '"') return "";
        pos_++;
        std::string result;
        while (pos_ < str_.size() && str_[pos_] != '"') {
            if (str_[pos_] == '\\' && pos_ + 1 < str_.size()) {
                pos_++;
                switch (str_[pos_]) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    default:   result += str_[pos_]; break;
                }
            } else {
                result += str_[pos_];
            }
            pos_++;
        }
        if (pos_ < str_.size()) pos_++;
        return result;
    }

    bool parse_object() {
        if (!expect('{')) return false;
        skip_ws();
        if (pos_ < str_.size() && str_[pos_] == '}') { pos_++; return true; }

        while (true) {
            std::string key = parse_string_val();
            if (key.empty()) return false;
            if (!expect(':')) return false;
            skip_ws();

            if (pos_ < str_.size() && str_[pos_] == '"') {
                strings_[key] = parse_string_val();
            } else if (pos_ < str_.size() && str_[pos_] == '[') {
                arrays_[key] = parse_array();
            } else if (pos_ < str_.size() && str_[pos_] == '{') {
                size_t start = pos_;
                skip_nested_object();
                strings_[key] = str_.substr(start, pos_ - start);
            } else if (pos_ < str_.size() &&
                       (str_.substr(pos_, 4) == "true" ||
                        str_.substr(pos_, 5) == "false")) {
                bool val = (str_[pos_] == 't');
                pos_ += val ? 4 : 5;
                strings_[key] = val ? "true" : "false";
            } else {
                // Number (int64)
                size_t start = pos_;
                while (pos_ < str_.size() &&
                       (std::isdigit(static_cast<unsigned char>(str_[pos_]))
                        || str_[pos_] == '-' || str_[pos_] == '+'))
                    pos_++;
                std::string numstr = str_.substr(start, pos_ - start);
                if (!numstr.empty())
                    ints_[key] = std::strtoll(numstr.c_str(), nullptr, 10);
            }

            skip_ws();
            if (pos_ < str_.size() && str_[pos_] == ',') { pos_++; continue; }
            if (pos_ < str_.size() && str_[pos_] == '}') { pos_++; return true; }
            return false;
        }
    }

    std::vector<std::string> parse_array() {
        std::vector<std::string> result;
        expect('[');
        skip_ws();
        if (pos_ < str_.size() && str_[pos_] == ']') { pos_++; return result; }
        while (true) {
            result.push_back(parse_string_val());
            skip_ws();
            if (pos_ < str_.size() && str_[pos_] == ',') { pos_++; continue; }
            if (pos_ < str_.size() && str_[pos_] == ']') { pos_++; return result; }
            return result;
        }
    }

    void skip_nested_object() {
        int depth = 0;
        while (pos_ < str_.size()) {
            if (str_[pos_] == '{') depth++;
            else if (str_[pos_] == '}') {
                depth--;
                if (depth == 0) { pos_++; return; }
            } else if (str_[pos_] == '"') {
                parse_string_val();
                continue;
            }
            pos_++;
        }
    }
};

}  // namespace tw

#endif  // TW_JSON_H
