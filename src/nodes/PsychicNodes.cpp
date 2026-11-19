// Psychic Rendering: motion-coupled looks that grow out of the plate instead of sitting on it.
//   FlowBreath   surfaces breathe: the image is pushed around by curl noise (swirls that neither
//                bunch up nor tear), the noise rides along with the plate's motion (vectors from
//                VectorGenerator) so the ripples stick to moving surfaces, and the push is
//                strongest on textured areas (foliage, bark, fabric) while flat ones (sky) hold
//   FractalFold  kaleidoscopic geometry folded out of the plate's own texture: recursive polar
//                folds (a KIFS) around a centre or on a grid of tiles, mixed back in where the
//                plate has detail (or brightness), so the patterns emerge from the surfaces
//
// Shared pieces: NoiseLib (simplex / curl), DetailMask, MotionHistory (the chained, scrub-safe
// position map). Everything is evaluated in full-resolution units, so proxy renders and tiling
// don't change the look.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "../core/Comp.h"
#include "../core/Engine.h"
#include "DetailMask.h"
#include "MotionHistory.h"
#include "NodeUtils.h"
#include "NoiseLib.h"
#include "Sample.h"

namespace strata {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline float smoothstepf(double e0, double e1, double v) {
    if (e1 <= e0) return v >= e0 ? 1.0f : 0.0f;
    const double t = std::clamp((v - e0) / (e1 - e0), 0.0, 1.0);
    return (float)(t * t * (3 - 2 * t));
}

uint64_t boxHash(uint64_t h, const Box& b) {
    h = hashCombine(h, ((uint64_t)(uint32_t)b.x1 << 32) ^ (uint32_t)b.y1);
    return hashCombine(h, ((uint64_t)(uint32_t)b.x2 << 32) ^ (uint32_t)b.y2);
}

std::vector<int> channelMap(const Image& src, const Image& out) {
    std::vector<int> m(out.nch());
    for (int c = 0; c < out.nch(); ++c) m[c] = src.ch(out.channels[c]);
    return m;
}

// ================================================================== FlowBreath
// curl2's speed for one octave is about this on average (RMS), so "strength" means pixels
// (measured in the tests); pushes are limited to kMaxPush x strength
constexpr float kCurlRms = 2.9f;
constexpr float kMaxPush = 2.5f;

// Where each pixel of a frame was on the anchor frame, as an offset (render pixels) on a coarse
// grid over the frame.
struct PosMap {
    Box D;
    double cell = 4;
    int gw = 0, gh = 0;
    std::vector<float> dx, dy;
    void at(double x, double y, float& ox, float& oy) const {
        if (gw <= 0 || gh <= 0) {
            ox = oy = 0;
            return;
        }
        const double fx = std::clamp((x - D.x1) / cell - 0.5, 0.0, (double)(gw - 1));
        const double fy = std::clamp((y - D.y1) / cell - 0.5, 0.0, (double)(gh - 1));
        const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
        const float tx = (float)(fx - x0), ty = (float)(fy - y0);
        auto bl = [&](const std::vector<float>& p) {
            const float a = p[(size_t)y0 * gw + x0] + (p[(size_t)y0 * gw + x1] - p[(size_t)y0 * gw + x0]) * tx;
            const float b = p[(size_t)y1 * gw + x0] + (p[(size_t)y1 * gw + x1] - p[(size_t)y1 * gw + x0]) * tx;
            return a + (b - a) * ty;
        };
        ox = bl(dx);
        oy = bl(dy);
    }
};

class FlowBreathNode : public Node {
public:
    int maxInputs() const override { return 3; }
    std::string inputLabel(int i) const override { return i == 0 ? "src" : i == 1 ? "vectors" : "mask"; }
    bool timeDependent() const override { return true; }
    Format format(Engine& e, double f) override { return input(0) ? input(0)->format(e, f) : Node::format(e, f); }
    Box dod(Engine& e, double f) override { return input(0) ? e.dod(input(0), f) : Box(); }
    ChannelSet channels(Engine& e, double f) override { return input(0) ? e.channels(input(0), f) : rgbaChannels(); }
    Box roi(Engine& e, int idx, const Box& req, double f) override {
        if (idx == 1) return Box();  // the vectors come in whole frames (renderRegion, in the chain)
        if (idx == 2) return req;
        const int p = pad(e, f);
        return req.pad(p, p);
    }
    int identityInput(double f) override {
        return !input(0) || params.num("strength", f, 6) == 0 || params.num("mix", f, 1) <= 0 ? 0 : -1;
    }

    uint64_t pastHash(Engine& e, double f) override {
        if (!adheres(f)) return 0;
        const Anchors A = anchors(e, (int)std::lround(f));
        uint64_t h = keyFor(e, A.cur, (int)std::lround(f));
        if (A.wPrev > 0) h = hashCombine(h, keyFor(e, A.prev, (int)std::lround(f)));
        return h;
    }

    void render(RenderArgs& a, Image& out) override {
        std::fill(out.data.begin(), out.data.end(), 0.0f);
        if (!input(0)) return;
        Engine& e = a.engine;
        const double f = a.frame, sc = e.scale();
        const Box R = a.region;
        const int P = pad(e, f);
        ImagePtr src = a.fetch(input(0), R.pad(P, P));
        const std::vector<int> map = channelMap(*src, out);

        const double size = std::max(1.0, params.num("flow_scale", f, 180));
        const double fpsv = comp ? std::max(1.0, comp->fps) : 24.0;
        const float z = (float)(f / fpsv * params.num("flow_speed", f, 0.3));
        const float strength = (float)(params.num("strength", f, 6) * sc);
        const float octaves = (float)std::clamp(params.num("octaves", f, 2), 1.0, 6.0);
        const uint32_t seed = (uint32_t)params.num("seed", f, 0);
        const double lock = std::clamp(params.num("detail_lock", f, 0.7), 0.0, 1.0);
        const double adh = std::clamp(params.num("adhesion", f, 1), 0.0, 1.0);
        const std::string view = params.str("view", "result");

        std::vector<float> detail;
        if (lock > 0 || view == "detail mask") {
            fx::DetailOptions o;
            o.scale = sc;
            o.radius = params.num("detail_size", f, 4);
            o.gain = params.num("detail_gain", f, 12);
            detail = fx::detailMask(*src, R, o);
        }
        // the anchor position maps (two while a re-anchor cross-fades)
        std::shared_ptr<const PosMap> cur, prev;
        float wPrev = 0;
        if (adheres(f) && adh > 0) {
            const int k = (int)std::lround(f);
            const Anchors A = anchors(e, k);
            cur = posMap(e, A.cur, k);
            if (A.wPrev > 0) prev = posMap(e, A.prev, k), wPrev = A.wPrev;
        }
        MaskMix mm = MaskMix::setup(a, this, 2);
        const Filter fl = parseFilter(params.str("filter", "cubic"));
        SampleOpts so;
        so.blackOutside = false;  // the edges extend: a push near the border pulls in border pixels, not black
        const int RW = R.width();
        const float norm = 1.0f / kCurlRms;
        const float maxPush = kMaxPush * strength;
        auto push = [&](const PosMap* pm, double X, double Y, float* d) {
            double qx = X, qy = Y;
            if (pm) {
                float ox, oy;
                pm->at(X, Y, ox, oy);
                qx += adh * ox, qy += adh * oy;
            }
            noise::curl2((float)(qx / sc / size), (float)(qy / sc / size), z, seed, d, octaves);
        };
        auto rows = [&](int b, int e2) {  // (tiles already render in parallel)
            for (int j = b; j < e2; ++j) {
                Engine::cancelPoint();
                const int y = R.y1 + j;
                for (int i = 0; i < RW; ++i) {
                    const int x = R.x1 + i;
                    const double X = x + 0.5, Y = y + 0.5;
                    const float dm = detail.empty() ? 1.0f : detail[(size_t)j * RW + i];
                    float* o = out.px(x, y);
                    if (view == "detail mask") {
                        for (int c = 0; c < out.nch(); ++c) o[c] = dm;
                        continue;
                    }
                    float d[2], dp[2];
                    push(cur.get(), X, Y, d);
                    if (prev) {
                        push(prev.get(), X, Y, dp);
                        d[0] += (dp[0] - d[0]) * wPrev;
                        d[1] += (dp[1] - d[1]) * wPrev;
                    }
                    const float w = (float)((1 - lock) + lock * dm) * mm.weight(x, y);
                    float px = d[0] * norm * strength * w, py = d[1] * norm * strength * w;
                    const float len = std::hypot(px, py);
                    if (len > maxPush) px *= maxPush / len, py *= maxPush / len;
                    if (view == "displacement") {
                        const float s = strength > 0 ? 0.5f / strength : 0.0f;
                        const float v[4] = {0.5f + px * s, 0.5f + py * s, 0.5f, 1.0f};
                        for (int c = 0; c < out.nch(); ++c) o[c] = c < 4 ? v[c] : 0.0f;
                        continue;
                    }
                    sample(*src, X - px, Y - py, fl, o, map, so);
                }
            }
        };
        rows(0, R.height());
    }

private:
    history::Chain<PosMap> chains_[2]{history::Chain<PosMap>(3), history::Chain<PosMap>(3)};

    struct Anchors {
        int cur = 1, prev = 1;
        float wPrev = 0;  // how much of the previous anchor's push still shows (cross-fade)
    };

    int start() const { return (int)std::lround(params.num("reference", 0.0, comp ? comp->firstFrame : 1)); }
    bool adheres(double f) const { return input(1) && params.num("adhesion", f, 1) > 0; }

    int pad(Engine& e, double f) const {
        const double sc = e.scale();
        fx::DetailOptions o;
        o.scale = sc;
        o.radius = params.num("detail_size", f, 4);
        return (int)std::ceil(kMaxPush * std::fabs(params.num("strength", f, 6)) * sc) + fx::detailPad(o) + 3;
    }

    // Chains restart every `reanchor` frames, cross-fading from the previous one over the period,
    // so no frame chains back further than two periods.
    Anchors anchors(Engine&, int k) const {
        const int s = start(), period = std::max(0, (int)std::lround(params.num("reanchor", 0.0, 48)));
        Anchors A;
        A.cur = history::anchorFor(k, s, period);
        if (period > 0 && A.cur - period >= s) {
            A.prev = A.cur - period;
            const float t = std::min(1.0f, (float)(k - A.cur + 1) / (float)period);
            A.wPrev = 1 - t * t * (3 - 2 * t);
        } else {
            A.prev = A.cur;
        }
        return A;
    }

    uint64_t base(Engine& e, int anchor) {
        const Box D = input(0) ? e.dod(input(0), anchor) : Box();
        uint64_t h = boxHash(0x7f4a7c159e3779b9ull, D);
        return hashCombine(h, (uint64_t)std::llround(e.scale() * 1e6));
    }
    history::Chain<PosMap>::StepHash stepHash(Engine& e) {
        return [this, &e](int k) { return hashCombine(e.hash(input(1), k), e.hash(input(1), k - 1)); };
    }
    uint64_t keyFor(Engine& e, int anchor, int k) { return chains_[slot(anchor)].key(anchor, k, base(e, anchor), stepHash(e)); }
    int slot(int anchor) const {
        const int period = std::max(1, (int)std::lround(params.num("reanchor", 0.0, 48)));
        return (int)(((long long)(anchor - start()) / period) & 1);
    }

    std::shared_ptr<const PosMap> posMap(Engine& e, int anchor, int k) {
        const double sc = e.scale();
        return chains_[slot(anchor)].at(
            anchor, k, base(e, anchor), stepHash(e),
            [&](int j, const PosMap* prev) {
                auto m = std::make_shared<PosMap>();
                m->D = e.dod(input(0), j);
                m->cell = std::max(1.0, std::round(4 * sc));
                m->gw = std::max(2, (int)std::ceil(m->D.width() / m->cell));
                m->gh = std::max(2, (int)std::ceil(m->D.height() / m->cell));
                m->dx.assign((size_t)m->gw * m->gh, 0.0f);
                m->dy.assign(m->dx.size(), 0.0f);
                if (!prev || m->D.empty()) return std::shared_ptr<const PosMap>(m);
                // where each grid point was a frame ago: backward vectors of this frame, or else the
                // forward vectors of the frame before, inverted (q + v(q) = x)
                ImagePtr vi = e.renderRegion(input(1), j, m->D);
                const bool hasBack = vi->ch("backward.u") >= 0 && vi->ch("backward.v") >= 0;
                history::Motion mv;
                if (hasBack) {
                    mv = history::readMotion(*vi, m->D, history::Direction::Backward);
                } else {
                    ImagePtr vp = e.renderRegion(input(1), j - 1, m->D);
                    mv = history::readMotion(*vp, m->D, history::Direction::Forward);
                }
                retime::parallelFor(m->gh, [&](int b, int e2) {
                    for (int gy = b; gy < e2; ++gy)
                        for (int gx = 0; gx < m->gw; ++gx) {
                            const double x = m->D.x1 + (gx + 0.5) * m->cell, y = m->D.y1 + (gy + 0.5) * m->cell;
                            double qx = x, qy = y;
                            float u, v;
                            if (hasBack) {
                                mv.at(x, y, u, v);
                                qx = x + u, qy = y + v;
                            } else {
                                for (int it = 0; it < 3; ++it) {
                                    mv.at(qx, qy, u, v);
                                    qx = x - u, qy = y - v;
                                }
                            }
                            float ox, oy;
                            prev->at(qx, qy, ox, oy);
                            m->dx[(size_t)gy * m->gw + gx] = (float)(qx - x + ox);
                            m->dy[(size_t)gy * m->gw + gx] = (float)(qy - y + oy);
                        }
                });
                return std::shared_ptr<const PosMap>(m);
            },
            [&e] { return e.cancelled(); });
    }
};

// ================================================================== FractalFold
class FractalFoldNode : public Node {
public:
    int maxInputs() const override { return 2; }
    std::string inputLabel(int i) const override { return i == 0 ? "src" : "mask"; }
    Format format(Engine& e, double f) override { return input(0) ? input(0)->format(e, f) : Node::format(e, f); }
    Box dod(Engine& e, double f) override {
        if (!input(0)) return Box();
        return format(e, f).box().unite(e.dod(input(0), f));
    }
    ChannelSet channels(Engine& e, double f) override { return input(0) ? e.channels(input(0), f) : rgbaChannels(); }
    Box roi(Engine& e, int idx, const Box& req, double f) override {
        if (idx == 0) return input(0) ? e.dod(input(0), f).unite(req.pad(detailPad(e, f), detailPad(e, f))) : Box();
        return req;
    }
    int identityInput(double f) override { return !input(0) || params.num("mix", f, 1) <= 0 ? 0 : -1; }

    void render(RenderArgs& a, Image& out) override {
        std::fill(out.data.begin(), out.data.end(), 0.0f);
        if (!input(0)) return;
        Engine& e = a.engine;
        const double f = a.frame, sc = e.scale();
        const Box sb = e.dod(input(0), f);
        if (sb.empty()) return;
        const Box R = a.region;
        const int dp = detailPad(e, f);
        ImagePtr src = a.fetch(input(0), sb.unite(R.pad(dp, dp)));
        const std::vector<int> map = channelMap(*src, out);
        const Format fm = format(e, f);
        const Box fb = fm.box().intersect(sb);

        // the fold, in full-resolution units
        const int N = std::clamp((int)params.num("symmetry", f, 6), 1, 64);
        const double seg = 2 * kPi / N;
        const double tile = std::max(0.0, params.num("tile_size", f, 0));
        const double rot = params.num("rotation", f, 0) * kPi / 180;
        const auto cxy = params.vec<2>("center", f, {fm.width / sc / 2.0, fm.height / sc / 2.0});
        const int iters = std::clamp((int)params.num("iterations", f, 3), 1, 12);
        const double k = std::clamp(params.num("fold_scale", f, 1.6), 1.0, 4.0);
        const double L = tile > 0 ? tile / 2 : std::min(fm.width, fm.height) / sc / 2;
        const double off = params.num("fold_offset", f, 0.3) * L;
        const double zoom = std::max(1e-3, params.num("zoom", f, 1));
        const double back = 1.0 / (std::pow(k, iters - 1) * zoom);
        const double cr = std::cos(rot), sr = std::sin(rot);

        // where the result shows: detail, brightness or everywhere, past a threshold
        const std::string drive = params.str("drive", "detail");
        const double th = params.num("detail_threshold", f, 0.3), soft = std::max(0.0, params.num("softness", f, 0.25));
        std::vector<float> detail;
        if (drive == "detail") {
            fx::DetailOptions o;
            o.scale = sc;
            o.radius = params.num("detail_size", f, 4);
            detail = fx::detailMask(*src, R, o);
        }
        const int iR = src->ch("R"), iG = src->ch("G"), iB = src->ch("B");
        MaskMix mm = MaskMix::setup(a, this, 1);
        const bool weightView = params.str("view", "result") == "weight";
        const Filter fl = parseFilter(params.str("filter", "cubic"));
        const double W = fb.width(), H = fb.height();
        auto mirror = [](double v, double lo, double len) {
            if (len <= 1) return lo + 0.5;
            const double t = v - lo, m = t - 2 * len * std::floor(t / (2 * len));
            return lo + (m > len ? 2 * len - m : m);
        };
        const int RW = R.width(), n = out.nch();
        auto rows = [&](int b, int e2) {  // (tiles already render in parallel)
            std::vector<float> folded(n);
            for (int j = b; j < e2; ++j) {
                Engine::cancelPoint();
                const int y = R.y1 + j;
                for (int i = 0; i < RW; ++i) {
                    const int x = R.x1 + i;
                    const double X = (x + 0.5) / sc, Y = (y + 0.5) / sc;
                    // the weight first: nothing to fold where it is 0
                    float w;
                    if (drive == "detail") w = detail[(size_t)j * RW + i];
                    else if (drive == "luminance" && src->bounds.contains(x, y) && iR >= 0 && iG >= 0 && iB >= 0) {
                        const float* p = src->px(x, y);
                        const double Yl = std::max(0.0, 0.2126 * p[iR] + 0.7152 * p[iG] + 0.0722 * p[iB]);
                        w = (float)(Yl / (1 + Yl) * 2);
                    } else w = drive == "everywhere" ? 1.0f : 0.0f;
                    if (drive != "everywhere") w = smoothstepf(th - soft / 2, th + soft / 2, w);
                    w *= mm.weight(x, y);
                    float* o = out.px(x, y);
                    if (weightView) {
                        for (int c = 0; c < n; ++c) o[c] = w;
                        continue;
                    }
                    const float* s0 = src->bounds.contains(x, y) ? src->px(x, y) : nullptr;
                    for (int c = 0; c < n; ++c) o[c] = s0 && map[c] >= 0 ? s0[map[c]] : 0.0f;
                    if (w <= 0) continue;
                    // into the fold's frame: around the centre, or the centre of this pixel's tile
                    double ox = cxy[0], oy = cxy[1];
                    double vx = X - ox, vy = Y - oy;
                    if (tile > 0) {
                        const double tx = std::round(vx / tile), ty = std::round(vy / tile);
                        vx -= tx * tile, vy -= ty * tile;
                        ox += tx * tile, oy += ty * tile;
                    }
                    double ux = cr * vx + sr * vy, uy = -sr * vx + cr * vy;  // turn by -rotation
                    for (int it = 0; it < iters; ++it) {
                        // polar fold: theta' = |mod(theta, 2 pi / N) - pi / N|
                        const double r = std::hypot(ux, uy);
                        double t = std::atan2(uy, ux);
                        t -= seg * std::floor(t / seg);
                        t = std::fabs(t - seg / 2);
                        ux = r * std::cos(t), uy = r * std::sin(t);
                        if (it + 1 < iters) {  // the IFS step: mirror, grow, shift, turn
                            ux = std::fabs(ux) * k - off, uy = std::fabs(uy) * k - off;
                            const double nx = cr * ux + sr * uy, ny = -sr * ux + cr * uy;
                            ux = nx, uy = ny;
                        }
                    }
                    ux *= back, uy *= back;
                    double sx = (ox + cr * ux - sr * uy) * sc, sy = (oy + sr * ux + cr * uy) * sc;  // back to the plate
                    if (!fb.empty() && (sx < fb.x1 || sx > fb.x2 || sy < fb.y1 || sy > fb.y2)) {
                        sx = mirror(sx, fb.x1, W);
                        sy = mirror(sy, fb.y1, H);
                    }
                    sample(*src, sx, sy, fl, folded.data(), map);
                    for (int c = 0; c < n; ++c) o[c] += (folded[c] - o[c]) * w;
                }
            }
        };
        rows(0, R.height());
    }

private:
    int detailPad(Engine& e, double f) const {
        fx::DetailOptions o;
        o.scale = e.scale();
        o.radius = params.num("detail_size", f, 4);
        return fx::detailPad(o);
    }
};

}  // namespace

void registerPsychicNodes() {
    auto& r = NodeRegistry::instance();
    r.add("FlowBreath", [] { return std::make_unique<FlowBreathNode>(); });
    r.add("FractalFold", [] { return std::make_unique<FractalFoldNode>(); });
}

}  // namespace strata
