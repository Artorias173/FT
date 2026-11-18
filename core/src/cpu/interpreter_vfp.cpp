// Interpréteur : unité de calcul flottant VFPv3 (simple et double précision).
//
// Les opérations utilisent l'arithmétique IEEE 754 de l'hôte, en arrondi au
// plus proche, ce qui donne des résultats identiques au bit près. Les modes
// non-IEEE de la console (mise à zéro des dénormaux, NaN par défaut) et les
// exceptions flottantes ne sont pas encore émulés.
#include <cmath>
#include <cstring>

#include "interpreter_util.h"
#include "vita/cpu/interpreter.h"
#include "vita/memory/memory.h"

namespace vita {

using namespace arm;

namespace {

constexpr u32 align4(u32 v) { return v & ~3u; }

// Les registres simple précision S0-S31 sont les moitiés de D0-D15.
u32 get_s(const CpuState& s, u32 n) {
    const u64 d = s.d[n >> 1];
    return (n & 1) ? static_cast<u32>(d >> 32) : static_cast<u32>(d);
}
void set_s(CpuState& s, u32 n, u32 v) {
    u64& d = s.d[n >> 1];
    d = (n & 1) ? (d & 0xFFFFFFFFull) | (static_cast<u64>(v) << 32) : (d & ~0xFFFFFFFFull) | v;
}

template <typename T>
struct Fp;

template <>
struct Fp<float> {
    using Bits = u32;
    static constexpr Bits kSign = 0x80000000u;
    static Bits raw(const CpuState& s, u32 i) { return get_s(s, i); }
    static void set_raw(CpuState& s, u32 i, Bits v) { set_s(s, i, v); }
    static Bits expand_imm(u32 imm8) {
        const u32 b6 = bit(imm8, 6);
        const u32 exp = ((b6 ^ 1) << 7) | (b6 ? 0x7C : 0) | bits(imm8, 5, 4);
        return (bit(imm8, 7) << 31) | (exp << 23) | (bits(imm8, 3, 0) << 19);
    }
};

template <>
struct Fp<double> {
    using Bits = u64;
    static constexpr Bits kSign = 0x8000000000000000ull;
    static Bits raw(const CpuState& s, u32 i) { return s.d[i]; }
    static void set_raw(CpuState& s, u32 i, Bits v) { s.d[i] = v; }
    static Bits expand_imm(u32 imm8) {
        const u64 b6 = bit(imm8, 6);
        const u64 exp = ((b6 ^ 1) << 10) | (b6 ? 0x3FC : 0) | bits(imm8, 5, 4);
        return (static_cast<u64>(bit(imm8, 7)) << 63) | (exp << 52) | (static_cast<u64>(bits(imm8, 3, 0)) << 48);
    }
};

template <typename T>
T get(const CpuState& s, u32 i) {
    const typename Fp<T>::Bits b = Fp<T>::raw(s, i);
    T v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

template <typename T>
void set(CpuState& s, u32 i, T v) {
    typename Fp<T>::Bits b;
    std::memcpy(&b, &v, sizeof(v));
    Fp<T>::set_raw(s, i, b);
}

// Conversion flottant -> entier avec saturation, comme le fait le matériel.
// rmode : 0 au plus proche, 1 vers +inf, 2 vers -inf, 3 vers zéro.
template <typename T>
u32 to_int(T value, bool is_signed, u32 rmode) {
    if (std::isnan(value)) return 0;
    double v = static_cast<double>(value);
    switch (rmode) {
        case 0: v = std::nearbyint(v); break;
        case 1: v = std::ceil(v); break;
        case 2: v = std::floor(v); break;
        default: v = std::trunc(v); break;
    }
    if (is_signed) {
        if (v >= 2147483647.0) return 0x7FFFFFFF;
        if (v <= -2147483648.0) return 0x80000000;
        return static_cast<u32>(static_cast<s32>(v));
    }
    if (v >= 4294967295.0) return 0xFFFFFFFF;
    if (v <= 0.0) return 0;
    return static_cast<u32>(v);
}

// VCMP : place le résultat de la comparaison dans les drapeaux NZCV du FPSCR.
template <typename T>
void compare(CpuState& s, T a, T b) {
    u32 nzcv;
    if (std::isnan(a) || std::isnan(b)) nzcv = 0x3;  // non ordonnés : C et V
    else if (a == b) nzcv = 0x6;                     // égaux : Z et C
    else if (a < b) nzcv = 0x8;                      // inférieur : N
    else nzcv = 0x2;                                 // supérieur : C
    s.fpscr = (s.fpscr & 0x0FFFFFFF) | (nzcv << 28);
}

// Instructions de calcul. d, n, m sont des numéros de registres dans la précision T.
// Renvoie false si l'encodage n'est pas pris en charge.
template <typename T>
bool arith(CpuState& s, u32 instr, u32 d, u32 n, u32 m, u32 sm, u32 sd) {
    using F = Fp<T>;
    const u32 opc1 = (bit(instr, 23) << 3) | bits(instr, 21, 20);
    const bool op6 = bit(instr, 6);
    const T vn = get<T>(s, n), vm = get<T>(s, m), vd = get<T>(s, d);
    const T product = vn * vm;  // arrondi avant l'addition : VMLA n'est pas fusionnée

    switch (opc1) {
        case 0x0: set<T>(s, d, op6 ? vd - product : vd + product); return true;   // VMLA / VMLS
        case 0x1: set<T>(s, d, op6 ? -vd - product : -vd + product); return true; // VNMLA / VNMLS
        case 0x2: set<T>(s, d, op6 ? -product : product); return true;            // VNMUL / VMUL
        case 0x3: set<T>(s, d, op6 ? vn - vm : vn + vm); return true;             // VSUB / VADD
        case 0x8:                                                                  // VDIV
            if (op6) return false;
            set<T>(s, d, vn / vm);
            return true;
        case 0xB: break;
        default: return false;  // VFMA, VFNMA... (VFPv4, absentes du Cortex-A9)
    }

    if (!op6) {  // VMOV (immédiat)
        F::set_raw(s, d, F::expand_imm((bits(instr, 19, 16) << 4) | bits(instr, 3, 0)));
        return true;
    }
    const bool b7 = bit(instr, 7);
    switch (bits(instr, 19, 16)) {
        case 0x0:  // VMOV (registre) / VABS
            F::set_raw(s, d, b7 ? F::raw(s, m) & ~F::kSign : F::raw(s, m));
            return true;
        case 0x1:  // VNEG / VSQRT
            if (b7) set<T>(s, d, std::sqrt(vm));
            else F::set_raw(s, d, F::raw(s, m) ^ F::kSign);
            return true;
        case 0x4: compare<T>(s, vd, vm); return true;          // VCMP / VCMPE
        case 0x5: compare<T>(s, vd, T(0)); return true;        // VCMP avec zéro
        case 0x8: {                                            // VCVT entier (Sm) -> flottant
            const u32 v = get_s(s, sm);
            set<T>(s, d, b7 ? static_cast<T>(static_cast<s32>(v)) : static_cast<T>(v));
            return true;
        }
        case 0xC: case 0xD: {                                  // VCVT flottant -> entier (Sd)
            const u32 rmode = b7 ? 3 : bits(s.fpscr, 23, 22);  // VCVT tronque, VCVTR suit le FPSCR
            set_s(s, sd, to_int<T>(vm, bit(instr, 16), rmode));
            return true;
        }
        default: return false;  // virgule fixe, demi-précision
    }
}

}  // namespace

Interpreter::StepResult Interpreter::vfp(u32 instr) {
    if (bits(instr, 27, 25) == 6) return vfp_load_store(instr);
    if (bits(instr, 27, 24) == 0xE) return bit(instr, 4) ? vfp_transfer(instr) : vfp_data_processing(instr);
    return StepResult::Undefined;
}

// VLDR, VSTR, VLDM, VSTM (VPUSH, VPOP) et VMOV entre deux registres ARM et un registre VFP.
Interpreter::StepResult Interpreter::vfp_load_store(u32 instr) {
    const bool dp = bit(instr, 8);
    const bool pre = bit(instr, 24), up = bit(instr, 23), writeback = bit(instr, 21), load = bit(instr, 20);
    const u32 rn = bits(instr, 19, 16);
    u32* r = state_.r;

    if (bits(instr, 24, 21) == 2) {  // VMOV Rt, Rt2 <-> Dm ou {Sm, Sm+1}
        const u32 rt = bits(instr, 15, 12), rt2 = bits(instr, 19, 16);
        if (rt == 15 || rt2 == 15) return StepResult::Undefined;
        if (dp) {
            const u32 m = (bit(instr, 5) << 4) | bits(instr, 3, 0);
            if (load) {
                r[rt] = static_cast<u32>(state_.d[m]);
                r[rt2] = static_cast<u32>(state_.d[m] >> 32);
            } else {
                state_.d[m] = r[rt] | (static_cast<u64>(r[rt2]) << 32);
            }
        } else {
            const u32 m = (bits(instr, 3, 0) << 1) | bit(instr, 5);
            if (m == 31) return StepResult::Undefined;
            if (load) {
                r[rt] = get_s(state_, m);
                r[rt2] = get_s(state_, m + 1);
            } else {
                set_s(state_, m, r[rt]);
                set_s(state_, m + 1, r[rt2]);
            }
        }
        return StepResult::Ok;
    }

    const u32 vd = dp ? (bit(instr, 22) << 4) | bits(instr, 15, 12) : (bits(instr, 15, 12) << 1) | bit(instr, 22);
    const u32 imm8 = bits(instr, 7, 0);

    auto transfer = [&](u32 reg_index, u32 addr) {
        if (dp) {
            if (load) state_.d[reg_index] = memory_.read<u32>(addr) | (static_cast<u64>(memory_.read<u32>(addr + 4)) << 32);
            else {
                memory_.write<u32>(addr, static_cast<u32>(state_.d[reg_index]));
                memory_.write<u32>(addr + 4, static_cast<u32>(state_.d[reg_index] >> 32));
            }
        } else {
            if (load) set_s(state_, reg_index, memory_.read<u32>(addr));
            else memory_.write<u32>(addr, get_s(state_, reg_index));
        }
    };

    if (pre && !writeback) {  // VLDR / VSTR
        const u32 base = rn == 15 ? align4(reg(15)) : r[rn];
        transfer(vd, up ? base + imm8 * 4 : base - imm8 * 4);
        return StepResult::Ok;
    }
    if (pre == up || rn == 15) return StepResult::Undefined;

    // VLDM / VSTM : IA (avec ou sans écriture de la base) ou DB (avec écriture).
    const u32 count = dp ? imm8 / 2 : imm8;
    if (count == 0 || vd + count > (dp ? 32u : 32u)) return StepResult::Undefined;
    const u32 base = r[rn];
    u32 addr = up ? base : base - imm8 * 4;
    for (u32 i = 0; i < count; ++i) {
        transfer(vd + i, addr);
        addr += dp ? 8 : 4;
    }
    if (writeback) r[rn] = up ? base + imm8 * 4 : base - imm8 * 4;
    return StepResult::Ok;
}

// VMOV entre un registre ARM et un registre simple / un élément 32 bits, VMRS, VMSR.
Interpreter::StepResult Interpreter::vfp_transfer(u32 instr) {
    const bool load = bit(instr, 20), c = bit(instr, 8);
    const u32 a = bits(instr, 23, 21), rt = bits(instr, 15, 12);
    u32* r = state_.r;

    if (!c && a == 0) {  // VMOV Rt <-> Sn
        const u32 n = (bits(instr, 19, 16) << 1) | bit(instr, 7);
        if (rt == 15) return StepResult::Undefined;
        if (load) r[rt] = get_s(state_, n); else set_s(state_, n, r[rt]);
        return StepResult::Ok;
    }
    if (!c && a == 7) {  // VMRS / VMSR
        const u32 sysreg = bits(instr, 19, 16);
        if (load) {
            u32 value;
            switch (sysreg) {
                case 0x1: value = state_.fpscr; break;
                case 0x8: value = 0x40000000; break;  // FPEXC : unité activée
                case 0x0: value = 0x41033094; break;  // FPSID : VFPv3 (ARM, Cortex-A9)
                default: return StepResult::Undefined;
            }
            if (rt == 15) {
                if (sysreg != 1) return StepResult::Undefined;
                state_.cpsr = (state_.cpsr & 0x0FFFFFFF) | (state_.fpscr & 0xF0000000);  // VMRS APSR_nzcv, FPSCR
            } else {
                r[rt] = value;
            }
        } else {
            if (sysreg != 1 || rt == 15) return StepResult::Undefined;
            state_.fpscr = r[rt];
        }
        return StepResult::Ok;
    }
    if (c && !bit(instr, 22) && bits(instr, 6, 5) == 0 && !bit(instr, 23)) {  // VMOV.32 Rt <-> Dd[x]
        const u32 dd = (bit(instr, 7) << 4) | bits(instr, 19, 16);
        const u32 lane = (dd << 1) | bit(instr, 21);
        if (rt == 15) return StepResult::Undefined;
        if (load) r[rt] = get_s(state_, lane); else set_s(state_, lane, r[rt]);
        return StepResult::Ok;
    }
    return StepResult::Undefined;  // éléments 8/16 bits, VDUP (NEON)
}

Interpreter::StepResult Interpreter::vfp_data_processing(u32 instr) {
    const bool dp = bit(instr, 8);
    const u32 vd = bits(instr, 15, 12), vn = bits(instr, 19, 16), vm = bits(instr, 3, 0);
    const u32 D = bit(instr, 22), N = bit(instr, 7), M = bit(instr, 5);
    // Numéros en simple précision (Vx:bit) et en double précision (bit:Vx).
    const u32 sd = (vd << 1) | D, sn = (vn << 1) | N, sm = (vm << 1) | M;
    const u32 dd = (D << 4) | vd, dn = (N << 4) | vn, dm = (M << 4) | vm;

    // VCVT entre simple et double précision : les deux précisions dans la même instruction.
    const bool other = (bit(instr, 23) && bits(instr, 21, 20) == 3 && bit(instr, 6));
    if (other && bits(instr, 19, 16) == 0x7 && bit(instr, 7)) {
        if (dp) set<float>(state_, sd, static_cast<float>(get<double>(state_, dm)));   // F32 <- F64
        else set<double>(state_, dd, static_cast<double>(get<float>(state_, sm)));     // F64 <- F32
        return StepResult::Ok;
    }

    const bool ok = dp ? arith<double>(state_, instr, dd, dn, dm, sm, sd)
                       : arith<float>(state_, instr, sd, sn, sm, sm, sd);
    return ok ? StepResult::Ok : StepResult::Undefined;
}

}  // namespace vita
