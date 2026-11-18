/*
 * Programmes en virgule flottante. Les résultats sont des représentations
 * binaires : l'émulateur doit être exact au bit près. Compiler avec
 * -ffp-contract=off pour interdire les multiplications-additions fusionnées.
 */
#include "programs.h"

typedef unsigned int u32;
typedef unsigned long long u64;

static volatile u32 g_fseed = 7;

static u32 fbits(float f) {
    union { float f; u32 u; } v;
    v.f = f;
    return v.u;
}

static u32 dbits(double d) {
    union { double d; u64 u; } v;
    v.d = d;
    return (u32)(v.u ^ (v.u >> 32));
}

static u32 test_harmonic(void) {
    float sf = 0.0f;
    double sd = 0.0;
    for (u32 i = 1; i <= 500 + g_fseed; ++i) {
        sf += 1.0f / (float)i;
        sd += 1.0 / (double)i;
    }
    return fbits(sf) ^ dbits(sd);
}

static u32 test_sqrt(void) {
    u32 acc = 0;
    for (u32 i = 1; i < 64; ++i) {
        float x = (float)(i * g_fseed) * 0.37f;
        float guess = x;  /* méthode de Newton */
        for (int k = 0; k < 8; ++k) guess = 0.5f * (guess + x / guess);
        acc = acc * 31 + fbits(guess) + fbits(__builtin_sqrtf(x)) + dbits(__builtin_sqrt((double)x * 3.0));
    }
    return acc;
}

static u32 test_matrix(void) {
    float a[16], b[16], c[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = (float)(i + (int)g_fseed) * 0.25f - 1.5f;
        b[i] = (float)(16 - i) * 0.125f + 0.75f;
    }
    for (int round = 0; round < 4; ++round) {
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + col];
                c[r * 4 + col] = sum;
            }
        for (int i = 0; i < 16; ++i) a[i] = c[i] * 0.01f;
    }
    u32 acc = 0;
    for (int i = 0; i < 16; ++i) acc = acc * 7 + fbits(c[i]);
    return acc;
}

static u32 test_conversions(void) {
    u32 acc = 0;
    for (int i = -40; i < 40; ++i) {
        float f = (float)i * 1.7f + (float)g_fseed * 0.1f;
        double d = (double)i * -2.3 + 0.5;
        int fi = (int)f;                 /* troncature vers zéro */
        unsigned du = (unsigned)(d < 0 ? -d : d);
        float back = (float)fi;
        double ud = (double)(unsigned)(i + 100) * 1000.0;
        acc = acc * 13 + (u32)fi + du + fbits(back) + dbits(ud) + fbits((float)d) + dbits((double)f);
    }
    return acc;
}

static float fmin_f(float a, float b) { return a < b ? a : b; }
static float fmax_f(float a, float b) { return a > b ? a : b; }

static u32 test_compare(void) {
    float values[32];
    u32 state = g_fseed;
    for (int i = 0; i < 32; ++i) {
        state = state * 1103515245u + 12345u;
        values[i] = (float)(int)(state >> 16) * 0.001f - 16.0f;
    }
    for (int i = 1; i < 32; ++i) {  /* tri par insertion sur des flottants */
        float key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) { values[j + 1] = values[j]; --j; }
        values[j + 1] = key;
    }
    float lo = values[0], hi = values[0];
    u32 acc = 0;
    for (int i = 0; i < 32; ++i) {
        lo = fmin_f(lo, values[i]);
        hi = fmax_f(hi, values[i]);
        float v = values[i] < 0.0f ? -values[i] : values[i];
        if (v >= 1.0f && v <= 8.0f) acc += 1u << (i & 15);
        acc = acc * 3 + fbits(v);
    }
    return acc ^ fbits(lo) ^ fbits(hi);
}

static u32 test_mandelbrot(void) {
    u32 acc = 0;
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 24; ++x) {
            double cr = -2.0 + x * 0.125 + g_fseed * 0.0001, ci = -1.2 + y * 0.2;
            double zr = 0.0, zi = 0.0;
            int it = 0;
            while (it < 64 && zr * zr + zi * zi <= 4.0) {
                double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                ++it;
            }
            acc = acc * 5 + (u32)it;
        }
    return acc;
}

typedef struct { float x, y, z; } Vec3;

static Vec3 cross(Vec3 a, Vec3 b) {
    Vec3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    return r;
}
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static u32 test_vectors(void) {
    Vec3 v = {1.0f, (float)g_fseed, -3.5f}, w = {0.25f, -2.0f, 4.0f};
    u32 acc = 0;
    for (int i = 0; i < 30; ++i) {
        Vec3 c = cross(v, w);
        float len = __builtin_sqrtf(dot(c, c));
        if (len > 0.0f) { c.x /= len; c.y /= len; c.z /= len; }  /* normalisation */
        v = w;
        w.x = c.x * 3.0f + 0.5f;
        w.y = c.y * -2.0f + v.z;
        w.z = c.z + v.x * 0.75f;
        acc = acc * 11 + fbits(len) + fbits(dot(v, w));
    }
    return acc;
}

static u32 test_mixed_precision(void) {
    float f = 1.0f / 3.0f;
    double d = f;  /* simple -> double */
    u32 acc = 0;
    for (int i = 0; i < 40; ++i) {
        d = d * 1.0001 + (double)f;
        f = (float)d * 0.5f;  /* double -> simple */
        acc = acc * 3 + fbits(f) + dbits(d);
    }
    return acc;
}

static u32 test_constants(void) {
    /* Constantes encodables en VMOV immédiat. */
    float k[8] = {0.5f, 2.0f, -1.25f, 10.0f, 0.125f, -31.0f, 1.0f, 3.0f};
    double kd[4] = {0.5, -2.0, 15.5, 0.25};
    float s = (float)g_fseed;
    double sd = (double)g_fseed;
    for (int i = 0; i < 8; ++i) s = s * k[i] + k[(i + 3) & 7];
    for (int i = 0; i < 4; ++i) sd = sd * kd[i] - kd[(i + 1) & 3];
    return fbits(s) ^ dbits(sd) ^ fbits(-s) ^ fbits(s < 0 ? -s : s);
}

static u32 test_physics(void) {
    /* Petite intégration de mouvement, typique d'un jeu. */
    float px[8], py[8], vx[8], vy[8];
    for (int i = 0; i < 8; ++i) {
        px[i] = (float)i;
        py[i] = 10.0f + (float)g_fseed;
        vx[i] = 0.5f * (float)(i - 4);
        vy[i] = 0.0f;
    }
    const float dt = 1.0f / 60.0f, gravity = -9.81f;
    for (int step = 0; step < 120; ++step)
        for (int i = 0; i < 8; ++i) {
            vy[i] += gravity * dt;
            px[i] += vx[i] * dt;
            py[i] += vy[i] * dt;
            if (py[i] < 0.0f) { py[i] = -py[i]; vy[i] = -vy[i] * 0.8f; }  /* rebond */
        }
    u32 acc = 0;
    for (int i = 0; i < 8; ++i) acc = acc * 17 + fbits(px[i]) + fbits(py[i]) + fbits(vy[i]);
    return acc;
}

typedef u32 (*test_fn)(void);
static test_fn const kTests[VITA_FLOAT_TEST_COUNT] = {
    test_harmonic, test_sqrt, test_matrix, test_conversions, test_compare,
    test_mandelbrot, test_vectors, test_mixed_precision, test_constants, test_physics,
};
static const char* const kNames[VITA_FLOAT_TEST_COUNT] = {
    "série harmonique", "racine carrée", "matrices 4x4", "conversions", "comparaisons",
    "mandelbrot", "vecteurs 3D", "précision mixte", "constantes", "physique",
};

unsigned run_float_test(int index) { return kTests[index](); }
const char* float_test_name(int index) { return kNames[index]; }
