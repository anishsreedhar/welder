// Camera and lens tricks: WhipTransition (a whip pan from A to B with motion blur, overshoot and
// settle) and PrismLens (the picture split into refracted copies by a faceted prism in front of
// the lens, with colour dispersion).
//
// Everything is evaluated per pixel in full-resolution pixels, so tiles and proxy scales agree.
#include <algorithm>
#include <cmath>
#include <vector>

#include "MoGraph.h"

namespace strata {

using namespace mograph;

namespace {

inline double wrapc(double v, double lo, double hi, int mode) {
    // 0: black (caller checks), 1: repeat, 2: mirror, 3: clamp
    const double w = hi - lo;
    if (w <= 0) return lo;
    if (mode == 1) return lo + (v - lo - w * std::floor((v - lo) / w));
    if (mode == 2) {
        double t = std::fmod(v - lo, 2 * w);
        if (t < 0) t += 2 * w;
        return lo + (t < w ? t : 2 * w - t);
    }
    return std::clamp(v, lo, hi - 1e-6);
}

// Bilinear RGBA at render-pixel (x, y), with the edges of `box` handled by `mode`.
struct EdgeSampler {
    Sampler S;
    Box box;
    int mode = 1;
    EdgeSampler(const Image& im, const Box& b, int m) : S(im, true), box(b), mode(m) {}
    void rgba(double x, double y, float* o) const {
        if (mode == 0 || box.empty()) {
            S.rgba(x, y, o);
            return;
        }
        // keep the bilinear footprint inside: wrap the sample point, then clamp the taps
        x = wrapc(x, box.x1 + 0.5, box.x2 - 0.5 + 1e-9, mode == 3 ? 3 : mode);
        y = wrapc(y, box.y1 + 0.5, box.y2 - 0.5 + 1e-9, mode == 3 ? 3 : mode);
        x = std::clamp(x, box.x1 + 0.5, box.x2 - 0.5);
        y = std::clamp(y, box.y1 + 0.5, box.y2 - 0.5);
        S.rgba(x, y, o);
    }
};

int edgeMode(const std::string& e) { return e == "black" ? 0 : e == "mirror" ? 2 : e == "clamp" ? 3 : 1; }

// ================================================================== WhipTransition
// A whips off-screen along the angle, B whips on from the other side and overshoots a little
// before it settles. The blur follows the speed; a short crossfade at the middle hides the swap.
class WhipTransitionNode : public TransitionBase {
    // offset (in travels) of A for q in 0..1 of the first half, of B for q in 0..1 of the second
    static double offA(double q) { return q * q * q; }
    static double offB(double q, double over) {
        const double u = 1 - q;
        return -u * u * u + over * std::sin(kPi * q) * u * 0.35;
    }
    void process(RenderArgs& a, const Image& A, const Image& B, double p, Image& fx) override {
        const double f = a.frame;
        Format fm = format(a.engine, f);
        const double ang = params.num("angle", f, 0) * kPi / 180, ca = std::cos(ang), sa = std::sin(ang);
        const double W = fm.width, H = fm.height;  // render pixels
        const double travel = std::max(0.1, params.num("distance", f, 1)) * (std::fabs(ca) * W + std::fabs(sa) * H);
        const double over = std::clamp(params.num("overshoot", f, 0.3), 0.0, 2.0);
        const double shutter = std::max(0.0, params.num("shutter", f, 0.5));
        const int ns = std::clamp((int)params.num("samples", f, 24), 1, 128);
        const double xfade = std::clamp(params.num("crossfade", f, 0.06), 0.0, 0.5);
        const int mode = edgeMode(params.str("edges", "repeat"));
        // progress per frame, for the speed (and so the blur)
        const double perFrame = params.str("timing", "frames") == "progress" ? 1.0 / 24 : 1.0 / std::max(1.0, params.num("length", f, 24));
        auto offset = [&](double pp, bool isA) {
            pp = std::clamp(pp, 0.0, 1.0);
            return isA ? offA(std::min(1.0, pp * 2)) * travel : offB(std::max(0.0, pp * 2 - 1), over) * travel;
        };
        const double dp = 0.01;
        auto blurLen = [&](bool isA) {
            const double v = (offset(p + dp, isA) - offset(p - dp, isA)) / (2 * dp) * perFrame;  // px per frame
            return v * shutter;
        };
        const double oA = offset(p, true), oB = offset(p, false), bA = blurLen(true), bB = blurLen(false);
        const float wB = (float)smooth((float)((p - (0.5 - xfade)) / std::max(1e-6, 2 * xfade)));
        const Box box = fm.box();
        EdgeSampler SA(A, box, mode), SB(B, box, mode);
        for (int y = fx.bounds.y1; y < fx.bounds.y2; ++y)
            for (int x = fx.bounds.x1; x < fx.bounds.x2; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int side = 0; side < 2; ++side) {
                    const float w = side == 0 ? 1 - wB : wB;
                    if (w <= 0) continue;
                    const double o = side == 0 ? oA : oB, bl = side == 0 ? bA : bB;
                    const EdgeSampler& S = side == 0 ? SA : SB;
                    const int n = std::fabs(bl) < 0.5 ? 1 : ns;
                    float sum[4] = {0, 0, 0, 0};
                    for (int i = 0; i < n; ++i) {
                        const double t = n == 1 ? 0 : (i + 0.5) / n - 0.5;
                        const double d = o + bl * t;
                        float v[4];
                        S.rgba(x + 0.5 - d * ca, y + 0.5 - d * sa, v);
                        for (int c = 0; c < 4; ++c) sum[c] += v[c];
                    }
                    for (int c = 0; c < 4; ++c) acc[c] += w * sum[c] / n;
                }
                put(fx, x, y, acc);
            }
    }
};

// ================================================================== PrismLens
// The picture and N refracted copies around it: each copy is shifted outward along its facet's
// direction (and a little magnified), and its red, green and blue are shifted by different
// amounts (dispersion). The copies average with the picture, or add to it.
class PrismLensNode : public FxBase {
    void process(RenderArgs& a, const Image& src, Image& fx) override {
        const double f = a.frame, sc = scale(a);
        Format fm = fmt(a);
        const int facets = std::clamp((int)params.num("facets", f, 3), 1, 12);
        const double spread = params.num("spread", f, 120) * sc;
        const double ang0 = params.num("angle", f, 90) * kPi / 180;
        const double disp = std::clamp(params.num("dispersion", f, 0.15), 0.0, 1.0);
        const double zoom = std::max(0.1, params.num("copy_scale", f, 1.03));
        const float op = (float)std::clamp(params.num("copy_opacity", f, 0.6), 0.0, 1.0);
        const float centre = (float)std::clamp(params.num("centre", f, 1.0), 0.0, 1.0);
        const bool add = params.str("blend", "average") == "add";
        auto c = params.vec<2>("center", f, {fm.width / sc / 2, fm.height / sc / 2});
        const double cx = c[0] * sc, cy = c[1] * sc;
        Sampler S(src, false);
        struct Facet { double dx, dy; };
        std::vector<Facet> fs;
        for (int i = 0; i < facets; ++i) {
            const double t = ang0 + i * 2 * kPi / facets;
            fs.push_back({std::cos(t) * spread, std::sin(t) * spread});
        }
        const double k[3] = {1 + disp, 1, 1 - disp};  // red's copy shifts furthest, blue's least
        for (int y = fx.bounds.y1; y < fx.bounds.y2; ++y)
            for (int x = fx.bounds.x1; x < fx.bounds.x2; ++x) {
                const double X = x + 0.5, Y = y + 0.5;
                float acc[4] = {0, 0, 0, 0};
                float norm = 0;
                if (centre > 0) {
                    float v[4];
                    S.rgba(X, Y, v);
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += centre * v[ch];
                    norm += centre;
                }
                for (const Facet& fc : fs) {
                    float v[4];
                    for (int ch = 0; ch < 3; ++ch) {
                        // where this channel of the copy comes from: undo the shift, then the magnification
                        const double sx = cx + (X - fc.dx * k[ch] - cx) / zoom, sy = cy + (Y - fc.dy * k[ch] - cy) / zoom;
                        v[ch] = S.at(sx, sy, ch);
                    }
                    v[3] = S.at(cx + (X - fc.dx - cx) / zoom, cy + (Y - fc.dy - cy) / zoom, 3);
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += op * v[ch];
                    norm += op;
                }
                if (!add && norm > 0)
                    for (int ch = 0; ch < 4; ++ch) acc[ch] /= norm;
                if (add) acc[3] = std::min(1.0f, acc[3]);
                put(fx, x, y, acc);
            }
    }
};

}  // namespace

void registerOpticsNodes() {
    auto& r = NodeRegistry::instance();
    r.add("WhipTransition", [] { return std::make_unique<WhipTransitionNode>(); });
    r.add("PrismLens", [] { return std::make_unique<PrismLensNode>(); });
}

}  // namespace strata
