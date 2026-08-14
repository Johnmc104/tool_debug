/**
 * @file handlers.h
 * @brief vsignal server command handlers — one function per command.
 */
#ifndef VSIGNAL_HANDLERS_H
#define VSIGNAL_HANDLERS_H

#include <string>
#include <sstream>
#include <chrono>
#include <utility>

#include <unistd.h>

#include "npi.h"
#include "npi_nl.h"
#include "npi_L1.h"

#include "tw/json.h"
#include "tw/protocol.h"

#include "common/protocol.h"
#include "common/json_parser.h"
#include "server/npi_helpers.h"

namespace vsignal {
namespace server {

// Globals defined in server_core.h (same TU, included before this header)
// g_design_source, g_start_time

static std::string handle_status(int id) {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      now - g_start_time).count();
    JsonObject data;
    data.set("design_source", g_design_source);
    data.set("uptime_seconds", uptime);
    data.set("pid", static_cast<int64_t>(getpid()));
    return make_ok_response(id, data.dump());
}

static std::string handle_info(int id) {
    JsonObject data;
    data.set("design_source", g_design_source);
    data.set("pid", static_cast<int64_t>(getpid()));
    return make_ok_response(id, data.dump());
}

static std::string handle_trace_driver(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    int assign_cell = static_cast<int>(params.get_int("assign_cell", 0));
    int pass_mod    = static_cast<int>(params.get_int("pass_mod", 0));
    bool compact    = params.get_bool("compact", false);
    int64_t limit   = params.get_int("limit", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_trace_driver(npi_str(sig), hdlVec, assign_cell, pass_mod);
    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace drivers for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("total", static_cast<int64_t>(hdlVec.size()));
    int64_t returned = (limit > 0 && limit < (int64_t)hdlVec.size())
                        ? limit : (int64_t)hdlVec.size();
    data.set("returned", returned);
    data.set_raw("drivers", nl_hdl_vec_to_json(hdlVec, compact, limit));
    return make_ok_response(id, data.dump());
}

static std::string handle_trace_load(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    int assign_cell = static_cast<int>(params.get_int("assign_cell", 0));
    int pass_mod    = static_cast<int>(params.get_int("pass_mod", 0));
    bool compact    = params.get_bool("compact", false);
    int64_t limit   = params.get_int("limit", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_trace_load(npi_str(sig), hdlVec, assign_cell, pass_mod);
    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace loads for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("total", static_cast<int64_t>(hdlVec.size()));
    int64_t returned = (limit > 0 && limit < (int64_t)hdlVec.size())
                        ? limit : (int64_t)hdlVec.size();
    data.set("returned", returned);
    data.set_raw("loads", nl_hdl_vec_to_json(hdlVec, compact, limit));
    return make_ok_response(id, data.dump());
}

static std::string handle_fanin_reg(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    bool stop_at_pin = params.get_bool("stop_at_pin", false);
    bool report_port = params.get_bool("report_primary_port", false);
    std::string scope = params.get_string("scope");
    bool compact     = params.get_bool("compact", false);
    int64_t limit    = params.get_int("limit", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_fanIn_reg_conn(
        npi_str(sig), hdlVec, stop_at_pin, report_port,
        scope.empty() ? nullptr : scope.c_str());
    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace fanin for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("total", static_cast<int64_t>(hdlVec.size()));
    int64_t returned = (limit > 0 && limit < (int64_t)hdlVec.size())
                        ? limit : (int64_t)hdlVec.size();
    data.set("returned", returned);
    data.set_raw("fanin", nl_hdl_vec_to_json(hdlVec, compact, limit));
    return make_ok_response(id, data.dump());
}

static std::string handle_fanout_reg(int id, const JsonParser& params) {
    std::string sig = params.get_string("signal");
    if (sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'signal' parameter");

    bool stop_at_pin = params.get_bool("stop_at_pin", false);
    bool report_port = params.get_bool("report_primary_port", false);
    std::string scope = params.get_string("scope");
    bool compact     = params.get_bool("compact", false);
    int64_t limit    = params.get_int("limit", 0);

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_fanOut_reg_conn(
        npi_str(sig), hdlVec, stop_at_pin, report_port,
        scope.empty() ? nullptr : scope.c_str());
    if (ret < 0)
        return make_error_response(id, err::SIGNAL_NOT_FOUND,
                                   "Failed to trace fanout for: " + sig);

    JsonObject data;
    data.set("signal", sig);
    data.set("total", static_cast<int64_t>(hdlVec.size()));
    int64_t returned = (limit > 0 && limit < (int64_t)hdlVec.size())
                        ? limit : (int64_t)hdlVec.size();
    data.set("returned", returned);
    data.set_raw("fanout", nl_hdl_vec_to_json(hdlVec, compact, limit));
    return make_ok_response(id, data.dump());
}

static std::string handle_trace_path(int id, const JsonParser& params) {
    std::string from_sig = params.get_string("from");
    std::string to_sig   = params.get_string("to");
    if (from_sig.empty() || to_sig.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'from' and/or 'to' parameters");

    int assign_cell = static_cast<int>(params.get_int("assign_cell", 0));

    nlHdlVec_t hdlVec;
    int ret = npi_nl_sig_2_sig_conn(
        npi_str(from_sig), npi_str(to_sig), hdlVec, assign_cell);
    if (ret < 0)
        return make_error_response(id, err::NO_PATH,
                                   "No path found from " + from_sig +
                                   " to " + to_sig);

    JsonObject data;
    data.set("from", from_sig);
    data.set("to", to_sig);
    data.set("count", static_cast<int64_t>(hdlVec.size()));
    data.set_raw("path", nl_hdl_vec_to_json(hdlVec));
    return make_ok_response(id, data.dump());
}

static std::string handle_inst_conn(int id, const JsonParser& params) {
    std::string inst = params.get_string("instance");
    if (inst.empty())
        return make_error_response(id, err::INVALID_PARAMS,
                                   "Missing 'instance' parameter");

    std::string level = params.get_string("level", "high");

    hdl2hdlVecPairVec_t pairVec;
    int ret;
    if (level == "low")
        ret = npi_inst_port_2_low_conn_sig(npi_str(inst), pairVec);
    else
        ret = npi_inst_port_2_high_conn_sig(npi_str(inst), pairVec);

    if (ret < 0)
        return make_error_response(id, err::INSTANCE_NOT_FOUND,
                                   "Failed to get connections for: " + inst);

    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < pairVec.size(); ++i) {
        if (i) arr << ",";
        JsonObject pair_obj;
        npiHandle port_hdl = pairVec[i].first;
        const char* port_name = npi_get_str(npiFullName, port_hdl);
        pair_obj.set("port", port_name ? port_name : "");

        hdlVec_t& sigs = pairVec[i].second;
        std::ostringstream sig_arr;
        sig_arr << "[";
        for (size_t j = 0; j < sigs.size(); ++j) {
            if (j) sig_arr << ",";
            const char* sig_name = npi_get_str(npiFullName, sigs[j]);
            sig_arr << "\"" << (sig_name ? sig_name : "") << "\"";
        }
        sig_arr << "]";
        pair_obj.set_raw("signals", sig_arr.str());
        arr << pair_obj.dump();
    }
    arr << "]";

    JsonObject data;
    data.set("instance", inst);
    data.set("level", level);
    data.set("count", static_cast<int64_t>(pairVec.size()));
    data.set_raw("connections", arr.str());
    return make_ok_response(id, data.dump());
}

}  // namespace server
}  // namespace vsignal

#endif  // VSIGNAL_HANDLERS_H
