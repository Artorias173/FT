// Arithmétique flottante aux règles ARM. Ce fichier est compilé avec
// -frounding-math pour que le compilateur respecte les changements de mode
// d'arrondi de l'hôte.
#include "fp.h"

#include <cfenv>
#include <cmath>
#include <cstring>

namespace vita::fp {

namespace {

template <typename T> struct Traits;

template <> struct Traits<float> {
    using Bits = u32;
    static constexpr Bits kSign = 0x80000000u, kExp = 0x7F800000u, kFrac = 0x007FFFFFu;
    static constexpr Bits kQuiet = 0x00400000u, kDefaultNaN = 0x7FC00000u;
};

template <> struct Traits<double> {
    using Bits = u64;
    static constexpr Bits kSign = 0x8000000000000000ull, kExp = 0x7FF0000000000000ull;
    static constexpr Bits kFrac = 0x000FFFFFFFFFFFFFull, kQuiet = 0x0008000000000000ull;
    static constexpr Bits kDefaultNaN = 0x7FF8000000000000ull;
};

template <typename T> typename Traits<T>::Bits to_bits(T v) {
    typename Traits<T>::Bits b;
    std::memcpy(&b, &v, sizeof(v));
    return b;
}
template <typename T> T from_bits(typename Traits<T>::Bits b) {
    T v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

template <typename T> bool is_nan(T v) {
    using F = Traits<T>;
    const auto b = to_bits(v);
    return (b & F::kExp) == F::kExp && (b & F::kFrac);
}
template <typename T> bool is_snan(T v) { return is_nan(v) && !(to_bits(v) & Traits<T>::kQuiet); }
template <typename T> bool is_zero(T v) { return (to_bits(v) & ~Traits<T>::kSign) == 0; }
template <typename T> bool is_inf(T v) { return (to_bits(v) & ~Traits<T>::kSign) == Traits<T>::kExp; }
template <typename T> bool is_denormal(T v) {
    const auto b = to_bits(v);
    return (b & Traits<T>::kExp) == 0 && (b & Traits<T>::kFrac);
}
template <typename T> bool sign_of(T v) { return to_bits(v) & Traits<T>::kSign; }
template <typename T> T signed_zero(bool negative) { return from_bits<T>(negative ? Traits<T>::kSign : 0); }
template <typename T> T signed_inf(bool negative) {
    return from_bits<T>(Traits<T>::kExp | (negative ? Traits<T>::kSign : 0));
}
template <typename T> T default_nan() { return from_bits<T>(Traits<T>::kDefaultNaN); }

template <typename T> T flush_input(T v, Ctx& c) {
    if (c.fz && is_denormal(v)) {
        c.exceptions |= kIDC;
        return signed_zero<T>(sign_of(v));
    }
    return v;
}

// Propagation ARM : NaN signalant d'abord (rendu silencieux), puis le premier NaN silencieux.
template <typename T> bool process_nans(T a, T b, Ctx& c, T& out) {
    if (is_snan(a)) out = from_bits<T>(to_bits(a) | Traits<T>::kQuiet);
    else if (is_snan(b)) out = from_bits<T>(to_bits(b) | Traits<T>::kQuiet);
    else if (is_nan(a)) out = a;
    else if (is_nan(b)) out = b;
    else return false;
    if (is_snan(a) || is_snan(b)) c.exceptions |= kIOC;
    if (c.dn) out = default_nan<T>();
    return true;
}
template <typename T> bool process_nan(T a, Ctx& c, T& out) { return process_nans(a, a, c, out); }

// Mode d'arrondi et drapeaux d'exception de l'hôte, le temps d'une opération.
class HostEnv {
public:
    explicit HostEnv(u32 rmode) {
        std::feclearexcept(FE_ALL_EXCEPT);
        if (rmode != kNearest) {
            static const int kModes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
            saved_ = std::fegetround();
            std::fesetround(kModes[rmode & 3]);
        }
    }
    ~HostEnv() {
        if (saved_ >= 0) std::fesetround(saved_);
    }
    u32 flags() const {
        const int f = std::fetestexcept(FE_ALL_EXCEPT);
        u32 r = 0;
        if (f & FE_INVALID) r |= kIOC;
        if (f & FE_DIVBYZERO) r |= kDZC;
        if (f & FE_OVERFLOW) r |= kOFC;
        if (f & FE_UNDERFLOW) r |= kUFC;
        if (f & FE_INEXACT) r |= kIXC;
        return r;
    }

private:
    int saved_ = -1;
};

// Applique au résultat de l'hôte les règles ARM : NaN par défaut, mise à zéro en sortie.
template <typename T> T finish(T r, u32 host_flags, Ctx& c) {
    if (is_nan(r)) {  // opération invalide (aucune entrée n'était un NaN)
        c.exceptions |= kIOC;
        return default_nan<T>();
    }
    if (c.fz && is_denormal(r)) {
        c.exceptions |= kUFC;  // ni IXC ni l'éventuel UFC de l'hôte
        return signed_zero<T>(sign_of(r));
    }
    c.exceptions |= host_flags;
    return r;
}

template <typename T, typename Op> T binary(T a, T b, Ctx& c, Op op) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    T out;
    if (process_nans(a, b, c, out)) return out;
    u32 flags;
    T r;
    {
        HostEnv env(c.rmode);
        volatile T va = a, vb = b;
        volatile T vr = op(va, vb);
        r = vr;
        flags = env.flags();
    }
    return finish(r, flags, c);
}

// Arrondit v / 2^shift selon le mode d'arrondi.
u64 round_shift(u64 v, int shift, bool negative, u32 rmode, bool& inexact) {
    if (shift <= 0) {
        inexact = false;
        return v << -shift;
    }
    u64 q, rem, half;
    if (shift >= 64) {
        q = 0;
        rem = v;
        half = ~0ull;  // le reste est toujours inférieur à la moitié
    } else {
        q = v >> shift;
        rem = v & ((1ull << shift) - 1);
        half = 1ull << (shift - 1);
    }
    inexact = rem != 0;
    switch (rmode) {
        case kNearest: if (rem > half || (rem == half && (q & 1))) ++q; break;
        case kPlusInf: if (inexact && !negative) ++q; break;
        case kMinusInf: if (inexact && negative) ++q; break;
        default: break;
    }
    return q;
}

double round_to_integral(double x, u32 rmode) {
    switch (rmode) {
        case kNearest: return std::nearbyint(x);  // l'hôte est en arrondi au plus proche
        case kPlusInf: return std::ceil(x);
        case kMinusInf: return std::floor(x);
        default: return std::trunc(x);
    }
}

// Saturation commune à to_int et to_fixed.
s64 saturate(double r, double x, s64 lo, s64 hi, Ctx& c) {
    if (r > static_cast<double>(hi)) { c.exceptions |= kIOC; return hi; }
    if (r < static_cast<double>(lo)) { c.exceptions |= kIOC; return lo; }
    if (r != x) c.exceptions |= kIXC;
    return static_cast<s64>(r);
}

// Algorithmes d'estimation de l'architecture ARMv7 (a dans [0.5, 1) ou [0.25, 1)).
double recip_estimate_core(double a) {
    const double q = std::floor(a * 512.0);
    const double r = 1.0 / ((q + 0.5) / 512.0);
    return std::floor(r * 256.0 + 0.5) / 256.0;
}
double rsqrt_estimate_core(double a) {
    double r;
    if (a < 0.5) r = 1.0 / std::sqrt((std::floor(a * 512.0) + 0.5) / 512.0);
    else r = 1.0 / std::sqrt((std::floor(a * 256.0) + 0.5) / 256.0);
    return std::floor(r * 256.0 + 0.5) / 256.0;
}
u64 dbits(double d) { return to_bits(d); }
double dfrom(u64 b) { return from_bits<double>(b); }

}  // namespace

template <typename T> T add(T a, T b, Ctx& c) { return binary(a, b, c, [](T x, T y) { return x + y; }); }
template <typename T> T sub(T a, T b, Ctx& c) { return binary(a, b, c, [](T x, T y) { return x - y; }); }
template <typename T> T mul(T a, T b, Ctx& c) { return binary(a, b, c, [](T x, T y) { return x * y; }); }
template <typename T> T div(T a, T b, Ctx& c) { return binary(a, b, c, [](T x, T y) { return x / y; }); }

template <typename T> T sqrt(T a, Ctx& c) {
    a = flush_input(a, c);
    T out;
    if (process_nan(a, c, out)) return out;
    u32 flags;
    T r;
    {
        HostEnv env(c.rmode);
        volatile T va = a;
        volatile T vr = std::sqrt(static_cast<T>(va));
        r = vr;
        flags = env.flags();
    }
    return finish(r, flags, c);
}

template <typename T> T neg(T a) { return from_bits<T>(to_bits(a) ^ Traits<T>::kSign); }
template <typename T> T abs(T a) { return from_bits<T>(to_bits(a) & ~Traits<T>::kSign); }

template <typename T> T max(T a, T b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    T out;
    if (process_nans(a, b, c, out)) return out;
    if (is_zero(a) && is_zero(b)) return signed_zero<T>(sign_of(a) && sign_of(b));
    return a > b ? a : b;
}

template <typename T> T min(T a, T b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    T out;
    if (process_nans(a, b, c, out)) return out;
    if (is_zero(a) && is_zero(b)) return signed_zero<T>(sign_of(a) || sign_of(b));
    return a < b ? a : b;
}

template <typename T> u32 compare(T a, T b, bool quiet_nan_signals, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    if (is_nan(a) || is_nan(b)) {
        if (quiet_nan_signals || is_snan(a) || is_snan(b)) c.exceptions |= kIOC;
        return 0x3;  // non ordonnés : C et V
    }
    if (a == b) return 0x6;  // Z et C
    return a < b ? 0x8 : 0x2;
}

template <typename T> bool compare_eq(T a, T b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    if (is_nan(a) || is_nan(b)) {
        if (is_snan(a) || is_snan(b)) c.exceptions |= kIOC;
        return false;
    }
    return a == b;
}

template <typename T> bool compare_ge(T a, T b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    if (is_nan(a) || is_nan(b)) {
        c.exceptions |= kIOC;
        return false;
    }
    return a >= b;
}

template <typename T> bool compare_gt(T a, T b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    if (is_nan(a) || is_nan(b)) {
        c.exceptions |= kIOC;
        return false;
    }
    return a > b;
}

float to_single(double v, Ctx& c) {
    v = flush_input(v, c);
    if (is_nan(v)) {
        if (is_snan(v)) c.exceptions |= kIOC;
        if (c.dn) return default_nan<float>();
        const u64 b = to_bits(v);
        return from_bits<float>(static_cast<u32>((b >> 32) & 0x80000000u) | 0x7FC00000u |
                                static_cast<u32>((b >> 29) & 0x3FFFFFu));
    }
    u32 flags;
    float r;
    {
        HostEnv env(c.rmode);
        volatile double vv = v;
        volatile float vr = static_cast<float>(vv);
        r = vr;
        flags = env.flags();
    }
    return finish(r, flags, c);
}

double to_double(float v, Ctx& c) {
    v = flush_input(v, c);
    if (is_nan(v)) {
        if (is_snan(v)) c.exceptions |= kIOC;
        if (c.dn) return default_nan<double>();
        const u32 b = to_bits(v);
        return from_bits<double>((static_cast<u64>(b & 0x80000000u) << 32) | 0x7FF8000000000000ull |
                                 (static_cast<u64>(b & 0x3FFFFFu) << 29));
    }
    return static_cast<double>(v);  // toujours exacte
}

template <typename T> u32 to_int(T v, bool is_signed, u32 rmode, Ctx& c) {
    v = flush_input(v, c);
    if (is_nan(v)) {
        c.exceptions |= kIOC;
        return 0;
    }
    const double x = static_cast<double>(v);
    const double r = round_to_integral(x, rmode);
    return is_signed ? static_cast<u32>(saturate(r, x, -2147483648ll, 2147483647ll, c))
                     : static_cast<u32>(saturate(r, x, 0, 4294967295ll, c));
}

template <typename T> T from_int(u32 v, bool is_signed, Ctx& c) {
    u32 flags;
    T r;
    {
        HostEnv env(c.rmode);
        volatile u32 vv = v;
        volatile T vr = is_signed ? static_cast<T>(static_cast<s32>(vv)) : static_cast<T>(vv);
        r = vr;
        flags = env.flags();
    }
    c.exceptions |= flags & kIXC;
    return r;
}

template <typename T> u64 to_fixed(T v, u32 size, u32 frac_bits, bool is_unsigned, u32 rmode, Ctx& c) {
    v = flush_input(v, c);
    if (is_nan(v)) {
        c.exceptions |= kIOC;
        return 0;
    }
    const double x = std::ldexp(static_cast<double>(v), static_cast<int>(frac_bits));
    const double r = round_to_integral(x, rmode);
    if (is_unsigned) return static_cast<u64>(saturate(r, x, 0, static_cast<s64>((1ull << size) - 1), c));
    const s64 limit = static_cast<s64>(1ull << (size - 1));
    return static_cast<u64>(saturate(r, x, -limit, limit - 1, c));
}

template <typename T> T from_fixed(u64 v, u32 size, u32 frac_bits, bool is_unsigned, Ctx& c) {
    const u64 m = size == 64 ? ~0ull : (1ull << size) - 1;
    s64 iv = static_cast<s64>(v & m);
    if (!is_unsigned && size < 64 && (iv >> (size - 1)) & 1) iv -= static_cast<s64>(1ull << size);
    u32 flags;
    T r;
    {
        HostEnv env(c.rmode);
        volatile s64 vv = iv;
        volatile T vr = static_cast<T>(vv);
        r = std::ldexp(static_cast<T>(vr), -static_cast<int>(frac_bits));  // exacte : puissance de deux
        flags = env.flags();
    }
    c.exceptions |= flags & kIXC;
    return r;
}

u16 to_half(float v, Ctx& c) {
    v = flush_input(v, c);
    const u32 b = to_bits(v);
    const u16 sign = static_cast<u16>((b >> 16) & 0x8000);
    if (is_nan(v)) {
        if (c.ahp) {  // le format alternatif n'a pas de NaN
            c.exceptions |= kIOC;
            return 0;
        }
        if (is_snan(v)) c.exceptions |= kIOC;
        if (c.dn) return 0x7E00;
        return static_cast<u16>(sign | 0x7E00 | ((b >> 13) & 0x1FF));
    }
    if (is_inf(v)) {
        if (c.ahp) {
            c.exceptions |= kIOC;
            return static_cast<u16>(sign | 0x7FFF);
        }
        return static_cast<u16>(sign | 0x7C00);
    }
    if (is_zero(v)) return sign;

    // Valeur exacte = sig * 2^e.
    const u32 exp = (b >> 23) & 0xFF;
    const u64 sig = exp ? (b & 0x7FFFFF) | 0x800000 : (b & 0x7FFFFF);
    const int e = exp ? static_cast<int>(exp) - 150 : -149;
    const int top = 63 - __builtin_clzll(sig);
    const int unbiased = top + e;
    int he = unbiased < -14 ? -24 : unbiased - 10;  // exposant du bit de poids faible de la demi-précision
    bool inexact;
    u64 hsig = round_shift(sig, he - e, sign, c.rmode, inexact);
    if (hsig >= 2048) {
        hsig >>= 1;
        ++he;
    }
    const u32 biased = hsig >= 1024 ? static_cast<u32>(he + 25) : 0;

    if (biased > (c.ahp ? 31u : 30u)) {  // dépassement
        if (c.ahp) {
            c.exceptions |= kIOC;
            return static_cast<u16>(sign | 0x7FFF);
        }
        c.exceptions |= kOFC | kIXC;
        const bool to_inf = c.rmode == kNearest || (c.rmode == kPlusInf && !sign) || (c.rmode == kMinusInf && sign);
        return static_cast<u16>(sign | (to_inf ? 0x7C00 : 0x7BFF));
    }
    if (inexact) {
        c.exceptions |= kIXC;
        if (unbiased < -14) c.exceptions |= kUFC;
    }
    return static_cast<u16>(sign | (biased << 10) | (hsig & 0x3FF));
}

float from_half(u16 h, Ctx& c) {
    const bool negative = h & 0x8000;
    const u32 exp = (h >> 10) & 0x1F, frac = h & 0x3FF;
    if (exp == 31 && !c.ahp) {
        if (frac == 0) return signed_inf<float>(negative);
        if (!(frac & 0x200)) c.exceptions |= kIOC;  // NaN signalant
        if (c.dn) return default_nan<float>();
        return from_bits<float>((negative ? 0x80000000u : 0) | 0x7FC00000u | ((frac & 0x1FF) << 13));
    }
    // Le FZ ne s'applique pas à la demi-précision ; toute valeur est exacte en simple précision.
    const float magnitude = exp ? std::ldexp(static_cast<float>(frac | 0x400), static_cast<int>(exp) - 25)
                                : std::ldexp(static_cast<float>(frac), -24);
    return negative ? -magnitude : magnitude;
}

float recip_estimate(float v, Ctx& c) {
    v = flush_input(v, c);
    float out;
    if (process_nan(v, c, out)) return out;
    const bool negative = sign_of(v);
    if (is_inf(v)) return signed_zero<float>(negative);
    if (is_zero(v)) {
        c.exceptions |= kDZC;
        return signed_inf<float>(negative);
    }
    const u32 b = to_bits(v), exp = (b >> 23) & 0xFF;
    if (exp >= 253) {
        c.exceptions |= kUFC;
        return signed_zero<float>(negative);
    }
    const u64 est = dbits(recip_estimate_core(dfrom(0x3FE0000000000000ull | (static_cast<u64>(b & 0x7FFFFF) << 29))));
    return from_bits<float>((b & 0x80000000u) | ((253 - exp) << 23) | static_cast<u32>((est >> 29) & 0x7FFFFF));
}

float rsqrt_estimate(float v, Ctx& c) {
    v = flush_input(v, c);
    float out;
    if (process_nan(v, c, out)) return out;
    if (is_zero(v)) {
        c.exceptions |= kDZC;
        return signed_inf<float>(sign_of(v));
    }
    if (sign_of(v)) {
        c.exceptions |= kIOC;
        return default_nan<float>();
    }
    if (is_inf(v)) return 0.0f;
    const u32 b = to_bits(v), exp = (b >> 23) & 0xFF;
    const u64 scaled = ((b >> 23) & 1 ? 0x3FD0000000000000ull : 0x3FE0000000000000ull) |
                       (static_cast<u64>(b & 0x7FFFFF) << 29);
    const u64 est = dbits(rsqrt_estimate_core(dfrom(scaled)));
    return from_bits<float>((((380 - exp) / 2) << 23) | static_cast<u32>((est >> 29) & 0x7FFFFF));
}

u32 recip_estimate_u32(u32 v) {
    if (!(v >> 31)) return 0xFFFFFFFF;
    const u64 est = dbits(recip_estimate_core(dfrom(0x3FE0000000000000ull | (static_cast<u64>(v & 0x7FFFFFFF) << 21))));
    return 0x80000000u | static_cast<u32>((est >> 21) & 0x7FFFFFFF);
}

u32 rsqrt_estimate_u32(u32 v) {
    if ((v >> 30) == 0) return 0xFFFFFFFF;
    const u64 scaled = (v >> 31) ? 0x3FE0000000000000ull | (static_cast<u64>(v & 0x7FFFFFFF) << 21)
                                 : 0x3FD0000000000000ull | (static_cast<u64>(v & 0x3FFFFFFF) << 22);
    const u64 est = dbits(rsqrt_estimate_core(dfrom(scaled)));
    return 0x80000000u | static_cast<u32>((est >> 21) & 0x7FFFFFFF);
}

float recip_step(float a, float b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    float out;
    if (process_nans(a, b, c, out)) return out;
    if ((is_inf(a) && is_zero(b)) || (is_zero(a) && is_inf(b))) return 2.0f;
    return sub(2.0f, mul(a, b, c), c);
}

float rsqrt_step(float a, float b, Ctx& c) {
    a = flush_input(a, c);
    b = flush_input(b, c);
    float out;
    if (process_nans(a, b, c, out)) return out;
    if ((is_inf(a) && is_zero(b)) || (is_zero(a) && is_inf(b))) return 1.5f;
    return mul(sub(3.0f, mul(a, b, c), c), 0.5f, c);
}

// Instanciations explicites.
#define VITA_FP_INSTANTIATE(T)                                                        \
    template T add<T>(T, T, Ctx&);                                                    \
    template T sub<T>(T, T, Ctx&);                                                    \
    template T mul<T>(T, T, Ctx&);                                                    \
    template T div<T>(T, T, Ctx&);                                                    \
    template T sqrt<T>(T, Ctx&);                                                      \
    template T neg<T>(T);                                                             \
    template T abs<T>(T);                                                             \
    template T max<T>(T, T, Ctx&);                                                    \
    template T min<T>(T, T, Ctx&);                                                    \
    template u32 compare<T>(T, T, bool, Ctx&);                                        \
    template bool compare_eq<T>(T, T, Ctx&);                                          \
    template bool compare_ge<T>(T, T, Ctx&);                                          \
    template bool compare_gt<T>(T, T, Ctx&);                                          \
    template u32 to_int<T>(T, bool, u32, Ctx&);                                       \
    template T from_int<T>(u32, bool, Ctx&);                                          \
    template u64 to_fixed<T>(T, u32, u32, bool, u32, Ctx&);                           \
    template T from_fixed<T>(u64, u32, u32, bool, Ctx&);

VITA_FP_INSTANTIATE(float)
VITA_FP_INSTANTIATE(double)

}  // namespace vita::fp
