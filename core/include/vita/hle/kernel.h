#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vita/common/types.h"
#include "vita/cpu/cpu.h"

namespace vita {

class Memory;

// Codes d'erreur renvoyés aux programmes. Les erreurs de fichiers suivent le
// schéma 0x80010000 | errno de la Vita ; pour les autres cas, dont la valeur
// exacte n'est pas encore relevée, on renvoie l'erreur noyau générique.
namespace sce_error {
constexpr u32 kGeneric = 0x80020001;
constexpr u32 kNoEntry = 0x80010002;      // ENOENT
constexpr u32 kBadFile = 0x80010009;      // EBADF
constexpr u32 kNoMemory = 0x8001000C;     // ENOMEM
constexpr u32 kExists = 0x80010011;       // EEXIST
constexpr u32 kNoDevice = 0x80010013;     // ENODEV
constexpr u32 kInvalid = 0x80010016;      // EINVAL
constexpr u32 kTooManyFiles = 0x80010018; // EMFILE
}  // namespace sce_error

// Thread émulé. Un seul cœur est simulé : les threads s'exécutent à tour de rôle.
struct Thread {
    enum class Status { Dormant, Ready, Waiting, Dead };
    enum class Wait { None, Delay, Sema, LwMutex, ThreadEnd, Vblank };

    u32 uid = 0;
    std::string name;
    CpuState state;
    Status status = Status::Dormant;
    Wait wait = Wait::None;
    u64 wake_time = 0;      // pour Delay et Vblank (microsecondes)
    u32 wait_object = 0;    // UID ou adresse attendue
    s32 wait_count = 0;     // ressources demandées
    GuestAddr wait_out = 0; // pointeur où écrire un résultat au réveil
    u64 wait_order = 0;     // ordre d'arrivée (files d'attente FIFO)
    GuestAddr entry = 0;
    GuestAddr stack_base = 0;
    u32 stack_size = 0;
    GuestAddr tls = 0;
    s32 exit_status = 0;
    s32 priority = 0;
    bool delete_pending = false;
};

// Tampon d'image déclaré par sceDisplaySetFrameBuf.
struct FrameBuffer {
    GuestAddr base = 0;
    u32 pitch = 0;          // en pixels
    u32 pixel_format = 0;   // 0 : A8B8G8R8
    u32 width = 0;
    u32 height = 0;
};

// État des commandes (boutons SCE_CTRL_*, sticks de 0 à 255, 128 au repos).
struct InputState {
    u32 buttons = 0;
    u8 lx = 128, ly = 128, rx = 128, ry = 128;
};

// Émulation haut niveau (HLE) des modules système de la Vita.
//
// Sur la Vita, un programme importe les fonctions système par leur NID (un
// identifiant 32 bits). Plutôt que d'émuler le vrai firmware, on remplace
// chaque appel importé (stub) par `svc #index ; bx lr`, et l'index renvoie
// vers une implémentation C++ de la fonction.
class Kernel {
public:
    using Handler = std::function<void(CpuState& state)>;

    // SVC réservés.
    static constexpr u32 kSvcModuleReturn = 0xFFFFFF;  // retour de module_start : fin du processus
    static constexpr u32 kSvcThreadReturn = 0xFFFFFE;  // retour d'un thread : sceKernelExitThread(r0)

    // Disposition mémoire des allocations du noyau.
    static constexpr GuestAddr kThreadStackArea = 0x60000000;
    static constexpr GuestAddr kHeapArea = 0x90000000;
    static constexpr u32 kTlsBlockSize = 0x1000;
    static constexpr u64 kVblankPeriodUs = 16667;  // 60 Hz

    explicit Kernel(Memory& memory);
    ~Kernel();

    void register_function(u32 nid, std::string name, Handler handler);
    u32 bind_svc(u32 nid);
    bool patch_import(GuestAddr stub_addr, u32 nid, const std::string& library = {});
    void handle_svc(u32 imm, CpuState& state);
    void register_builtin_modules();

    void set_exit_callback(std::function<void(s32 code)> cb) { on_exit_ = std::move(cb); }
    // Appelé quand le thread courant cède la main : l'émulateur doit arrêter le CPU.
    void set_reschedule_callback(std::function<void()> cb) { on_reschedule_ = std::move(cb); }
    void request_exit(s32 code);
    bool exit_requested() const { return exit_requested_; }

    u32 unimplemented_calls() const { return unimplemented_calls_; }

    // --- Threads -------------------------------------------------------------
    u32 create_main_thread(const CpuState& initial, GuestAddr stack_base, u32 stack_size, GuestAddr tls);
    Thread* thread(u32 uid);
    Thread* current() { return current_; }
    void set_current(Thread* t) { current_ = t; }
    // Adresse du piège exécuté quand la fonction d'un thread rend la main.
    void set_thread_return_trap(GuestAddr addr) { thread_return_trap_ = addr; }
    // Réveille les attentes échues et choisit le prochain thread prêt (tourniquet).
    Thread* schedule();
    u64 next_wake_time() const;   // prochaine échéance (UINT64_MAX si aucune)
    bool all_threads_finished() const;
    size_t thread_count() const { return threads_.size(); }

    // Temps du processus, en microsecondes depuis la création du noyau.
    u64 now_us() const;

    // --- Affichage, commandes, sorties ---------------------------------------
    FrameBuffer framebuffer() const;
    void set_input(const InputState& input);
    std::string stdout_text() const;
    // Associe un périphérique Vita (« ux0: », « app0: »...) à un dossier de l'hôte.
    void mount(const std::string& device, const std::string& host_dir);

private:
    struct Function {
        u32 nid;
        std::string name;
        Handler handler;
    };
    struct MemBlock {
        std::string name;
        GuestAddr base;
        u32 size;
    };
    struct Semaphore {
        std::string name;
        s32 count;
        s32 max;
    };
    struct LwMutex {
        u32 owner = 0;  // UID du thread propriétaire (0 : libre)
        s32 count = 0;
    };
    struct File {
        int fd;  // descripteur de l'hôte
    };

    // Modules (fichiers séparés).
    void register_sysmem();
    void register_threads();
    void register_sync();
    void register_io();
    void register_display_ctrl();
    void register_processmgr();

    u32 new_uid();
    void block(Thread::Wait kind, u32 object, s32 count, u64 wake_time, GuestAddr out = 0);
    void yield();
    void finish_thread(Thread& t, s32 status);
    void wake_sema_waiters(u32 uid);
    GuestAddr alloc_area(u32 size, GuestAddr start, u32 align);
    void close_all_files();
    std::string resolve_path(const std::string& vita_path, u32& error) const;
    void write_console(const char* data, size_t size);

    Memory& memory_;
    std::unordered_map<u32, Function> functions_;
    std::vector<u32> import_table_;  // index SVC -> NID
    std::unordered_map<u32, std::string> libraries_;  // NID -> bibliothèque (diagnostic)
    std::function<void(s32)> on_exit_;
    std::function<void()> on_reschedule_;
    bool exit_requested_ = false;
    u32 unimplemented_calls_ = 0;

    u32 next_uid_ = 0x40010003;
    u64 wait_counter_ = 0;
    u64 start_time_;
    std::map<u32, std::unique_ptr<Thread>> threads_;
    Thread* current_ = nullptr;
    u32 last_scheduled_ = 0;
    GuestAddr thread_return_trap_ = 0;

    std::map<u32, MemBlock> memblocks_;
    std::map<u32, Semaphore> semaphores_;
    std::map<GuestAddr, LwMutex> lwmutexes_;  // indexés par l'adresse de leur zone de travail
    std::map<u32, std::unique_ptr<File>> files_;
    std::map<std::string, std::string> mounts_;

    mutable std::mutex shared_mutex_;  // état lu par l'interface (image, commandes, console)
    FrameBuffer framebuffer_;
    InputState input_;
    u32 ctrl_sampling_mode_ = 0;
    std::string console_;
};

}  // namespace vita
