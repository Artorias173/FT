// Interpréteur : jeu d'instructions ARM (A32).
#include "interpreter_util.h"
#include "vita/cpu/interpreter.h"
#include "vita/memory/memory.h"

namespace vita {

using namespace arm;
using AluOp = Interpreter::AluOp;

namespace {
constexpr AluOp kArmAluOps[16] = {AluOp::And, AluOp::Eor, AluOp::Sub, AluOp::Rsb, AluOp::Add, AluOp::Adc,
                                  AluOp::Sbc, AluOp::Rsc, AluOp::Tst, AluOp::Teq, AluOp::Cmp, AluOp::Cmn,
                                  AluOp::Orr, AluOp::Mov, AluOp::Bic, AluOp::Mvn};
}  // namespace

Interpreter::StepResult Interpreter::exec_arm(u32 instr, u32 pc) {
    if (bits(instr, 31, 28) == 0xF) {
        // Espace inconditionnel.
        if (bits(instr, 27, 25) == 5) {  // BLX (immédiat) : passe en Thumb
            const u32 offset = sign_extend((bits(instr, 23, 0) << 2) | (bit(instr, 24) << 1), 26);
            state_.r[14] = pc + 4;
            bx_to((pc + 8 + offset) | 1);
            return StepResult::Ok;
        }
        if (bits(instr, 27, 26) == 1) return StepResult::Ok;  // PLD, PLI, DMB, DSB, ISB, CLREX
        return StepResult::Undefined;
    }

    switch (bits(instr, 27, 25)) {
        case 0:
            if (bit(instr, 4) && bit(instr, 7)) {
                if (bits(instr, 6, 5) == 0) return bit(instr, 24) ? arm_sync(instr) : arm_multiply(instr);
                return arm_extra_load_store(instr);
            }
            if ((instr & 0x01900000) == 0x01000000) return arm_misc(instr);
            return arm_data_processing(instr);

        case 1:
            if ((instr & 0x0FB00000) == 0x03000000) {  // MOVW / MOVT
                const u32 rd = bits(instr, 15, 12);
                const u32 imm16 = (bits(instr, 19, 16) << 12) | bits(instr, 11, 0);
                if (rd == 15) return StepResult::Undefined;
                state_.r[rd] = bit(instr, 22) ? (state_.r[rd] & 0xFFFF) | (imm16 << 16) : imm16;
                return StepResult::Ok;
            }
            if ((instr & 0x0FB00000) == 0x03200000) {  // MSR (immédiat) et indications (NOP, WFI...)
                if (bit(instr, 22)) return StepResult::Undefined;
                if (bit(instr, 19)) {
                    const u32 imm = ror(bits(instr, 7, 0), bits(instr, 11, 8) * 2);
                    state_.cpsr = (state_.cpsr & 0x07FFFFFF) | (imm & 0xF8000000);
                }
                return StepResult::Ok;
            }
            return arm_data_processing(instr);

        case 2:
            return arm_load_store(instr);

        case 3:
            return bit(instr, 4) ? arm_media(instr) : arm_load_store(instr);

        case 4:  // LDM / STM
            if (bit(instr, 22)) return StepResult::Undefined;  // variantes système
            return load_store_multiple(bits(instr, 19, 16), bits(instr, 15, 0), bit(instr, 20), bit(instr, 23),
                                       bit(instr, 24), bit(instr, 21));

        case 5: {  // B / BL
            const u32 offset = sign_extend(bits(instr, 23, 0) << 2, 26);
            if (bit(instr, 24)) state_.r[14] = pc + 4;
            branch_to(pc + 8 + offset);
            return StepResult::Ok;
        }

        case 6:
            return StepResult::Undefined;  // chargements coprocesseur (VLDR, VSTM...)

        default:
            if (bit(instr, 24)) {
                svc(bits(instr, 23, 0), pc + 4);
                return StepResult::Ok;
            }
            return coprocessor(instr);
    }
}

Interpreter::StepResult Interpreter::arm_data_processing(u32 instr) {
    const u32 opcode = bits(instr, 24, 21);
    const bool set_flags = bit(instr, 20);
    const bool test = opcode >= 8 && opcode <= 11;
    if (test && !set_flags) return StepResult::Undefined;

    const u32 rn = bits(instr, 19, 16), rd = bits(instr, 15, 12), rm = bits(instr, 3, 0);
    if (set_flags && rd == 15 && !test) return StepResult::Undefined;  // retour d'exception (mode système)

    Shifted op2;
    if (bit(instr, 25)) {
        const u32 rot = bits(instr, 11, 8) * 2;
        op2.value = ror(bits(instr, 7, 0), rot);
        op2.carry = rot ? bit(op2.value, 31) : carry();
    } else if (!bit(instr, 4)) {
        op2 = shift_imm_c(reg(rm), bits(instr, 6, 5), bits(instr, 11, 7), carry());
    } else {
        const u32 rs = bits(instr, 11, 8);
        if (rd == 15 || rn == 15 || rm == 15 || rs == 15) return StepResult::Undefined;
        op2 = shift_c(state_.r[rm], bits(instr, 6, 5), state_.r[rs] & 0xFF, carry());
    }

    const u32 result = alu(kArmAluOps[opcode], reg(rn), op2.value, op2.carry, set_flags);
    if (!test) write_reg(rd, result);
    return StepResult::Ok;
}

Interpreter::StepResult Interpreter::arm_misc(u32 instr) {
    const u32 op = bits(instr, 22, 21);
    const u32 rd = bits(instr, 15, 12), rm = bits(instr, 3, 0);
    if (bit(instr, 7)) {  // multiplications sur demi-mots (bit 4 = 0 ici)
        // op : 00 SMLAxy, 01 SMLAWy (bit 5 = 0) / SMULWy (bit 5 = 1), 10 SMLALxy, 11 SMULxy.
        const bool accumulate = op == 0 || op == 2 || (op == 1 && !bit(instr, 5));
        halfword_multiply(op, bit(instr, 5), bit(instr, 6), bits(instr, 19, 16), rd, rm, bits(instr, 11, 8),
                          accumulate);
        return StepResult::Ok;
    }
    switch (bits(instr, 6, 4)) {
        case 0:
            if (bit(instr, 22)) return StepResult::Undefined;  // SPSR
            if (!bit(instr, 21)) {                             // MRS
                state_.r[rd] = state_.cpsr & 0xF80F0000;       // APSR
            } else if (bit(instr, 19)) {                       // MSR APSR_nzcvq
                state_.cpsr = (state_.cpsr & 0x07FFFFFF) | (state_.r[rm] & 0xF8000000);
            }
            return StepResult::Ok;
        case 1:
            if (op == 1) { bx_to(reg(rm)); return StepResult::Ok; }                    // BX
            if (op == 3) { state_.r[rd] = count_leading_zeros(state_.r[rm]); return StepResult::Ok; }  // CLZ
            return StepResult::Undefined;
        case 3:
            if (op == 1) {  // BLX (registre)
                const u32 target = reg(rm);
                state_.r[14] = state_.r[15] + 4;
                bx_to(target);
                return StepResult::Ok;
            }
            return StepResult::Undefined;
        default:
            return StepResult::Undefined;  // BKPT, arithmétique saturée...
    }
}

Interpreter::StepResult Interpreter::arm_multiply(u32 instr) {
    const bool s = bit(instr, 20);
    const u32 rd_hi = bits(instr, 19, 16), ra_lo = bits(instr, 15, 12);
    const u32 rm = state_.r[bits(instr, 11, 8)], rn = state_.r[bits(instr, 3, 0)];

    auto write_long = [&](u64 v) {
        state_.r[ra_lo] = static_cast<u32>(v);
        state_.r[rd_hi] = static_cast<u32>(v >> 32);
        if (s) {
            u32 f = state_.cpsr & ~(CpuState::kFlagN | CpuState::kFlagZ);
            if (v >> 63) f |= CpuState::kFlagN;
            if (v == 0) f |= CpuState::kFlagZ;
            state_.cpsr = f;
        }
    };
    const u64 acc = (static_cast<u64>(state_.r[rd_hi]) << 32) | state_.r[ra_lo];

    switch (bits(instr, 23, 21)) {
        case 0: state_.r[rd_hi] = rn * rm; if (s) set_nz(state_.r[rd_hi]); return StepResult::Ok;   // MUL
        case 1: state_.r[rd_hi] = rn * rm + state_.r[ra_lo]; if (s) set_nz(state_.r[rd_hi]); return StepResult::Ok;  // MLA
        case 2:  // UMAAL
            if (s) return StepResult::Undefined;
            write_long(static_cast<u64>(rn) * rm + state_.r[ra_lo] + state_.r[rd_hi]);
            return StepResult::Ok;
        case 3:  // MLS
            if (s) return StepResult::Undefined;
            state_.r[rd_hi] = state_.r[ra_lo] - rn * rm;
            return StepResult::Ok;
        case 4: write_long(static_cast<u64>(rn) * rm); return StepResult::Ok;           // UMULL
        case 5: write_long(static_cast<u64>(rn) * rm + acc); return StepResult::Ok;     // UMLAL
        case 6: write_long(static_cast<u64>(static_cast<s64>(static_cast<s32>(rn)) * static_cast<s32>(rm)));
                return StepResult::Ok;                                                  // SMULL
        default: write_long(static_cast<u64>(static_cast<s64>(static_cast<s32>(rn)) * static_cast<s32>(rm)) + acc);
                return StepResult::Ok;                                                  // SMLAL
    }
}

// Accès exclusifs. Un seul cœur est émulé pour l'instant : STREX réussit toujours.
Interpreter::StepResult Interpreter::arm_sync(u32 instr) {
    const u32 addr = state_.r[bits(instr, 19, 16)];
    const u32 rd = bits(instr, 15, 12), rt = bits(instr, 3, 0);
    switch (bits(instr, 23, 20)) {
        case 0x9: state_.r[rd] = memory_.read<u32>(addr); return StepResult::Ok;  // LDREX
        case 0xD: state_.r[rd] = memory_.read<u8>(addr); return StepResult::Ok;   // LDREXB
        case 0xF: state_.r[rd] = memory_.read<u16>(addr); return StepResult::Ok;  // LDREXH
        case 0xB:                                                                  // LDREXD
            if (rd & 1 || rd == 14) return StepResult::Undefined;
            state_.r[rd] = memory_.read<u32>(addr);
            state_.r[rd + 1] = memory_.read<u32>(addr + 4);
            return StepResult::Ok;
        case 0x8: memory_.write<u32>(addr, state_.r[rt]); state_.r[rd] = 0; return StepResult::Ok;  // STREX
        case 0xC: memory_.write<u8>(addr, static_cast<u8>(state_.r[rt])); state_.r[rd] = 0; return StepResult::Ok;
        case 0xE: memory_.write<u16>(addr, static_cast<u16>(state_.r[rt])); state_.r[rd] = 0; return StepResult::Ok;
        case 0xA:                                                                  // STREXD
            if (rt & 1 || rt == 14) return StepResult::Undefined;
            memory_.write<u32>(addr, state_.r[rt]);
            memory_.write<u32>(addr + 4, state_.r[rt + 1]);
            state_.r[rd] = 0;
            return StepResult::Ok;
        default:
            return StepResult::Undefined;  // SWP/SWPB (obsolètes)
    }
}

// LDRH, STRH, LDRSB, LDRSH, LDRD, STRD.
Interpreter::StepResult Interpreter::arm_extra_load_store(u32 instr) {
    const bool pre = bit(instr, 24), up = bit(instr, 23), load = bit(instr, 20);
    const bool writeback = !pre || bit(instr, 21);
    const u32 rn = bits(instr, 19, 16), rt = bits(instr, 15, 12);
    const u32 offset = bit(instr, 22) ? (bits(instr, 11, 8) << 4) | bits(instr, 3, 0) : state_.r[bits(instr, 3, 0)];

    const u32 base = reg(rn);
    const u32 offset_addr = up ? base + offset : base - offset;
    const u32 addr = pre ? offset_addr : base;
    const u32 op2 = bits(instr, 6, 5);

    if (op2 != 1 && !load && (rt & 1 || rt == 14)) return StepResult::Undefined;  // LDRD/STRD : Rt pair

    u32 v1 = 0, v2 = 0;
    switch ((op2 << 1) | load) {
        case 2: memory_.write<u16>(addr, static_cast<u16>(reg(rt))); break;                     // STRH
        case 3: v1 = memory_.read<u16>(addr); break;                                           // LDRH
        case 4: v1 = memory_.read<u32>(addr); v2 = memory_.read<u32>(addr + 4); break;         // LDRD
        case 5: v1 = sign_extend(memory_.read<u8>(addr), 8); break;                            // LDRSB
        case 6: memory_.write<u32>(addr, reg(rt)); memory_.write<u32>(addr + 4, reg(rt + 1)); break;  // STRD
        case 7: v1 = sign_extend(memory_.read<u16>(addr), 16); break;                          // LDRSH
        default: return StepResult::Undefined;
    }
    if (writeback && rn != 15) state_.r[rn] = offset_addr;
    if (load || op2 == 2) {
        write_reg(rt, v1);
        if (!load) state_.r[rt + 1] = v2;  // LDRD (L=0 dans cet encodage)
    }
    return StepResult::Ok;
}

// LDR, STR, LDRB, STRB.
Interpreter::StepResult Interpreter::arm_load_store(u32 instr) {
    const bool pre = bit(instr, 24), up = bit(instr, 23), byte = bit(instr, 22), load = bit(instr, 20);
    const bool writeback = !pre || bit(instr, 21);
    const u32 rn = bits(instr, 19, 16), rt = bits(instr, 15, 12);

    const u32 offset = bit(instr, 25)
        ? shift_imm_c(reg(bits(instr, 3, 0)), bits(instr, 6, 5), bits(instr, 11, 7), carry()).value
        : bits(instr, 11, 0);
    const u32 base = reg(rn);
    const u32 offset_addr = up ? base + offset : base - offset;
    const u32 addr = pre ? offset_addr : base;

    if (load) {
        const u32 value = byte ? memory_.read<u8>(addr) : memory_.read<u32>(addr);
        if (writeback && rn != 15) state_.r[rn] = offset_addr;
        if (rt == 15) bx_to(value); else state_.r[rt] = value;  // LoadWritePC
    } else {
        const u32 value = reg(rt);
        if (byte) memory_.write<u8>(addr, static_cast<u8>(value)); else memory_.write<u32>(addr, value);
        if (writeback && rn != 15) state_.r[rn] = offset_addr;
    }
    return StepResult::Ok;
}

// Extensions, inversions d'octets et champs de bits.
Interpreter::StepResult Interpreter::arm_media(u32 instr) {
    const u32 op1 = bits(instr, 24, 20), op2 = bits(instr, 7, 5);
    const u32 rd = bits(instr, 15, 12), rn = bits(instr, 19, 16), rm = bits(instr, 3, 0);

    if (bits(op1, 4, 3) == 1) {
        const u32 sub = bits(op1, 2, 0);
        if (op2 == 3) {  // SXTB, SXTH, UXTB, UXTH (+ variantes avec addition)
            static constexpr int kKind[8] = {-1, -1, 4, 0, -1, -1, 5, 1};
            u32 out;
            if (kKind[sub] < 0 || !extend(static_cast<u32>(kKind[sub]), state_.r[rm], bits(instr, 11, 10) * 8, out))
                return StepResult::Undefined;
            state_.r[rd] = rn == 15 ? out : out + state_.r[rn];
            return StepResult::Ok;
        }
        if (sub == 3 && op2 == 1) { state_.r[rd] = byte_swap(state_.r[rm]); return StepResult::Ok; }  // REV
        if (sub == 3 && op2 == 5) { state_.r[rd] = rev16(state_.r[rm]); return StepResult::Ok; }      // REV16
        if (sub == 7 && op2 == 1) { state_.r[rd] = bit_reverse(state_.r[rm]); return StepResult::Ok; } // RBIT
        if (sub == 7 && op2 == 5) { state_.r[rd] = revsh(state_.r[rm]); return StepResult::Ok; }      // REVSH
        return StepResult::Undefined;
    }

    const u32 lsb = bits(instr, 11, 7), field = bits(instr, 20, 16);
    if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) { state_.r[rd] = sbfx(state_.r[rm], lsb, field + 1); return StepResult::Ok; }
    if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) { state_.r[rd] = ubfx(state_.r[rm], lsb, field + 1); return StepResult::Ok; }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {  // BFI, ou BFC si Rn = 15
        state_.r[rd] = bfi(state_.r[rd], rm == 15 ? 0 : state_.r[rm], lsb, field);
        return StepResult::Ok;
    }
    return StepResult::Undefined;
}

}  // namespace vita
