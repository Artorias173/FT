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
    kernel_.set_reschedule_callback([this] { cpu_->halt(); });
    kernel_.set_exit_callback([this](s32 code) {
        cpu_->halt();
        set_status(EmuState::Stopped, "Programme terminé (code " + std::to_string(code) + ")");
    });

    set_status(EmuState::Idle, "Prêt");
    return true;
}

void Emulator::mount(const std::string& device, const std::string& host_dir) {
    mounts_[device] = host_dir;
    kernel_.mount(device, host_dir);
}

LoadResult Emulator::load(const std::string& path) {
    if (!mounts_.count("app0:")) {
        const size_t slash = path.find_last_of('/');
        mount("app0:", slash == std::string::npos ? "." : path.substr(0, slash));
    }
    return finish_load(load_executable_file(memory_, path));
}

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
    memory_.write<u32>(kTrapAddr + 8, 0xEF000000 | Kernel::kSvcThreadReturn);  // svc #kSvcThreadReturn
    memory_.write<u32>(kTrapAddr + 12, 0xEAFFFFFE);                           // b .
    kernel_.set_thread_return_trap(kTrapAddr + 8);
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
    kernel_.create_main_thread(s, kStackTop - kStackSize, kStackSize, kTlsAddr);
    loaded_thread_ = kernel_.current();
    LOG_INFO("%u stub(s) redirigé(s)", patched);
    set_status(EmuState::Loaded, result.message);
    return result;
}

CpuExit Emulator::run_slice(u64 max_instructions) {
    constexpr u64 kQuantum = 20000;  // instructions avant de passer au thread suivant
    u64 budget = max_instructions;
    while (true) {
        if (kernel_.exit_requested()) return CpuExit::Halted;

        // Synchronisation verticale : on présente l'image courante.
        const u64 vblank = kernel_.now_us() / Kernel::kVblankPeriodUs;
        if (vblank != last_vblank_) {
            last_vblank_ = vblank;
            present_frame();
        }
        if (budget == 0) return CpuExit::BudgetExhausted;

        Thread* t = kernel_.schedule();
        if (!t) {
            if (kernel_.all_threads_finished()) {
                set_status(EmuState::Stopped, "Tous les threads sont terminés");
                return CpuExit::Halted;
            }
            const u64 wake = kernel_.next_wake_time();
            if (wake == UINT64_MAX) {
                set_status(EmuState::Error, "Interblocage : tous les threads attendent");
                return CpuExit::Halted;
            }
            // Personne n'est prêt : on attend la prochaine échéance (par petites étapes).
            const u64 now = kernel_.now_us();
            if (wake > now) std::this_thread::sleep_for(std::chrono::microseconds(std::min<u64>(wake - now, 2000)));
            if (quit_) return CpuExit::BudgetExhausted;
            continue;
        }

        if (t != loaded_thread_) {
            if (loaded_thread_) loaded_thread_->state = cpu_->state();
            cpu_->state() = t->state;
            loaded_thread_ = t;
        }
        const CpuExit exit = cpu_->run(std::min(budget, kQuantum));
        budget -= std::min(budget, cpu_->executed());
        t->state = cpu_->state();

        if (exit == CpuExit::Undefined || exit == CpuExit::MemoryFault) {
            set_status(EmuState::Error, cpu_->last_error());
            return exit;
        }
        // Halted : le thread a cédé la main (attente, fin) ou le processus se termine.
    }
}

void Emulator::present_frame() {
    if (!renderer_) return;
    const FrameBuffer fb = kernel_.framebuffer();
    const u8* pixels = fb.base ? memory_.host_ptr(fb.base, fb.pitch * fb.height * 4) : nullptr;
    if (!pixels) {
        renderer_->present(nullptr);
        return;
    }
    const Frame frame{pixels, fb.width, fb.height, fb.pitch, fb.pixel_format};
    renderer_->present(&frame);
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
    }
}

}  // namespace vita
