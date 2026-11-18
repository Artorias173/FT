// Interpréteur : jeux d'instructions Thumb (16 bits) et Thumb-2 (32 bits).
#include "interpreter_util.h"
#include "vita/cpu/interpreter.h"
#include "vita/memory/memory.h"

namespace vita {

using namespace arm;
using AluOp = Interpreter::AluOp;

namespace {
constexpr u32 align4(u32 v) { return v & ~3u; }
}  // namespace

// ---------------------------------------------------------------------------
// Instructions 16 bits
// ---------------------------------------------------------------------------

Interpreter::StepResult Interpreter::exec_thumb16(u16 hw, u32 pc) {
    const u32 op5 = hw >> 11;
    const bool sf = !in_it_block();  // en Thumb 16 bits, les drapeaux ne changent pas dans un bloc IT
    u32* r = state_.r;

    // Décalage immédiat, addition / soustraction, opérations sur immédiat 8 bits.
    if (op5 <= 2) {
        const Shifted s = shift_imm_c(r[bits(hw, 5, 3)], op5, bits(hw, 10, 6), carry());
        r[hw & 7] = alu(AluOp::Mov, 0, s.value, s.carry, sf);
        return StepResult::Ok;
    }
    if (op5 == 3) {
        const u32 b = bit(hw, 10) ? bits(hw, 8, 6) : r[bits(hw, 8, 6)];
        r[hw & 7] = alu(bit(hw, 9) ? AluOp::Sub : AluOp::Add, r[bits(hw, 5, 3)], b, carry(), sf);
        return StepResult::Ok;
    }
    if (op5 <= 7) {
        const u32 rdn = bits(hw, 10, 8), imm8 = hw & 0xFF;
        switch (op5) {
            case 4: r[rdn] = alu(AluOp::Mov, 0, imm8, carry(), sf); break;
            case 5: alu(AluOp::Cmp, r[rdn], imm8, carry(), true); break;
            case 6: r[rdn] = alu(AluOp::Add, r[rdn], imm8, carry(), sf); break;
            default: r[rdn] = alu(AluOp::Sub, r[rdn], imm8, carry(), sf); break;
        }
        return StepResult::Ok;
    }

    // Opérations registre à registre.
    if ((hw >> 10) == 0x10) {
        const u32 rm = bits(hw, 5, 3), rdn = hw & 7;
        const u32 a = r[rdn], b = r[rm];
        switch (bits(hw, 9, 6)) {
            case 0x0: r[rdn] = alu(AluOp::And, a, b, carry(), sf); break;
            case 0x1: r[rdn] = alu(AluOp::Eor, a, b, carry(), sf); break;
            case 0x2: case 0x3: case 0x4: case 0x7: {
                static constexpr u32 kType[8] = {0, 0, kLsl, kLsr, kAsr, 0, 0, kRor};
                const Shifted s = shift_c(a, kType[bits(hw, 9, 6)], b & 0xFF, carry());
                r[rdn] = alu(AluOp::Mov, 0, s.value, s.carry, sf);
                break;
            }
            case 0x5: r[rdn] = alu(AluOp::Adc, a, b, carry(), sf); break;
            case 0x6: r[rdn] = alu(AluOp::Sbc, a, b, carry(), sf); break;
            case 0x8: alu(AluOp::Tst, a, b, carry(), true); break;
            case 0x9: r[rdn] = alu(AluOp::Rsb, b, 0, carry(), sf); break;  // NEG : Rd = 0 - Rm
            case 0xA: alu(AluOp::Cmp, a, b, carry(), true); break;
            case 0xB: alu(AluOp::Cmn, a, b, carry(), true); break;
            case 0xC: r[rdn] = alu(AluOp::Orr, a, b, carry(), sf); break;
            case 0xD: r[rdn] = a * b; if (sf) set_nz(r[rdn]); break;      // MUL
            case 0xE: r[rdn] = alu(AluOp::Bic, a, b, carry(), sf); break;
            default: r[rdn] = alu(AluOp::Mvn, 0, b, carry(), sf); break;
        }
        return StepResult::Ok;
    }

    // Registres hauts et branchements avec changement de mode.
    if ((hw >> 10) == 0x11) {
        const u32 rdn = (bit(hw, 7) << 3) | (hw & 7), rm = bits(hw, 6, 3);
        switch (bits(hw, 9, 8)) {
            case 0: write_reg(rdn, reg(rdn) + reg(rm)); break;               // ADD (sans drapeaux)
            case 1: alu(AluOp::Cmp, reg(rdn), reg(rm), carry(), true); break; // CMP
            case 2: write_reg(rdn, reg(rm)); break;                           // MOV
            default:
                if (bit(hw, 7)) {  // BLX
                    const u32 target = reg(rm);
                    r[14] = (pc + 2) | 1;
                    bx_to(target);
                } else {           // BX
                    bx_to(reg(rm));
                }
                break;
        }
        return StepResult::Ok;
    }

    if (op5 == 9) {  // LDR (littéral)
        r[bits(hw, 10, 8)] = memory_.read<u32>(align4(pc + 4) + (hw & 0xFF) * 4);
        return StepResult::Ok;
    }

    if ((hw >> 12) == 5) {  // chargement / stockage, décalage par registre
        const u32 addr = r[bits(hw, 5, 3)] + r[bits(hw, 8, 6)];
        const u32 rt = hw & 7;
        switch (bits(hw, 11, 9)) {
            case 0: memory_.write<u32>(addr, r[rt]); break;                          // STR
            case 1: memory_.write<u16>(addr, static_cast<u16>(r[rt])); break;        // STRH
            case 2: memory_.write<u8>(addr, static_cast<u8>(r[rt])); break;          // STRB
            case 3: r[rt] = sign_extend(memory_.read<u8>(addr), 8); break;           // LDRSB
            case 4: r[rt] = memory_.read<u32>(addr); break;                          // LDR
            case 5: r[rt] = memory_.read<u16>(addr); break;                          // LDRH
            case 6: r[rt] = memory_.read<u8>(addr); break;                           // LDRB
            default: r[rt] = sign_extend(memory_.read<u16>(addr), 16); break;        // LDRSH
        }
        return StepResult::Ok;
    }

    if (op5 >= 0xC && op5 <= 0x11) {  // chargement / stockage, décalage immédiat
        const u32 imm5 = bits(hw, 10, 6), base = r[bits(hw, 5, 3)], rt = hw & 7;
        switch (op5) {
            case 0xC: memory_.write<u32>(base + imm5 * 4, r[rt]); break;
            case 0xD: r[rt] = memory_.read<u32>(base + imm5 * 4); break;
            case 0xE: memory_.write<u8>(base + imm5, static_cast<u8>(r[rt])); break;
            case 0xF: r[rt] = memory_.read<u8>(base + imm5); break;
            case 0x10: memory_.write<u16>(base + imm5 * 2, static_cast<u16>(r[rt])); break;
            default: r[rt] = memory_.read<u16>(base + imm5 * 2); break;
        }
        return StepResult::Ok;
    }

    const u32 rt8 = bits(hw, 10, 8), imm8x4 = (hw & 0xFF) * 4;
    switch (op5) {
        case 0x12: memory_.write<u32>(r[13] + imm8x4, r[rt8]); return StepResult::Ok;  // STR [SP]
        case 0x13: r[rt8] = memory_.read<u32>(r[13] + imm8x4); return StepResult::Ok;  // LDR [SP]
        case 0x14: r[rt8] = align4(pc + 4) + imm8x4; return StepResult::Ok;            // ADR
        case 0x15: r[rt8] = r[13] + imm8x4; return StepResult::Ok;                     // ADD Rd, SP
        case 0x18: return load_store_multiple(rt8, hw & 0xFF, false, true, false, true);  // STMIA
        case 0x19: return load_store_multiple(rt8, hw & 0xFF, true, true, false, !bit(hw, rt8));  // LDMIA
        case 0x1C:                                                                      // B
            branch_to(pc + 4 + sign_extend((hw & 0x7FF) << 1, 12));
            return StepResult::Ok;
        default: break;
    }

    if ((hw >> 12) == 0xD) {  // branchement conditionnel, SVC, UDF
        const u32 cond = bits(hw, 11, 8);
        if (cond == 0xE) return StepResult::Undefined;
        if (cond == 0xF) {
            svc(hw & 0xFF, pc + 2);
            return StepResult::Ok;
        }
        if (condition_passed(cond)) branch_to(pc + 4 + sign_extend((hw & 0xFF) << 1, 9));
        return StepResult::Ok;
    }

    if ((hw >> 12) == 0xB) {  // instructions diverses
        if ((hw & 0xFF00) == 0xB000) {  // ADD / SUB SP, #imm
            const u32 imm = (hw & 0x7F) * 4;
            r[13] = bit(hw, 7) ? r[13] - imm : r[13] + imm;
            return StepResult::Ok;
        }
        if ((hw & 0xF500) == 0xB100) {  // CBZ / CBNZ
            const u32 imm = (bit(hw, 9) << 6) | (bits(hw, 7, 3) << 1);
            if ((r[hw & 7] != 0) == bit(hw, 11)) branch_to(pc + 4 + imm);
            return StepResult::Ok;
        }
        if ((hw & 0xFF00) == 0xB200) {  // SXTH, SXTB, UXTH, UXTB
            static constexpr u32 kKind[4] = {0, 4, 1, 5};
            u32 out;
            extend(kKind[bits(hw, 7, 6)], r[bits(hw, 5, 3)], 0, out);
            r[hw & 7] = out;
            return StepResult::Ok;
        }
        if ((hw & 0xFE00) == 0xB400)  // PUSH
            return load_store_multiple(13, (hw & 0xFF) | (bit(hw, 8) << 14), false, false, true, true);
        if ((hw & 0xFE00) == 0xBC00)  // POP
            return load_store_multiple(13, (hw & 0xFF) | (bit(hw, 8) << 15), true, true, false, true);
        if ((hw & 0xFF00) == 0xBA00) {  // REV, REV16, REVSH
            const u32 v = r[bits(hw, 5, 3)];
            switch (bits(hw, 7, 6)) {
                case 0: r[hw & 7] = byte_swap(v); return StepResult::Ok;
                case 1: r[hw & 7] = rev16(v); return StepResult::Ok;
                case 3: r[hw & 7] = revsh(v); return StepResult::Ok;
                default: return StepResult::Undefined;
            }
        }
        if ((hw & 0xFF00) == 0xBF00) {  // IT, ou indication (NOP, YIELD, WFI...)
            if (hw & 0xF) state_.itstate = static_cast<u8>(hw & 0xFF);
            return StepResult::Ok;
        }
    }
    return StepResult::Undefined;  // BKPT, CPS, SETEND...
}

// ---------------------------------------------------------------------------
// Instructions 32 bits (Thumb-2)
// ---------------------------------------------------------------------------

Interpreter::StepResult Interpreter::exec_thumb32(u16 hw1, u16 hw2, u32 pc) {
    const u32 op1 = bits(hw1, 12, 11), op2 = bits(hw1, 10, 4);
    const u32 instr32 = (static_cast<u32>(hw1) << 16) | hw2;

    // NEON : même encodage qu'en ARM, à la position du bit U près.
    if ((hw1 & 0xEF00) == 0xEF00) return neon_data(0xF2000000 | (bit(hw1, 12) << 24) | (instr32 & 0x00FFFFFF));
    if ((hw1 & 0xFF10) == 0xF900) return neon_load_store(0xF4000000 | (instr32 & 0x00FFFFFF));

    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) {  // LDM / STM (PUSH.W, POP.W)
            const u32 mode = bits(hw1, 8, 7), rn = hw1 & 0xF;
            if (mode == 1) return load_store_multiple(rn, hw2, bit(hw1, 4), true, false, bit(hw1, 5));
            if (mode == 2) return load_store_multiple(rn, hw2, bit(hw1, 4), false, true, bit(hw1, 5));
            return StepResult::Undefined;  // RFE, SRS
        }
        if ((op2 & 0x64) == 0x04) return thumb32_dual_exclusive_table(hw1, hw2, pc);
        if ((op2 & 0x60) == 0x20) {  // traitement de données, registre décalé
            const u32 imm5 = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
            const Shifted s = shift_imm_c(state_.r[hw2 & 0xF], bits(hw2, 5, 4), imm5, carry());
            return thumb32_alu(bits(hw1, 8, 5), bit(hw1, 4), hw1 & 0xF, bits(hw2, 11, 8), s.value, s.carry);
        }
        return coprocessor(instr32);
    }

    if (op1 == 2) {
        if (bit(hw2, 15)) return thumb32_branch_misc(hw1, hw2, pc);
        if (bit(hw1, 9)) return thumb32_plain_immediate(hw1, hw2, pc);
        // Traitement de données, immédiat modifié.
        const u32 imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | (hw2 & 0xFF);
        const Shifted s = thumb_expand_imm_c(imm12, carry());
        return thumb32_alu(bits(hw1, 8, 5), bit(hw1, 4), hw1 & 0xF, bits(hw2, 11, 8), s.value, s.carry);
    }

    // op1 == 3
    if ((hw1 & 0xFE00) == 0xF800) return thumb32_load_store_single(hw1, hw2);
    if ((hw1 & 0xFF00) == 0xFA00) return thumb32_data_processing_register(hw1, hw2);
    if ((hw1 & 0xFF00) == 0xFB00) return thumb32_multiply(hw1, hw2);
    if (op2 & 0x40) return coprocessor(instr32);
    return StepResult::Undefined;
}

Interpreter::StepResult Interpreter::thumb32_alu(u32 op, bool set_flags, u32 rn, u32 rd, u32 operand,
                                                 bool shifter_carry) {
    const bool test = rd == 15 && set_flags;
    AluOp alu_op;
    switch (op) {
        case 0x0: alu_op = test ? AluOp::Tst : AluOp::And; break;
        case 0x1: alu_op = AluOp::Bic; break;
        case 0x2: alu_op = rn == 15 ? AluOp::Mov : AluOp::Orr; break;
        case 0x3: alu_op = rn == 15 ? AluOp::Mvn : AluOp::Orn; break;
        case 0x4: alu_op = test ? AluOp::Teq : AluOp::Eor; break;
        case 0x8: alu_op = test ? AluOp::Cmn : AluOp::Add; break;
        case 0xA: alu_op = AluOp::Adc; break;
        case 0xB: alu_op = AluOp::Sbc; break;
        case 0xD: alu_op = test ? AluOp::Cmp : AluOp::Sub; break;
        case 0xE: alu_op = AluOp::Rsb; break;
        default: return StepResult::Undefined;  // PKHBT/PKHTB
    }
    const bool is_test = alu_op == AluOp::Tst || alu_op == AluOp::Teq || alu_op == AluOp::Cmn || alu_op == AluOp::Cmp;
    if (!is_test && rd == 15) return StepResult::Undefined;

    const u32 result = alu(alu_op, state_.r[rn], operand, shifter_carry, set_flags);
    if (!is_test) state_.r[rd] = result;
    return StepResult::Ok;
}

Interpreter::StepResult Interpreter::thumb32_plain_immediate(u16 hw1, u16 hw2, u32 pc) {
    const u32 rn = hw1 & 0xF, rd = bits(hw2, 11, 8);
    const u32 imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | (hw2 & 0xFF);
    const u32 imm16 = ((hw1 & 0xF) << 12) | imm12;
    const u32 lsb = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
    u32* r = state_.r;

    switch (bits(hw1, 8, 4)) {
        case 0x00: r[rd] = (rn == 15 ? align4(pc + 4) : r[rn]) + imm12; return StepResult::Ok;  // ADDW, ADR
        case 0x0A: r[rd] = (rn == 15 ? align4(pc + 4) : r[rn]) - imm12; return StepResult::Ok;  // SUBW, ADR
        case 0x04: r[rd] = imm16; return StepResult::Ok;                                          // MOVW
        case 0x0C: r[rd] = (r[rd] & 0xFFFF) | (imm16 << 16); return StepResult::Ok;              // MOVT
        case 0x14: r[rd] = sbfx(r[rn], lsb, (hw2 & 0x1F) + 1); return StepResult::Ok;            // SBFX
        case 0x1C: r[rd] = ubfx(r[rn], lsb, (hw2 & 0x1F) + 1); return StepResult::Ok;            // UBFX
        case 0x16: r[rd] = bfi(r[rd], rn == 15 ? 0 : r[rn], lsb, hw2 & 0x1F); return StepResult::Ok;  // BFI, BFC
        default: return StepResult::Undefined;  // SSAT, USAT...
    }
}

Interpreter::StepResult Interpreter::thumb32_branch_misc(u16 hw1, u16 hw2, u32 pc) {
    const u32 kind = (bit(hw2, 14) << 1) | bit(hw2, 12);
    const u32 s = bit(hw1, 10), j1 = bit(hw2, 13), j2 = bit(hw2, 11);

    if (kind == 0) {
        if (bits(hw1, 9, 7) != 7) {  // B<cond>.W
            const u32 imm = sign_extend((s << 20) | (j2 << 19) | (j1 << 18) | (bits(hw1, 5, 0) << 12) |
                                        (bits(hw2, 10, 0) << 1), 21);
            if (condition_passed(bits(hw1, 9, 6))) branch_to(pc + 4 + imm);
            return StepResult::Ok;
        }
        switch (bits(hw1, 10, 4)) {
            case 0x38: case 0x39:  // MSR APSR_nzcvq, Rn
                if (bit(hw2, 11)) state_.cpsr = (state_.cpsr & 0x07FFFFFF) | (state_.r[hw1 & 0xF] & 0xF8000000);
                return StepResult::Ok;
            case 0x3A: return StepResult::Ok;  // NOP.W, YIELD, WFE, WFI, SEV
            case 0x3B: return StepResult::Ok;  // CLREX, DSB, DMB, ISB
            case 0x3E: case 0x3F:  // MRS Rd, APSR
                state_.r[bits(hw2, 11, 8)] = state_.cpsr & 0xF80F0000;
                return StepResult::Ok;
            default: return StepResult::Undefined;
        }
    }

    const u32 i1 = !(j1 ^ s), i2 = !(j2 ^ s);
    const u32 high = (s << 24) | (i1 << 23) | (i2 << 22) | (bits(hw1, 9, 0) << 12);
    if (kind == 1) {  // B.W
        branch_to(pc + 4 + sign_extend(high | (bits(hw2, 10, 0) << 1), 25));
    } else if (kind == 3) {  // BL
        state_.r[14] = (pc + 4) | 1;
        branch_to(pc + 4 + sign_extend(high | (bits(hw2, 10, 0) << 1), 25));
    } else {  // BLX (immédiat) : passe en ARM
        if (bit(hw2, 0)) return StepResult::Undefined;
        state_.r[14] = (pc + 4) | 1;
        bx_to(align4(pc + 4) + sign_extend(high | (bits(hw2, 10, 1) << 2), 25));
    }
    return StepResult::Ok;
}

// LDR, LDRB, LDRH, LDRSB, LDRSH, STR, STRB, STRH (formes 32 bits).
Interpreter::StepResult Interpreter::thumb32_load_store_single(u16 hw1, u16 hw2) {
    const u32 size = bits(hw1, 6, 5), rn = hw1 & 0xF, rt = bits(hw2, 15, 12);
    const bool sign = bit(hw1, 8), load = bit(hw1, 4);
    if (size == 3 || (!load && sign) || (sign && size == 2)) return StepResult::Undefined;

    u32 addr, wback_addr = 0;
    bool writeback = false;
    if (rn == 15) {  // littéral
        if (!load) return StepResult::Undefined;
        const u32 base = align4(state_.r[15] + 4), imm = hw2 & 0xFFF;
        addr = bit(hw1, 7) ? base + imm : base - imm;
    } else if (bit(hw1, 7)) {  // immédiat 12 bits
        addr = state_.r[rn] + (hw2 & 0xFFF);
    } else if (bit(hw2, 11)) {  // immédiat 8 bits, pré/post-indexé
        const bool pre = bit(hw2, 10), up = bit(hw2, 9);
        writeback = bit(hw2, 8);
        if (!pre && !writeback) return StepResult::Undefined;
        wback_addr = up ? state_.r[rn] + (hw2 & 0xFF) : state_.r[rn] - (hw2 & 0xFF);
        addr = pre ? wback_addr : state_.r[rn];
    } else if (bits(hw2, 11, 6) == 0) {  // registre décalé
        addr = state_.r[rn] + (state_.r[hw2 & 0xF] << bits(hw2, 5, 4));
    } else {
        return StepResult::Undefined;
    }

    if (load && rt == 15 && size != 2) return StepResult::Ok;  // PLD, PLI : simples indications

    if (load) {
        u32 v;
        switch (size) {
            case 0: v = memory_.read<u8>(addr); if (sign) v = sign_extend(v, 8); break;
            case 1: v = memory_.read<u16>(addr); if (sign) v = sign_extend(v, 16); break;
            default: v = memory_.read<u32>(addr); break;
        }
        if (writeback) state_.r[rn] = wback_addr;
        if (rt == 15) bx_to(v); else state_.r[rt] = v;
    } else {
        const u32 v = state_.r[rt];
        switch (size) {
            case 0: memory_.write<u8>(addr, static_cast<u8>(v)); break;
            case 1: memory_.write<u16>(addr, static_cast<u16>(v)); break;
            default: memory_.write<u32>(addr, v); break;
        }
        if (writeback) state_.r[rn] = wback_addr;
    }
    return StepResult::Ok;
}

// LDRD/STRD, accès exclusifs (LDREX...) et tables de sauts (TBB, TBH).
Interpreter::StepResult Interpreter::thumb32_dual_exclusive_table(u16 hw1, u16 hw2, u32 pc) {
    const bool pre = bit(hw1, 8), up = bit(hw1, 7), writeback = bit(hw1, 5), load = bit(hw1, 4);
    const u32 rn = hw1 & 0xF, rt = bits(hw2, 15, 12), rt2 = bits(hw2, 11, 8);
    u32* r = state_.r;

    if (!pre && !writeback) {
        if (!up) {  // LDREX / STREX (un seul cœur émulé : STREX réussit toujours)
            const u32 addr = r[rn] + (hw2 & 0xFF) * 4;
            if (load) {
                r[rt] = memory_.read<u32>(addr);
            } else {
                memory_.write<u32>(addr, r[rt]);
                r[rt2] = 0;
            }
            return StepResult::Ok;
        }
        const u32 addr = r[rn], rd = hw2 & 0xF;
        switch ((bits(hw2, 7, 4) << 1) | load) {
            case 0x01: {  // TBB
                const u32 base = rn == 15 ? pc + 4 : r[rn];
                branch_to(pc + 4 + 2 * memory_.read<u8>(base + r[hw2 & 0xF]));
                return StepResult::Ok;
            }
            case 0x03: {  // TBH
                const u32 base = rn == 15 ? pc + 4 : r[rn];
                branch_to(pc + 4 + 2 * memory_.read<u16>(base + 2 * r[hw2 & 0xF]));
                return StepResult::Ok;
            }
            case 0x09: r[rt] = memory_.read<u8>(addr); return StepResult::Ok;   // LDREXB
            case 0x0B: r[rt] = memory_.read<u16>(addr); return StepResult::Ok;  // LDREXH
            case 0x0F: r[rt] = memory_.read<u32>(addr); r[rt2] = memory_.read<u32>(addr + 4); return StepResult::Ok;  // LDREXD
            case 0x08: memory_.write<u8>(addr, static_cast<u8>(r[rt])); r[rd] = 0; return StepResult::Ok;    // STREXB
            case 0x0A: memory_.write<u16>(addr, static_cast<u16>(r[rt])); r[rd] = 0; return StepResult::Ok;  // STREXH
            case 0x0E:                                                                                       // STREXD
                memory_.write<u32>(addr, r[rt]);
                memory_.write<u32>(addr + 4, r[rt2]);
                r[rd] = 0;
                return StepResult::Ok;
            default: return StepResult::Undefined;
        }
    }

    // LDRD / STRD (immédiat)
    const u32 base = rn == 15 ? align4(pc + 4) : r[rn];
    const u32 imm = (hw2 & 0xFF) * 4;
    const u32 offset_addr = up ? base + imm : base - imm;
    const u32 addr = pre ? offset_addr : base;
    if (load) {
        const u32 v1 = memory_.read<u32>(addr), v2 = memory_.read<u32>(addr + 4);
        if (writeback) r[rn] = offset_addr;
        r[rt] = v1;
        r[rt2] = v2;
    } else {
        memory_.write<u32>(addr, r[rt]);
        memory_.write<u32>(addr + 4, r[rt2]);
        if (writeback) r[rn] = offset_addr;
    }
    return StepResult::Ok;
}

// Décalages par registre, extensions, REV/RBIT/CLZ.
Interpreter::StepResult Interpreter::thumb32_data_processing_register(u16 hw1, u16 hw2) {
    if (bits(hw2, 15, 12) != 0xF) return StepResult::Undefined;
    const u32 op1 = bits(hw1, 7, 4), op2 = bits(hw2, 7, 4);
    const u32 rn = hw1 & 0xF, rd = bits(hw2, 11, 8), rm = hw2 & 0xF;
    u32* r = state_.r;

    if (op1 < 8 && op2 == 0) {  // LSL, LSR, ASR, ROR (registre)
        const Shifted s = shift_c(r[rn], bits(op1, 2, 1), r[rm] & 0xFF, carry());
        r[rd] = alu(AluOp::Mov, 0, s.value, s.carry, op1 & 1);
        return StepResult::Ok;
    }
    if (op1 < 8 && (op2 & 8)) {  // SXTH, UXTH, SXTB, UXTB (+ variantes avec addition)
        u32 out;
        if (!extend(op1 & 7, r[rm], bits(hw2, 5, 4) * 8, out)) return StepResult::Undefined;
        r[rd] = rn == 15 ? out : out + r[rn];
        return StepResult::Ok;
    }
    if ((op1 & 0xC) == 8 && (op2 & 0xC) == 8) {
        switch (((op1 & 3) << 2) | (op2 & 3)) {
            case 0x4: r[rd] = byte_swap(r[rm]); return StepResult::Ok;           // REV
            case 0x5: r[rd] = rev16(r[rm]); return StepResult::Ok;               // REV16
            case 0x6: r[rd] = bit_reverse(r[rm]); return StepResult::Ok;         // RBIT
            case 0x7: r[rd] = revsh(r[rm]); return StepResult::Ok;               // REVSH
            case 0xC: r[rd] = count_leading_zeros(r[rm]); return StepResult::Ok; // CLZ
            default: return StepResult::Undefined;
        }
    }
    return StepResult::Undefined;  // additions parallèles (UADD8...)
}

// MUL, MLA, MLS, et multiplications longues (SMULL, UMULL, SMLAL, UMLAL).
Interpreter::StepResult Interpreter::thumb32_multiply(u16 hw1, u16 hw2) {
    const u32 op1 = bits(hw1, 6, 4);
    const u32 rn = state_.r[hw1 & 0xF], rm = state_.r[hw2 & 0xF];
    const u32 ra_lo = bits(hw2, 15, 12), rd_hi = bits(hw2, 11, 8);
    u32* r = state_.r;

    if (!bit(hw1, 7)) {
        const u32 rn_idx = hw1 & 0xF, rm_idx = hw2 & 0xF;
        if (op1 == 1 && bits(hw2, 7, 6) == 0) {  // SMLAxy / SMULxy
            halfword_multiply(ra_lo == 15 ? 3 : 0, bit(hw2, 5), bit(hw2, 4), rd_hi, ra_lo, rn_idx, rm_idx, ra_lo != 15);
            return StepResult::Ok;
        }
        if (op1 == 3 && bits(hw2, 7, 5) == 0) {  // SMLAWy / SMULWy
            halfword_multiply(1, false, bit(hw2, 4), rd_hi, ra_lo, rn_idx, rm_idx, ra_lo != 15);
            return StepResult::Ok;
        }
        if ((op1 == 5 || op1 == 6) && bits(hw2, 7, 5) == 0) {  // SMMUL / SMMLA (5), SMMLS (6)
            if (op1 == 6 && ra_lo == 15) return StepResult::Undefined;
            most_significant_multiply(rd_hi, ra_lo, rn_idx, rm_idx, op1 == 6, bit(hw2, 4));
            return StepResult::Ok;
        }
        if (op1 != 0) return StepResult::Undefined;  // SMUAD, SMLSD...
        switch (bits(hw2, 7, 4)) {
            case 0: r[rd_hi] = rn * rm + (ra_lo == 15 ? 0 : r[ra_lo]); return StepResult::Ok;  // MUL, MLA
            case 1: r[rd_hi] = r[ra_lo] - rn * rm; return StepResult::Ok;                        // MLS
            default: return StepResult::Undefined;
        }
    }

    if (op1 == 4 && bits(hw2, 7, 6) == 2) {  // SMLALxy
        halfword_multiply(2, bit(hw2, 5), bit(hw2, 4), rd_hi, ra_lo, hw1 & 0xF, hw2 & 0xF, true);
        return StepResult::Ok;
    }
    const u64 acc = (static_cast<u64>(r[rd_hi]) << 32) | r[ra_lo];
    const s64 sprod = static_cast<s64>(static_cast<s32>(rn)) * static_cast<s32>(rm);
    const u64 uprod = static_cast<u64>(rn) * rm;
    u64 result;
    switch ((op1 << 4) | bits(hw2, 7, 4)) {
        case 0x00: result = static_cast<u64>(sprod); break;        // SMULL
        case 0x20: result = uprod; break;                          // UMULL
        case 0x40: result = static_cast<u64>(sprod) + acc; break;  // SMLAL
        case 0x60: result = uprod + acc; break;                    // UMLAL
        case 0x66: result = uprod + r[ra_lo] + r[rd_hi]; break;    // UMAAL
        default: return StepResult::Undefined;  // SDIV/UDIV : absents du Cortex-A9
    }
    r[ra_lo] = static_cast<u32>(result);
    r[rd_hi] = static_cast<u32>(result >> 32);
    return StepResult::Ok;
}

}  // namespace vita
