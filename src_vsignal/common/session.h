#ifndef VSIGNAL_SESSION_H
#define VSIGNAL_SESSION_H

#include "tw/json.h"
#include "common/json_parser.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace vsignal {

struct DesignSession {
    std::string source;
    std::vector<std::string> args;

    std::string json() const {
        tw::JsonObject object;
        object.set("schema_version", int64_t(1));
        object.set("source", source);
        object.set_array("args", args);
        return object.dump();
    }

    static DesignSession parse(const std::string& text) {
        JsonParser parser;
        if (!parser.parse(text) || parser.get_int("schema_version", 0) != 1)
            throw std::runtime_error("Invalid vsignal worker session");
        DesignSession session;
        session.source = parser.get_string("source");
        session.args = parser.get_array("args");
        if (session.source.empty() || session.args.empty())
            throw std::runtime_error("Incomplete vsignal worker session");
        return session;
    }
};

} // namespace vsignal

#endif
