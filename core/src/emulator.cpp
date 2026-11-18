#include "vita/emulator.h"

#include <chrono>

#include "vita/common/log.h"
#include "vita/cpu/interpreter.h"

namespace vita {

Emulator::Emulator() : kernel_(memory_) {}

Emulator::~Emulator() {
    stop();
    if (renderer_) renderer_->shutdown();
}

bool Emulator::init(std::unique_ptr<Renderer> renderer) {
    if (!memory_.init()) {
        set_status(EmuState::Error, "Échec de l'initialisation mémoire");
        return false;
    }
    renderer_ = renderer ? std::move(renderer) : create_renderer();

    cpu_ = std::make_unique<Interpreter>(memory_);
    cpu_->set_svc_handler([this](u32 imm, CpuState& s) { kernel_.handle_svc(imm, s); });

    kernel_.register_builtin_modules();
    kernel_.set_exit_callback([this](s32 code) {
        cpu_->halt();
        set_status(EmuState::Stopped, "Programme terminé (code " + std::to_string(code) + ")");
    });

    set_status(EmuState::Idle, "Prêt");
    return true;
}

LoadResult Emulator::load(const std::string& path) { return finish_load(load_executable_file(memory_, path)); }

LoadResult Emulator::load(const std::vector<u8>& data) { return finish_load(load_executable(memory_, data)); }

LoadResult Emulator::finish_load(LoadResult result) {
    if (!result.ok()) {
        set_status(EmuState::Error, result.message);
        return result;
    }
    auto fail = [&](const char* message) {
        result.status = LoadStatus::MapFailed;
        result.message = message;
        set_status(EmuState::Error, result.message);
        return result;
    };
    const LoadedModule& mod = result.module;

    // Appels système : chaque stub importé est redirigé vers le HLE.
    u32 patched = 0;
    for (const FunctionImport& imp : mod.imports) {
        if (kernel_.patch_import(imp.stub, imp.nid, imp.library)) ++patched;
        else LOG_WARN("Stub d'import illisible à 0x%08x (NID 0x%08x)", imp.stub, imp.nid);
    }
    if (mod.variable_imports) LOG_WARN("%u import(s) de variables non résolu(s)", mod.variable_imports);

    // Pile, piège de retour et TLS du thread principal.
    if (!memory_.map(kStackTop - kStackSize, kStackSize)) return fail("Impossible d'allouer la pile");
    if (!memory_.map(kTrapAddr, Memory::kPageSize)) return fail("Impossible d'allouer le piège de retour");
    memory_.write<u32>(kTrapAddr, 0xEF000000 | Kernel::kSvcModuleReturn);  // svc #kSvcModuleReturn
    memory_.write<u32>(kTrapAddr + 4, 0xEAFFFFFE);                        // b .
    if (!memory_.map(kTlsAddr, kTlsSize)) return fail("Impossible d'allouer la TLS");
    if (mod.tls_start && mod.tls_filesz && mod.tls_filesz <= kTlsSize) {
        // Image initiale des variables TLS. La disposition exacte attendue par le
        // système de la Vita reste à confirmer.
        for (u32 i = 0; i < mod.tls_filesz; ++i) memory_.write<u8>(kTlsAddr + i, memory_.read<u8>(mod.tls_start + i));
    }

    CpuState& s = cpu_->state();
    s = CpuState{};
    s.sp() = kStackTop;
    s.lr() = kTrapAddr;
    s.fpscr = kInitialFpscr;
    s.tpidruro = kTlsAddr;
    s.pc() = mod.entry & ~1u;
    if (mod.entry & 1) s.cpsr |= CpuState::kFlagT;  // point d'entrée Thumb
    if (mod.entry_is_module_start) {
        s.r[0] = 0;  // args : taille des arguments
        s.r[1] = 0;  // argp : aucun argument
    }

    module_ = mod;
    LOG_INFO("%u stub(s) redirigé(s)", patched);
    set_status(EmuState::Loaded, result.message);
    return result;
}

CpuExit Emulator::run_slice(u64 max_instructions) {
    const CpuExit exit = cpu_->run(max_instructions);
    if (exit == CpuExit::Undefined || exit == CpuExit::MemoryFault)
        set_status(EmuState::Error, cpu_->last_error());
    return exit;
}

void Emulator::start() {
    if (state_ != EmuState::Loaded) return;
    quit_ = false;
    set_status(EmuState::Running, "En cours d'exécution");
    thread_ = std::thread(&Emulator::thread_main, this);
}

void Emulator::pause() {
    if (state_ == EmuState::Running) set_status(EmuState::Paused, "En pause");
}

void Emulator::resume() {
    if (state_ == EmuState::Paused) set_status(EmuState::Running, "En cours d'exécution");
}

void Emulator::stop() {
    quit_ = true;
    if (cpu_) cpu_->halt();
    if (thread_.joinable()) thread_.join();
}

bool Emulator::attach_surface(void* native_window, u32 width, u32 height) {
    return renderer_ && renderer_->init(native_window, width, height);
}

std::string Emulator::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void Emulator::set_status(EmuState state, std::string message) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = std::move(message);
    }
    state_ = state;
    LOG_INFO("État : %s", status().c_str());
}

void Emulator::thread_main() {
    using namespace std::chrono_literals;
    while (!quit_) {
        if (state_ == EmuState::Paused) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        if (state_ != EmuState::Running) break;

        const CpuExit exit = run_slice();
        if (exit != CpuExit::BudgetExhausted) break;
        // Tant que SceDisplay n'est pas émulé, on présente une image par tranche.
        renderer_->present();
    }
}

}  // namespace vita
