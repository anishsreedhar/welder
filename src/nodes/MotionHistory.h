// Frame history for effects that depend on earlier frames (trails, advected textures, feedback).
//
// Welder renders any frame on demand and in any order (scrubbing, a farm render of frames
// 200-300), so a node can't keep "the previous frame I rendered": that would depend on the order
// frames were looked at. Instead frame f's state is DEFINED as a chain of steps from an anchor
// frame: state(anchor) = step(anchor, null), state(k) = step(k, state(k - 1)). Chain caches the
// last few states by a hash of the whole chain (everything each step depended on), so:
//   - playing forward costs one step per frame (the previous frame's state is in the cache),
//   - jumping (frame 100 back to 1, or forward past the cache) rebuilds from the nearest cached
//     ancestor or the anchor: never a stale state, never ghosting, the same pixels as a fresh render,
//   - a change anywhere earlier in the chain (a knob, an upstream node) changes the key, so it
//     re-renders; nodes also return chainKey from Node::pastHash so the engine's own caches agree.
// Resolution, proxy scale and format belong in `base`, so changing them starts new chains.
//
// Also here: motion vectors read from an image (VectorGenerator's forward / backward channels or an
// RG flowmap), and the earlier frames a trail reads.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/Node.h"
#include "Retime.h"

namespace strata::history {

// ---------------------------------------------------------------- the chain
// The anchor for frame f: chains start at `start` and, with period > 0, start again every period
// frames (so a long shot never chains back thousands of frames). Frames before start use start.
inline int anchorFor(int f, int start, int period) {
    if (f <= start || period <= 0) return start;
    return start + ((f - start) / period) * period;
}

template <class State>
class Chain {
public:
    using StepHash = std::function<uint64_t(int k)>;  // what step k depends on (inputs at k, ...)
    using Step = std::function<std::shared_ptr<const State>(int k, const State* prev)>;  // prev null at the anchor

    explicit Chain(size_t capacity = 3) : cache_(capacity) {}

    // The key of the chain anchor..f (f clamped to the anchor; at most maxSteps steps back from f).
    uint64_t key(int anchor, int f, uint64_t base, const StepHash& stepHash, int maxSteps = 20000) const {
        const int first = firstFrame(anchor, f, maxSteps);
        uint64_t h = hashCombine(base, (uint64_t)(int64_t)first);
        for (int k = first; k <= std::max(f, first); ++k) h = hashCombine(h, stepHash(k));
        return h;
    }

    // The state at frame f. Throws RenderCancelled when `cancelled` says so between steps.
    std::shared_ptr<const State> at(int anchor, int f, uint64_t base, const StepHash& stepHash, const Step& step,
                                    const std::function<bool()>& cancelled = {}, int maxSteps = 20000) {
        const int first = firstFrame(anchor, f, maxSteps);
        const int steps = std::max(f, first) - first;
        std::vector<uint64_t> keys((size_t)steps + 1);
        uint64_t h = hashCombine(base, (uint64_t)(int64_t)first);
        for (int k = 0; k <= steps; ++k) keys[(size_t)k] = h = hashCombine(h, stepHash(first + k));
        int have = -1;
        std::shared_ptr<const State> S;
        for (int k = steps; k >= 0 && !S; --k)
            if ((S = cache_.peek(keys[(size_t)k]))) have = k;
        for (int k = have + 1; k <= steps; ++k) {
            if (cancelled && cancelled()) throw RenderCancelled();
            auto prev = S;
            S = cache_.get(keys[(size_t)k], [&] {
                ++steps_;
                return step(first + k, k == 0 ? nullptr : prev.get());
            });
        }
        return S;
    }

    void clear() { cache_.clear(); }
    // Steps run so far (for tests and profiling).
    long stepsRun() const { return steps_.load(); }

private:
    static int firstFrame(int anchor, int f, int maxSteps) { return std::max(anchor, f - std::max(0, maxSteps)); }
    mutable retime::SharedCache<State> cache_;
    std::atomic<long> steps_{0};
};

// ---------------------------------------------------------------- motion vectors
// A 2D vector per pixel of `box`, in render pixels per frame (+x right, +y up).
struct Motion {
    Box box;
    int w = 0, h = 0;
    std::vector<float> u, v;
    bool empty() const { return w <= 0 || h <= 0; }
    // bilinear at a continuous image position (pixel centres at +0.5), edges extended
    void at(double x, double y, float& ou, float& ov) const {
        if (empty()) {
            ou = ov = 0;
            return;
        }
        const double fx = std::clamp(x - box.x1 - 0.5, 0.0, (double)(w - 1)), fy = std::clamp(y - box.y1 - 0.5, 0.0, (double)(h - 1));
        const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float tx = (float)(fx - x0), ty = (float)(fy - y0);
        auto bl = [&](const std::vector<float>& p) {
            const float a = p[(size_t)y0 * w + x0] + (p[(size_t)y0 * w + x1] - p[(size_t)y0 * w + x0]) * tx;
            const float b = p[(size_t)y1 * w + x0] + (p[(size_t)y1 * w + x1] - p[(size_t)y1 * w + x0]) * tx;
            return a + (b - a) * ty;
        };
        ou = bl(u);
        ov = bl(v);
    }
    // speed (pixels per frame) at pixel (x, y) of the box
    float speed(int x, int y) const {
        if (!box.contains(x, y)) return 0;
        const size_t i = (size_t)(y - box.y1) * w + (x - box.x1);
        return std::hypot(u[i], v[i]);
    }
};

enum class Direction { Forward, Backward };

// The motion in `img` over `box` (zero outside img): forward.u / .v (or backward.u / .v) when the
// image has them, otherwise R / G as a flowmap (0.5 = still, 0 / 1 = -rgSpeed / +rgSpeed pixels per
// frame). Times `mult`. With neither, all zero (`found` false): a node falls back to "no motion".
inline Motion readMotion(const Image& img, const Box& box, Direction dir, double mult = 1, double rgSpeed = 8,
                         bool* found = nullptr) {
    Motion m;
    m.box = box;
    m.w = box.width();
    m.h = box.height();
    m.u.assign((size_t)std::max(0, m.w) * std::max(0, m.h), 0.0f);
    m.v.assign(m.u.size(), 0.0f);
    const std::string p = dir == Direction::Forward ? "forward." : "backward.";
    const int cu = img.ch(p + "u"), cv = img.ch(p + "v"), R = img.ch("R"), G = img.ch("G");
    const bool vec = cu >= 0 && cv >= 0, rg = !vec && R >= 0 && G >= 0;
    if (found) *found = vec || rg;
    if (!vec && !rg) return m;
    const Box in = box.intersect(img.bounds);
    for (int y = in.y1; y < in.y2; ++y)
        for (int x = in.x1; x < in.x2; ++x) {
            const float* q = img.px(x, y);
            const size_t i = (size_t)(y - box.y1) * m.w + (x - box.x1);
            if (vec) {
                m.u[i] = (float)(q[cu] * mult);
                m.v[i] = (float)(q[cv] * mult);
            } else {
                m.u[i] = (float)((q[R] - 0.5) * 2 * rgSpeed * mult);
                m.v[i] = (float)((q[G] - 0.5) * 2 * rgSpeed * mult);
            }
        }
    return m;
}

// ---------------------------------------------------------------- trails
// The earlier frames a trail of n frames reads at frame f, newest first: f-1, f-2, ... f-n, minus
// any before `first` (the start of the input). Frame `first` itself has none: the trail is empty
// there, never garbage.
inline std::vector<int> pastFrames(int f, int n, int first) {
    std::vector<int> r;
    for (int k = 1; k <= std::max(0, n) && f - k >= first; ++k) r.push_back(f - k);
    return r;
}

}  // namespace strata::history
