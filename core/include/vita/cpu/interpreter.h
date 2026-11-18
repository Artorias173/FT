#pragma once

#include <atomic>

#include "vita/cpu/cpu.h"

namespace vita {

// Interpréteur ARM (jeu d'instructions A32) volontairement minimal.
//
// Pris en charge : traitement de données (les 16 opcodes, opérande
// immédiate ou registre décalé par un immédiat), MOVW/MOVT, LDR/STR/LDRB/STRB,
// B/BL/BX/BLX (registre), SVC et les codes de condition.
// Pas encore pris en charge : Thumb-2 (la majorité du code Vita), VFP/NEON,
// multiplications, LDM/STM et accès exclusifs.
class Interpreter final : public CpuBackend {
public:
    explicit Interpreter(Memory& memory) : memory_(memory) {}

    CpuExit run(u64 max_instructions) override;
    void halt() override { halt_requested_ = true; }
    CpuState& state() override { return state_; }
    const std::string& last_error() const override { return last_error_; }

private:
    enum class StepResult { Ok, Undefined, Fault };

    StepResult step();
    bool condition_passed(u32 cond) const;
    u32 reg(u32 n) const;  // lecture de registre (PC lu = adresse + 8)
    void write_reg(u32 n, u32 value, bool& pc_written);

    StepResult exec_data_processing(u32 instr, bool& pc_written);
    StepResult exec_load_store(u32 instr, bool& pc_written);

    Memory& memory_;
    CpuState state_;
    std::atomic<bool> halt_requested_{false};
    std::string last_error_;
};

}  // namespace vita
