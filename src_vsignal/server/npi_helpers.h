/**
 * @file npi_helpers.h
 * @brief NPI netlist data conversion helpers for vsignal server.
 */
#ifndef VSIGNAL_NPI_HELPERS_H
#define VSIGNAL_NPI_HELPERS_H

#include <string>
#include <vector>
#include <sstream>
#include <cstring>

#include "npi.h"
#include "npi_nl.h"

#include "common/json_parser.h"

namespace vsignal {
namespace server {

static inline char* npi_str(const std::string& s) {
    return const_cast<char*>(s.c_str());
}

static const char* nl_obj_type_str(int type) {
    switch (type) {
        case npiNlInst:             return "instance";
        case npiNlPort:             return "port";
        case npiNlInstPort:         return "inst_port";
        case npiNlDeclNet:          return "net";
        case npiNlConcatNet:        return "concat_net";
        case npiNlSliceNet:         return "slice_net";
        case npiNlPseudoPort:       return "pseudo_port";
        case npiNlPseudoInstPort:   return "pseudo_inst_port";
        case npiNlPseudoNet:        return "pseudo_net";
        case npiNlLib:              return "lib";
        case npiNlCell:             return "cell";
        case npiNlCellPin:          return "cell_pin";
        default:                    return "unknown";
    }
}

static const char* nl_direction_str(int dir) {
    switch (dir) {
        case npiNlInput:  return "input";
        case npiNlOutput: return "output";
        case npiNlInout:  return "inout";
        default:          return "none";
    }
}

static std::string nl_handle_to_json(npiNlHandle hdl, bool compact = false) {
    if (!hdl) return "{}";

    int obj_type = npi_nl_get(npiNlType, hdl);
    const char* name      = npi_nl_get_str(npiNlName, hdl);
    const char* full_name = npi_nl_get_str(npiNlFullName, hdl);

    JsonObject obj;

    if (compact) {
        obj.set("name", name ? name : "");
    } else {
        obj.set("type", nl_obj_type_str(obj_type));
        obj.set("name",      name      ? name      : "");
        obj.set("full_name", full_name ? full_name : "");

        if (obj_type == npiNlInst) {
            const char* def_name = npi_nl_get_str(npiNlDefName, hdl);
            if (def_name) obj.set("def_name", def_name);
            int cell_type = npi_nl_get(npiNlCellType, hdl);
            if (cell_type > 0) obj.set("cell_type", static_cast<int64_t>(cell_type));
        }

        if (obj_type == npiNlPort || obj_type == npiNlInstPort ||
            obj_type == npiNlPseudoPort || obj_type == npiNlPseudoInstPort) {
            int dir  = npi_nl_get(npiNlDirection, hdl);
            int size = npi_nl_get(npiNlSize, hdl);
            obj.set("direction", nl_direction_str(dir));
            if (size > 0) obj.set("size", static_cast<int64_t>(size));
        }

        if (obj_type == npiNlDeclNet || obj_type == npiNlConcatNet ||
            obj_type == npiNlSliceNet || obj_type == npiNlPseudoNet) {
            int size = npi_nl_get(npiNlSize, hdl);
            if (size > 0) obj.set("size", static_cast<int64_t>(size));
        }
    }

    return obj.dump();
}

static std::string nl_hdl_vec_to_json(nlHdlVec_t& hdlVec,
                                       bool compact = false,
                                       int64_t limit = 0) {
    size_t count = hdlVec.size();
    if (limit > 0 && static_cast<size_t>(limit) < count)
        count = static_cast<size_t>(limit);

    std::ostringstream arr;
    arr << "[";
    for (size_t i = 0; i < count; ++i) {
        if (i) arr << ",";
        arr << nl_handle_to_json(hdlVec[i], compact);
    }
    arr << "]";
    return arr.str();
}

}  // namespace server
}  // namespace vsignal

#endif  // VSIGNAL_NPI_HELPERS_H
