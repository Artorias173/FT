#pragma once

#include <functional>
#include <string>

#include "vita/common/types.h"

namespace vita {

class Memory;

// État architectural d'un cœur ARMv7-A (Cortex-A9 sur PS Vita).
struct CpuState {
    u32 r[16]{};  // r13 = SP, r14 = LR, r15 = PC
    u32 cpsr = 0x10;  // mode utilisateur, ARM, drapeaux à zéro
    u32 fpscr = 0;
    u64 d[32]{};      // registres VFP/NEON (D0-D31)
    u32 tpidruro = 0; // registre TLS (TPIDRURO), utilisé par le système de la Vita
    u8 itstate = 0;   // état du bloc IT en Thumb-2 (condition + masque)

    static constexpr u32 kFlagN = 1u << 31;
    static constexpr u32 kFlagZ = 1u << 30;
    static constexpr u32 kFlagC = 1u << 29;
    static constexpr u32 kFlagV = 1u << 28;
    static constexpr u32 kFlagT = 1u << 5;  // mode Thumb

    u32& sp() { return r[13]; }
    u32& lr() { return r[14]; }
    u32& pc() { return r[15]; }
    bool thumb() const { return cpsr & kFlagT; }
};

enum class CpuExit {
    BudgetExhausted,  // nombre d'instructions demandé atteint
    Halted,           // arrêt demandé (par le HLE par exemple)
    MemoryFault,      // accès à une adresse non mappée
    Undefined,        // instruction non prise en charge
};

// Interface commune aux back-ends CPU. Un interpréteur sert de référence ;
// un recompilateur dynamique (par exemple dynarmic, ARMv7 vers ARM64) pourra
// ensuite prendre sa place sans toucher au reste de l'émulateur.
class CpuBackend {
public:
    // Appelé sur une instruction SVC, avec son numéro immédiat.
    using SvcHandler = std::function<void(u32 imm, CpuState& state)>;

    virtual ~CpuBackend() = default;

    virtual CpuExit run(u64 max_instructions) = 0;
    virtual void halt() = 0;
    virtual CpuState& state() = 0;
    virtual const std::string& last_error() const = 0;

    void set_svc_handler(SvcHandler handler) { svc_handler_ = std::move(handler); }

protected:
    SvcHandler svc_handler_;
};

}  // namespace vita
