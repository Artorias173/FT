// Interpréteur : unité de calcul flottant VFPv3 (simple et double précision).
//
// Les calculs passent par fp.h, qui applique les règles ARM (FPSCR : mise à
// zéro des dénormaux, NaN par défaut, mode d'arrondi, drapeaux d'exception).
#include <cstring>

#include "fp.h"
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

template <typename T> struct Reg;

template <> struct Reg<float> {
    using Bits = u32;
    static Bits raw(const CpuState& s, u32 i) { return get_s(s, i); }
    static void set_raw(CpuState& s, u32 i, Bits v) { set_s(s, i, v); }
    static Bits expand_imm(u32 imm8) {
        const u32 b6 = bit(imm8, 6);
        const u32 exp = ((b6 ^ 1) << 7) | (b6 ? 0x7C : 0) | bits(imm8, 5, 4);
        return (bit(imm8, 7) << 31) | (exp << 23) | (bits(imm8, 3, 0) << 19);
    }
};

template <> struct Reg<double> {
    using Bits = u64;
    static Bits raw(const CpuState& s, u32 i) { return s.d[i]; }
    static void set_raw(CpuState& s, u32 i, Bits v) { s.d[i] = v; }
    static Bits expand_imm(u32 imm8) {
        const u64 b6 = bit(imm8, 6);
        const u64 exp = ((b6 ^ 1) << 10) | (b6 ? 0x3FC : 0) | bits(imm8, 5, 4);
        return (static_cast<u64>(bit(imm8, 7)) << 63) | (exp << 52) | (static_cast<u64>(bits(imm8, 3, 0)) << 48);
    }
};

template <typename T> T get(const CpuState& s, u32 i) {
    const typename Reg<T>::Bits b = Reg<T>::raw(s, i);
    T v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

template <typename T> void set(CpuState& s, u32 i, T v) {
    typename Reg<T>::Bits b;
    std::memcpy(&b, &v, sizeof(v));
    Reg<T>::set_raw(s, i, b);
}

// Instructions de calcul. d, n, m : numéros de registres dans la précision T ;
// sm, sd : numéros simple précision (pour les conversions avec les entiers).
template <typename T>
bool arith(CpuState& s, fp::Ctx& c, u32 instr, u32 d, u32 n, u32 m, u32 sm, u32 sd) {
    const u32 opc1 = (bit(instr, 23) << 3) | bits(instr, 21, 20);
    const bool op6 = bit(instr, 6);
    const T vn = get<T>(s, n), vm = get<T>(s, m), vd = get<T>(s, d);

    switch (opc1) {
        case 0x0: {  // VMLA / VMLS : produit arrondi, puis addition (pas de fusion)
            const T p = fp::mul(vn, vm, c);
            set<T>(s, d, fp::add(vd, op6 ? fp::neg(p) : p, c));
            return true;
        }
        case 0x1: {  // VNMLS / VNMLA
            const T p = fp::mul(vn, vm, c);
            set<T>(s, d, fp::add(fp::neg(vd), op6 ? fp::neg(p) : p, c));
            return true;
        }
        case 0x2: {  // VMUL / VNMUL
            const T p = fp::mul(vn, vm, c);
            set<T>(s, d, op6 ? fp::neg(p) : p);
            return true;
        }
        case 0x3: set<T>(s, d, op6 ? fp::sub(vn, vm, c) : fp::add(vn, vm, c)); return true;  // VSUB / VADD
        case 0x8:                                                                               // VDIV
            if (op6) return false;
            set<T>(s, d, fp::div(vn, vm, c));
            return true;
        case 0xB: break;
        default: return false;  // VFMA, VFNMA... (VFPv4, absentes du Cortex-A9)
    }

    if (!op6) {  // VMOV (immédiat)
        Reg<T>::set_raw(s, d, Reg<T>::expand_imm((bits(instr, 19, 16) << 4) | bits(instr, 3, 0)));
        return true;
    }
    const bool b7 = bit(instr, 7);
    const u32 opc2 = bits(instr, 19, 16);
    switch (opc2) {
        case 0x0: set<T>(s, d, b7 ? fp::abs(vm) : vm); return true;              // VABS / VMOV (registre)
        case 0x1: set<T>(s, d, b7 ? fp::sqrt(vm, c) : fp::neg(vm)); return true; // VSQRT / VNEG
        case 0x4: case 0x5: {                                                    // VCMP / VCMPE (avec zéro si 0x5)
            const u32 nzcv = fp::compare(vd, opc2 == 0x5 ? T(0) : vm, b7, c);
            s.fpscr = (s.fpscr & 0x0FFFFFFF) | (nzcv << 28);
            return true;
        }
        case 0x8:  // VCVT entier (Sm) -> flottant
            set<T>(s, d, fp::from_int<T>(get_s(s, sm), b7, c));
            return true;
        case 0xC: case 0xD:  // VCVT (troncature) / VCVTR (mode du FPSCR) flottant -> entier (Sd)
            set_s(s, sd, fp::to_int(vm, opc2 == 0xD, b7 ? fp::kZero : c.rmode, c));
            return true;
        case 0xA: case 0xB: case 0xE: case 0xF: {  // VCVT virgule fixe, sur place dans Vd
            const u32 size = b7 ? 32 : 16;
            const u32 frac_bits = size - ((bits(instr, 3, 0) << 1) | bit(instr, 5));
            const bool is_unsigned = bit(instr, 16);
            if (bit(instr, 18)) {  // flottant -> virgule fixe (troncature)
                const u64 v = fp::to_fixed(vd, size, frac_bits, is_unsigned, fp::kZero, c);
                Reg<T>::set_raw(s, d, static_cast<typename Reg<T>::Bits>(v));
            } else {               // virgule fixe -> flottant
                set<T>(s, d, fp::from_fixed<T>(Reg<T>::raw(s, d), size, frac_bits, is_unsigned, c));
            }
            return true;
        }
        default: return false;
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

    fp::Ctx ctx = fp::Ctx::from_fpscr(state_.fpscr);
    bool ok = true;
    const bool other = bit(instr, 23) && bits(instr, 21, 20) == 3 && bit(instr, 6);
    const u32 opc2 = bits(instr, 19, 16);

    if (other && opc2 == 0x7 && bit(instr, 7)) {
        // VCVT entre simple et double précision : les deux précisions dans la même instruction.
        if (dp) set<float>(state_, sd, fp::to_single(get<double>(state_, dm), ctx));  // F32 <- F64
        else set<double>(state_, dd, fp::to_double(get<float>(state_, sm), ctx));     // F64 <- F32
    } else if (other && (opc2 == 0x2 || opc2 == 0x3)) {
        // VCVTB / VCVTT : demi-précision dans la moitié basse (B) ou haute (T) de Sm / Sd.
        if (dp) return StepResult::Undefined;
        const bool top = bit(instr, 7);
        if (opc2 == 0x2) {  // F32 <- F16
            const u16 h = static_cast<u16>(top ? get_s(state_, sm) >> 16 : get_s(state_, sm));
            set<float>(state_, sd, fp::from_half(h, ctx));
        } else {            // F16 <- F32
            const u32 h = fp::to_half(get<float>(state_, sm), ctx);
            const u32 cur = get_s(state_, sd);
            set_s(state_, sd, top ? (cur & 0xFFFF) | (h << 16) : (cur & 0xFFFF0000) | h);
        }
    } else {
        ok = dp ? arith<double>(state_, ctx, instr, dd, dn, dm, sm, sd)
                : arith<float>(state_, ctx, instr, sd, sn, sm, sm, sd);
    }
    state_.fpscr |= ctx.exceptions;  // drapeaux cumulés
    return ok ? StepResult::Ok : StepResult::Undefined;
}

}  // namespace vita
