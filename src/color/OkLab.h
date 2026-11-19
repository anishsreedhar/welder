// OKLab and OKLCH: a perceptual colour space (Bjorn Ottosson, 2020, public matrices) where equal
// steps look about equally different, lightness L is independent of hue, and hue turns don't
// shift brightness. Used for hue / chroma effects that must keep the plate's lightness.
//
// The input is LINEAR Rec.709 / sRGB primaries (scene-linear, any range: HDR and negative values
// go through, cube roots are signed). A comp working in another space (ACEScg, ...) converts to
// linear Rec.709 first (an OCIO step) and back afterwards.
//
// L is 0 for black and 1 for diffuse white (1, 1, 1); a, b are the green-red and blue-yellow axes
// (about -0.4..0.4 for sRGB colours). OKLCH is the polar form: C = hypot(a, b), h = atan2(b, a) in
// radians (-pi..pi).
//
// The inverse matrices are computed in double from the forward ones (rather than the rounded
// published inverses), so a round trip is exact to float precision.
#pragma once

#include <cmath>

namespace strata::color::oklab {

namespace detail {

struct Mat3 {
    double m[3][3];
    void apply(const double in[3], double out[3]) const {
        const double x = in[0], y = in[1], z = in[2];
        for (int r = 0; r < 3; ++r) out[r] = m[r][0] * x + m[r][1] * y + m[r][2] * z;
    }
    Mat3 inverse() const {
        const auto& a = m;
        const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        Mat3 r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const int i1 = (j + 1) % 3, i2 = (j + 2) % 3, j1 = (i + 1) % 3, j2 = (i + 2) % 3;
                r.m[i][j] = (a[i1][j1] * a[i2][j2] - a[i1][j2] * a[i2][j1]) / det;
            }
        return r;
    }
};

// linear sRGB -> LMS cone response
constexpr Mat3 kM1 = {{{0.4122214708, 0.5363325363, 0.0514459929},
                       {0.2119034982, 0.6806995451, 0.1073969566},
                       {0.0883024619, 0.2817188376, 0.6299787005}}};
// LMS^(1/3) -> Lab
constexpr Mat3 kM2 = {{{0.2104542553, 0.7936177850, -0.0040720468},
                       {1.9779984951, -2.4285922050, 0.4505937099},
                       {0.0259040371, 0.7827717662, -0.8086757660}}};

inline const Mat3& m1inv() {
    static const Mat3 m = kM1.inverse();
    return m;
}
inline const Mat3& m2inv() {
    static const Mat3 m = kM2.inverse();
    return m;
}

}  // namespace detail

// linear Rec.709 RGB -> OKLab (L, a, b)
inline void fromLinear(const double rgb[3], double lab[3]) {
    double lms[3];
    detail::kM1.apply(rgb, lms);
    for (double& v : lms) v = std::cbrt(v);
    detail::kM2.apply(lms, lab);
}
// OKLab -> linear Rec.709 RGB
inline void toLinear(const double lab[3], double rgb[3]) {
    double lms[3];
    detail::m2inv().apply(lab, lms);
    for (double& v : lms) v = v * v * v;
    detail::m1inv().apply(lms, rgb);
}

// OKLab <-> OKLCH (C >= 0, h in radians)
inline void toLch(const double lab[3], double lch[3]) {
    lch[0] = lab[0];
    lch[1] = std::hypot(lab[1], lab[2]);
    lch[2] = std::atan2(lab[2], lab[1]);
}
inline void fromLch(const double lch[3], double lab[3]) {
    lab[0] = lch[0];
    lab[1] = lch[1] * std::cos(lch[2]);
    lab[2] = lch[1] * std::sin(lch[2]);
}

// float pixel conveniences (rgb in, lab out; the maths runs in double)
inline void fromLinear(const float rgb[3], float lab[3]) {
    const double i[3] = {rgb[0], rgb[1], rgb[2]};
    double o[3];
    fromLinear(i, o);
    for (int k = 0; k < 3; ++k) lab[k] = (float)o[k];
}
inline void toLinear(const float lab[3], float rgb[3]) {
    const double i[3] = {lab[0], lab[1], lab[2]};
    double o[3];
    toLinear(i, o);
    for (int k = 0; k < 3; ++k) rgb[k] = (float)o[k];
}

}  // namespace strata::color::oklab
