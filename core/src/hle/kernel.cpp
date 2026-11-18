#include "vita/hle/kernel.h"

#include "vita/common/log.h"
#include "vita/memory/memory.h"

namespace vita {

namespace {
constexpr u32 kSvcMaxImm = 0x00FFFFFF;
constexpr u32 kArmBxLr = 0xE12FFF1E;
constexpr u32 encode_svc(u32 imm) { return 0xEF000000 | (imm & kSvcMaxImm); }
}  // namespace

void Kernel::register_function(u32 nid, std::string name, Handler handler) {
    functions_[nid] = Function{nid, std::move(name), std::move(handler)};
}

u32 Kernel::bind_svc(u32 nid) {
    import_table_.push_back(nid);
    return static_cast<u32>(import_table_.size() - 1);
}

bool Kernel::patch_import(GuestAddr stub_addr, u32 nid) {
    if (!memory_.is_mapped(stub_addr, 8)) return false;
    const u32 index = bind_svc(nid);
    memory_.write<u32>(stub_addr, encode_svc(index));
    memory_.write<u32>(stub_addr + 4, kArmBxLr);
    return true;
}

void Kernel::handle_svc(u32 imm, CpuState& state) {
    if (imm >= import_table_.size()) {
        LOG_WARN("SVC #%u inconnu (retour à 0x%08x)", imm, state.r[15]);
        state.r[0] = 0x80020001;  // code d'erreur générique
        return;
    }
    const u32 nid = import_table_[imm];
    auto it = functions_.find(nid);
    if (it == functions_.end()) {
        LOG_WARN("Fonction non implémentée NID 0x%08x", nid);
        state.r[0] = 0;  // valeur neutre pour permettre au programme de continuer
        return;
    }
    it->second.handler(state);
}

void Kernel::register_builtin_modules() {
    // SceLibKernel
    register_function(0x7595D9AA, "sceKernelExitProcess", [this](CpuState& s) {
        LOG_INFO("sceKernelExitProcess(%d)", static_cast<s32>(s.r[0]));
        request_exit(static_cast<s32>(s.r[0]));
    });
    // Les autres modules (SceThreadmgr, SceIofilemgr, SceGxm, SceCtrl,
    // SceDisplay, SceAudio...) s'ajouteront ici, un fichier par module.
}

}  // namespace vita
