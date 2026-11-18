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

LoadResult Emulator::load(const std::string& path) { return finish_load(load_elf_file(memory_, path)); }

LoadResult Emulator::load(const std::vector<u8>& elf_data) { return finish_load(load_elf(memory_, elf_data)); }

LoadResult Emulator::finish_load(LoadResult result) {
    if (!result.ok()) {
        set_status(EmuState::Error, result.message);
        return result;
    }
    if (!memory_.map(kStackTop - kStackSize, kStackSize)) {
        result.status = LoadStatus::MapFailed;
        result.message = "Impossible d'allouer la pile";
        set_status(EmuState::Error, result.message);
        return result;
    }

    CpuState& s = cpu_->state();
    s = CpuState{};
    s.sp() = kStackTop;
    s.fpscr = kInitialFpscr;
    s.pc() = result.module.entry & ~1u;
    if (result.module.entry & 1) s.cpsr |= CpuState::kFlagT;  // point d'entrée Thumb

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
