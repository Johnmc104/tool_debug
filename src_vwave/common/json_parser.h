/**
 * @file json_parser.h
 * @brief vwave JSON — thin alias over shared tw::JsonObject / tw::JsonParser.
 */
#ifndef WAVE_JSON_PARSER_H
#define WAVE_JSON_PARSER_H

#include "tw/json.h"

namespace wave {
    using JsonObject = tw::JsonObject;
    using JsonParser = tw::JsonParser;
}

#endif  // WAVE_JSON_PARSER_H
