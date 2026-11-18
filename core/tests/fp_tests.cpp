// Tests des règles flottantes ARM (fp.h) : valeurs vérifiées à la main
// d'après le manuel de référence de l'architecture ARMv7.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/cpu/fp.h"

using namespace vita;

extern int g_failures;

#define FP_CHECK(cond)                                                                 \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

namespace {

u32 fb(float f) { u32 b; std::memcpy(&b, &f, 4); return b; }
float bf(u32 b) { float f; std::memcpy(&f, &b, 4); return f; }
u64 db(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }

fp::Ctx ieee(u32 rmode = fp::kNearest) { fp::Ctx c; c.rmode = rmode; return c; }
fp::Ctx console() { return fp::Ctx::from_fpscr((1u << 25) | (1u << 24)); }

void test_nans() {
    const float inf = bf(0x7F800000);
    // Opération invalide : NaN par défaut ARM positif (le x86 donne 0xFFC00000).
    fp::Ctx c = ieee();
    FP_CHECK(fb(fp::mul(0.0f, inf, c)) == 0x7FC00000);
    FP_CHECK(c.exceptions & fp::kIOC);
    c = ieee();
    FP_CHECK(fb(fp::sub(inf, inf, c)) == 0x7FC00000);
    c = ieee();
    FP_CHECK(fb(fp::sqrt(-1.0f, c)) == 0x7FC00000);
    FP_CHECK(db(fp::sqrt(-1.0, c)) == 0x7FF8000000000000ull);

    // Propagation sans DN : premier NaN silencieux, NaN signalant prioritaire et rendu silencieux.
    c = ieee();
    FP_CHECK(fb(fp::add(bf(0x7FC00001), bf(0x7FC00002), c)) == 0x7FC00001);
    FP_CHECK(!(c.exceptions & fp::kIOC));
    FP_CHECK(fb(fp::add(bf(0xFFC00001), 1.0f, c)) == 0xFFC00001);
    FP_CHECK(fb(fp::add(1.0f, bf(0x7FC00003), c)) == 0x7FC00003);
    c = ieee();
    FP_CHECK(fb(fp::add(bf(0x7FC00001), bf(0x7F800005), c)) == 0x7FC00005);
    FP_CHECK(c.exceptions & fp::kIOC);

    // Avec DN : toujours le NaN par défaut.
    c = console();
    FP_CHECK(fb(fp::add(bf(0xFFC00001), 1.0f, c)) == 0x7FC00000);

    // Conversions de NaN entre précisions : charge utile conservée sans DN.
    c = ieee();
    FP_CHECK(fb(fp::to_single(fp::to_double(bf(0x7FC12345), c), c)) == 0x7FC12345);
}

void test_flush_to_zero() {
    const float denorm = bf(0x00000001);  // plus petit dénormal positif
    fp::Ctx c = console();
    FP_CHECK(fb(fp::add(denorm, 0.0f, c)) == 0);
    FP_CHECK(c.exceptions & fp::kIDC);
    FP_CHECK(fb(fp::mul(bf(0x80000010), 1.0f, c)) == 0x80000000);  // signe conservé

    // Résultat minuscule mis à zéro en sortie, avec UFC et sans IXC.
    c = console();
    FP_CHECK(fb(fp::mul(std::ldexp(1.0f, -100), std::ldexp(1.0f, -30), c)) == 0);
    FP_CHECK(c.exceptions == fp::kUFC);

    // Sans FZ, le dénormal est conservé.
    c = ieee();
    FP_CHECK(fb(fp::mul(std::ldexp(1.0f, -100), std::ldexp(1.0f, -30), c)) == fb(std::ldexp(1.0f, -130)));
    c = ieee();
    FP_CHECK(fb(fp::add(denorm, denorm, c)) == 0x00000002);

    // Comparaison : un dénormal vaut zéro avec FZ.
    c = console();
    FP_CHECK(fp::compare(denorm, 0.0f, false, c) == 0x6);
    c = ieee();
    FP_CHECK(fp::compare(denorm, 0.0f, false, c) == 0x2);
}

void test_rounding() {
    const float tiny = std::ldexp(1.0f, -24);  // la moitié d'un ulp de 1.0
    fp::Ctx c = ieee(fp::kNearest);
    FP_CHECK(fp::add(1.0f, tiny, c) == 1.0f);  // égalité : au pair
    FP_CHECK(c.exceptions & fp::kIXC);
    c = ieee(fp::kPlusInf);
    FP_CHECK(fp::add(1.0f, tiny, c) == std::nextafter(1.0f, 2.0f));
    c = ieee(fp::kMinusInf);
    FP_CHECK(fp::add(-1.0f, -tiny, c) == std::nextafter(-1.0f, -2.0f));
    c = ieee(fp::kZero);
    FP_CHECK(fp::add(-1.0f, -tiny, c) == -1.0f);
    FP_CHECK(fp::div(1.0f, 3.0f, c) == bf(0x3EAAAAAA));
    c = ieee(fp::kPlusInf);
    FP_CHECK(fp::div(1.0f, 3.0f, c) == bf(0x3EAAAAAB));
    c = ieee();
    FP_CHECK(fb(fp::div(1.0f, 0.0f, c)) == 0x7F800000 && (c.exceptions & fp::kDZC));
}

void test_conversions() {
    fp::Ctx c = ieee();
    FP_CHECK(fp::to_int(2.5f, true, fp::kNearest, c) == 2);  // au pair
    FP_CHECK(fp::to_int(3.5f, true, fp::kNearest, c) == 4);
    FP_CHECK(fp::to_int(-2.7f, true, fp::kZero, c) == static_cast<u32>(-2));
    FP_CHECK(fp::to_int(-2.2f, true, fp::kMinusInf, c) == static_cast<u32>(-3));
    c = ieee();
    FP_CHECK(fp::to_int(3e9f, true, fp::kZero, c) == 0x7FFFFFFF && (c.exceptions & fp::kIOC));
    c = ieee();
    FP_CHECK(fp::to_int(-1.0f, false, fp::kZero, c) == 0 && (c.exceptions & fp::kIOC));
    c = ieee();
    FP_CHECK(fp::to_int(bf(0x7FC00000), true, fp::kZero, c) == 0 && (c.exceptions & fp::kIOC));
    FP_CHECK(fp::to_int(1e300, false, fp::kZero, c) == 0xFFFFFFFF);

    // Virgule fixe.
    c = ieee();
    FP_CHECK(fp::to_fixed(1.5f, 16, 8, false, fp::kZero, c) == 384);
    FP_CHECK(fp::to_fixed(-1.5f, 16, 8, false, fp::kZero, c) == static_cast<u64>(-384));
    FP_CHECK(fp::to_fixed(200.0f, 16, 8, false, fp::kZero, c) == 0x7FFF);    // saturation 16 bits
    FP_CHECK(fp::to_fixed(300.0f, 16, 8, true, fp::kZero, c) == 0xFFFF);
    FP_CHECK(fp::to_fixed(0.999f, 32, 31, false, fp::kZero, c) == 2145336192);
    FP_CHECK(fp::from_fixed<float>(384, 16, 8, false, c) == 1.5f);
    FP_CHECK(fp::from_fixed<float>(0xFE80, 16, 8, false, c) == -1.5f);       // 16 bits signé
    FP_CHECK(fp::from_fixed<float>(0xFE80, 16, 8, true, c) == 254.5f);
    FP_CHECK(fp::from_fixed<double>(0x80000000u, 32, 31, false, c) == -1.0);

    // Demi-précision IEEE.
    c = ieee();
    FP_CHECK(fp::to_half(1.0f, c) == 0x3C00);
    FP_CHECK(fp::to_half(-2.0f, c) == 0xC000);
    FP_CHECK(fp::to_half(65504.0f, c) == 0x7BFF);
    FP_CHECK(fp::to_half(65520.0f, c) == 0x7C00);  // dépasse : infini en arrondi au plus proche
    fp::Ctx rz = ieee(fp::kZero);
    FP_CHECK(fp::to_half(65520.0f, rz) == 0x7BFF);  // ...mais le plus grand fini vers zéro
    FP_CHECK(fp::to_half(std::ldexp(1.0f, -24), c) == 0x0001);       // plus petit dénormal
    FP_CHECK(fp::to_half(std::ldexp(1.0f, -25), c) == 0x0000);       // égalité : au pair (zéro)
    FP_CHECK(fp::to_half(std::ldexp(1.5f, -25), c) == 0x0001);
    FP_CHECK(fp::to_half(1.0f / 3.0f, c) == 0x3555);
    FP_CHECK(fp::from_half(0x3555, c) == 0.333251953125f);
    FP_CHECK(fp::from_half(0x0001, c) == std::ldexp(1.0f, -24));
    FP_CHECK(fb(fp::from_half(0xFC00, c)) == 0xFF800000);
    FP_CHECK(fb(fp::from_half(0x7E01, c)) == (0x7FC00000 | (1u << 13)));

    // Demi-précision alternative (AHP) : pas d'infini, exposant 31 normal.
    fp::Ctx ahp = ieee();
    ahp.ahp = true;
    FP_CHECK(fp::to_half(65536.0f, ahp) == 0x7C00);   // représentable en AHP
    FP_CHECK(fp::from_half(0x7C00, ahp) == 65536.0f);
    FP_CHECK(fp::to_half(1e10f, ahp) == 0x7FFF && (ahp.exceptions & fp::kIOC));
    FP_CHECK(fp::to_half(bf(0x7FC00000), ahp) == 0);
}

void test_estimates() {
    fp::Ctx c = fp::Ctx::standard(0);
    FP_CHECK(fb(fp::recip_estimate(1.0f, c)) == 0x3F7F8000);  // 0.998046875
    FP_CHECK(fb(fp::recip_estimate(2.0f, c)) == 0x3EFF8000);
    FP_CHECK(fb(fp::recip_estimate(-4.0f, c)) == 0xBE7F8000);
    FP_CHECK(fb(fp::rsqrt_estimate(1.0f, c)) == 0x3F7F8000);
    FP_CHECK(fb(fp::rsqrt_estimate(4.0f, c)) == 0x3EFF8000);
    FP_CHECK(fb(fp::recip_estimate(0.0f, c)) == 0x7F800000 && (c.exceptions & fp::kDZC));
    FP_CHECK(fb(fp::rsqrt_estimate(-1.0f, c)) == 0x7FC00000);
    FP_CHECK(fp::recip_step(2.0f, 0.5f, c) == 1.0f);
    FP_CHECK(fp::recip_step(bf(0x7F800000), 0.0f, c) == 2.0f);
    FP_CHECK(fp::rsqrt_step(1.0f, 1.0f, c) == 1.0f);
    FP_CHECK(fp::recip_estimate_u32(0x80000000u) == 0xFF800000u);
    FP_CHECK(fp::recip_estimate_u32(0x7FFFFFFFu) == 0xFFFFFFFFu);
    // Une itération de Newton-Raphson rapproche de 1/3.
    const float x = 3.0f;
    float e = fp::recip_estimate(x, c);
    e = fp::mul(e, fp::recip_step(x, e, c), c);
    FP_CHECK(std::fabs(e - 1.0f / 3.0f) < 1e-5f);
}

}  // namespace

void run_fp_tests() {
    test_nans();
    test_flush_to_zero();
    test_rounding();
    test_conversions();
    test_estimates();
}
