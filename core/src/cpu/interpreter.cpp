#include "vita/cpu/interpreter.h"

#include <cstdio>

#include "vita/memory/memory.h"

namespace vita {

namespace {

constexpr u32 bits(u32 v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
constexpr bool bit(u32 v, int n) { return (v >> n) & 1; }
constexpr u32 ror(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

struct AddResult {
    u32 value;
    bool carry;
    bool overflow;
};

AddResult add_with_carry(u32 a, u32 b, bool carry_in) {
    const u64 usum = static_cast<u64>(a) + b + carry_in;
    const s64 ssum = static_cast<s64>(static_cast<s32>(a)) + static_cast<s32>(b) + carry_in;
    const u32 result = static_cast<u32>(usum);
    return {result, (usum >> 32) != 0, static_cast<s64>(static_cast<s32>(result)) != ssum};
}

}  // namespace

CpuExit Interpreter::run(u64 max_instructions) {
    halt_requested_ = false;
    for (u64 i = 0; i < max_instructions; ++i) {
        if (halt_requested_) return CpuExit::Halted;

        switch (step()) {
            case StepResult::Ok:
                break;
            case StepResult::Undefined:
                return CpuExit::Undefined;
            case StepResult::Fault:
                return CpuExit::MemoryFault;
        }
        // Le gestionnaire SVC peut aussi demander l'arrêt.
        if (halt_requested_) return CpuExit::Halted;
    }
    return CpuExit::BudgetExhausted;
}

bool Interpreter::condition_passed(u32 cond) const {
    const u32 f = state_.cpsr;
    const bool n = f & CpuState::kFlagN, z = f & CpuState::kFlagZ;
    const bool c = f & CpuState::kFlagC, v = f & CpuState::kFlagV;
    switch (cond) {
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
        default: return true;             // AL (0xF : encodages inconditionnels)
    }
}

u32 Interpreter::reg(u32 n) const { return n == 15 ? state_.r[15] + 8 : state_.r[n]; }

void Interpreter::write_reg(u32 n, u32 value, bool& pc_written) {
    if (n == 15) {
        state_.r[15] = value & ~1u;
        pc_written = true;
    } else {
        state_.r[n] = value;
    }
}

Interpreter::StepResult Interpreter::step() {
    char buf[96];
    const u32 pc = state_.r[15];

    if (state_.thumb()) {
        std::snprintf(buf, sizeof(buf), "Mode Thumb pas encore pris en charge (PC=0x%08x)", pc);
        last_error_ = buf;
        return StepResult::Undefined;
    }

    const u32 instr = memory_.read<u32>(pc);
    if (memory_.take_fault()) {
        std::snprintf(buf, sizeof(buf), "Lecture d'instruction hors mémoire à 0x%08x", pc);
        last_error_ = buf;
        return StepResult::Fault;
    }

    bool pc_written = false;
    StepResult result = StepResult::Ok;
    const u32 cond = bits(instr, 31, 28);

    if (cond != 0xF && !condition_passed(cond)) {
        // Instruction ignorée : la condition n'est pas remplie.
    } else if ((instr & 0x0FFFFFF0) == 0x012FFF10 || (instr & 0x0FFFFFF0) == 0x012FFF30) {
        // BX / BLX (registre)
        const u32 target = reg(bits(instr, 3, 0));
        if (bit(instr, 5)) state_.r[14] = pc + 4;
        if (target & 1) state_.cpsr |= CpuState::kFlagT; else state_.cpsr &= ~CpuState::kFlagT;
        state_.r[15] = target & ~1u;
        pc_written = true;
    } else if ((instr & 0x0E000000) == 0x0A000000 && cond != 0xF) {
        // B / BL
        const s32 offset = static_cast<s32>(bits(instr, 23, 0) << 8) >> 6;  // extension de signe, *4
        if (bit(instr, 24)) state_.r[14] = pc + 4;
        state_.r[15] = pc + 8 + offset;
        pc_written = true;
    } else if ((instr & 0x0F000000) == 0x0F000000 && cond != 0xF) {
        // SVC : on laisse le HLE répondre à l'appel système
        state_.r[15] = pc + 4;
        pc_written = true;
        if (svc_handler_) svc_handler_(bits(instr, 23, 0), state_);
    } else if ((instr & 0x0FB00000) == 0x03000000) {
        // MOVW (bit 22 = 0) / MOVT (bit 22 = 1)
        const u32 imm16 = (bits(instr, 19, 16) << 12) | bits(instr, 11, 0);
        const u32 rd = bits(instr, 15, 12);
        if (bit(instr, 22))
            state_.r[rd] = (state_.r[rd] & 0xFFFF) | (imm16 << 16);
        else
            state_.r[rd] = imm16;
    } else if ((instr & 0x0C000000) == 0x00000000 && cond != 0xF) {
        result = exec_data_processing(instr, pc_written);
    } else if ((instr & 0x0C000000) == 0x04000000 && cond != 0xF) {
        result = exec_load_store(instr, pc_written);
    } else {
        result = StepResult::Undefined;
    }

    if (result == StepResult::Undefined) {
        std::snprintf(buf, sizeof(buf), "Instruction ARM non prise en charge 0x%08x à 0x%08x",
                      instr, pc);
        last_error_ = buf;
        return result;
    }
    if (result == StepResult::Fault) return result;

    if (!pc_written) state_.r[15] = pc + 4;
    return StepResult::Ok;
}

Interpreter::StepResult Interpreter::exec_data_processing(u32 instr, bool& pc_written) {
    const bool imm = bit(instr, 25);
    // Avec I=0, bits 7 et 4 à 1 : multiplications et load/store étendus (non gérés).
    if (!imm && bit(instr, 4) && bit(instr, 7)) return StepResult::Undefined;
    // Décalage par registre : pas encore géré.
    if (!imm && bit(instr, 4)) return StepResult::Undefined;

    const u32 opcode = bits(instr, 24, 21);
    const bool set_flags = bit(instr, 20);
    // TST/TEQ/CMP/CMN sans S : espace des instructions « misc » (MRS, MSR, CLZ...).
    if (opcode >= 8 && opcode <= 11 && !set_flags) return StepResult::Undefined;

    const u32 rn = bits(instr, 19, 16);
    const u32 rd = bits(instr, 15, 12);
    bool carry = state_.cpsr & CpuState::kFlagC;
    u32 op2;

    if (imm) {
        const u32 rot = bits(instr, 11, 8) * 2;
        op2 = ror(bits(instr, 7, 0), rot);
        if (rot) carry = bit(op2, 31);
    } else {
        const u32 rm_val = reg(bits(instr, 3, 0));
        const u32 type = bits(instr, 6, 5);
        const u32 amount = bits(instr, 11, 7);
        switch (type) {
            case 0:  // LSL
                op2 = rm_val << amount;
                if (amount) carry = bit(rm_val, 32 - amount);
                break;
            case 1:  // LSR (0 code un décalage de 32)
                op2 = amount ? rm_val >> amount : 0;
                carry = bit(rm_val, amount ? amount - 1 : 31);
                break;
            case 2:  // ASR (0 code un décalage de 32)
                op2 = amount ? static_cast<u32>(static_cast<s32>(rm_val) >> amount)
                             : (bit(rm_val, 31) ? 0xFFFFFFFFu : 0);
                carry = bit(rm_val, amount ? amount - 1 : 31);
                break;
            default:  // ROR, ou RRX si le décalage vaut 0
                if (amount) {
                    op2 = ror(rm_val, amount);
                    carry = bit(op2, 31);
                } else {
                    op2 = (rm_val >> 1) | (static_cast<u32>(carry) << 31);
                    carry = bit(rm_val, 0);
                }
                break;
        }
    }

    const u32 a = reg(rn);
    const bool c_in = state_.cpsr & CpuState::kFlagC;
    bool overflow = state_.cpsr & CpuState::kFlagV;
    u32 result = 0;

    auto arithmetic = [&](u32 x, u32 y, bool cin) {
        const AddResult r = add_with_carry(x, y, cin);
        carry = r.carry;
        overflow = r.overflow;
        return r.value;
    };

    switch (opcode) {
        case 0x0: case 0x8: result = a & op2; break;                      // AND / TST
        case 0x1: case 0x9: result = a ^ op2; break;                      // EOR / TEQ
        case 0x2: case 0xA: result = arithmetic(a, ~op2, true); break;    // SUB / CMP
        case 0x3: result = arithmetic(op2, ~a, true); break;              // RSB
        case 0x4: case 0xB: result = arithmetic(a, op2, false); break;    // ADD / CMN
        case 0x5: result = arithmetic(a, op2, c_in); break;               // ADC
        case 0x6: result = arithmetic(a, ~op2, c_in); break;              // SBC
        case 0x7: result = arithmetic(op2, ~a, c_in); break;              // RSC
        case 0xC: result = a | op2; break;                                // ORR
        case 0xD: result = op2; break;                                    // MOV
        case 0xE: result = a & ~op2; break;                               // BIC
        case 0xF: result = ~op2; break;                                   // MVN
    }

    const bool writes_result = opcode < 8 || opcode > 11;
    if (writes_result) write_reg(rd, result, pc_written);

    if (set_flags && rd != 15) {
        u32 f = state_.cpsr & ~(CpuState::kFlagN | CpuState::kFlagZ | CpuState::kFlagC | CpuState::kFlagV);
        if (result & 0x80000000u) f |= CpuState::kFlagN;
        if (result == 0) f |= CpuState::kFlagZ;
        if (carry) f |= CpuState::kFlagC;
        if (overflow) f |= CpuState::kFlagV;
        state_.cpsr = f;
    }
    return StepResult::Ok;
}

Interpreter::StepResult Interpreter::exec_load_store(u32 instr, bool& pc_written) {
    const bool reg_offset = bit(instr, 25);
    if (reg_offset && bit(instr, 4)) return StepResult::Undefined;  // espace « media »

    const bool pre = bit(instr, 24), up = bit(instr, 23), byte = bit(instr, 22);
    const bool writeback = bit(instr, 21), load = bit(instr, 20);
    const u32 rn = bits(instr, 19, 16), rd = bits(instr, 15, 12);

    u32 offset;
    if (!reg_offset) {
        offset = bits(instr, 11, 0);
    } else {
        const u32 rm_val = reg(bits(instr, 3, 0));
        const u32 amount = bits(instr, 11, 7);
        switch (bits(instr, 6, 5)) {
            case 0: offset = rm_val << amount; break;
            case 1: offset = amount ? rm_val >> amount : 0; break;
            case 2: offset = static_cast<u32>(static_cast<s32>(rm_val) >> (amount ? amount : 31)); break;
            default: offset = ror(rm_val, amount); break;  // RRX non géré
        }
    }

    const u32 base = reg(rn);
    const u32 offset_addr = up ? base + offset : base - offset;
    const u32 addr = pre ? offset_addr : base;

    if (load) {
        const u32 value = byte ? memory_.read<u8>(addr) : memory_.read<u32>(addr);
        if (memory_.take_fault()) {
            last_error_ = "Lecture hors mémoire";
            return StepResult::Fault;
        }
        if (!pre || writeback) write_reg(rn, offset_addr, pc_written);
        if (rd == 15) {
            // LDR PC : branchement avec échange ARM/Thumb (ARMv5T et plus).
            if (value & 1) state_.cpsr |= CpuState::kFlagT; else state_.cpsr &= ~CpuState::kFlagT;
        }
        write_reg(rd, value, pc_written);
    } else {
        const u32 value = reg(rd);
        if (byte) memory_.write<u8>(addr, static_cast<u8>(value)); else memory_.write<u32>(addr, value);
        if (memory_.take_fault()) {
            last_error_ = "Écriture hors mémoire";
            return StepResult::Fault;
        }
        if (!pre || writeback) write_reg(rn, offset_addr, pc_written);
    }
    return StepResult::Ok;
}

}  // namespace vita
