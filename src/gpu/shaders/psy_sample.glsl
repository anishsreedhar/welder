// Sample.h's sample() for the Psychic kernels: bilinear (kind 1) or BC-cubic (kind 2, any B, C
// without negative lobes) at a continuous position in the source buffer's local pixels (centres
// at +0.5). black = false: extended edges (SampleOpts::blackOutside = false: the position and the
// taps clamp to the buffer); true: taps outside the buffer count as 0. Needs `float src[]`
// declared before it is included.

struct PsyTaps {
    ivec2 base;  // the first tap
    vec4 wx, wy;
    int n;       // taps per axis (2 or 4)
};

void psyBcWeights4(float t, float B, float C, out vec4 w) {
    float n3 = (12.0 - 9.0 * B - 6.0 * C) / 6.0, n2 = (-18.0 + 12.0 * B + 6.0 * C) / 6.0, n0 = (6.0 - 2.0 * B) / 6.0;
    float f3 = (-B - 6.0 * C) / 6.0, f2 = (6.0 * B + 30.0 * C) / 6.0, f1 = (-12.0 * B - 48.0 * C) / 6.0, f0 = (8.0 * B + 24.0 * C) / 6.0;
    float a = 1.0 + t, b = t, c = 1.0 - t, d = 2.0 - t;
    w.x = ((f3 * a + f2) * a + f1) * a + f0;
    w.y = (n3 * b + n2) * b * b + n0;
    w.z = c < 1.0 ? (n3 * c + n2) * c * c + n0 : ((f3 * c + f2) * c + f1) * c + f0;
    w.w = d < 2.0 ? ((f3 * d + f2) * d + f1) * d + f0 : 0.0;
}

PsyTaps psyTaps(float x, float y, int sw, int sh, int kind, float B, float C, bool black) {
    if (!black) {
        x = clamp(x, 0.5, float(sw) - 0.5);
        y = clamp(y, 0.5, float(sh) - 0.5);
    }
    float px = x - 0.5, py = y - 0.5;
    int ix = int(floor(px)), iy = int(floor(py));
    float tx = px - float(ix), ty = py - float(iy);
    PsyTaps t;
    if (kind == 1) {
        t.base = ivec2(ix, iy);
        t.wx = vec4(1.0 - tx, tx, 0, 0);
        t.wy = vec4(1.0 - ty, ty, 0, 0);
        t.n = 2;
    } else {
        t.base = ivec2(ix - 1, iy - 1);
        psyBcWeights4(tx, B, C, t.wx);
        psyBcWeights4(ty, B, C, t.wy);
        t.n = 4;
    }
    return t;
}

float psyGather(PsyTaps t, int sw, int sh, int sn, int ch, bool black) {
    float acc = 0.0;
    for (int j = 0; j < t.n; ++j) {
        int yy = t.base.y + j;
        if (yy < 0 || yy >= sh) {
            if (black) continue;
            yy = clamp(yy, 0, sh - 1);
        }
        float r = 0.0;
        for (int i = 0; i < t.n; ++i) {
            int xx = t.base.x + i;
            if (xx < 0 || xx >= sw) {
                if (black) continue;
                xx = clamp(xx, 0, sw - 1);
            }
            r += t.wx[i] * src[(yy * sw + xx) * sn + ch];
        }
        acc += t.wy[j] * r;
    }
    return acc;
}
