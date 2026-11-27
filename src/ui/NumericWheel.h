#pragma once
#include <cmath>

namespace strata::ui::kit {
// Keep small trackpad/high-resolution wheel deltas until they form a whole
// integer step. Fractional controls can respond immediately to every delta.
struct NumericWheel {
    double remainder = 0, lastTime = -1, lastStep = 0, lastValue = 0;
    bool lastWhole = false;
    double advance(double value, double delta, double step, bool whole, double time) {
        if (lastTime < 0 || time - lastTime > .75 || step != lastStep || whole != lastWhole || value != lastValue) remainder = 0;
        lastTime = time; lastStep = step; lastWhole = whole;
        double next = value;
        if (whole) {
            remainder += delta * step;
            const double ticks = std::trunc(remainder + std::copysign(1e-9, remainder));
            if (ticks != 0) { next = std::round(value) + ticks; remainder -= ticks; }
            if (std::fabs(remainder) < 1e-9) remainder = 0;
        } else next += delta * step;
        lastValue = next;
        return next;
    }
};
}
