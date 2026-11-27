#pragma once
#include "Params.h"

namespace strata::expr {
// The unmodified value beneath an expression: a key curve or a static value.
inline double baseValue(const json& value, double frame, double fallback) {
    if (params::isAnimated(value)) return params::evalCurve(value, frame);
    if (value.is_number()) return value.get<double>();
    if (value.is_object() && value.contains("value") && value["value"].is_number()) return value["value"].get<double>();
    return fallback;
}
inline json withExpression(const json& previous, const std::string& text, double frame, double fallback) {
    if (text.empty()) {
        if (params::isAnimated(previous)) { json result = previous; result.erase("expr"); return result; }
        return baseValue(previous, frame, fallback);
    }
    json result = previous.is_object() ? previous : json::object();
    if (!params::isAnimated(result)) result["value"] = baseValue(previous, frame, fallback);
    result["expr"] = text;
    return result;
}
}
