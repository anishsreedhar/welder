#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace strata::ui::colorwheel {

using RGB = std::array<double, 3>;
constexpr double tau = 6.2831853071795864769;
struct HSV { double h = 0, s = 0, v = 0; };
struct Point { double x = 0, y = 0; };
enum class Drag { Hue, Saturation, HueSaturation, Value };

inline double signedRoot(double value) { return std::copysign(std::sqrt(std::fabs(value)),value); }
inline double wrap(double hue) { return hue - std::floor(hue); }

// Use the signed component with the greatest magnitude. This preserves negative
// and mixed-sign grades, including the complementary hue shown in the recording.
inline HSV fromRGB(const RGB& rgb, HSV fallback = {}) {
    const auto [lo, hi] = std::minmax_element(rgb.begin(), rgb.end());
    const double value = std::fabs(*lo) > std::fabs(*hi) ? *lo : *hi;
    if (std::fabs(value) < 1e-14) return {fallback.h, fallback.s, 0};
    RGB normalized = {rgb[0] / value, rgb[1] / value, rgb[2] / value};
    const double minimum = *std::min_element(normalized.begin(), normalized.end());
    const double delta = 1 - minimum;
    if (delta < 1e-14) return {fallback.h, 0, value};
    double hue;
    if (normalized[0] >= normalized[1] && normalized[0] >= normalized[2])
        hue = (normalized[1] - normalized[2]) / delta;
    else if (normalized[1] >= normalized[2]) hue = 2 + (normalized[2] - normalized[0]) / delta;
    else hue = 4 + (normalized[0] - normalized[1]) / delta;
    return {wrap(hue / 6), delta, value};
}

inline RGB toRGB(HSV hsv) {
    const double h = wrap(hsv.h) * 6;
    const int sector = (int)std::floor(h);
    const double fraction = h - sector;
    const double p = hsv.v * (1 - hsv.s), q = hsv.v * (1 - hsv.s * fraction),
                 t = hsv.v * (1 - hsv.s * (1 - fraction));
    switch (sector % 6) {
        case 0: return {hsv.v, t, p};
        case 1: return {q, hsv.v, p};
        case 2: return {p, hsv.v, t};
        case 3: return {p, q, hsv.v};
        case 4: return {t, p, hsv.v};
        default: return {hsv.v, p, q};
    }
}

// Temperature (R-B), magenta/green ((R+B)/2-G), and mean intensity.
inline RGB toTMI(const RGB& rgb) {
    return {rgb[0] - rgb[2], (rgb[0] + rgb[2]) * .5 - rgb[1], (rgb[0] + rgb[1] + rgb[2]) / 3};
}
inline RGB fromTMI(const RGB& tmi) {
    return {tmi[2] + tmi[0] * .5 + tmi[1] / 3, tmi[2] - tmi[1] * 2 / 3,
            tmi[2] - tmi[0] * .5 + tmi[1] / 3};
}

struct State {
    RGB rgb{};
    HSV hsv{};
    bool initialized = false, dragging = false;
    Drag mode = Drag::HueSaturation;
    HSV start{};
    Point mouse{}, puck{};
    bool ring = false;

    void sync(const RGB& next, double savedHue = 0, double savedSaturation = 0) {
        if (dragging) return;
        if (!initialized) hsv = fromRGB(next, {savedHue, savedSaturation, 0});
        else if (next != rgb) hsv = fromRGB(next, {hsv.h, 0, 0});
        rgb = next;
        initialized = true;
    }
    void set(HSV next) { next.h = wrap(next.h); hsv = next; rgb = toRGB(next); initialized = true; }
    Point position(double radius) const {
        const double distance = radius * std::clamp(hsv.s, 0.0, 1.0);
        return {distance * std::cos(-hsv.h * tau), distance * std::sin(-hsv.h * tau)};
    }
    void begin(Point point, double radius, Drag drag, bool ringDrag) {
        start = hsv; mouse = point; puck = position(radius); mode = drag; ring = ringDrag; dragging = true;
    }
    void update(Point point, double radius, bool absolute, double valueSpeed = .005) {
        if (!dragging) return;
        const Point delta{point.x - mouse.x, point.y - mouse.y};
        if (!absolute && delta.x == 0 && delta.y == 0) return; // a relative click never jumps
        HSV next = start;
        if (mode == Drag::Value) next.v += delta.x * valueSpeed;
        else {
            Point target = absolute ? point : Point{puck.x + delta.x, puck.y + delta.y};
            if (mode != Drag::Saturation) {
                if (ring && !absolute)
                    next.h = start.h - (std::atan2(point.y, point.x) - std::atan2(mouse.y, mouse.x)) / tau;
                else if (std::hypot(target.x, target.y) > 1e-10) next.h = -std::atan2(target.y, target.x) / tau;
            }
            if (mode != Drag::Hue) next.s = std::clamp(std::hypot(target.x, target.y) / radius, 0.0, 1.0);
        }
        set(next);
    }
    void end() { dragging = false; }
};

} // namespace strata::ui::colorwheel
