#pragma once

#include <atomic>

#include "vita/cpu/cpu.h"

namespace vita {

// Interpréteur ARMv7-A : jeux d'instructions ARM (A32) et Thumb/Thumb-2 (T32).
//
// Il couvre les instructions entières qu'émet un compilateur pour un
// Cortex-A9 : traitement de données, multiplications, chargements et
// stockages (simples, doubles, multiples, exclusifs), branchements avec
// changement de mode, blocs IT, TBB/TBH, extraction de champs de bits,
// extensions, inversion d'octets, et lecture du registre TLS (TPIDRURO).
// Le calcul flottant VFPv3 (simple et double précision) est pris en charge.
// Pas encore pris en charge : NEON (calcul vectoriel), les conversions en
// virgule fixe ou en demi-précision, et les opérations SIMD sur 32 bits (UADD8...).
class Interpreter final : public CpuBackend {
public:
    explicit Interpreter(Memory& memory) : memory_(memory) {}

    CpuExit run(u64 max_instructions) override;
    void halt() override { halt_requested_ = true; }
    CpuState& state() override { return state_; }
    const std::string& last_error() const override { return last_error_; }

    enum class AluOp { And, Eor, Sub, Rsb, Add, Adc, Sbc, Rsc, Tst, Teq, Cmp, Cmn, Orr, Mov, Bic, Mvn, Orn };

private:
    enum class StepResult { Ok, Undefined };

    StepResult step();

    // Mode ARM (interpreter_arm.cpp)
    StepResult exec_arm(u32 instr, u32 pc);
    StepResult arm_data_processing(u32 instr);
    StepResult arm_misc(u32 instr);
    StepResult arm_multiply(u32 instr);
    StepResult arm_sync(u32 instr);
    StepResult arm_extra_load_store(u32 instr);
    StepResult arm_load_store(u32 instr);
    StepResult arm_media(u32 instr);

    // Mode Thumb (interpreter_thumb.cpp)
    StepResult exec_thumb16(u16 hw, u32 pc);
    StepResult exec_thumb32(u16 hw1, u16 hw2, u32 pc);
    StepResult thumb32_load_store_single(u16 hw1, u16 hw2);
    StepResult thumb32_dual_exclusive_table(u16 hw1, u16 hw2, u32 pc);
    StepResult thumb32_data_processing_register(u16 hw1, u16 hw2);
    StepResult thumb32_branch_misc(u16 hw1, u16 hw2, u32 pc);
    StepResult thumb32_plain_immediate(u16 hw1, u16 hw2, u32 pc);
    StepResult thumb32_multiply(u16 hw1, u16 hw2);
    StepResult thumb32_alu(u32 op, bool set_flags, u32 rn, u32 rd, u32 operand, bool shifter_carry);

    // Outils communs (interpreter.cpp)
    bool condition_passed(u32 cond) const;
    bool in_it_block() const { return state_.itstate & 0xF; }
    void advance_it();
    u32 reg(u32 n) const;             // lecture ; le PC vaut adresse + 8 (ARM) ou + 4 (Thumb)
    void write_reg(u32 n, u32 value); // écriture ; vers le PC = branchement selon le mode
    void branch_to(u32 addr);         // BranchWritePC : sans changement de mode
    void bx_to(u32 addr);             // BXWritePC : le bit 0 choisit ARM ou Thumb
    u32 alu(AluOp op, u32 a, u32 b, bool shifter_carry, bool set_flags);
    void set_nz(u32 result);
    bool carry() const { return state_.cpsr & CpuState::kFlagC; }
    StepResult load_store_multiple(u32 rn, u32 list, bool load, bool increment, bool before, bool writeback);
    StepResult coprocessor(u32 instr);  // instruction coprocesseur (32 bits, ARM ou Thumb)

    // Calcul flottant VFPv3 (interpreter_vfp.cpp). Encodage identique en ARM et en Thumb-2.
    StepResult vfp(u32 instr);
    StepResult vfp_load_store(u32 instr);
    StepResult vfp_transfer(u32 instr);
    StepResult vfp_data_processing(u32 instr);
    void svc(u32 imm, u32 return_addr);
    // Multiplications signées sur demi-mots, communes à ARM et Thumb-2.
    // kind : 0 SMLAxy, 1 SMLAWy/SMULWy, 2 SMLALxy, 3 SMULxy.
    void halfword_multiply(u32 kind, bool n_top, bool m_top, u32 rd_hi, u32 ra_lo, u32 rn, u32 rm, bool accumulate);

    Memory& memory_;
    CpuState state_;
    bool pc_written_ = false;
    std::atomic<bool> halt_requested_{false};
    std::string last_error_;
};

}  // namespace vita
