#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vita/common/types.h"
#include "vita/cpu/cpu.h"

namespace vita {

class Memory;

// Émulation haut niveau (HLE) des modules système de la Vita.
//
// Sur la Vita, un programme importe les fonctions système par leur NID (un
// identifiant 32 bits). Plutôt que d'émuler le vrai firmware, on remplace
// chaque appel importé (stub) par `svc #index ; bx lr`, et l'index renvoie
// vers une implémentation C++ de la fonction.
class Kernel {
public:
    using Handler = std::function<void(CpuState& state)>;

    explicit Kernel(Memory& memory) : memory_(memory) {}

    void register_function(u32 nid, std::string name, Handler handler);

    // Remplace l'appel importé (stub) situé à `stub_addr` par une redirection vers le NID.
    bool patch_import(GuestAddr stub_addr, u32 nid);

    // À brancher sur CpuBackend::set_svc_handler.
    void handle_svc(u32 imm, CpuState& state);

    void set_exit_callback(std::function<void(s32 code)> cb) { on_exit_ = std::move(cb); }
    void request_exit(s32 code) { if (on_exit_) on_exit_(code); }

    // Enregistre les quelques fonctions déjà émulées.
    void register_builtin_modules();

private:
    struct Function {
        u32 nid;
        std::string name;
        Handler handler;
    };

    Memory& memory_;
    std::unordered_map<u32, Function> functions_;
    std::vector<u32> import_table_;  // index SVC -> NID
    std::function<void(s32)> on_exit_;
};

}  // namespace vita
