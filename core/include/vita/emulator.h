#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "vita/cpu/cpu.h"
#include "vita/gpu/renderer.h"
#include "vita/hle/kernel.h"
#include "vita/loader/elf_loader.h"
#include "vita/memory/memory.h"

namespace vita {

enum class EmuState { Idle, Loaded, Running, Paused, Stopped, Error };

// Point d'entrée du cœur : relie mémoire, CPU, HLE et rendu.
// Une seule instance par session de jeu. Le CPU tourne sur son propre thread.
class Emulator {
public:
    // Disposition mémoire simplifiée (les adresses réelles viendront plus tard).
    static constexpr GuestAddr kStackTop = 0x80000000;
    static constexpr GuestAddr kTrapAddr = 0x7FF00000;  // adresse de retour de module_start
    static constexpr GuestAddr kTlsAddr = 0x7FF10000;   // zone TLS du thread principal
    static constexpr u32 kTlsSize = 0x10000;
    static constexpr u32 kStackSize = 1 * 1024 * 1024;
    static constexpr u64 kInstructionsPerSlice = 100000;
    // FPSCR de départ d'un thread Vita : arrondi au plus proche, mise à zéro
    // des dénormaux (FZ) et NaN par défaut (DN). Valeur reprise de Vita3K.
    static constexpr u32 kInitialFpscr = (1u << 25) | (1u << 24);

    Emulator();
    ~Emulator();

    bool init(std::unique_ptr<Renderer> renderer = nullptr);
    LoadResult load(const std::string& path);
    LoadResult load(const std::vector<u8>& data);
    const LoadedModule& module() const { return module_; }

    void start();   // lance le thread d'émulation
    void pause();
    void resume();
    void stop();

    // Exécute une tranche d'instructions sur le thread appelant (tests, débogage).
    CpuExit run_slice(u64 max_instructions = kInstructionsPerSlice);

    bool attach_surface(void* native_window, u32 width, u32 height);

    EmuState state() const { return state_; }
    std::string status() const;
    CpuState& cpu_state() { return cpu_->state(); }
    Memory& memory() { return memory_; }
    Kernel& kernel() { return kernel_; }

private:
    LoadResult finish_load(LoadResult result);
    void set_status(EmuState state, std::string message);
    void thread_main();

    Memory memory_;
    std::unique_ptr<CpuBackend> cpu_;
    Kernel kernel_;
    std::unique_ptr<Renderer> renderer_;
    LoadedModule module_;

    std::thread thread_;
    std::atomic<EmuState> state_{EmuState::Idle};
    std::atomic<bool> quit_{false};
    mutable std::mutex status_mutex_;
    std::string status_ = "Inactif";
};

}  // namespace vita
