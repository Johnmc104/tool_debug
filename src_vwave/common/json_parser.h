/**
 * @file json_parser.h
 * @brief Minimal JSON parser for protocol messages (no external dependency).
 *
 * Only handles flat objects with string/int values and string arrays,
 * which is sufficient for the wave protocol.
 */
#ifndef WAVE_JSON_PARSER_H
#define WAVE_JSON_PARSER_H

#include <string>
#include <map>
#include <vector>
#include <cstdlib>
#include <cctype>

namespace wave {

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

    std::string get_string(const std::string& key, const std::string& def = "") const {
        auto it = strings_.find(key);
        if (it != strings_.end()) return it->second;
        // Fallback: check int map and convert
        auto it2 = ints_.find(key);
        if (it2 != ints_.end()) return std::to_string(it2->second);
        return def;
    }

    int64_t get_int(const std::string& key, int64_t def = 0) const {
        auto it = ints_.find(key);
        if (it != ints_.end()) return it->second;
        // Fallback: try string
        auto it2 = strings_.find(key);
        if (it2 != strings_.end()) return std::strtoll(it2->second.c_str(), nullptr, 10);
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

    std::map<std::string, std::string> strings_;
    std::map<std::string, int64_t> ints_;
    std::map<std::string, std::vector<std::string>> arrays_;

    void skip_ws() {
        while (pos_ < str_.size() && std::isspace((unsigned char)str_[pos_])) pos_++;
    }

    bool expect(char c) {
        skip_ws();
        if (pos_ < str_.size() && str_[pos_] == c) { pos_++; return true; }
        return false;
    }

    std::string parse_string_val() {
        skip_ws();
        if (pos_ >= str_.size() || str_[pos_] != '"') return "";
        pos_++; // skip opening "
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
        if (pos_ < str_.size()) pos_++; // skip closing "
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
                // Nested object: skip and store as raw string
                size_t start = pos_;
                skip_nested_object();
                strings_[key] = str_.substr(start, pos_ - start);
            } else {
                // Number
                size_t start = pos_;
                while (pos_ < str_.size() && (std::isdigit((unsigned char)str_[pos_])
                       || str_[pos_] == '-' || str_[pos_] == '+'))
                    pos_++;
                std::string numstr = str_.substr(start, pos_ - start);
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
            else if (str_[pos_] == '}') { depth--; if (depth == 0) { pos_++; return; } }
            else if (str_[pos_] == '"') { parse_string_val(); continue; }
            pos_++;
        }
    }
};

} // namespace wave

#endif // WAVE_JSON_PARSER_H
