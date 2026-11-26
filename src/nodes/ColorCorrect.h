#pragma once

#include "Curves.h"

namespace strata::cc {

inline json defaultLookup() {
    return {{"shadow", {{0.0, 1.0}, {0.09, 0.0}}},
            {"highlight", {{0.5, 0.0}, {1.0, 1.0}}}};
}

// Range curves have flat ends and monotone, smooth interpolation. Midtones
// are a derived curve, so the three weights always add to one.
struct RangeCurve {
    LookupCurve curve;
    RangeCurve(const json& lookup, const char* name) {
        const json def = defaultLookup();
        curve = LookupCurve::fromJson(lookup.is_object() && lookup.contains(name) ? lookup[name] : def[name],
                                     LookupCurve::fromJson(def[name]).pts);
    }
    double eval(double x) const {
        const auto& p = curve.pts;
        if (std::isnan(x)) return 0;
        if (p.size() == 1 || x <= p.front().first) return std::clamp(p.front().second, 0.0, 1.0);
        if (x >= p.back().first) return std::clamp(p.back().second, 0.0, 1.0);
        auto slope = [&](int i) {
            if (i == 0 || i == (int)p.size() - 1) return 0.0;
            const double a = (p[i].second - p[i - 1].second) / std::max(1e-12, p[i].first - p[i - 1].first);
            const double b = (p[i + 1].second - p[i].second) / std::max(1e-12, p[i + 1].first - p[i].first);
            return a * b <= 0 ? 0.0 : 2 * a * b / (a + b);
        };
        int i = (int)(std::upper_bound(p.begin(), p.end(), std::make_pair(x, 1e300)) - p.begin()) - 1;
        i = std::clamp(i, 0, (int)p.size() - 2);
        return std::clamp(LookupCurve::hermite(x, p[i].first, p[i].second, slope(i),
                                             p[i + 1].first, p[i + 1].second, slope(i + 1)), 0.0, 1.0);
    }
};

}  // namespace strata::cc
