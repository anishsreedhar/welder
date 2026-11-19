#include "DetailMask.h"

#include <algorithm>
#include <cmath>

#include "Retime.h"

namespace strata::fx {

int detailPad(const DetailOptions& o) {
    return std::max(1, (int)std::lround(std::max(0.0, o.radius) * std::max(1e-3, o.scale))) + 1;
}

std::vector<float> detailMask(const Image& src, const Box& region, const DetailOptions& o) {
    const int RW = region.width(), RH = region.height();
    std::vector<float> out((size_t)std::max(0, RW) * std::max(0, RH), 0.0f);
    if (region.empty() || src.bounds.empty()) return out;
    const float wts[3] = {0.2126f, 0.7152f, 0.0722f};
    const retime::Plane L = retime::luminance(src, wts);
    const Box& sb = src.bounds;
    const int W = L.w, H = L.h;
    auto lum = [&](int x, int y) {  // x, y in image coordinates, clamped to the source
        return L.v[(size_t)std::clamp(y - sb.y1, 0, H - 1) * W + std::clamp(x - sb.x1, 0, W - 1)];
    };
    const double sc = std::max(1e-3, o.scale);
    const int r = std::max(1, (int)std::lround(std::max(0.0, o.radius) * sc));
    // summed-area tables of L and L^2 over region padded by r (double: no drift on big frames)
    const Box P = region.pad(r, r);
    const int PW = P.width() + 1, PH = P.height() + 1;
    std::vector<double> s1((size_t)PW * PH, 0.0), s2((size_t)PW * PH, 0.0);
    // sums of L - ref (ref: a value from the region) so the variance doesn't cancel catastrophically
    const double ref = lum((region.x1 + region.x2) / 2, (region.y1 + region.y2) / 2);
    for (int y = 1; y < PH; ++y) {
        double a1 = 0, a2 = 0;
        for (int x = 1; x < PW; ++x) {
            const double l = lum(P.x1 + x - 1, P.y1 + y - 1) - ref;
            a1 += l;
            a2 += l * l;
            s1[(size_t)y * PW + x] = s1[(size_t)(y - 1) * PW + x] + a1;
            s2[(size_t)y * PW + x] = s2[(size_t)(y - 1) * PW + x] + a2;
        }
    }
    const double n = (double)(2 * r + 1) * (2 * r + 1);
    const double ew = o.edgeWeight, cw = o.contrastWeight, gain = std::max(0.0, o.gain);
    retime::parallelFor(RH, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            const int y = region.y1 + j;
            for (int i = 0; i < RW; ++i) {
                const int x = region.x1 + i;
                // Sobel, normalised to the gradient per render pixel, then per full-res pixel
                const float a = lum(x - 1, y - 1), bb = lum(x, y - 1), c = lum(x + 1, y - 1);
                const float d = lum(x - 1, y), f = lum(x + 1, y);
                const float g = lum(x - 1, y + 1), h = lum(x, y + 1), k = lum(x + 1, y + 1);
                const double gx = ((c + 2 * f + k) - (a + 2 * d + g)) / 8.0, gy = ((g + 2 * h + k) - (a + 2 * bb + c)) / 8.0;
                const double edge = std::sqrt(gx * gx + gy * gy) * sc;
                // local standard deviation
                const int x0 = i, y0 = j, x1 = i + 2 * r + 1, y1 = j + 2 * r + 1;  // SAT indices (padded box)
                auto box = [&](const std::vector<double>& s) {
                    return s[(size_t)y1 * PW + x1] - s[(size_t)y0 * PW + x1] - s[(size_t)y1 * PW + x0] + s[(size_t)y0 * PW + x0];
                };
                const double m = box(s1) / n;
                double var = box(s2) / n - m * m;
                if (var < 1e-12) var = 0;  // rounding, not texture
                const double raw = ew * edge + cw * std::sqrt(var);
                out[(size_t)j * RW + i] = (float)(1.0 - std::exp(-gain * std::max(0.0, raw)));
            }
        }
    });
    return out;
}

}  // namespace strata::fx
