/**
 * @file json_parser.h
 * @brief vsignal JSON — thin alias over shared tw::JsonObject / tw::JsonParser.
 */
#ifndef VSIGNAL_JSON_PARSER_H
#define VSIGNAL_JSON_PARSER_H

#include "tw/json.h"

namespace vsignal {
    using JsonObject = tw::JsonObject;
    using JsonParser = tw::JsonParser;
}

#endif  // VSIGNAL_JSON_PARSER_H
