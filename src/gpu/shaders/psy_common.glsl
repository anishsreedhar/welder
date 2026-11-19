// Shared GPU maths for the Psychic Rendering kernels (psy_*.comp): the GPU twins of
// nodes/NoiseLib (simplex with derivatives, fractal sum, curl2), color/OkLab.h, Sample.h's
// bilinear / BC-cubic lookup with extended edges, and DetailMask's luminance. Each follows its
// CPU version operation by operation, so the GPU and CPU paths render the same picture (to float
// rounding; the tests compare them).

// ---------------------------------------------------------------- hash and simplex noise
uint psyMix(uint h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
uint psyHash3(int x, int y, int z, uint s) {
    return psyMix(s * 0x165667b1u + uint(x) * 0x8da6b343u + uint(y) * 0xd8163841u + uint(z) * 0xcb1ab31fu);
}
const vec3 kPsyG3[16] = vec3[16](vec3(1, 1, 0), vec3(-1, 1, 0), vec3(1, -1, 0), vec3(-1, -1, 0), vec3(1, 0, 1), vec3(-1, 0, 1),
                                 vec3(1, 0, -1), vec3(-1, 0, -1), vec3(0, 1, 1), vec3(0, -1, 1), vec3(0, 1, -1), vec3(0, -1, -1),
                                 vec3(1, 1, 0), vec3(0, -1, 1), vec3(-1, 1, 0), vec3(0, -1, -1));
int psyFl(float v) {
    int i = int(v);
    return v < float(i) ? i - 1 : i;
}

// (value, d/dx, d/dy, d/dz), the value roughly -1..1
vec4 psySimplex3d(float x, float y, float z, uint seed) {
    const float F3 = 1.0 / 3.0, G3 = 1.0 / 6.0, K = 72.0;
    float s = (x + y + z) * F3;
    int i = psyFl(x + s), j = psyFl(y + s), k = psyFl(z + s);
    float t = float(i + j + k) * G3;
    float x0 = x - (float(i) - t), y0 = y - (float(j) - t), z0 = z - (float(k) - t);
    ivec3 c1, c2;
    if (x0 >= y0) {
        if (y0 >= z0) c1 = ivec3(1, 0, 0), c2 = ivec3(1, 1, 0);
        else if (x0 >= z0) c1 = ivec3(1, 0, 0), c2 = ivec3(1, 0, 1);
        else c1 = ivec3(0, 0, 1), c2 = ivec3(1, 0, 1);
    } else {
        if (y0 < z0) c1 = ivec3(0, 0, 1), c2 = ivec3(0, 1, 1);
        else if (x0 < z0) c1 = ivec3(0, 1, 0), c2 = ivec3(0, 1, 1);
        else c1 = ivec3(0, 1, 0), c2 = ivec3(1, 1, 0);
    }
    vec4 r = vec4(0);
    for (int c = 0; c < 4; ++c) {
        ivec3 ci = c == 0 ? ivec3(0) : c == 1 ? c1 : c == 2 ? c2 : ivec3(1);
        float off = float(c) * G3;
        float dx = x0 - float(ci.x) + off, dy = y0 - float(ci.y) + off, dz = z0 - float(ci.z) + off;
        float tt = 0.5 - dx * dx - dy * dy - dz * dz;
        if (tt <= 0.0) continue;
        vec3 g = kPsyG3[psyHash3(i + ci.x, j + ci.y, k + ci.z, seed) & 15u];
        float gd = g.x * dx + g.y * dy + g.z * dz;
        float t2 = tt * tt, t4 = t2 * t2, t3 = t2 * tt;
        r.x += t4 * gd;
        r.y += t4 * g.x - 8.0 * t3 * gd * dx;
        r.z += t4 * g.y - 8.0 * t3 * gd * dy;
        r.w += t4 * g.z - 8.0 * t3 * gd * dz;
    }
    return r * K;
}

vec4 psySimplexFbm3d(float x, float y, float z, uint seed, float octaves) {
    const float lac = 2.0, gain = 0.5;
    octaves = clamp(octaves, 0.0, 16.0);
    vec4 r = vec4(0);
    float freq = 1.0, amp = 1.0;
    int n = int(ceil(octaves));
    for (int o = 0; o < n; ++o) {
        float w = min(1.0, octaves - float(o));
        vec4 d = psySimplex3d(x * freq + float(o) * 17.31, y * freq - float(o) * 9.77, z * freq + float(o) * 3.13, seed + uint(o) * 0x9e3779b9u);
        float a = amp / freq * w;
        r.x += a * d.x;
        r.y += a * freq * d.y;
        r.z += a * freq * d.z;
        r.w += a * freq * d.w;
        freq *= lac;
        amp *= gain;
    }
    return r;
}

vec2 psyCurl2(float x, float y, float z, uint seed, float octaves) {
    vec4 p = psySimplexFbm3d(x, y, z, seed, octaves);
    return vec2(p.z, -p.y);
}

// ---------------------------------------------------------------- OKLab (linear Rec.709 in)
vec3 psyCbrt(vec3 v) { return sign(v) * pow(abs(v), vec3(1.0 / 3.0)); }
vec3 psyToOklab(vec3 c) {
    vec3 lms = vec3(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b,
                    0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b,
                    0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    lms = psyCbrt(lms);
    return vec3(0.2104542553 * lms.x + 0.7936177850 * lms.y - 0.0040720468 * lms.z,
                1.9779984951 * lms.x - 2.4285922050 * lms.y + 0.4505937099 * lms.z,
                0.0259040371 * lms.x + 0.7827717662 * lms.y - 0.8086757660 * lms.z);
}
vec3 psyFromOklab(vec3 L) {
    vec3 lms = vec3(0.999999998451 * L.x + 0.396337792174 * L.y + 0.215803758061 * L.z,
                    1.000000008882 * L.x - 0.105561342324 * L.y - 0.063854174772 * L.z,
                    1.000000054672 * L.x - 0.089484182095 * L.y - 1.291485537864 * L.z);
    lms = lms * lms * lms;
    return vec3(4.076741661348 * lms.x - 3.307711590408 * lms.y + 0.230969928729 * lms.z,
                -1.268438004092 * lms.x + 2.609757400663 * lms.y - 0.341319396310 * lms.z,
                -0.004196086542 * lms.x - 0.703418614459 * lms.y + 1.707614700931 * lms.z);
}

// ---------------------------------------------------------------- display-encoded luminance
float psyDisplayEncode(float l) {
    if (!(l > 0.0)) return 0.0;
    if (l <= 0.0031308) return 12.92 * l;
    if (l <= 1.0) return 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    return 1.0 + 0.25 * log2(l);
}
