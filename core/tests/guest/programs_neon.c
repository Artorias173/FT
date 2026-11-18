/*
 * Programmes NEON. Compilés pour la Vita, ils utilisent les intrinsics de
 * <arm_neon.h> ; compilés nativement, une référence scalaire calcule le même
 * résultat. Les deux doivent coïncider exactement (compiler avec
 * -ffp-contract=off).
 */
#include <stdint.h>

#include "programs.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#define NEON 1
void* memcpy(void* dst, const void* src, unsigned n);  /* fourni par guest_start.c */
#else
#include <string.h>
#define NEON 0
#endif

typedef uint32_t u32;

static volatile u32 g_nseed = 99;

static void fill(void* dst, int n, u32 seed) {
    unsigned char* p = (unsigned char*)dst;
    u32 s = seed * 2654435761u + g_nseed;
    for (int i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;
        p[i] = (unsigned char)(s >> 16);
    }
}

static u32 hash(const void* src, int n) {
    const unsigned char* p = (const unsigned char*)src;
    u32 h = 2166136261u;
    for (int i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void fill_floats(float* f, int n, u32 seed, float scale, float offset) {
    int32_t raw[64];
    fill(raw, n * 4, seed);
    for (int i = 0; i < n; ++i) f[i] = (float)(raw[i] >> 16) * scale + offset;
}

#if !NEON
static int64_t sat_s(int64_t v, int bits) {
    const int64_t hi = (1ll << (bits - 1)) - 1, lo = -hi - 1;
    return v > hi ? hi : v < lo ? lo : v;
}
static int64_t sat_u(int64_t v, int bits) {
    const int64_t hi = (int64_t)((1ull << bits) - 1);
    return v > hi ? hi : v < 0 ? 0 : v;
}
#endif

/* 1. Arithmétique entière de base. */
static u32 test_int_arith(void) {
    int32_t a[4], b[4], r1[4];
    uint8_t c[16], d[16], r2[16], r3[16];
    int16_t e[8], f[8], r4[8], r5[8], r6[8], r7[8];
    uint16_t g[8], h[8], r8[8], r9[8];
    fill(a, 16, 1); fill(b, 16, 2); fill(c, 16, 3); fill(d, 16, 4);
    fill(e, 16, 5); fill(f, 16, 6); fill(g, 16, 7); fill(h, 16, 8);
#if NEON
    int32x4_t va = vld1q_s32(a), vb = vld1q_s32(b);
    vst1q_s32(r1, vmlsq_s32(vmlaq_s32(vaddq_s32(va, vb), va, vb), vb, vb));
    uint8x16_t vc = vld1q_u8(c), vd = vld1q_u8(d);
    vst1q_u8(r2, vmulq_u8(vsubq_u8(vc, vd), vd));
    vst1q_u8(r3, vabaq_u8(vc, vc, vd));
    int16x8_t ve = vld1q_s16(e), vf = vld1q_s16(f);
    vst1q_s16(r4, vhaddq_s16(ve, vf));
    vst1q_s16(r5, vhsubq_s16(ve, vf));
    vst1q_s16(r6, vmaxq_s16(vabdq_s16(ve, vf), vf));
    vst1q_s16(r7, vminq_s16(ve, vf));
    uint16x8_t vg = vld1q_u16(g), vh = vld1q_u16(h);
    vst1q_u16(r8, vrhaddq_u16(vg, vh));
    vst1q_u16(r9, vmaxq_u16(vg, vh));
#else
    for (int i = 0; i < 4; ++i) {
        uint32_t t = (uint32_t)a[i] + (uint32_t)b[i] + (uint32_t)a[i] * (uint32_t)b[i];
        r1[i] = (int32_t)(t - (uint32_t)b[i] * (uint32_t)b[i]);
    }
    for (int i = 0; i < 16; ++i) {
        r2[i] = (uint8_t)((uint8_t)(c[i] - d[i]) * d[i]);
        r3[i] = (uint8_t)(c[i] + (c[i] > d[i] ? c[i] - d[i] : d[i] - c[i]));
    }
    for (int i = 0; i < 8; ++i) {
        r4[i] = (int16_t)((e[i] + f[i]) >> 1);
        r5[i] = (int16_t)((e[i] - f[i]) >> 1);
        int16_t ad = (int16_t)(e[i] > f[i] ? e[i] - f[i] : f[i] - e[i]);
        r6[i] = ad > f[i] ? ad : f[i];
        r7[i] = e[i] < f[i] ? e[i] : f[i];
        r8[i] = (uint16_t)((g[i] + h[i] + 1) >> 1);
        r9[i] = g[i] > h[i] ? g[i] : h[i];
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(r6, 16) * 13 ^ hash(r7, 16) * 17 ^ hash(r8, 16) * 19 ^ hash(r9, 16) * 23;
}

/* 2. Arithmétique saturée. */
static u32 test_saturating(void) {
    int16_t a[8], b[8], r1[8], r2[8], r3[8];
    uint8_t c[16], d[16], r4[16], r5[16];
    int32_t e[4], r6[4];
    int16_t r7[4];
    uint8_t r8[8];
    int16_t f[8];
    fill(a, 16, 11); fill(b, 16, 12); fill(c, 16, 13); fill(d, 16, 14); fill(e, 16, 15); fill(f, 16, 16);
    a[0] = -32768; b[0] = -32768;  /* cas de saturation de VQDMULH */
#if NEON
    int16x8_t va = vld1q_s16(a), vb = vld1q_s16(b);
    vst1q_s16(r1, vqaddq_s16(va, vb));
    vst1q_s16(r2, vqdmulhq_s16(va, vb));
    vst1q_s16(r3, vqrdmulhq_s16(va, vb));
    uint8x16_t vc = vld1q_u8(c), vd = vld1q_u8(d);
    vst1q_u8(r4, vqsubq_u8(vc, vd));
    vst1q_u8(r5, vqaddq_u8(vc, vd));
    int32x4_t ve = vld1q_s32(e);
    vst1q_s32(r6, vqsubq_s32(ve, vshlq_n_s32(ve, 1)));
    vst1_s16(r7, vqmovn_s32(ve));
    vst1_u8(r8, vqmovun_s16(vld1q_s16(f)));
#else
    for (int i = 0; i < 8; ++i) {
        r1[i] = (int16_t)sat_s((int64_t)a[i] + b[i], 16);
        r2[i] = (int16_t)sat_s((2ll * a[i] * b[i]) >> 16, 16);
        r3[i] = (int16_t)sat_s((2ll * a[i] * b[i] + 0x8000) >> 16, 16);
        r8[i] = (uint8_t)sat_u(f[i], 8);
    }
    for (int i = 0; i < 16; ++i) {
        r4[i] = (uint8_t)sat_u((int64_t)c[i] - d[i], 8);
        r5[i] = (uint8_t)sat_u((int64_t)c[i] + d[i], 8);
    }
    for (int i = 0; i < 4; ++i) {
        int32_t sh = (int32_t)((uint32_t)e[i] << 1);
        r6[i] = (int32_t)sat_s((int64_t)e[i] - sh, 32);
        r7[i] = (int16_t)sat_s(e[i], 16);
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(r6, 16) * 13 ^ hash(r7, 8) * 17 ^ hash(r8, 8) * 19;
}

/* 3. Décalages immédiats et par registre. */
static u32 test_shifts(void) {
    uint16_t a[8], r1[8], r2[8], r9[8];
    int32_t b[4], r3[4], r6[4], r10[4];
    int16_t c[8], r4[8];
    uint32_t d[4], r5[4], r11[4], r12[4];
    int8_t s[16];
    uint16_t r7[4];
    int16_t r8[4];
    uint8_t u8in[16], r13[16];
    fill(a, 16, 21); fill(b, 16, 22); fill(c, 16, 23); fill(d, 16, 24); fill(s, 16, 25); fill(u8in, 16, 26);
    int32_t shifts[4];
    for (int i = 0; i < 4; ++i) shifts[i] = (s[i] % 40) - 8;  /* entre -47 et +31 */
#if NEON
    uint16x8_t va = vld1q_u16(a);
    vst1q_u16(r1, vshlq_n_u16(va, 3));
    vst1q_u16(r2, vrshrq_n_u16(va, 4));
    int32x4_t vb = vld1q_s32(b);
    vst1q_s32(r3, vshrq_n_s32(vb, 5));
    vst1q_s16(r4, vsraq_n_s16(vld1q_s16(c), vld1q_s16(c), 2));
    uint32x4_t vd = vld1q_u32(d);
    vst1q_u32(r5, vsliq_n_u32(vd, vd, 8));
    vst1q_s32(r6, vshlq_s32(vb, vld1q_s32(shifts)));
    vst1_u16(r7, vqshrun_n_s32(vb, 12));
    vst1_s16(r8, vqrshrn_n_s32(vb, 14));
    vst1q_u16(r9, vrsraq_n_u16(va, va, 7));
    vst1q_s32(r10, vrshlq_s32(vb, vld1q_s32(shifts)));
    vst1q_u32(r11, vsriq_n_u32(vd, vrev64q_u32(vd), 8));
    vst1q_u32(r12, vqshlq_u32(vd, vld1q_s32(shifts)));
    vst1q_u8(r13, vqshlq_n_u8(vld1q_u8(u8in), 2));
#else
    for (int i = 0; i < 8; ++i) {
        r1[i] = (uint16_t)(a[i] << 3);
        r2[i] = (uint16_t)((a[i] + 8) >> 4);
        r4[i] = (int16_t)(c[i] + (c[i] >> 2));
        r9[i] = (uint16_t)(a[i] + ((a[i] + 64) >> 7));
    }
    for (int i = 0; i < 4; ++i) {
        r3[i] = b[i] >> 5;
        r5[i] = (d[i] << 8) | (d[i] & 0xFF);
        int sh = shifts[i];
        int64_t v = b[i];
        r6[i] = sh >= 32 ? 0 : sh >= 0 ? (int32_t)((uint32_t)b[i] << sh) : (int32_t)(v >> (sh < -32 ? 32 : -sh));
        r10[i] = sh >= 32 ? 0 : sh >= 0 ? (int32_t)((uint32_t)b[i] << sh)
                                        : (int32_t)((v + (1ll << ((sh < -33 ? 33 : -sh) - 1))) >> (sh < -33 ? 33 : -sh));
        r7[i] = (uint16_t)sat_u(b[i] >> 12, 16);
        r8[i] = (int16_t)sat_s(((int64_t)b[i] + (1 << 13)) >> 14, 16);
        uint32_t other = d[i ^ 1];
        r11[i] = (other >> 8) | (d[i] & 0xFF000000u);
        uint64_t uv = d[i];
        r12[i] = sh >= 0 ? (uint32_t)sat_u(sh >= 32 ? (uv ? (int64_t)1 << 40 : 0) : (int64_t)(uv << sh), 32)
                         : (uint32_t)(uv >> (sh < -32 ? 32 : -sh));
    }
    for (int i = 0; i < 16; ++i) r13[i] = (uint8_t)sat_u((int64_t)u8in[i] << 2, 8);
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(r6, 16) * 13 ^ hash(r7, 8) * 17 ^ hash(r8, 8) * 19 ^ hash(r9, 16) * 23 ^ hash(r10, 16) * 29 ^
           hash(r11, 16) * 31 ^ hash(r12, 16) * 37 ^ hash(r13, 16) * 41;
}

/* 4. Élargissements et rétrécissements. */
static u32 test_widening(void) {
    int16_t a[4], b[4];
    int32_t r1[4], r9[4], r10[4];
    uint8_t c[8], d[8];
    uint16_t acc[8], r2[8], r5[8], r6[8], r8[8];
    int8_t e[8], f[8];
    int16_t r3[8];
    int32_t r4[4];
    uint32_t g[4], h[4];
    uint16_t r7[4];
    int16_t i16[8];
    fill(a, 8, 31); fill(b, 8, 32); fill(c, 8, 33); fill(d, 8, 34); fill(acc, 16, 35);
    fill(e, 8, 36); fill(f, 8, 37); fill(g, 16, 38); fill(h, 16, 39); fill(i16, 16, 40);
    uint8_t q16[16];
    fill(q16, 16, 41);
#if NEON
    vst1q_s32(r1, vmull_s16(vld1_s16(a), vld1_s16(b)));
    vst1q_u16(r2, vmlal_u8(vld1q_u16(acc), vld1_u8(c), vld1_u8(d)));
    vst1q_s16(r3, vaddl_s8(vld1_s8(e), vld1_s8(f)));
    vst1q_s32(r4, vpadalq_s16(vmovl_s16(vld1_s16(a)), vld1q_s16(i16)));
    vst1q_u16(r5, vsubw_u8(vld1q_u16(acc), vld1_u8(c)));
    vst1q_u16(r6, vpaddlq_u8(vld1q_u8(q16)));
    vst1_u16(r7, vraddhn_u32(vld1q_u32(g), vld1q_u32(h)));
    vst1q_u16(r8, vabdl_u8(vld1_u8(c), vld1_u8(d)));
    vst1q_s32(r9, vqdmull_s16(vld1_s16(a), vld1_s16(b)));
    vst1q_s32(r10, vqdmlal_s16(vld1q_s32(r1), vld1_s16(a), vld1_s16(b)));
#else
    for (int i = 0; i < 4; ++i) {
        r1[i] = a[i] * b[i];
        r4[i] = a[i] + i16[2 * i] + i16[2 * i + 1];
        r7[i] = (uint16_t)((g[i] + h[i] + 0x8000u) >> 16);
        r9[i] = (int32_t)sat_s(2ll * a[i] * b[i], 32);
        r10[i] = (int32_t)sat_s((int64_t)r1[i] + r9[i], 32);
    }
    for (int i = 0; i < 8; ++i) {
        r2[i] = (uint16_t)(acc[i] + c[i] * d[i]);
        r3[i] = (int16_t)(e[i] + f[i]);
        r5[i] = (uint16_t)(acc[i] - c[i]);
        r6[i] = (uint16_t)(q16[2 * i] + q16[2 * i + 1]);
        r8[i] = (uint16_t)(c[i] > d[i] ? c[i] - d[i] : d[i] - c[i]);
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(r6, 16) * 13 ^ hash(r7, 8) * 17 ^ hash(r8, 16) * 19 ^ hash(r9, 16) * 23 ^ hash(r10, 16) * 29;
}

/* 5. Calcul flottant vectoriel. */
static u32 test_float_ops(void) {
    float a[4], b[4], c[4], r1[4], r2[4], r3[4], r4[4], r5[4], r6[2], r7[2];
    fill_floats(a, 4, 51, 0.01f, -1.0f);
    fill_floats(b, 4, 52, 0.003f, 0.5f);
    fill_floats(c, 4, 53, 0.02f, 2.0f);
#if NEON
    float32x4_t va = vld1q_f32(a), vb = vld1q_f32(b), vc = vld1q_f32(c);
    vst1q_f32(r1, vmlaq_f32(vc, va, vb));
    vst1q_f32(r2, vmlsq_f32(vsubq_f32(va, vb), vb, vc));
    vst1q_f32(r3, vmaxq_f32(vabsq_f32(va), vnegq_f32(vb)));
    vst1q_f32(r4, vminq_f32(vabdq_f32(va, vc), vmulq_f32(vb, vc)));
    vst1q_f32(r5, vaddq_f32(va, vdupq_n_f32(2.5f)));
    vst1_f32(r6, vpadd_f32(vget_low_f32(va), vget_high_f32(vb)));
    vst1_f32(r7, vpmax_f32(vget_low_f32(vc), vget_high_f32(va)));
#else
    for (int i = 0; i < 4; ++i) {
        float p = a[i] * b[i];
        r1[i] = c[i] + p;
        float q = b[i] * c[i];
        r2[i] = (a[i] - b[i]) - q;
        float aa = a[i] < 0 ? -a[i] : a[i], nb = -b[i];
        r3[i] = aa > nb ? aa : nb;
        float d = a[i] - c[i];
        d = d < 0 ? -d : d;
        r4[i] = d < q ? d : q;
        r5[i] = a[i] + 2.5f;
    }
    r6[0] = a[0] + a[1]; r6[1] = b[2] + b[3];
    r7[0] = c[0] > c[1] ? c[0] : c[1]; r7[1] = a[2] > a[3] ? a[2] : a[3];
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(r6, 8) * 13 ^ hash(r7, 8) * 17;
}

/* 6. Conversions flottant <-> entier, virgule fixe, demi-précision. */
static u32 test_float_convert(void) {
    float a[4], r2[4], r4[4], r6[4];
    int32_t r1[4], r5[4], ints[4];
    uint32_t r3[4];
    uint16_t halves[4];
    fill_floats(a, 4, 61, 0.37f, 3.0f);
    fill(ints, 16, 62);
#if NEON
    float32x4_t va = vld1q_f32(a);
    vst1q_s32(r1, vcvtq_s32_f32(va));
    vst1q_f32(r2, vcvtq_f32_s32(vld1q_s32(ints)));
    vst1q_u32(r3, vcvtq_u32_f32(vabsq_f32(va)));
    vst1q_f32(r4, vcvtq_n_f32_s32(vld1q_s32(ints), 8));
    vst1q_s32(r5, vcvtq_n_s32_f32(va, 10));
    float16x4_t h = vcvt_f16_f32(va);
    vst1_u16(halves, vreinterpret_u16_f16(h));
    vst1q_f32(r6, vcvt_f32_f16(h));
#else
    for (int i = 0; i < 4; ++i) {
        r1[i] = (int32_t)a[i];
        r2[i] = (float)ints[i];
        r3[i] = (uint32_t)(a[i] < 0 ? -a[i] : a[i]);
        r4[i] = (float)ints[i] / 256.0f;
        r5[i] = (int32_t)(a[i] * 1024.0f);
        _Float16 hf = (_Float16)a[i];
        memcpy(&halves[i], &hf, 2);
        r6[i] = (float)hf;
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(halves, 8) * 13 ^ hash(r6, 16) * 17;
}

/* 7. Estimations d'inverse et Newton-Raphson. */
static u32 test_estimates(void) {
    float x[4] = {1.0f, 2.0f, 4.0f, 0.5f}, est[4], rest[4];
    float in[4], recip[4], rsqrt[4];
    fill_floats(in, 4, 71, 0.05f, 1700.0f);
    u32 good = 0;
    (void)x;
#if NEON
    vst1q_f32(est, vrecpeq_f32(vld1q_f32(x)));
    vst1q_f32(rest, vrsqrteq_f32(vld1q_f32(x)));
    float32x4_t v = vld1q_f32(in);
    float32x4_t r = vrecpeq_f32(v);
    r = vmulq_f32(vrecpsq_f32(v, r), r);
    r = vmulq_f32(vrecpsq_f32(v, r), r);
    vst1q_f32(recip, r);
    float32x4_t s = vrsqrteq_f32(v);
    s = vmulq_f32(vrsqrtsq_f32(vmulq_f32(v, s), s), s);
    s = vmulq_f32(vrsqrtsq_f32(vmulq_f32(v, s), s), s);
    vst1q_f32(rsqrt, s);
#else
    /* Valeurs de l'algorithme d'estimation ARMv7. */
    const u32 kEst[4] = {0x3F7F8000u, 0x3EFF8000u, 0x3E7F8000u, 0x3FFF8000u};
    const u32 kRest[4] = {0x3F7F8000u, 0x3F348000u, 0x3EFF8000u, 0x3FB48000u};
    memcpy(est, kEst, 16);
    memcpy(rest, kRest, 16);
    for (int i = 0; i < 4; ++i) {
        recip[i] = 1.0f / in[i];
        rsqrt[i] = 1.0f / __builtin_sqrtf(in[i]);
    }
#endif
    for (int i = 0; i < 4; ++i) {
        float e1 = recip[i] * in[i] - 1.0f, e2 = rsqrt[i] * rsqrt[i] * in[i] - 1.0f;
        if (e1 < 1e-5f && e1 > -1e-5f) ++good;
        if (e2 < 1e-4f && e2 > -1e-4f) ++good;
    }
    return hash(est, 16) ^ hash(rest, 16) * 3 ^ good * 0x9E3779B9u;
}

/* 8. Comparaisons et sélections. */
static u32 test_compare_select(void) {
    int32_t a[4], b[4];
    uint32_t r1[4], r2[4], r6[4];
    float f[4], g[4];
    uint8_t c[16], d[16], r3[16];
    uint16_t e[8], h[8], r4[8], r5[8];
    fill(a, 16, 81); fill(b, 16, 82); fill(c, 16, 83); fill(d, 16, 84); fill(e, 16, 85); fill(h, 16, 86);
    fill_floats(f, 4, 87, 0.1f, 0.0f);
    fill_floats(g, 4, 88, 0.1f, 0.0f);
    g[1] = f[1];
#if NEON
    uint32x4_t gt = vcgtq_s32(vld1q_s32(a), vld1q_s32(b));
    vst1q_u32(r1, gt);
    vst1q_u32(r2, vbslq_u32(gt, vreinterpretq_u32_s32(vld1q_s32(a)), vreinterpretq_u32_s32(vld1q_s32(b))));
    vst1q_u8(r3, vcleq_u8(vld1q_u8(c), vld1q_u8(d)));
    vst1q_u16(r4, vtstq_u16(vld1q_u16(e), vld1q_u16(h)));
    vst1q_u16(r5, vceqq_u16(vandq_u16(vld1q_u16(e), vdupq_n_u16(3)), vdupq_n_u16(1)));
    vst1q_u32(r6, vorrq_u32(vceqq_f32(vld1q_f32(f), vld1q_f32(g)), vcltq_f32(vld1q_f32(f), vdupq_n_f32(0.0f))));
#else
    for (int i = 0; i < 4; ++i) {
        r1[i] = a[i] > b[i] ? 0xFFFFFFFFu : 0;
        r2[i] = (uint32_t)(a[i] > b[i] ? a[i] : b[i]);
        r6[i] = (f[i] == g[i] || f[i] < 0.0f) ? 0xFFFFFFFFu : 0;
    }
    for (int i = 0; i < 16; ++i) r3[i] = c[i] <= d[i] ? 0xFF : 0;
    for (int i = 0; i < 8; ++i) {
        r4[i] = (e[i] & h[i]) ? 0xFFFF : 0;
        r5[i] = (e[i] & 3) == 1 ? 0xFFFF : 0;
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^ hash(r6, 16) * 13;
}

/* 9. Permutations : zip, unzip, transposition, inversion, extraction, tables. */
static u32 test_permutes(void) {
    uint16_t a[8], b[8], z0[8], z1[8];
    uint8_t c[16], d[16], u0[16], u1[16], rv[16], ex[16], tb[8], tx[8];
    int32_t e[4], f[4], t0[4], t1[4];
    uint16_t r32[8];
    int16_t dl[8];
    uint8_t idx[8];
    fill(a, 16, 91); fill(b, 16, 92); fill(c, 16, 93); fill(d, 16, 94); fill(e, 16, 95); fill(f, 16, 96);
    fill(idx, 8, 97);
    for (int i = 0; i < 8; ++i) idx[i] &= 31;  /* une partie hors de la table */
#if NEON
    uint16x8x2_t z = vzipq_u16(vld1q_u16(a), vld1q_u16(b));
    vst1q_u16(z0, z.val[0]); vst1q_u16(z1, z.val[1]);
    uint8x16x2_t u = vuzpq_u8(vld1q_u8(c), vld1q_u8(d));
    vst1q_u8(u0, u.val[0]); vst1q_u8(u1, u.val[1]);
    int32x4x2_t t = vtrnq_s32(vld1q_s32(e), vld1q_s32(f));
    vst1q_s32(t0, t.val[0]); vst1q_s32(t1, t.val[1]);
    vst1q_u8(rv, vrev64q_u8(vld1q_u8(c)));
    vst1q_u16(r32, vrev32q_u16(vld1q_u16(a)));
    vst1q_u8(ex, vextq_u8(vld1q_u8(c), vld1q_u8(d), 5));
    uint8x8x2_t table = {{vld1_u8(c), vld1_u8(d)}};
    vst1_u8(tb, vtbl2_u8(table, vld1_u8(idx)));
    vst1_u8(tx, vtbx1_u8(vld1_u8(d + 8), vld1_u8(c + 8), vld1_u8(idx)));
    vst1q_s16(dl, vdupq_lane_s16(vld1_s16((const int16_t*)a), 2));
#else
    for (int i = 0; i < 4; ++i) {
        z0[2 * i] = a[i]; z0[2 * i + 1] = b[i];
        z1[2 * i] = a[4 + i]; z1[2 * i + 1] = b[4 + i];
    }
    for (int i = 0; i < 8; ++i) {
        u0[i] = c[2 * i]; u0[8 + i] = d[2 * i];
        u1[i] = c[2 * i + 1]; u1[8 + i] = d[2 * i + 1];
        r32[i] = a[i ^ 1];
        uint8_t k = idx[i];
        tb[i] = k < 8 ? c[k] : k < 16 ? d[k - 8] : 0;
        tx[i] = k < 8 ? c[8 + k] : d[8 + i];
        dl[i] = (int16_t)a[2];
    }
    t0[0] = e[0]; t0[1] = f[0]; t0[2] = e[2]; t0[3] = f[2];
    t1[0] = e[1]; t1[1] = f[1]; t1[2] = e[3]; t1[3] = f[3];
    for (int i = 0; i < 16; ++i) {
        rv[i] = c[(i & 8) + 7 - (i & 7)];
        ex[i] = i + 5 < 16 ? c[i + 5] : d[i + 5 - 16];
    }
#endif
    return hash(z0, 16) ^ hash(z1, 16) * 3 ^ hash(u0, 16) * 5 ^ hash(u1, 16) * 7 ^ hash(t0, 16) * 11 ^
           hash(t1, 16) * 13 ^ hash(rv, 16) * 17 ^ hash(r32, 16) * 19 ^ hash(ex, 16) * 23 ^ hash(tb, 8) * 29 ^
           hash(tx, 8) * 31 ^ hash(dl, 16) * 37;
}

/* 10. Chargements et stockages entrelacés (VLD2/3/4, VST2/3/4, voies, duplication). */
static u32 test_interleaved(void) {
    uint16_t src16[32], o2a[8], o2b[8];
    uint8_t src8[48], o3[3][8], st3[24], st4[32];
    int16_t src4[16], o4[4][4];
    float fdup[4], fval = 1.75f;
    uint16_t lane[8];
    uint32_t lane_out = 0, src32[4];
    fill(src16, 64, 101); fill(src8, 48, 102); fill(src4, 32, 103); fill(lane, 16, 104); fill(src32, 16, 105);
#if NEON
    uint16x8x2_t v2 = vld2q_u16(src16);
    vst1q_u16(o2a, v2.val[0]); vst1q_u16(o2b, v2.val[1]);
    uint8x8x3_t v3 = vld3_u8(src8);
    vst1_u8(o3[0], v3.val[0]); vst1_u8(o3[1], v3.val[1]); vst1_u8(o3[2], v3.val[2]);
    uint8x8x3_t w3 = {{v3.val[2], v3.val[0], v3.val[1]}};
    vst3_u8(st3, w3);
    int16x4x4_t v4 = vld4_s16(src4);
    vst1_s16(o4[0], v4.val[0]); vst1_s16(o4[1], v4.val[1]); vst1_s16(o4[2], v4.val[2]); vst1_s16(o4[3], v4.val[3]);
    uint8x8x4_t w4 = {{vld1_u8(src8), vld1_u8(src8 + 8), vld1_u8(src8 + 16), vld1_u8(src8 + 24)}};
    vst4_u8(st4, w4);
    vst1q_f32(fdup, vld1q_dup_f32(&fval));
    uint16x8_t l = vld1q_u16(lane);
    l = vld1q_lane_u16(&src16[3], l, 5);
    vst1q_u16(lane, l);
    vst1q_lane_u32(&lane_out, vld1q_u32(src32), 2);
#else
    for (int i = 0; i < 8; ++i) { o2a[i] = src16[2 * i]; o2b[i] = src16[2 * i + 1]; }
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 3; ++k) o3[k][i] = src8[3 * i + k];
    for (int i = 0; i < 8; ++i) {
        st3[3 * i] = o3[2][i]; st3[3 * i + 1] = o3[0][i]; st3[3 * i + 2] = o3[1][i];
    }
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) o4[k][i] = src4[4 * i + k];
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) st4[4 * i + k] = src8[8 * k + i];
    for (int i = 0; i < 4; ++i) fdup[i] = fval;
    lane[5] = src16[3];
    lane_out = src32[2];
#endif
    return hash(o2a, 16) ^ hash(o2b, 16) * 3 ^ hash(o3, 24) * 5 ^ hash(st3, 24) * 7 ^ hash(o4, 32) * 11 ^
           hash(st4, 32) * 13 ^ hash(fdup, 16) * 17 ^ hash(lane, 16) * 19 ^ lane_out * 23;
}

/* 11. Comptages de bits et opérations par paires. */
static u32 test_bits_pairwise(void) {
    uint8_t a[16], r1[16], r6[8];
    uint16_t b[8], r2[8];
    int8_t c[16], r3[16];
    int16_t d[4], r4[4];
    int32_t e[2], f[2], r5[2];
    fill(a, 16, 111); fill(b, 16, 112); fill(c, 16, 113); fill(d, 8, 114); fill(e, 8, 115); fill(f, 8, 116);
    b[0] = 0; b[1] = 1;
#if NEON
    vst1q_u8(r1, vcntq_u8(vld1q_u8(a)));
    vst1q_u16(r2, vclzq_u16(vld1q_u16(b)));
    vst1q_s8(r3, vclsq_s8(vld1q_s8(c)));
    vst1_s16(r4, vpadd_s16(vld1_s16(d), vld1_s16(d)));
    vst1_s32(r5, vpmin_s32(vld1_s32(e), vld1_s32(f)));
    vst1_u8(r6, vpmax_u8(vld1_u8(a), vld1_u8(a + 8)));
#else
    for (int i = 0; i < 16; ++i) {
        r1[i] = (uint8_t)__builtin_popcount(a[i]);
        int8_t v = c[i];
        uint8_t t = (uint8_t)(v < 0 ? ~v : v);
        int n = 0;
        for (int k = 6; k >= 0 && !((t >> k) & 1); --k) ++n;
        r3[i] = (int8_t)n;
    }
    for (int i = 0; i < 8; ++i) r2[i] = (uint16_t)(b[i] ? __builtin_clz(b[i]) - 16 : 16);
    r4[0] = (int16_t)(d[0] + d[1]); r4[1] = (int16_t)(d[2] + d[3]); r4[2] = r4[0]; r4[3] = r4[1];
    r5[0] = e[0] < e[1] ? e[0] : e[1]; r5[1] = f[0] < f[1] ? f[0] : f[1];
    for (int i = 0; i < 4; ++i) {
        r6[i] = a[2 * i] > a[2 * i + 1] ? a[2 * i] : a[2 * i + 1];
        r6[4 + i] = a[8 + 2 * i] > a[8 + 2 * i + 1] ? a[8 + 2 * i] : a[8 + 2 * i + 1];
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 8) * 7 ^ hash(r5, 8) * 11 ^ hash(r6, 8) * 13;
}

/* 12. Produit de matrices 4x4 par scalaires (code typique d'un jeu). */
static u32 test_matrix_neon(void) {
    float m[16], n[16], r[16];
    fill_floats(m, 16, 121, 0.001f, -0.5f);
    fill_floats(n, 16, 122, 0.002f, 0.25f);
    for (int round = 0; round < 3; ++round) {
#if NEON
        float32x4_t c0 = vld1q_f32(m), c1 = vld1q_f32(m + 4), c2 = vld1q_f32(m + 8), c3 = vld1q_f32(m + 12);
        for (int j = 0; j < 4; ++j) {
            float32x4_t col = vld1q_f32(n + 4 * j);
            float32x4_t acc = vmulq_lane_f32(c0, vget_low_f32(col), 0);
            acc = vmlaq_lane_f32(acc, c1, vget_low_f32(col), 1);
            acc = vmlaq_lane_f32(acc, c2, vget_high_f32(col), 0);
            acc = vmlaq_lane_f32(acc, c3, vget_high_f32(col), 1);
            vst1q_f32(r + 4 * j, acc);
        }
#else
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) {
                float acc = m[i] * n[4 * j];
                float p1 = m[4 + i] * n[4 * j + 1];
                acc = acc + p1;
                float p2 = m[8 + i] * n[4 * j + 2];
                acc = acc + p2;
                float p3 = m[12 + i] * n[4 * j + 3];
                r[4 * j + i] = acc + p3;
            }
#endif
        memcpy(m, r, sizeof(m));
    }
    return hash(r, 64);
}

/* 13. Immédiats modifiés et transferts avec les registres ARM. */
static u32 test_immediates(void) {
    uint32_t a[4], r1[4], r2[4];
    uint16_t b[8], r3[8], r6[8];
    uint8_t r4[16];
    int16_t r5[8];
    uint32_t lanes = 0;
    fill(a, 16, 131); fill(b, 16, 132);
    volatile uint16_t dynamic = (uint16_t)(g_nseed * 77u);
#if NEON
    vst1q_u32(r1, vorrq_u32(vld1q_u32(a), vdupq_n_u32(0x00FF0000u)));
    vst1q_u32(r2, vmvnq_u32(vdupq_n_u32(0x0000AB00u)));
    vst1q_u16(r3, vbicq_u16(vld1q_u16(b), vdupq_n_u16(0x00F0)));
    vst1q_u8(r4, vdupq_n_u8(0x5A));
    vst1q_s16(r5, vsetq_lane_s16(-7, vdupq_n_s16(1234), 6));
    vst1q_u16(r6, vdupq_n_u16(dynamic));
    uint8x16_t v = vld1q_u8((const uint8_t*)a);
    lanes = vgetq_lane_u8(v, 11) + ((uint32_t)vgetq_lane_u16(vld1q_u16(b), 5) << 8) +
            (uint32_t)vgetq_lane_s16(vld1q_s16((const int16_t*)b), 3);
#else
    for (int i = 0; i < 4; ++i) {
        r1[i] = a[i] | 0x00FF0000u;
        r2[i] = ~0x0000AB00u;
    }
    for (int i = 0; i < 8; ++i) {
        r3[i] = (uint16_t)(b[i] & ~0x00F0);
        r5[i] = i == 6 ? -7 : 1234;
        r6[i] = dynamic;
    }
    for (int i = 0; i < 16; ++i) r4[i] = 0x5A;
    lanes = ((const uint8_t*)a)[11] + ((uint32_t)b[5] << 8) + (uint32_t)(int16_t)b[3];
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11 ^
           hash(r6, 16) * 13 ^ lanes * 17;
}

/* 14. Éléments de 64 bits. */
static u32 test_64bit_lanes(void) {
    uint64_t a[2], b[2], r1[2], r2[2], r4[2];
    int64_t c[2], r3[2], r5[2];
    fill(a, 16, 141); fill(b, 16, 142); fill(c, 16, 143);
    c[1] = 0x7FFFFFFFFFFFFFF0ll;
#if NEON
    vst1q_u64(r1, vaddq_u64(vld1q_u64(a), vld1q_u64(b)));
    vst1q_u64(r2, vshrq_n_u64(vld1q_u64(a), 13));
    vst1q_s64(r3, vqaddq_s64(vld1q_s64(c), vdupq_n_s64(100)));
    vst1q_u64(r4, vsraq_n_u64(vld1q_u64(b), vld1q_u64(a), 60));
    vst1q_s64(r5, vshlq_n_s64(vsubq_s64(vld1q_s64(c), vreinterpretq_s64_u64(vld1q_u64(a))), 7));
#else
    for (int i = 0; i < 2; ++i) {
        r1[i] = a[i] + b[i];
        r2[i] = a[i] >> 13;
        r3[i] = (c[i] > 0 && c[i] > INT64_MAX - 100) ? INT64_MAX : c[i] + 100;
        r4[i] = b[i] + (a[i] >> 60);
        r5[i] = (int64_t)(((uint64_t)c[i] - a[i]) << 7);
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 16) * 7 ^ hash(r5, 16) * 11;
}

/* 15. Multiplications par scalaire (entiers) et rétrécissements simples. */
static u32 test_scalar_int(void) {
    int16_t a[8], b[4], r1[8], r4[4];
    int32_t acc[4], r2[4], r3[4];
    uint32_t w[4];
    uint16_t r5[4];
    fill(a, 16, 151); fill(b, 8, 152); fill(acc, 16, 153); fill(w, 16, 154);
#if NEON
    int16x4_t vb = vld1_s16(b);
    vst1q_s16(r1, vmulq_lane_s16(vld1q_s16(a), vb, 1));
    vst1q_s32(r2, vmlal_lane_s16(vld1q_s32(acc), vld1_s16(a), vb, 3));
    vst1q_s32(r3, vqdmulhq_lane_s32(vld1q_s32(acc), vreinterpret_s32_s16(vb), 0));
    vst1_s16(r4, vmls_lane_s16(vld1_s16(a + 4), vld1_s16(a), vb, 2));
    vst1_u16(r5, vmovn_u32(vld1q_u32(w)));
#else
    int32_t s32;
    memcpy(&s32, b, 4);
    for (int i = 0; i < 8; ++i) r1[i] = (int16_t)(a[i] * b[1]);
    for (int i = 0; i < 4; ++i) {
        r2[i] = (int32_t)((uint32_t)acc[i] + (uint32_t)(a[i] * b[3]));
        r3[i] = (int32_t)sat_s((2ll * acc[i] * s32) >> 32, 32);
        r4[i] = (int16_t)(a[4 + i] - a[i] * b[2]);
        r5[i] = (uint16_t)w[i];
    }
#endif
    return hash(r1, 16) ^ hash(r2, 16) * 3 ^ hash(r3, 16) * 5 ^ hash(r4, 8) * 7 ^ hash(r5, 8) * 11;
}

/* 16. Traitement d'image : mélange alpha de pixels RGBA 8 bits. */
static u32 test_alpha_blend(void) {
    uint8_t src[64], dst[64], out[64];
    fill(src, 64, 161); fill(dst, 64, 162);
#if NEON
    for (int p = 0; p < 64; p += 32) {
        uint8x8x4_t s = vld4_u8(src + p), d = vld4_u8(dst + p);
        uint8x8_t alpha = s.val[3], inv = vmvn_u8(alpha);
        uint8x8x4_t o;
        for (int ch = 0; ch < 3; ++ch) {
            uint16x8_t t = vmull_u8(s.val[ch], alpha);
            t = vmlal_u8(t, d.val[ch], inv);
            o.val[ch] = vrshrn_n_u16(t, 8);
        }
        o.val[3] = vmax_u8(s.val[3], d.val[3]);
        vst4_u8(out + p, o);
    }
#else
    for (int i = 0; i < 16; ++i) {
        const uint8_t a = src[4 * i + 3], inv = (uint8_t)~a;
        for (int ch = 0; ch < 3; ++ch) {
            uint16_t t = (uint16_t)(src[4 * i + ch] * a + dst[4 * i + ch] * inv);
            out[4 * i + ch] = (uint8_t)((t + 128) >> 8);
        }
        out[4 * i + 3] = src[4 * i + 3] > dst[4 * i + 3] ? src[4 * i + 3] : dst[4 * i + 3];
    }
#endif
    return hash(out, 64);
}

typedef u32 (*test_fn)(void);
static test_fn const kTests[VITA_NEON_TEST_COUNT] = {
    test_int_arith, test_saturating, test_shifts, test_widening, test_float_ops, test_float_convert,
    test_estimates, test_compare_select, test_permutes, test_interleaved, test_bits_pairwise,
    test_matrix_neon, test_immediates, test_64bit_lanes, test_scalar_int, test_alpha_blend,
};
static const char* const kNames[VITA_NEON_TEST_COUNT] = {
    "neon entiers", "neon saturation", "neon décalages", "neon élargissement", "neon flottants",
    "neon conversions", "neon estimations", "neon comparaisons", "neon permutations", "neon entrelacés",
    "neon bits et paires", "neon matrices", "neon immédiats", "neon 64 bits", "neon scalaires",
    "neon mélange alpha",
};

unsigned run_neon_test(int index) { return kTests[index](); }
const char* neon_test_name(int index) { return kNames[index]; }
