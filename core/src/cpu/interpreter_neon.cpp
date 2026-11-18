// Interpréteur : jeu d'instructions vectoriel NEON (Advanced SIMD, ARMv7).
//
// Les registres Q0-Q15 sont les paires de D0-D31. Les calculs flottants NEON
// utilisent toujours les réglages « standard » (mise à zéro des dénormaux,
// NaN par défaut, arrondi au plus proche), quel que soit le FPSCR ; leurs
// exceptions et les saturations (QC) s'accumulent dans le FPSCR.
#include <cstring>

#include "fp.h"
#include "interpreter_util.h"
#include "vita/cpu/interpreter.h"
#include "vita/memory/memory.h"

namespace vita {

using namespace arm;
using s128 = __int128;

namespace {

// ---------------------------------------------------------------------------
// Vecteurs et éléments
// ---------------------------------------------------------------------------

struct Vec {
    u64 d[2] = {0, 0};
};

Vec load(const CpuState& s, u32 reg, bool q) {
    Vec v;
    v.d[0] = s.d[reg];
    if (q) v.d[1] = s.d[reg + 1];
    return v;
}

void store(CpuState& s, u32 reg, bool q, const Vec& v) {
    s.d[reg] = v.d[0];
    if (q) s.d[reg + 1] = v.d[1];
}

constexpr u64 ones(u32 esize) { return esize >= 64 ? ~0ull : (1ull << esize) - 1; }

u64 elem(const Vec& v, u32 i, u32 esize) {
    const u32 pos = i * esize;
    return esize == 64 ? v.d[pos >> 6] : (v.d[pos >> 6] >> (pos & 63)) & ones(esize);
}

void set_elem(Vec& v, u32 i, u32 esize, u64 value) {
    const u32 pos = i * esize;
    u64& w = v.d[pos >> 6];
    if (esize == 64) {
        w = value;
        return;
    }
    const u64 m = ones(esize) << (pos & 63);
    w = (w & ~m) | ((value << (pos & 63)) & m);
}

s64 sext(u64 v, u32 esize) { return esize >= 64 ? static_cast<s64>(v) : static_cast<s64>(v << (64 - esize)) >> (64 - esize); }

// Valeur d'un élément, signée ou non, sur 128 bits.
s128 ext(u64 v, u32 esize, bool is_unsigned) {
    return is_unsigned ? static_cast<s128>(v & ones(esize)) : static_cast<s128>(sext(v, esize));
}

u64 sat_signed(s128 v, u32 esize, bool& sat) {
    const s128 hi = (static_cast<s128>(1) << (esize - 1)) - 1, lo = -hi - 1;
    if (v > hi) { sat = true; v = hi; }
    if (v < lo) { sat = true; v = lo; }
    return static_cast<u64>(v) & ones(esize);
}

u64 sat_unsigned(s128 v, u32 esize, bool& sat) {
    const s128 hi = (static_cast<s128>(1) << esize) - 1;
    if (v > hi) { sat = true; v = hi; }
    if (v < 0) { sat = true; v = 0; }
    return static_cast<u64>(v);
}

u64 saturate(s128 v, u32 esize, bool is_unsigned, bool& sat) {
    return is_unsigned ? sat_unsigned(v, esize, sat) : sat_signed(v, esize, sat);
}

float as_f32(u64 b) {
    const u32 w = static_cast<u32>(b);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

u64 f32_bits(float f) {
    u32 w;
    std::memcpy(&w, &f, 4);
    return w;
}

// Décalage à gauche sur 128 bits sans comportement indéfini pour les valeurs négatives.
s128 shl(s128 v, u32 n) { return v * (static_cast<s128>(1) << n); }

// Décalage arrondi vers la droite d'une valeur sur 128 bits (n >= 1, plafonné).
s128 shift_right(s128 v, u32 n, bool round, u32 esize) {
    if (n > esize + 1) n = esize + 1;
    if (round) v += static_cast<s128>(1) << (n - 1);
    return v >> n;
}

// VSHL, VRSHL, VQSHL, VQRSHL : décalage par la valeur signée d'un registre.
u64 shift_by_register(u64 value, s32 shift, u32 esize, bool is_unsigned, bool round, bool sat_mode, bool& sat) {
    const s128 v = ext(value, esize, is_unsigned);
    s128 r;
    if (shift >= 0) {
        if (static_cast<u32>(shift) >= esize) r = v == 0 ? 0 : (v > 0 ? static_cast<s128>(1) << 100 : -(static_cast<s128>(1) << 100));
        else r = shl(v, static_cast<u32>(shift));
        if (!sat_mode) return static_cast<u32>(shift) >= esize ? 0 : static_cast<u64>(r) & ones(esize);
    } else {
        r = shift_right(v, static_cast<u32>(-shift), round, esize);
    }
    return sat_mode ? saturate(r, esize, is_unsigned, sat) : static_cast<u64>(r) & ones(esize);
}

u32 count_leading_zeros_n(u64 v, u32 esize) {
    u32 n = 0;
    for (int b = static_cast<int>(esize) - 1; b >= 0 && !((v >> b) & 1); --b) ++n;
    return n;
}

u64 poly_mul(u64 a, u64 b, u32 esize) {  // multiplication polynomiale (sans retenue)
    u64 r = 0;
    for (u32 i = 0; i < esize; ++i)
        if ((b >> i) & 1) r ^= a << i;
    return r;
}

u64 rep(u64 v, u32 esize) {  // réplique un motif sur 64 bits
    u64 r = 0;
    for (u32 i = 0; i < 64; i += esize) r |= (v & ones(esize)) << i;
    return r;
}

// Immédiat modifié NEON (AdvSIMDExpandImm). Renvoie false si l'encodage est réservé.
bool expand_imm(u32 op, u32 cmode, u32 imm8, u64& out) {
    switch (cmode >> 1) {
        case 0: out = rep(imm8, 32); return true;
        case 1: out = rep(static_cast<u64>(imm8) << 8, 32); return true;
        case 2: out = rep(static_cast<u64>(imm8) << 16, 32); return true;
        case 3: out = rep(static_cast<u64>(imm8) << 24, 32); return true;
        case 4: out = rep(imm8, 16); return true;
        case 5: out = rep(static_cast<u64>(imm8) << 8, 16); return true;
        case 6: out = rep((cmode & 1) ? (static_cast<u64>(imm8) << 16) | 0xFFFF : (static_cast<u64>(imm8) << 8) | 0xFF, 32); return true;
        default:
            if (!(cmode & 1)) {
                if (!op) { out = rep(imm8, 8); return true; }
                out = 0;
                for (u32 i = 0; i < 8; ++i)
                    if ((imm8 >> i) & 1) out |= 0xFFull << (i * 8);
                return true;
            }
            if (op) return false;
            {
                const u32 b6 = bit(imm8, 6);
                const u32 f = (bit(imm8, 7) << 31) | ((b6 ^ 1) << 30) | (b6 ? 0x3E000000u : 0) | (bits(imm8, 5, 0) << 19);
                out = rep(f, 32);
            }
            return true;
    }
}

// ---------------------------------------------------------------------------
// Groupes d'instructions
// ---------------------------------------------------------------------------

struct Regs {
    u32 d, n, m;
    bool q;
};

Regs decode_regs(u32 instr) {
    return {(bit(instr, 22) << 4) | bits(instr, 15, 12), (bit(instr, 7) << 4) | bits(instr, 19, 16),
            (bit(instr, 5) << 4) | bits(instr, 3, 0), bit(instr, 6)};
}

// Trois registres de même taille.
bool three_same(CpuState& s, fp::Ctx& c, bool& sat, u32 instr) {
    const Regs r = decode_regs(instr);
    const bool u = bit(instr, 24), b = bit(instr, 4);
    const u32 a = bits(instr, 11, 8), size = bits(instr, 21, 20);
    if (r.q && ((r.d | r.n | r.m) & 1)) return false;
    const u32 esize = 8u << size, count = (r.q ? 128 : 64) / esize;
    const Vec vn = load(s, r.n, r.q), vm = load(s, r.m, r.q), vd = load(s, r.d, r.q);
    Vec out;

    // Opérations bit à bit sur des mots entiers.
    if (a == 1 && b) {
        for (int w = 0; w < 2; ++w) {
            const u64 x = vn.d[w], y = vm.d[w], z = vd.d[w];
            u64 v;
            switch ((u << 2) | size) {
                case 0: v = x & y; break;                   // VAND
                case 1: v = x & ~y; break;                  // VBIC
                case 2: v = x | y; break;                   // VORR (VMOV)
                case 3: v = x | ~y; break;                  // VORN
                case 4: v = x ^ y; break;                   // VEOR
                case 5: v = (z & x) | (~z & y); break;      // VBSL
                case 6: v = (z & ~y) | (x & y); break;      // VBIT
                default: v = (z & y) | (x & ~y); break;     // VBIF
            }
            out.d[w] = v;
        }
        store(s, r.d, r.q, out);
        return true;
    }

    // Opérations par paires (registres D uniquement).
    auto pairwise = [&](auto op, u32 es) {
        if (r.q) return false;
        const u32 n = 64 / es, half = n / 2;
        for (u32 e = 0; e < half; ++e) {
            set_elem(out, e, es, op(elem(vn, 2 * e, es), elem(vn, 2 * e + 1, es)));
            set_elem(out, e + half, es, op(elem(vm, 2 * e, es), elem(vm, 2 * e + 1, es)));
        }
        store(s, r.d, false, out);
        return true;
    };

    // Opérations flottantes (taille 32 bits obligatoire).
    if (a >= 0xD) {
        if (bit(instr, 20)) return false;
        const bool op21 = bit(instr, 21);
        auto f2 = [&](auto op) {
            for (u32 e = 0; e < (r.q ? 4u : 2u); ++e)
                set_elem(out, e, 32, op(as_f32(elem(vn, e, 32)), as_f32(elem(vm, e, 32)), as_f32(elem(vd, e, 32))));
            store(s, r.d, r.q, out);
            return true;
        };
        auto fpair = [&](auto op) { return pairwise([&](u64 x, u64 y) { return op(as_f32(x), as_f32(y)); }, 32); };
        const u64 kTrue = 0xFFFFFFFF;
        switch ((a << 2) | (b << 1) | u) {
            case (0xD << 2) | 0:  // VADD.F32 / VSUB.F32
                return f2([&](float x, float y, float) { return f32_bits(op21 ? fp::sub(x, y, c) : fp::add(x, y, c)); });
            case (0xD << 2) | 1:  // VPADD.F32 / VABD.F32
                if (!op21) return fpair([&](float x, float y) { return f32_bits(fp::add(x, y, c)); });
                return f2([&](float x, float y, float) { return f32_bits(fp::abs(fp::sub(x, y, c))); });
            case (0xD << 2) | 2:  // VMLA.F32 / VMLS.F32
                return f2([&](float x, float y, float z) {
                    const float p = fp::mul(x, y, c);
                    return f32_bits(op21 ? fp::sub(z, p, c) : fp::add(z, p, c));
                });
            case (0xD << 2) | 3:  // VMUL.F32
                if (op21) return false;
                return f2([&](float x, float y, float) { return f32_bits(fp::mul(x, y, c)); });
            case (0xE << 2) | 0:  // VCEQ.F32
                if (op21) return false;
                return f2([&](float x, float y, float) { return fp::compare_eq(x, y, c) ? kTrue : 0; });
            case (0xE << 2) | 1:  // VCGE.F32 / VCGT.F32
                return f2([&](float x, float y, float) {
                    return (op21 ? fp::compare_gt(x, y, c) : fp::compare_ge(x, y, c)) ? kTrue : 0;
                });
            case (0xE << 2) | 3:  // VACGE / VACGT
                return f2([&](float x, float y, float) {
                    return (op21 ? fp::compare_gt(fp::abs(x), fp::abs(y), c) : fp::compare_ge(fp::abs(x), fp::abs(y), c)) ? kTrue : 0;
                });
            case (0xF << 2) | 0:  // VMAX.F32 / VMIN.F32
                return f2([&](float x, float y, float) { return f32_bits(op21 ? fp::min(x, y, c) : fp::max(x, y, c)); });
            case (0xF << 2) | 1:  // VPMAX.F32 / VPMIN.F32
                return fpair([&](float x, float y) { return f32_bits(op21 ? fp::min(x, y, c) : fp::max(x, y, c)); });
            case (0xF << 2) | 2:  // VRECPS / VRSQRTS
                return f2([&](float x, float y, float) {
                    return f32_bits(op21 ? fp::rsqrt_step(x, y, c) : fp::recip_step(x, y, c));
                });
            default: return false;  // VFMA/VFMS (VFPv4)...
        }
    }

    if (a == 0xA) {  // VPMAX / VPMIN
        if (size == 3) return false;
        return pairwise([&](u64 x, u64 y) {
            const s128 vx = ext(x, esize, u), vy = ext(y, esize, u);
            return b ? (vx < vy ? x : y) : (vx > vy ? x : y);
        }, esize);
    }
    if (a == 0xB && b) {  // VPADD (entiers)
        if (u || size == 3) return false;
        return pairwise([&](u64 x, u64 y) { return (x + y) & ones(esize); }, esize);
    }

    for (u32 e = 0; e < count; ++e) {
        const u64 x = elem(vn, e, esize), y = elem(vm, e, esize), z = elem(vd, e, esize);
        const s128 sx = ext(x, esize, u), sy = ext(y, esize, u);
        u64 v;
        switch ((a << 1) | b) {
            case 0x00: if (size == 3) return false; v = static_cast<u64>((sx + sy) >> 1); break;      // VHADD
            case 0x01: v = saturate(sx + sy, esize, u, sat); break;                                     // VQADD
            case 0x02: if (size == 3) return false; v = static_cast<u64>((sx + sy + 1) >> 1); break;  // VRHADD
            case 0x04: if (size == 3) return false; v = static_cast<u64>((sx - sy) >> 1); break;      // VHSUB
            case 0x05: v = saturate(sx - sy, esize, u, sat); break;                                     // VQSUB
            case 0x06: if (size == 3) return false; v = sx > sy ? ~0ull : 0; break;                    // VCGT
            case 0x07: if (size == 3) return false; v = sx >= sy ? ~0ull : 0; break;                   // VCGE
            case 0x08: v = shift_by_register(y, static_cast<s8>(x), esize, u, false, false, sat); break; // VSHL
            case 0x09: v = shift_by_register(y, static_cast<s8>(x), esize, u, false, true, sat); break;  // VQSHL
            case 0x0A: v = shift_by_register(y, static_cast<s8>(x), esize, u, true, false, sat); break;  // VRSHL
            case 0x0B: v = shift_by_register(y, static_cast<s8>(x), esize, u, true, true, sat); break;   // VQRSHL
            case 0x0C: if (size == 3) return false; v = sx > sy ? x : y; break;                         // VMAX
            case 0x0D: if (size == 3) return false; v = sx < sy ? x : y; break;                         // VMIN
            case 0x0E: if (size == 3) return false; v = static_cast<u64>(sx > sy ? sx - sy : sy - sx); break;  // VABD
            case 0x0F: if (size == 3) return false; v = z + static_cast<u64>(sx > sy ? sx - sy : sy - sx); break; // VABA
            case 0x10: v = u ? x - y : x + y; break;                                                     // VADD / VSUB
            case 0x11:                                                                                   // VTST / VCEQ
                if (size == 3) return false;
                v = (u ? x == y : (x & y) != 0) ? ~0ull : 0;
                break;
            case 0x12: if (size == 3) return false; v = u ? z - x * y : z + x * y; break;              // VMLA / VMLS
            case 0x13:                                                                                   // VMUL
                if (size == 3 || (u && size != 0)) return false;
                v = u ? poly_mul(x, y, esize) : x * y;
                break;
            case 0x16: {                                                                                 // VQDMULH / VQRDMULH
                if (size != 1 && size != 2) return false;
                const s128 p = 2 * static_cast<s128>(sext(x, esize)) * sext(y, esize) + (u ? static_cast<s128>(1) << (esize - 1) : 0);
                v = sat_signed(p >> esize, esize, sat);
                break;
            }
            default: return false;
        }
        set_elem(out, e, esize, v & ones(esize));
    }
    store(s, r.d, r.q, out);
    return true;
}

// Trois registres de tailles différentes (élargissement / rétrécissement).
bool three_diff(CpuState& s, bool& sat, u32 instr) {
    const Regs r = decode_regs(instr);
    const bool u = bit(instr, 24);
    const u32 a = bits(instr, 11, 8), size = bits(instr, 21, 20);
    const u32 esize = 8u << size, wsize = esize * 2, count = 64 / esize;
    const bool narrow = a == 4 || a == 6;
    const bool wide_n = a == 1 || a == 3;
    if ((!narrow && (r.d & 1)) || ((narrow || wide_n) && (r.n & 1)) || (narrow && (r.m & 1))) return false;

    const Vec vn = load(s, r.n, narrow || wide_n), vm = load(s, r.m, narrow), vd = load(s, r.d, !narrow);
    Vec out;
    for (u32 e = 0; e < count; ++e) {
        if (narrow) {  // VADDHN, VRADDHN, VSUBHN, VRSUBHN : moitié haute de la somme
            const u64 x = elem(vn, e, wsize), y = elem(vm, e, wsize);
            u64 v = a == 4 ? x + y : x - y;
            if (u) v += 1ull << (esize - 1);
            set_elem(out, e, esize, (v >> esize) & ones(esize));
            continue;
        }
        const s128 n = wide_n ? ext(elem(vn, e, wsize), wsize, u) : ext(elem(vn, e, esize), esize, u);
        const s128 m = ext(elem(vm, e, esize), esize, u);
        const s128 acc = ext(elem(vd, e, wsize), wsize, u);
        s128 v;
        switch (a) {
            case 0x0: case 0x1: v = n + m; break;                        // VADDL / VADDW
            case 0x2: case 0x3: v = n - m; break;                        // VSUBL / VSUBW
            case 0x5: v = acc + (n > m ? n - m : m - n); break;          // VABAL
            case 0x7: v = n > m ? n - m : m - n; break;                  // VABDL
            case 0x8: v = acc + n * m; break;                            // VMLAL
            case 0xA: v = acc - n * m; break;                            // VMLSL
            case 0xC: v = n * m; break;                                  // VMULL
            case 0x9: case 0xB: case 0xD: {                              // VQDMLAL, VQDMLSL, VQDMULL
                if (u || size == 0) return false;
                const s128 p = static_cast<s128>(sext(sat_signed(2 * n * m, wsize, sat), wsize));
                if (a == 0xD) v = p;
                else v = static_cast<s128>(sext(sat_signed(a == 0x9 ? acc + p : acc - p, wsize, sat), wsize));
                break;
            }
            case 0xE:                                                    // VMULL.P8
                if (u || size != 0) return false;
                v = static_cast<s128>(poly_mul(elem(vn, e, 8), elem(vm, e, 8), 8));
                break;
            default: return false;
        }
        set_elem(out, e, wsize, static_cast<u64>(v) & ones(wsize));
    }
    store(s, r.d, !narrow, out);
    return true;
}

// Deux registres et un scalaire (élément d'un registre D).
bool scalar_ops(CpuState& s, fp::Ctx& c, bool& sat, u32 instr) {
    const u32 a = bits(instr, 11, 8), size = bits(instr, 21, 20);
    const bool u = bit(instr, 24);
    const u32 d = (bit(instr, 22) << 4) | bits(instr, 15, 12), n = (bit(instr, 7) << 4) | bits(instr, 19, 16);
    u32 m, index;
    if (size == 1) { m = bits(instr, 2, 0); index = (bit(instr, 5) << 1) | bit(instr, 3); }
    else if (size == 2) { m = bits(instr, 3, 0); index = bit(instr, 5); }
    else return false;
    const u32 esize = 8u << size, wsize = esize * 2;
    const u64 scalar = elem(load(s, m, false), index, esize);
    const bool is_long = a == 2 || a == 3 || a == 6 || a == 7 || a == 0xA || a == 0xB;

    if (is_long) {
        if (d & 1) return false;
        if ((a == 3 || a == 7 || a == 0xB) && u) return false;
        const Vec vn = load(s, n, false), vd = load(s, d, true);
        Vec out;
        const s128 sm = ext(scalar, esize, u);
        for (u32 e = 0; e < 64 / esize; ++e) {
            const s128 x = ext(elem(vn, e, esize), esize, u), acc = ext(elem(vd, e, wsize), wsize, u);
            s128 v;
            switch (a) {
                case 0x2: v = acc + x * sm; break;  // VMLAL
                case 0x6: v = acc - x * sm; break;  // VMLSL
                case 0xA: v = x * sm; break;        // VMULL
                default: {                          // VQDMLAL, VQDMLSL, VQDMULL
                    const s128 p = static_cast<s128>(sext(sat_signed(2 * x * sm, wsize, sat), wsize));
                    v = a == 0xB ? p : static_cast<s128>(sext(sat_signed(a == 3 ? acc + p : acc - p, wsize, sat), wsize));
                    break;
                }
            }
            set_elem(out, e, wsize, static_cast<u64>(v) & ones(wsize));
        }
        store(s, d, true, out);
        return true;
    }

    const bool q = u;  // pour ces formes, le bit 24 indique un registre Q
    if (q && ((d | n) & 1)) return false;
    const Vec vn = load(s, n, q), vd = load(s, d, q);
    Vec out;
    const bool is_float = a == 1 || a == 5 || a == 9;
    if (is_float && size != 2) return false;
    for (u32 e = 0; e < (q ? 128 : 64) / esize; ++e) {
        const u64 x = elem(vn, e, esize), z = elem(vd, e, esize);
        u64 v;
        switch (a) {
            case 0x0: v = z + x * scalar; break;  // VMLA
            case 0x4: v = z - x * scalar; break;  // VMLS
            case 0x8: v = x * scalar; break;      // VMUL
            case 0x1: case 0x5: {                 // VMLA.F32 / VMLS.F32
                const float p = fp::mul(as_f32(x), as_f32(scalar), c);
                v = f32_bits(a == 1 ? fp::add(as_f32(z), p, c) : fp::sub(as_f32(z), p, c));
                break;
            }
            case 0x9: v = f32_bits(fp::mul(as_f32(x), as_f32(scalar), c)); break;  // VMUL.F32
            case 0xC: case 0xD: {                                                    // VQDMULH / VQRDMULH
                const s128 p = 2 * static_cast<s128>(sext(x, esize)) * sext(scalar, esize) +
                               (a == 0xD ? static_cast<s128>(1) << (esize - 1) : 0);
                v = sat_signed(p >> esize, esize, sat);
                break;
            }
            default: return false;
        }
        set_elem(out, e, esize, v & ones(esize));
    }
    store(s, d, q, out);
    return true;
}

// Deux registres et une valeur de décalage immédiate.
bool shift_imm(CpuState& s, fp::Ctx& c, bool& sat, u32 instr) {
    const Regs r = decode_regs(instr);
    const bool u = bit(instr, 24), l = bit(instr, 7);
    const u32 a = bits(instr, 11, 8), imm6 = bits(instr, 21, 16);
    u32 esize;
    if (l) esize = 64;
    else if (imm6 & 0x20) esize = 32;
    else if (imm6 & 0x10) esize = 16;
    else if (imm6 & 0x08) esize = 8;
    else return false;
    const u32 rshift = (l ? 64 : 2 * esize) - imm6;
    const u32 lshift = l ? imm6 : imm6 - esize;

    // Rétrécissements : Dd <- Qm (éléments sources de 2 * esize bits).
    if (a == 8 || a == 9) {
        if (l || (r.m & 1)) return false;
        const bool round = bit(instr, 6);
        const Vec vm = load(s, r.m, true);
        Vec out;
        const u32 wsize = esize * 2;
        for (u32 e = 0; e < 64 / esize; ++e) {
            const u64 x = elem(vm, e, wsize);
            u64 v;
            if (a == 8 && !u) {  // VSHRN / VRSHRN
                v = static_cast<u64>(shift_right(ext(x, wsize, true), rshift, round, wsize)) & ones(esize);
            } else {             // VQSHRUN / VQRSHRUN (source signée) ou VQSHRN / VQRSHRN
                const bool src_unsigned = a == 9 && u;
                const bool dst_unsigned = a == 8 || u;
                v = saturate(shift_right(ext(x, wsize, src_unsigned), rshift, round, wsize), esize, dst_unsigned, sat);
            }
            set_elem(out, e, esize, v);
        }
        store(s, r.d, false, out);
        return true;
    }
    if (a == 0xA) {  // VSHLL / VMOVL : Qd <- Dm
        if (l || r.q || (r.d & 1)) return false;
        const Vec vm = load(s, r.m, false);
        Vec out;
        for (u32 e = 0; e < 64 / esize; ++e)
            set_elem(out, e, esize * 2, static_cast<u64>(shl(ext(elem(vm, e, esize), esize, u), lshift)) & ones(esize * 2));
        store(s, r.d, true, out);
        return true;
    }

    if (r.q && ((r.d | r.m) & 1)) return false;
    const Vec vm = load(s, r.m, r.q), vd = load(s, r.d, r.q);
    Vec out;
    const u32 count = (r.q ? 128 : 64) / esize;

    if (a == 0xE || a == 0xF) {  // VCVT entre virgule fixe et flottant (32 bits)
        if (esize != 32) return false;
        const u32 frac = 64 - imm6;
        for (u32 e = 0; e < count; ++e) {
            const u64 x = elem(vm, e, 32);
            const u64 v = a == 0xE ? f32_bits(fp::from_fixed<float>(x, 32, frac, u, c))
                                   : fp::to_fixed(as_f32(x), 32, frac, u, fp::kZero, c) & 0xFFFFFFFF;
            set_elem(out, e, 32, v);
        }
        store(s, r.d, r.q, out);
        return true;
    }

    for (u32 e = 0; e < count; ++e) {
        const u64 x = elem(vm, e, esize), z = elem(vd, e, esize);
        u64 v;
        switch (a) {
            case 0x0: v = static_cast<u64>(shift_right(ext(x, esize, u), rshift, false, esize)); break;      // VSHR
            case 0x1: v = z + static_cast<u64>(shift_right(ext(x, esize, u), rshift, false, esize)); break;  // VSRA
            case 0x2: v = static_cast<u64>(shift_right(ext(x, esize, u), rshift, true, esize)); break;       // VRSHR
            case 0x3: v = z + static_cast<u64>(shift_right(ext(x, esize, u), rshift, true, esize)); break;   // VRSRA
            case 0x4: {                                                                                      // VSRI
                if (!u) return false;
                const u64 mask = rshift >= esize ? 0 : ones(esize) >> rshift;
                v = (z & ~mask) | ((rshift >= esize ? 0 : x >> rshift) & mask);
                break;
            }
            case 0x5:                                                                                        // VSHL / VSLI
                if (!u) v = x << lshift;
                else v = (z & ~(ones(esize) << lshift)) | (x << lshift);
                break;
            case 0x6:                                                                                        // VQSHLU
                if (!u) return false;
                v = sat_unsigned(shl(ext(x, esize, false), lshift), esize, sat);
                break;
            case 0x7: v = saturate(shl(ext(x, esize, u), lshift), esize, u, sat); break;                       // VQSHL
            default: return false;
        }
        set_elem(out, e, esize, v & ones(esize));
    }
    store(s, r.d, r.q, out);
    return true;
}

// Deux registres, opérations diverses.
bool two_misc(CpuState& s, fp::Ctx& c, bool& sat, u32 instr) {
    const Regs r = decode_regs(instr);
    const u32 size = bits(instr, 19, 18), a = bits(instr, 17, 16), b = bits(instr, 10, 6);
    const u32 esize = 8u << size;

    // Formes à registres de tailles différentes.
    if (a == 2 && (b == 8 || b == 9 || (b >> 1) == 5)) {  // VMOVN, VQMOVUN, VQMOVN : Dd <- Qm
        if (size == 3 || (r.m & 1)) return false;
        const Vec vm = load(s, r.m, true);
        Vec out;
        for (u32 e = 0; e < 64 / esize; ++e) {
            const u64 x = elem(vm, e, esize * 2);
            u64 v;
            if (b == 8) v = x & ones(esize);
            else if (b == 9) v = sat_unsigned(ext(x, esize * 2, false), esize, sat);
            else v = saturate(ext(x, esize * 2, bit(instr, 6)), esize, bit(instr, 6), sat);
            set_elem(out, e, esize, v);
        }
        store(s, r.d, false, out);
        return true;
    }
    if (a == 2 && b == 12) {  // VSHLL (décalage de esize) : Qd <- Dm
        if (size == 3 || (r.d & 1)) return false;
        const Vec vm = load(s, r.m, false);
        Vec out;
        for (u32 e = 0; e < 64 / esize; ++e) set_elem(out, e, esize * 2, elem(vm, e, esize) << esize);
        store(s, r.d, true, out);
        return true;
    }
    if (a == 2 && (b == 24 || b == 28)) {  // VCVT.F16.F32 (Dd <- Qm) / VCVT.F32.F16 (Qd <- Dm)
        if (size != 1) return false;
        Vec out;
        if (b == 24) {
            if (r.m & 1) return false;
            const Vec vm = load(s, r.m, true);
            for (u32 e = 0; e < 4; ++e) set_elem(out, e, 16, fp::to_half(as_f32(elem(vm, e, 32)), c));
            store(s, r.d, false, out);
        } else {
            if (r.d & 1) return false;
            const Vec vm = load(s, r.m, false);
            for (u32 e = 0; e < 4; ++e) set_elem(out, e, 32, f32_bits(fp::from_half(static_cast<u16>(elem(vm, e, 16)), c)));
            store(s, r.d, true, out);
        }
        return true;
    }

    if (r.q && ((r.d | r.m) & 1)) return false;
    const u32 count = (r.q ? 128 : 64) / esize;
    Vec vm = load(s, r.m, r.q), vd = load(s, r.d, r.q);
    Vec out;

    if (a == 2 && (b >> 1) <= 3) {  // VSWP, VTRN, VUZP, VZIP : modifient deux registres
        switch (b >> 1) {
            case 0: store(s, r.d, r.q, vm); store(s, r.m, r.q, vd); return true;  // VSWP
            case 1:                                                                // VTRN
                for (u32 e = 0; e + 1 < count; e += 2) {
                    const u64 t = elem(vd, e + 1, esize);
                    set_elem(vd, e + 1, esize, elem(vm, e, esize));
                    set_elem(vm, e, esize, t);
                }
                break;
            default: {  // VUZP / VZIP sur la concaténation Vd:Vm
                if (!r.q && size == 2) return false;
                u64 all[32], res[32];
                for (u32 e = 0; e < count; ++e) {
                    all[e] = elem(vd, e, esize);
                    all[count + e] = elem(vm, e, esize);
                }
                for (u32 e = 0; e < count; ++e) {
                    if ((b >> 1) == 2) {  // VUZP : pairs puis impairs
                        res[e] = all[2 * e];
                        res[count + e] = all[2 * e + 1];
                    } else {              // VZIP : entrelacement
                        res[2 * e] = all[e];
                        res[2 * e + 1] = all[count + e];
                    }
                }
                for (u32 e = 0; e < count; ++e) {
                    set_elem(vd, e, esize, res[e]);
                    set_elem(vm, e, esize, res[count + e]);
                }
                break;
            }
        }
        store(s, r.d, r.q, vd);
        store(s, r.m, r.q, vm);
        return true;
    }

    if (a == 0 && (b >> 2) == 2) {  // VPADDL / (b >> 2) == 6 : VPADAL
        if (size == 3) return false;
    }
    for (u32 e = 0; e < count; ++e) {
        const u64 x = elem(vm, e, esize);
        u64 v;
        if (a == 0) {
            const u32 op = b >> 1;
            if ((b >> 2) == 2 || (b >> 2) == 6) {  // VPADDL / VPADAL (éléments de 2 * esize)
                if (size == 3) return false;
                if (e >= count / 2) break;
                const bool uns = bit(instr, 7);
                const s128 sum = ext(elem(vm, 2 * e, esize), esize, uns) + ext(elem(vm, 2 * e + 1, esize), esize, uns);
                const u64 acc = (b >> 2) == 6 ? elem(vd, e, esize * 2) : 0;
                set_elem(out, e, esize * 2, (acc + static_cast<u64>(sum)) & ones(esize * 2));
                continue;
            }
            switch (op) {
                case 0x0: case 0x1: case 0x2: {  // VREV64, VREV32, VREV16
                    const u32 group = 64u >> op;
                    if (esize >= group) return false;
                    const u32 per = group / esize, base = e / per * per;
                    v = elem(vm, base + (per - 1 - (e - base)), esize);
                    break;
                }
                case 0x8: {  // VCLS
                    if (size == 3) return false;
                    const u64 t = (sext(x, esize) < 0) ? ~x & ones(esize) : x;
                    v = count_leading_zeros_n(t, esize) - 1;
                    break;
                }
                case 0x9: if (size == 3) return false; v = count_leading_zeros_n(x, esize); break;  // VCLZ
                case 0xA: if (size != 0) return false; v = static_cast<u64>(__builtin_popcountll(x)); break;  // VCNT
                case 0xB: if (size != 0) return false; v = ~x; break;                               // VMVN
                case 0xE: if (size == 3) return false; v = sat_signed(ext(x, esize, false) < 0 ? -ext(x, esize, false) : ext(x, esize, false), esize, sat); break;  // VQABS
                case 0xF: if (size == 3) return false; v = sat_signed(-ext(x, esize, false), esize, sat); break;  // VQNEG
                default: return false;
            }
        } else if (a == 1) {
            const bool is_float = bit(instr, 10);
            const u32 op = bits(instr, 9, 7);
            if (is_float) {
                if (size != 2) return false;
                const float f = as_f32(x);
                const u64 kTrue = 0xFFFFFFFF;
                switch (op) {
                    case 0: v = fp::compare_gt(f, 0.0f, c) ? kTrue : 0; break;
                    case 1: v = fp::compare_ge(f, 0.0f, c) ? kTrue : 0; break;
                    case 2: v = fp::compare_eq(f, 0.0f, c) ? kTrue : 0; break;
                    case 3: v = fp::compare_ge(0.0f, f, c) ? kTrue : 0; break;  // VCLE #0
                    case 4: v = fp::compare_gt(0.0f, f, c) ? kTrue : 0; break;  // VCLT #0
                    case 6: v = f32_bits(fp::abs(f)); break;
                    case 7: v = f32_bits(fp::neg(f)); break;
                    default: return false;
                }
            } else {
                if (size == 3) return false;
                const s128 sx = ext(x, esize, false);
                switch (op) {
                    case 0: v = sx > 0 ? ~0ull : 0; break;
                    case 1: v = sx >= 0 ? ~0ull : 0; break;
                    case 2: v = sx == 0 ? ~0ull : 0; break;
                    case 3: v = sx <= 0 ? ~0ull : 0; break;
                    case 4: v = sx < 0 ? ~0ull : 0; break;
                    case 6: v = static_cast<u64>(sx < 0 ? -sx : sx); break;  // VABS
                    case 7: v = static_cast<u64>(-sx); break;                // VNEG
                    default: return false;
                }
            }
        } else if (a == 3) {
            if (size != 2) return false;
            if ((b & 0x18) == 0x10) {  // VRECPE / VRSQRTE (bit 8 : flottant ou U32)
                const bool rsqrt = bit(instr, 7), is_float = bit(instr, 8);
                if (is_float) v = f32_bits(rsqrt ? fp::rsqrt_estimate(as_f32(x), c) : fp::recip_estimate(as_f32(x), c));
                else v = rsqrt ? fp::rsqrt_estimate_u32(static_cast<u32>(x)) : fp::recip_estimate_u32(static_cast<u32>(x));
            } else if ((b >> 3) == 3) {  // VCVT entre entiers et flottants
                switch (bits(instr, 8, 7)) {
                    case 0: v = f32_bits(fp::from_int<float>(static_cast<u32>(x), true, c)); break;
                    case 1: v = f32_bits(fp::from_int<float>(static_cast<u32>(x), false, c)); break;
                    case 2: v = fp::to_int(as_f32(x), true, fp::kZero, c); break;
                    default: v = fp::to_int(as_f32(x), false, fp::kZero, c); break;
                }
            } else {
                return false;
            }
        } else {
            return false;
        }
        set_elem(out, e, esize, v & ones(esize));
    }
    // VPADDL / VPADAL écrivent des éléments élargis.
    store(s, r.d, r.q, out);
    return true;
}

}  // namespace

Interpreter::StepResult Interpreter::neon_data(u32 instr) {
    const bool u = bit(instr, 24);
    const u32 a = bits(instr, 23, 19), b = bits(instr, 11, 8), c4 = bits(instr, 7, 4);
    fp::Ctx ctx = fp::Ctx::standard(state_.fpscr);
    bool sat = false, ok;

    if (!bit(a, 4)) {
        ok = three_same(state_, ctx, sat, instr);
    } else if (bit(c4, 0)) {
        if (!bit(c4, 3) && (a & 7) == 0) {  // un registre et un immédiat modifié
            const u32 d = (bit(instr, 22) << 4) | bits(instr, 15, 12);
            const bool q = bit(instr, 6), op = bit(instr, 5);
            const u32 cmode = bits(instr, 11, 8);
            const u32 imm8 = (bit(instr, 24) << 7) | (bits(instr, 18, 16) << 4) | bits(instr, 3, 0);
            u64 imm;
            if ((q && (d & 1)) || !expand_imm(op, cmode, imm8, imm)) return StepResult::Undefined;
            Vec v = load(state_, d, q);
            const bool logical = (cmode & 1) && cmode < 12;
            for (int w = 0; w < 2; ++w) {
                if (logical) v.d[w] = op ? v.d[w] & ~imm : v.d[w] | imm;  // VBIC / VORR
                else v.d[w] = (op && cmode != 14) ? ~imm : imm;           // VMVN / VMOV
            }
            store(state_, d, q, v);
            ok = true;
        } else {
            ok = shift_imm(state_, ctx, sat, instr);
        }
    } else if ((a & 6) == 6) {
        const Regs r = decode_regs(instr);
        if (!u) {  // VEXT
            const u32 imm4 = bits(instr, 11, 8), bytes = r.q ? 16 : 8;
            if ((!r.q && imm4 > 7) || (r.q && ((r.d | r.n | r.m) & 1))) return StepResult::Undefined;
            const Vec vn = load(state_, r.n, r.q), vm = load(state_, r.m, r.q);
            u8 all[32];
            for (u32 i = 0; i < bytes; ++i) {
                all[i] = static_cast<u8>(elem(vn, i, 8));
                all[bytes + i] = static_cast<u8>(elem(vm, i, 8));
            }
            Vec out;
            for (u32 i = 0; i < bytes; ++i) set_elem(out, i, 8, all[i + imm4]);
            store(state_, r.d, r.q, out);
            ok = true;
        } else if (!bit(b, 3)) {
            ok = two_misc(state_, ctx, sat, instr);
        } else if ((b & 0xC) == 8) {  // VTBL / VTBX
            const u32 len = bits(instr, 9, 8) + 1;
            if (r.n + len > 32) return StepResult::Undefined;
            const bool extension = bit(instr, 6);
            const Vec vm = load(state_, r.m, false), vd = load(state_, r.d, false);
            Vec out;
            for (u32 i = 0; i < 8; ++i) {
                const u32 index = static_cast<u32>(elem(vm, i, 8));
                u64 v;
                if (index < 8 * len) v = (state_.d[r.n + index / 8] >> ((index % 8) * 8)) & 0xFF;
                else v = extension ? elem(vd, i, 8) : 0;
                set_elem(out, i, 8, v);
            }
            store(state_, r.d, false, out);
            ok = true;
        } else if (b == 0xC && !bit(c4, 3)) {  // VDUP (scalaire)
            const u32 imm4 = bits(instr, 19, 16);
            u32 esize, index;
            if (imm4 & 1) { esize = 8; index = imm4 >> 1; }
            else if (imm4 & 2) { esize = 16; index = imm4 >> 2; }
            else if (imm4 & 4) { esize = 32; index = imm4 >> 3; }
            else return StepResult::Undefined;
            if (r.q && (r.d & 1)) return StepResult::Undefined;
            const u64 v = rep(elem(load(state_, r.m, false), index, esize), esize);
            store(state_, r.d, r.q, Vec{{v, v}});
            ok = true;
        } else {
            ok = false;
        }
    } else if (!bit(c4, 2)) {
        ok = three_diff(state_, sat, instr);
    } else {
        ok = scalar_ops(state_, ctx, sat, instr);
    }

    state_.fpscr |= ctx.exceptions;
    if (sat) state_.fpscr |= fp::kQC;
    return ok ? StepResult::Ok : StepResult::Undefined;
}

// VLD1-4 / VST1-4 : structures multiples, un élément (une voie) ou toutes les voies.
Interpreter::StepResult Interpreter::neon_load_store(u32 instr) {
    const bool all_mode = bit(instr, 23), load = bit(instr, 21);
    const u32 rn = bits(instr, 19, 16), rm = bits(instr, 3, 0);
    const u32 d = (bit(instr, 22) << 4) | bits(instr, 15, 12);
    if (rn == 15) return StepResult::Undefined;
    u32 addr = state_.r[rn];
    u32 transferred;

    auto read = [&](u32 a, u32 ebytes) -> u64 {
        switch (ebytes) {
            case 1: return memory_.read<u8>(a);
            case 2: return memory_.read<u16>(a);
            case 4: return memory_.read<u32>(a);
            default: return memory_.read<u64>(a);
        }
    };
    auto write = [&](u32 a, u32 ebytes, u64 v) {
        switch (ebytes) {
            case 1: memory_.write<u8>(a, static_cast<u8>(v)); break;
            case 2: memory_.write<u16>(a, static_cast<u16>(v)); break;
            case 4: memory_.write<u32>(a, static_cast<u32>(v)); break;
            default: memory_.write<u64>(a, v); break;
        }
    };
    auto lane = [&](u32 reg, u32 index, u32 esize) -> u64 {
        return esize == 64 ? state_.d[reg] : (state_.d[reg] >> (index * esize)) & ones(esize);
    };
    auto set_lane = [&](u32 reg, u32 index, u32 esize, u64 v) {
        if (esize == 64) { state_.d[reg] = v; return; }
        const u64 m = ones(esize) << (index * esize);
        state_.d[reg] = (state_.d[reg] & ~m) | ((v << (index * esize)) & m);
    };

    if (!all_mode) {  // structures multiples
        const u32 type = bits(instr, 11, 8), size = bits(instr, 7, 6);
        const u32 ebytes = 1u << size, esize = 8 * ebytes, elements = 8 / ebytes;
        u32 n, regs, inc = 1;
        switch (type) {
            case 0x7: n = 1; regs = 1; break;
            case 0xA: n = 1; regs = 2; break;
            case 0x6: n = 1; regs = 3; break;
            case 0x2: n = 1; regs = 4; break;
            case 0x8: n = 2; regs = 1; inc = 1; break;
            case 0x9: n = 2; regs = 1; inc = 2; break;
            case 0x3: n = 2; regs = 2; inc = 2; break;
            case 0x4: n = 3; regs = 1; inc = 1; break;
            case 0x5: n = 3; regs = 1; inc = 2; break;
            case 0x0: n = 4; regs = 1; inc = 1; break;
            case 0x1: n = 4; regs = 1; inc = 2; break;
            default: return StepResult::Undefined;
        }
        if (n > 1 && size == 3) return StepResult::Undefined;
        if (n == 1) {
            if (d + regs > 32) return StepResult::Undefined;
            for (u32 r = 0; r < regs; ++r)
                for (u32 e = 0; e < elements; ++e, addr += ebytes) {
                    if (load) set_lane(d + r, e, esize, read(addr, ebytes));
                    else write(addr, ebytes, lane(d + r, e, esize));
                }
            transferred = 8 * regs;
        } else {
            // VLD2 {d0-d3} = deux paires (d, d+2) puis (d+1, d+3).
            if (d + (n - 1) * inc + (regs - 1) >= 32) return StepResult::Undefined;
            for (u32 r = 0; r < regs; ++r)
                for (u32 e = 0; e < elements; ++e)
                    for (u32 k = 0; k < n; ++k, addr += ebytes) {
                        const u32 reg = d + r + k * inc;
                        if (load) set_lane(reg, e, esize, read(addr, ebytes));
                        else write(addr, ebytes, lane(reg, e, esize));
                    }
            transferred = 8 * regs * n;
        }
    } else if (bits(instr, 11, 10) == 3) {  // VLDn vers toutes les voies
        if (!load) return StepResult::Undefined;
        const u32 n = bits(instr, 9, 8) + 1, size = bits(instr, 7, 6);
        const bool t = bit(instr, 5);
        if (size == 3) return StepResult::Undefined;
        const u32 ebytes = 1u << size, esize = 8 * ebytes;
        if (n == 1) {
            const u64 v = rep(read(addr, ebytes), esize);
            const u32 regs = t ? 2 : 1;
            if (d + regs > 32) return StepResult::Undefined;
            for (u32 r = 0; r < regs; ++r) state_.d[d + r] = v;
        } else {
            const u32 inc = t ? 2 : 1;
            if (d + (n - 1) * inc >= 32) return StepResult::Undefined;
            for (u32 k = 0; k < n; ++k) state_.d[d + k * inc] = rep(read(addr + k * ebytes, ebytes), esize);
        }
        transferred = n * ebytes;
    } else {  // un élément (une voie)
        const u32 size = bits(instr, 11, 10), n = bits(instr, 9, 8) + 1, ia = bits(instr, 7, 4);
        const u32 ebytes = 1u << size, esize = 8 * ebytes;
        const u32 index = ia >> (size + 1);
        const u32 inc = size == 0 ? 1 : size == 1 ? (bit(ia, 1) ? 2 : 1) : (bit(ia, 2) ? 2 : 1);
        if (d + (n - 1) * inc >= 32) return StepResult::Undefined;
        for (u32 k = 0; k < n; ++k, addr += ebytes) {
            if (load) set_lane(d + k * inc, index, esize, read(addr, ebytes));
            else write(addr, ebytes, lane(d + k * inc, index, esize));
        }
        transferred = n * ebytes;
    }

    if (rm != 15) state_.r[rn] += rm == 13 ? transferred : state_.r[rm];
    return StepResult::Ok;
}

}  // namespace vita
