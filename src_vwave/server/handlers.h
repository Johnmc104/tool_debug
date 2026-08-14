/**
 * @file handlers.h
 * @brief vwave server command handlers — FSDB query functions.
 *
 * Globals (g_file_hdl, g_fsdb_path, g_start_time) are declared in
 * server_core.h and visible here because server_core.h includes this
 * file after declaring them.
 */
#ifndef WAVE_HANDLERS_H
#define WAVE_HANDLERS_H

#include <string>
#include <vector>
#include <sstream>
#include <chrono>
#include <unordered_map>

#include <unistd.h>

#include "npi.h"
#include "npi_fsdb.h"
#include "npi_L1.h"

#include "tw/json.h"
#include "tw/protocol.h"

#include "common/protocol.h"
#include "common/json_parser.h"

namespace wave {
namespace server {

// ─── NPI helpers ────────────────────────────────────────────────────────────

// Extract leaf name from full_name (e.g. "tb.u_cpu.clk" → "clk")
static std::string leaf_name(const char* full) {
    if (!full) return "";
    std::string fn(full);
    auto dot = fn.rfind('.');
    return (dot != std::string::npos) ? fn.substr(dot + 1) : fn;
}

// Safe signal name: use npiFsdbSigName, fall back to leaf of full_name
// if it contains non-ASCII bytes (T-2022 NPI bug on first signal).
static std::string safe_sig_name(npiFsdbSigHandle sig) {
    const char* name = npi_fsdb_sig_property_str(npiFsdbSigName, sig);
    if (name) {
        for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
            if (*p > 0x7E || (*p < 0x20 && *p != '\t')) {
                const char* full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
                return leaf_name(full);
            }
        return name;
    }
    const char* full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
    return leaf_name(full);
}

static const char* direction_str(int dir) {
    switch (dir) {
        case npiFsdbDirInput:  return "input";
        case npiFsdbDirOutput: return "output";
        case npiFsdbDirInout:  return "inout";
        default:               return "none";
    }
}

static bool wildcard_match(const char* pattern, const char* str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            ++pattern;
            if (!*pattern) return true;
            while (*str) {
                if (wildcard_match(pattern, str)) return true;
                ++str;
            }
            return false;
        } else if (*pattern == '?' || *pattern == *str) {
            ++pattern; ++str;
        } else {
            return false;
        }
    }
    while (*pattern == '*') ++pattern;
    return !*pattern && !*str;
}

// ─── Signal handle cache ────────────────────────────────────────────────────

static std::unordered_map<std::string, npiFsdbSigHandle> g_sig_cache;

static npiFsdbSigHandle cached_sig_by_name(const std::string& name) {
    auto it = g_sig_cache.find(name);
    if (it != g_sig_cache.end()) return it->second;
    npiFsdbSigHandle hdl = npi_fsdb_sig_by_name(g_file_hdl, name.c_str(), nullptr);
    if (hdl) g_sig_cache[name] = hdl;
    return hdl;
}

// ─── Status / Info ──────────────────────────────────────────────────────────

static std::string handle_status(int id) {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      now - g_start_time).count();
    JsonObject data;
    data.set("fsdb_file", g_fsdb_path);
    data.set("uptime_seconds", uptime);
    data.set("pid", static_cast<int64_t>(getpid()));
    return make_ok_response(id, data.dump());
}

static std::string handle_file_info(int id) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    npiFsdbTime min_t = 0, max_t = 0;
    npi_fsdb_min_time(g_file_hdl, &min_t);
    npi_fsdb_max_time(g_file_hdl, &max_t);

    NPI_INT32 is_completed = 0;
    npi_fsdb_file_property(npiFsdbFileIsCompleted, g_file_hdl, &is_completed);
    const char* scale_unit = npi_fsdb_file_property_str(npiFsdbFileScaleUnit, g_file_hdl);
    const char* version = npi_fsdb_file_property_str(npiFsdbFileVersion, g_file_hdl);

    JsonObject data;
    data.set("file", g_fsdb_path);
    data.set("min_time", static_cast<int64_t>(min_t));
    data.set("max_time", static_cast<int64_t>(max_t));
    data.set("scale_unit", scale_unit ? scale_unit : "");
    data.set("version", version ? version : "");
    data.set("is_completed", static_cast<int64_t>(is_completed));
    return make_ok_response(id, data.dump());
}

// ─── Hierarchy ──────────────────────────────────────────────────────────────

static void collect_scopes_depth(npiFsdbScopeHandle parent,
                                 std::vector<std::string>& out,
                                 int cur_depth, int max_depth) {
    npiFsdbScopeIter iter = npi_fsdb_iter_child_scope(parent);
    if (!iter) return;
    npiFsdbScopeHandle scope;
    while ((scope = npi_fsdb_iter_scope_next(iter)) != nullptr) {
        const char* name = npi_fsdb_scope_property_str(npiFsdbScopeFullName, scope);
        if (name) out.push_back(name);
        if (cur_depth < max_depth)
            collect_scopes_depth(scope, out, cur_depth + 1, max_depth);
        if (out.size() >= 500) break;
    }
    npi_fsdb_iter_scope_stop(iter);
}

static std::string handle_list_scopes(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string path = params.get_string("path");
    bool compact = params.get_bool("compact", false);
    int depth = static_cast<int>(params.get_int("depth", 1));
    if (depth < 1) depth = 1;
    if (depth > 10) depth = 10;
    std::vector<std::string> scope_names;

    if (path.empty()) {
        npiFsdbScopeIter iter = npi_fsdb_iter_top_scope(g_file_hdl);
        if (iter) {
            npiFsdbScopeHandle scope;
            while ((scope = npi_fsdb_iter_scope_next(iter)) != nullptr) {
                const char* name = npi_fsdb_scope_property_str(npiFsdbScopeFullName, scope);
                if (name) scope_names.push_back(name);
                if (depth > 1)
                    collect_scopes_depth(scope, scope_names, 2, depth);
            }
            npi_fsdb_iter_scope_stop(iter);
        }
    } else {
        npiFsdbScopeHandle parent = npi_fsdb_scope_by_name(
            g_file_hdl, path.c_str(), nullptr);
        if (!parent)
            return make_error_response(id, err::SCOPE_NOT_FOUND,
                                       "Scope '" + path + "' not found");
        collect_scopes_depth(parent, scope_names, 1, depth);
    }

    if (compact && !path.empty()) {
        std::string prefix = path + ".";
        for (auto& s : scope_names) {
            if (s.compare(0, prefix.size(), prefix) == 0)
                s = s.substr(prefix.size());
        }
    }

    JsonObject data;
    data.set("path", path.empty() ? "/" : path);
    data.set_array("scopes", scope_names);
    data.set("count", static_cast<int64_t>(scope_names.size()));
    if (scope_names.size() >= 500) data.set("truncated", static_cast<int64_t>(1));
    return make_ok_response(id, data.dump());
}

static std::string handle_list_signals(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string path = params.get_string("path");
    if (path.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'path' parameter");

    bool compact = params.get_bool("compact", false);
    npiFsdbScopeHandle scope = npi_fsdb_scope_by_name(g_file_hdl, path.c_str(), nullptr);
    if (!scope)
        return make_error_response(id, err::SCOPE_NOT_FOUND,
                                   "Scope '" + path + "' not found");

    if (compact) {
        std::vector<std::string> compact_sigs;
        npiFsdbSigIter iter = npi_fsdb_iter_sig(scope);
        if (iter) {
            npiFsdbSigHandle sig;
            while ((sig = npi_fsdb_iter_sig_next(iter)) != nullptr) {
                const char* sig_full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
                NPI_INT32 left = 0, right = 0, dir = 0;
                npi_fsdb_sig_property(npiFsdbSigLeftRange, sig, &left);
                npi_fsdb_sig_property(npiFsdbSigRightRange, sig, &right);
                npi_fsdb_sig_property(npiFsdbSigDirection, sig, &dir);

                std::string name;
                if (sig_full) {
                    std::string fn(sig_full);
                    auto dot = fn.rfind('.');
                    name = (dot != std::string::npos) ? fn.substr(dot+1) : fn;
                }
                std::ostringstream entry;
                entry << name;
                if (left != 0 || right != 0) entry << "[" << left << ":" << right << "]";
                if (dir == npiFsdbDirInput) entry << " i";
                else if (dir == npiFsdbDirOutput) entry << " o";
                else if (dir == npiFsdbDirInout) entry << " io";
                compact_sigs.push_back(entry.str());
            }
            npi_fsdb_iter_sig_stop(iter);
        }
        JsonObject data;
        data.set("path", path);
        data.set_array("signals", compact_sigs);
        data.set("count", static_cast<int64_t>(compact_sigs.size()));
        return make_ok_response(id, data.dump());
    }

    std::ostringstream arr;
    arr << "[";
    npiFsdbSigIter iter = npi_fsdb_iter_sig(scope);
    bool first = true;
    if (iter) {
        npiFsdbSigHandle sig;
        while ((sig = npi_fsdb_iter_sig_next(iter)) != nullptr) {
            std::string sig_name = safe_sig_name(sig);
            const char* sig_full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
            NPI_INT32 left = 0, right = 0, dir = 0;
            npi_fsdb_sig_property(npiFsdbSigLeftRange, sig, &left);
            npi_fsdb_sig_property(npiFsdbSigRightRange, sig, &right);
            npi_fsdb_sig_property(npiFsdbSigDirection, sig, &dir);

            if (!first) arr << ",";
            first = false;
            JsonObject sig_obj;
            sig_obj.set("name", sig_name);
            sig_obj.set("full_name", sig_full ? sig_full : "");
            sig_obj.set("left", static_cast<int64_t>(left));
            sig_obj.set("right", static_cast<int64_t>(right));
            sig_obj.set("direction", direction_str(dir));
            arr << sig_obj.dump();
        }
        npi_fsdb_iter_sig_stop(iter);
    }
    arr << "]";

    JsonObject data;
    data.set("path", path);
    data.set_raw("signals", arr.str());
    return make_ok_response(id, data.dump());
}

static std::string handle_signal_info(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string sig_name = params.get_string("signal");
    if (sig_name.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal' parameter");

    npiFsdbSigHandle sig = cached_sig_by_name(sig_name);
    if (!sig)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Signal '" + sig_name + "' not found");

    std::string name = safe_sig_name(sig);
    const char* full = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
    NPI_INT32 left = 0, right = 0, dir = 0;
    npi_fsdb_sig_property(npiFsdbSigLeftRange, sig, &left);
    npi_fsdb_sig_property(npiFsdbSigRightRange, sig, &right);
    npi_fsdb_sig_property(npiFsdbSigDirection, sig, &dir);

    JsonObject data;
    data.set("name", name);
    data.set("full_name", full ? full : "");
    data.set("left", static_cast<int64_t>(left));
    data.set("right", static_cast<int64_t>(right));
    data.set("direction", direction_str(dir));
    return make_ok_response(id, data.dump());
}

// ─── Value queries ──────────────────────────────────────────────────────────

static std::string handle_get_value_at(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    int64_t time = params.get_int("time", -1);
    if (time < 0)
        return make_error_response(id, err::INVALID_TIME, "Missing or invalid 'time'");

    std::string radix_str = params.get_string("radix", "bin");
    npiFsdbValType format = npiFsdbBinStrVal;
    if      (radix_str == "hex") format = npiFsdbHexStrVal;
    else if (radix_str == "oct") format = npiFsdbOctStrVal;
    else if (radix_str == "dec") format = npiFsdbDecStrVal;

    std::vector<std::string> signals = params.get_array("signals");
    if (signals.empty()) {
        std::string single = params.get_string("signal");
        if (!single.empty()) signals.push_back(single);
    }
    if (signals.empty())
        return make_error_response(id, err::INVALID_PARAMS, "No signals specified");

    npiFsdbTime fsdb_time = static_cast<npiFsdbTime>(time);
    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < signals.size(); ++i) {
        if (i) arr << ",";
        npiFsdbSigHandle sig = cached_sig_by_name(signals[i]);
        JsonObject val_obj;
        val_obj.set("signal", signals[i]);
        if (!sig) {
            val_obj.set("error", "not found");
        } else {
            std::string val_str;
            if (npi_fsdb_sig_value_at(g_file_hdl, signals[i].c_str(),
                                       fsdb_time, val_str, format))
                val_obj.set("value", val_str);
            else {
                val_obj.set("value", "x");
                val_obj.set("error", "read failed");
            }
            val_obj.set("actual_time", static_cast<int64_t>(fsdb_time));
        }
        arr << val_obj.dump();
    }
    arr << "]";

    JsonObject data;
    data.set("time", static_cast<int64_t>(time));
    data.set_raw("values", arr.str());
    return make_ok_response(id, data.dump());
}

static std::string handle_get_value_between(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string sig_name = params.get_string("signal");
    if (sig_name.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal'");

    int64_t begin = params.get_int("begin", -1);
    int64_t end   = params.get_int("end", -1);
    if (begin < 0 || end < 0 || end < begin)
        return make_error_response(id, err::INVALID_TIME, "Invalid time range");

    std::string radix_str = params.get_string("radix", "bin");
    npiFsdbValType format = npiFsdbBinStrVal;
    if      (radix_str == "hex") format = npiFsdbHexStrVal;
    else if (radix_str == "oct") format = npiFsdbOctStrVal;
    else if (radix_str == "dec") format = npiFsdbDecStrVal;

    npiFsdbSigHandle sig = cached_sig_by_name(sig_name);
    if (!sig)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Signal '" + sig_name + "' not found");

    npiFsdbTime begin_t = static_cast<npiFsdbTime>(begin);
    npiFsdbTime end_t   = static_cast<npiFsdbTime>(end);
    int64_t limit = params.get_int("limit", 1000);
    if (limit < 1) limit = 1;
    if (limit > 100000) limit = 100000;

    fsdbTimeValPairVec_t vcVec;
    if (npi_fsdb_sig_value_between(g_file_hdl, sig_name.c_str(),
                                    begin_t, end_t, vcVec, format)) {
        int64_t total = static_cast<int64_t>(vcVec.size());
        size_t output_count = (total <= limit) ? vcVec.size() : static_cast<size_t>(limit);
        std::ostringstream arr;
        arr << "[";
        for (size_t i = 0; i < output_count; ++i) {
            if (i) arr << ",";
            JsonObject vc;
            vc.set("time", static_cast<int64_t>(vcVec[i].first));
            vc.set("value", vcVec[i].second);
            arr << vc.dump();
        }
        arr << "]";

        JsonObject data;
        data.set("signal", sig_name);
        data.set("begin", static_cast<int64_t>(begin));
        data.set("end", static_cast<int64_t>(end));
        data.set("total_changes", total);
        data.set_raw("changes", arr.str());
        if (total > limit) data.set("truncated", static_cast<int64_t>(1));
        return make_ok_response(id, data.dump());
    }

    JsonObject data;
    data.set("signal", sig_name);
    data.set("begin", static_cast<int64_t>(begin));
    data.set("end", static_cast<int64_t>(end));
    data.set("total_changes", static_cast<int64_t>(0));
    data.set_raw("changes", "[]");
    return make_ok_response(id, data.dump());
}

// ─── Find signals ───────────────────────────────────────────────────────────

static void find_signals_recursive(npiFsdbScopeHandle scope,
                                   const std::string& pattern,
                                   std::vector<std::string>& results,
                                   int depth, int max_depth) {
    if (depth > max_depth) return;
    npiFsdbSigIter sig_iter = npi_fsdb_iter_sig(scope);
    if (sig_iter) {
        npiFsdbSigHandle sig;
        while ((sig = npi_fsdb_iter_sig_next(sig_iter)) != nullptr) {
            std::string sn = safe_sig_name(sig);
            const char* fn = npi_fsdb_sig_property_str(npiFsdbSigFullName, sig);
            if (!sn.empty() && wildcard_match(pattern.c_str(), sn.c_str()))
                if (fn) results.push_back(fn);
            if (results.size() >= 200) break;
        }
        npi_fsdb_iter_sig_stop(sig_iter);
    }
    if (results.size() >= 200) return;
    npiFsdbScopeIter child_iter = npi_fsdb_iter_child_scope(scope);
    if (child_iter) {
        npiFsdbScopeHandle child;
        while ((child = npi_fsdb_iter_scope_next(child_iter)) != nullptr) {
            find_signals_recursive(child, pattern, results, depth+1, max_depth);
            if (results.size() >= 200) break;
        }
        npi_fsdb_iter_scope_stop(child_iter);
    }
}

static std::string handle_find_signals(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string pattern = params.get_string("pattern");
    std::string scope   = params.get_string("scope");
    if (pattern.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'pattern'");

    std::vector<std::string> results;
    if (!scope.empty()) {
        npiFsdbScopeHandle start_scope = npi_fsdb_scope_by_name(
            g_file_hdl, scope.c_str(), nullptr);
        if (!start_scope)
            return make_error_response(id, err::SCOPE_NOT_FOUND,
                                       "Scope '" + scope + "' not found");
        find_signals_recursive(start_scope, pattern, results, 0, 20);
    } else {
        npiFsdbScopeIter top_iter = npi_fsdb_iter_top_scope(g_file_hdl);
        if (top_iter) {
            npiFsdbScopeHandle ts;
            while ((ts = npi_fsdb_iter_scope_next(top_iter)) != nullptr) {
                find_signals_recursive(ts, pattern, results, 0, 20);
                if (results.size() >= 200) break;
            }
            npi_fsdb_iter_scope_stop(top_iter);
        }
    }

    JsonObject data;
    data.set("pattern", pattern);
    if (!scope.empty()) data.set("scope", scope);
    data.set_array("signals", results);
    data.set("count", static_cast<int64_t>(results.size()));
    if (results.size() >= 200) data.set("truncated", static_cast<int64_t>(1));
    return make_ok_response(id, data.dump());
}

// ─── Edge navigation ────────────────────────────────────────────────────────

static std::string handle_next_edge(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string sig_name = params.get_string("signal");
    int64_t from_time = params.get_int("time", -1);
    std::string edge = params.get_string("edge", "any");
    std::string dir = params.get_string("dir", "forward");

    if (sig_name.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal'");
    if (from_time < 0)
        return make_error_response(id, err::INVALID_TIME, "Missing 'time'");

    npiFsdbSigHandle sig = cached_sig_by_name(sig_name);
    if (!sig)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Signal '" + sig_name + "' not found");

    npiFsdbTime search_t = static_cast<npiFsdbTime>(from_time);
    bool found = false;
    int64_t found_time = -1;
    std::string found_value;

    if (edge == "rising" || edge == "falling") {
        const char* target = (edge == "rising") ? "1" : "0";
        npiFsdbTime vcTime = 0;
        int ret;
        if (dir == "backward")
            ret = npi_fsdb_sig_find_value_backward(
                g_file_hdl, sig_name.c_str(), target,
                search_t, vcTime, npiFsdbBinStrVal);
        else
            ret = npi_fsdb_sig_find_value_forward(
                g_file_hdl, sig_name.c_str(), target,
                search_t + 1, vcTime, npiFsdbBinStrVal);
        if (ret) {
            found = true;
            found_time = static_cast<int64_t>(vcTime);
            found_value = target;
        }
    } else {
        npiFsdbVctHandle vct = npi_fsdb_create_vct(sig);
        if (vct) {
            npi_fsdb_goto_time(vct, search_t);
            int ret;
            if (dir == "backward") ret = npi_fsdb_goto_prev(vct);
            else ret = npi_fsdb_goto_next(vct);
            if (ret) {
                npiFsdbTime t = 0;
                npi_fsdb_vct_time(vct, &t);
                found = true;
                found_time = static_cast<int64_t>(t);
                npiFsdbValue val;
                val.format = npiFsdbBinStrVal;
                if (npi_fsdb_vct_value(vct, &val) && val.value.str)
                    found_value = val.value.str;
            }
            npi_fsdb_release_vct(vct);
        }
    }

    JsonObject data;
    data.set("signal", sig_name);
    data.set("from_time", from_time);
    data.set("edge", edge);
    data.set("dir", dir);
    if (found) {
        data.set("found_time", found_time);
        data.set("value", found_value);
    } else {
        data.set("found_time", static_cast<int64_t>(-1));
    }
    return make_ok_response(id, data.dump());
}

// ─── VC count ───────────────────────────────────────────────────────────────

static std::string handle_vc_count(int id, const JsonParser& params) {
    if (!g_file_hdl)
        return make_error_response(id, err::FSDB_OPEN_FAILED, "No FSDB file loaded");

    std::string sig_name = params.get_string("signal");
    if (sig_name.empty())
        return make_error_response(id, err::INVALID_PARAMS, "Missing 'signal'");

    int64_t begin = params.get_int("begin", 0);
    int64_t end = params.get_int("end", -1);
    if (end < 0) {
        npiFsdbTime mt = 0;
        npi_fsdb_max_time(g_file_hdl, &mt);
        end = static_cast<int64_t>(mt);
    }

    npiFsdbSigHandle sig = cached_sig_by_name(sig_name);
    if (!sig)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Signal '" + sig_name + "' not found");

    signed long long count = npi_fsdb_sig_vc_count(
        g_file_hdl, sig_name.c_str(),
        static_cast<npiFsdbTime>(begin), static_cast<npiFsdbTime>(end));

    JsonObject data;
    data.set("signal", sig_name);
    data.set("begin", begin);
    data.set("end", end);
    data.set("count", static_cast<int64_t>(count));
    return make_ok_response(id, data.dump());
}

}  // namespace server
}  // namespace wave

#endif  // WAVE_HANDLERS_H
