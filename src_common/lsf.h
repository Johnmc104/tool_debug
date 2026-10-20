/**
 * @file lsf.h
 * @brief Shared LSF command and proxy response helpers.
 *
 * Process ownership and protocol dispatch remain in each tool's proxy because
 * vwave and vsignal have different session and stdio protocols.  These small
 * helpers are identical and belong to the common runtime layer.
 */
#ifndef TW_LSF_H
#define TW_LSF_H

#include "json.h"

#include <cctype>
#include <string>
#include <sys/types.h>
#include <vector>

namespace tw {
namespace lsf {

inline std::vector<std::string> split_command(const std::string& text) {
    std::vector<std::string> result;
    std::string word;
    char quote = 0;
    for (char value : text) {
        if (quote) {
            if (value == quote) quote = 0;
            else word += value;
        } else if (value == '\'' || value == '"') {
            quote = value;
        } else if (std::isspace(static_cast<unsigned char>(value))) {
            if (!word.empty()) {
                result.push_back(word);
                word.clear();
            }
        } else {
            word += value;
        }
    }
    if (!word.empty()) result.push_back(word);
    return result;
}

inline std::string replace_pid(const std::string& response, pid_t pid) {
    const std::string key = "\"pid\":";
    auto pos = response.find(key);
    if (pos == std::string::npos) return response;
    auto begin = pos + key.size();
    auto end = begin;
    while (end < response.size() &&
           std::isdigit(static_cast<unsigned char>(response[end]))) ++end;
    if (end == begin) return response;
    return response.substr(0, begin) +
           std::to_string(static_cast<long long>(pid)) + response.substr(end);
}

inline std::string add_status_fields(const std::string& response,
                                     const std::string& job_id,
                                     const std::string& job_name) {
    auto marker = response.find("\"data\":{");
    if (marker == std::string::npos) return response;
    JsonObject fields;
    fields.set("run_mode", "lsf");
    if (!job_id.empty()) fields.set("job_id", job_id);
    if (!job_name.empty()) fields.set("job_name", job_name);
    std::string prefix = fields.dump();
    prefix.erase(0, 1);
    if (!prefix.empty() && prefix.back() == '}') prefix.pop_back();
    return response.substr(0, marker + 8) + prefix + "," +
           response.substr(marker + 8);
}

}  // namespace lsf
}  // namespace tw

#endif  // TW_LSF_H
