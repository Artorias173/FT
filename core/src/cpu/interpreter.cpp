// Interpréteur : boucle d'exécution et outils communs aux modes ARM et Thumb.
#include "vita/cpu/interpreter.h"

#include <cstdio>

#include "interpreter_util.h"
#include "vita/memory/memory.h"

namespace vita {

using namespace arm;

CpuExit Interpreter::run(u64 max_instructions) {
    halt_requested_ = false;
    executed_ = 0;
    for (u64 i = 0; i < max_instructions; ++i) {
        if (halt_requested_) return CpuExit::Halted;
        const u32 pc = state_.r[15];
        if (step() == StepResult::Undefined) return CpuExit::Undefined;
        ++executed_;

        GuestAddr fault = 0;
        if (memory_.take_fault(&fault)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Accès mémoire invalide à 0x%08x (PC=0x%08x)", fault, pc);
            last_error_ = buf;
            state_.r[15] = pc;  // on reste sur l'instruction fautive
            return CpuExit::MemoryFault;
        }
        // Le gestionnaire SVC peut demander l'arrêt.
        if (halt_requested_) return CpuExit::Halted;
    }
    return CpuExit::BudgetExhausted;
}

Interpreter::StepResult Interpreter::step() {
    pc_written_ = false;
    const u32 pc = state_.r[15];
    char buf[112];

    if (!state_.thumb()) {
        const u32 instr = memory_.read<u32>(pc);
        if (memory_.has_fault()) return StepResult::Ok;  // run() signale la faute
        const u32 cond = bits(instr, 31, 28);
        StepResult r = StepResult::Ok;
        if (cond == 0xF || condition_passed(cond)) r = exec_arm(instr, pc);
        if (r == StepResult::Undefined) {
            std::snprintf(buf, sizeof(buf), "Instruction ARM non prise en charge 0x%08x à 0x%08x", instr, pc);
            last_error_ = buf;
            return r;
        }
        if (!pc_written_) state_.r[15] = pc + 4;
        return StepResult::Ok;
    }

    const u16 hw1 = memory_.read<u16>(pc);
    const bool wide = (hw1 >> 11) >= 0x1D;
    const u16 hw2 = wide ? memory_.read<u16>(pc + 2) : 0;
    if (memory_.has_fault()) return StepResult::Ok;

    const bool in_it = in_it_block();
    const bool is_it_instr = !wide && (hw1 & 0xFF00) == 0xBF00 && (hw1 & 0xF);
    StepResult r = StepResult::Ok;
    if (!in_it || condition_passed(state_.itstate >> 4))
        r = wide ? exec_thumb32(hw1, hw2, pc) : exec_thumb16(hw1, pc);

    if (r == StepResult::Undefined) {
        if (wide)
            std::snprintf(buf, sizeof(buf), "Instruction Thumb-2 non prise en charge %04x %04x à 0x%08x", hw1, hw2, pc);
        else
            std::snprintf(buf, sizeof(buf), "Instruction Thumb non prise en charge %04x à 0x%08x", hw1, pc);
        last_error_ = buf;
        return r;
    }
    if (in_it && !is_it_instr) advance_it();
    if (!pc_written_) state_.r[15] = pc + (wide ? 4 : 2);
    return StepResult::Ok;
}

bool Interpreter::condition_passed(u32 cond) const {
    const u32 f = state_.cpsr;
    const bool n = f & CpuState::kFlagN, z = f & CpuState::kFlagZ;
    const bool c = f & CpuState::kFlagC, v = f & CpuState::kFlagV;
    switch (cond & 0xF) {
        case 0x0: return z;               // EQ
        case 0x1: return !z;              // NE
        case 0x2: return c;               // CS/HS
        case 0x3: return !c;              // CC/LO
        case 0x4: return n;               // MI
        case 0x5: return !n;              // PL
        case 0x6: return v;               // VS
        case 0x7: return !v;              // VC
        case 0x8: return c && !z;         // HI
        case 0x9: return !c || z;         // LS
        case 0xA: return n == v;          // GE
        case 0xB: return n != v;          // LT
        case 0xC: return !z && n == v;    // GT
        case 0xD: return z || n != v;     // LE
        default: return true;             // AL
    }
}

void Interpreter::advance_it() {
    if ((state_.itstate & 0x7) == 0)
        state_.itstate = 0;
    else
        state_.itstate = (state_.itstate & 0xE0) | ((state_.itstate << 1) & 0x1F);
}

u32 Interpreter::reg(u32 n) const {
    if (n != 15) return state_.r[n];
    return state_.r[15] + (state_.thumb() ? 4 : 8);
}

void Interpreter::write_reg(u32 n, u32 value) {
    if (n != 15) {
        state_.r[n] = value;
    } else if (state_.thumb()) {
        branch_to(value);  // ALUWritePC en Thumb
    } else {
        bx_to(value);      // ALUWritePC en ARM (ARMv7) : avec changement de mode
    }
}

void Interpreter::branch_to(u32 addr) {
    state_.r[15] = state_.thumb() ? addr & ~1u : addr & ~3u;
    pc_written_ = true;
}

void Interpreter::bx_to(u32 addr) {
    if (addr & 1) {
        state_.cpsr |= CpuState::kFlagT;
        state_.r[15] = addr & ~1u;
    } else {
        state_.cpsr &= ~CpuState::kFlagT;
        state_.r[15] = addr & ~3u;
    }
    pc_written_ = true;
}

void Interpreter::set_nz(u32 result) {
    u32 f = state_.cpsr & ~(CpuState::kFlagN | CpuState::kFlagZ);
    if (result & 0x80000000u) f |= CpuState::kFlagN;
    if (result == 0) f |= CpuState::kFlagZ;
    state_.cpsr = f;
}

u32 Interpreter::alu(AluOp op, u32 a, u32 b, bool shifter_carry, bool set_flags) {
    bool c = shifter_carry;
    bool v = state_.cpsr & CpuState::kFlagV;
    auto add = [&](u32 x, u32 y, bool cin) {
        const u64 usum = static_cast<u64>(x) + y + cin;
        const u32 r = static_cast<u32>(usum);
        c = (usum >> 32) != 0;
        v = ((~(x ^ y)) & (x ^ r)) >> 31;
        return r;
    };
    const bool cf = carry();
    u32 r = 0;
    switch (op) {
        case AluOp::And: case AluOp::Tst: r = a & b; break;
        case AluOp::Eor: case AluOp::Teq: r = a ^ b; break;
        case AluOp::Orr: r = a | b; break;
        case AluOp::Orn: r = a | ~b; break;
        case AluOp::Bic: r = a & ~b; break;
        case AluOp::Mov: r = b; break;
        case AluOp::Mvn: r = ~b; break;
        case AluOp::Add: case AluOp::Cmn: r = add(a, b, false); break;
        case AluOp::Adc: r = add(a, b, cf); break;
        case AluOp::Sub: case AluOp::Cmp: r = add(a, ~b, true); break;
        case AluOp::Sbc: r = add(a, ~b, cf); break;
        case AluOp::Rsb: r = add(~a, b, true); break;
        case AluOp::Rsc: r = add(~a, b, cf); break;
    }
    if (set_flags) {
        u32 f = state_.cpsr & ~(CpuState::kFlagN | CpuState::kFlagZ | CpuState::kFlagC | CpuState::kFlagV);
        if (r & 0x80000000u) f |= CpuState::kFlagN;
        if (r == 0) f |= CpuState::kFlagZ;
        if (c) f |= CpuState::kFlagC;
        if (v) f |= CpuState::kFlagV;
        state_.cpsr = f;
    }
    return r;
}

Interpreter::StepResult Interpreter::load_store_multiple(u32 rn, u32 list, bool load, bool increment,
                                                         bool before, bool writeback) {
    list &= 0xFFFF;
    const u32 count = static_cast<u32>(__builtin_popcount(list));
    if (count == 0 || rn == 15) return StepResult::Undefined;

    const u32 base = state_.r[rn];
    const u32 new_base = increment ? base + 4 * count : base - 4 * count;
    u32 addr = increment ? (before ? base + 4 : base) : (before ? new_base : new_base + 4);

    if (load) {
        u32 pc_value = 0;
        for (u32 i = 0; i < 16; ++i) {
            if (!bit(list, i)) continue;
            const u32 value = memory_.read<u32>(addr);
            addr += 4;
            if (i == 15) pc_value = value; else state_.r[i] = value;
        }
        if (writeback && !bit(list, rn)) state_.r[rn] = new_base;
        if (bit(list, 15)) bx_to(pc_value);  // LoadWritePC : avec changement de mode
    } else {
        for (u32 i = 0; i < 16; ++i) {
            if (!bit(list, i)) continue;
            memory_.write<u32>(addr, reg(i));
            addr += 4;
        }
        if (writeback) state_.r[rn] = new_base;
    }
    return StepResult::Ok;
}

Interpreter::StepResult Interpreter::coprocessor(u32 instr) {
    // Coprocesseurs 10 et 11 : unité flottante VFP. En Thumb, la variante T=1
    // (bit 28) de cet espace correspond à NEON, pas encore pris en charge.
    if (bits(instr, 11, 9) == 5) return bit(instr, 28) ? StepResult::Undefined : vfp(instr);

    // Coprocesseur 15 : seul MRC p15, 0, Rt, c13, c0, 3 (lecture de TPIDRURO,
    // le pointeur TLS) est géré.
    const bool is_mrc = bits(instr, 27, 24) == 0xE && bit(instr, 20) && bit(instr, 4);
    if (is_mrc && bits(instr, 11, 8) == 15 && bits(instr, 23, 21) == 0 && bits(instr, 19, 16) == 13 &&
        bits(instr, 3, 0) == 0 && bits(instr, 7, 5) == 3) {
        const u32 rt = bits(instr, 15, 12);
        if (rt == 15) return StepResult::Undefined;
        state_.r[rt] = state_.tpidruro;
        return StepResult::Ok;
    }
    return StepResult::Undefined;
}

void Interpreter::most_significant_multiply(u32 rd, u32 ra, u32 rn, u32 rm, bool subtract, bool round) {
    const s64 product = static_cast<s64>(static_cast<s32>(state_.r[rn])) * static_cast<s32>(state_.r[rm]);
    s64 acc = ra == 15 ? 0 : static_cast<s64>(static_cast<u64>(state_.r[ra]) << 32);
    u64 result = subtract ? static_cast<u64>(acc) - static_cast<u64>(product) : static_cast<u64>(acc) + static_cast<u64>(product);
    if (round) result += 0x80000000u;
    state_.r[rd] = static_cast<u32>(result >> 32);
}

void Interpreter::halfword_multiply(u32 kind, bool n_top, bool m_top, u32 rd_hi, u32 ra_lo, u32 rn, u32 rm,
                                    bool accumulate) {
    constexpr u32 kFlagQ = 1u << 27;
    u32* r = state_.r;
    const s64 m = half(r[rm], m_top);
    auto add_sat_flag = [&](s64 value) {
        const s64 sum = value + (accumulate ? static_cast<s32>(r[ra_lo]) : 0);
        if (sum != static_cast<s32>(sum)) state_.cpsr |= kFlagQ;  // débordement : drapeau Q
        return static_cast<u32>(sum);
    };
    switch (kind) {
        case 0: case 3:  // SMLAxy / SMULxy
            r[rd_hi] = add_sat_flag(half(r[rn], n_top) * m);
            break;
        case 1:  // SMLAWy / SMULWy : produit 32 x 16 bits, on garde les 32 bits du haut
            r[rd_hi] = add_sat_flag((static_cast<s64>(static_cast<s32>(r[rn])) * m) >> 16);
            break;
        default: {  // SMLALxy
            const s64 acc = static_cast<s64>((static_cast<u64>(r[rd_hi]) << 32) | r[ra_lo]);
            const u64 result = static_cast<u64>(acc + half(r[rn], n_top) * m);
            r[ra_lo] = static_cast<u32>(result);
            r[rd_hi] = static_cast<u32>(result >> 32);
            break;
        }
    }
}

void Interpreter::svc(u32 imm, u32 return_addr) {
    state_.r[15] = return_addr;
    pc_written_ = true;
    if (svc_handler_) svc_handler_(imm, state_);
}

}  // namespace vita
