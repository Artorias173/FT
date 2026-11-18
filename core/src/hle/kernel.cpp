// Noyau HLE : répartition des appels système, objets et ordonnanceur.
#include "vita/hle/kernel.h"

#include <unistd.h>

#include <chrono>
#include <limits>

#include "vita/common/log.h"
#include "vita/memory/memory.h"

namespace vita {

namespace {
constexpr u32 kSvcMaxImm = 0x00FFFFFF;
constexpr u32 kArmBxLr = 0xE12FFF1E;
constexpr u32 encode_svc(u32 imm) { return 0xEF000000 | (imm & kSvcMaxImm); }

u64 steady_us() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
}  // namespace

Kernel::Kernel(Memory& memory) : memory_(memory), start_time_(steady_us()) {}

Kernel::~Kernel() { close_all_files(); }

u64 Kernel::now_us() const { return steady_us() - start_time_; }

void Kernel::register_function(u32 nid, std::string name, Handler handler) {
    functions_[nid] = Function{nid, std::move(name), std::move(handler)};
}

u32 Kernel::bind_svc(u32 nid) {
    import_table_.push_back(nid);
    return static_cast<u32>(import_table_.size() - 1);
}

bool Kernel::patch_import(GuestAddr stub_addr, u32 nid, const std::string& library) {
    if (!memory_.is_mapped(stub_addr, 8)) return false;
    if (!library.empty()) libraries_[nid] = library;
    const u32 index = bind_svc(nid);
    memory_.write<u32>(stub_addr, encode_svc(index));
    memory_.write<u32>(stub_addr + 4, kArmBxLr);
    return true;
}

void Kernel::request_exit(s32 code) {
    exit_requested_ = true;
    if (on_exit_) on_exit_(code);
}

void Kernel::handle_svc(u32 imm, CpuState& state) {
    if (imm == kSvcModuleReturn) {
        LOG_INFO("module_start a rendu la main (%d)", static_cast<s32>(state.r[0]));
        request_exit(static_cast<s32>(state.r[0]));
        return;
    }
    if (imm == kSvcThreadReturn) {
        if (current_) finish_thread(*current_, static_cast<s32>(state.r[0]));
        yield();
        return;
    }
    if (imm >= import_table_.size()) {
        LOG_WARN("SVC #%u inconnu (retour à 0x%08x)", imm, state.r[15]);
        state.r[0] = sce_error::kGeneric;
        return;
    }
    const u32 nid = import_table_[imm];
    auto it = functions_.find(nid);
    if (it == functions_.end()) {
        auto lib = libraries_.find(nid);
        LOG_WARN("Fonction non implémentée NID 0x%08x (%s)", nid, lib != libraries_.end() ? lib->second.c_str() : "?");
        ++unimplemented_calls_;
        state.r[0] = 0;  // valeur neutre pour permettre au programme de continuer
        return;
    }
    it->second.handler(state);
}

u32 Kernel::new_uid() {
    const u32 uid = next_uid_;
    next_uid_ += 2;
    return uid;
}

GuestAddr Kernel::alloc_area(u32 size, GuestAddr start, u32 align) {
    const u32 rounded = (size + Memory::kPageSize - 1) & ~(Memory::kPageSize - 1);
    const GuestAddr addr = memory_.find_free(rounded, start, align);
    if (!addr || !memory_.map(addr, rounded)) return 0;
    return addr;
}

// --- Threads et ordonnancement -------------------------------------------------

u32 Kernel::create_main_thread(const CpuState& initial, GuestAddr stack_base, u32 stack_size, GuestAddr tls) {
    auto t = std::make_unique<Thread>();
    t->uid = new_uid();
    t->name = "main_thread";
    t->state = initial;
    t->status = Thread::Status::Ready;
    t->stack_base = stack_base;
    t->stack_size = stack_size;
    t->tls = tls;
    current_ = t.get();
    last_scheduled_ = t->uid;
    const u32 uid = t->uid;
    threads_[uid] = std::move(t);
    return uid;
}

Thread* Kernel::thread(u32 uid) {
    auto it = threads_.find(uid);
    return it == threads_.end() || it->second->delete_pending ? nullptr : it->second.get();
}

void Kernel::block(Thread::Wait kind, u32 object, s32 count, u64 wake_time, GuestAddr out) {
    Thread* t = current_;
    t->status = Thread::Status::Waiting;
    t->wait = kind;
    t->wait_object = object;
    t->wait_count = count;
    t->wake_time = wake_time;
    t->wait_out = out;
    t->wait_order = ++wait_counter_;
    yield();
}

void Kernel::yield() {
    if (on_reschedule_) on_reschedule_();
}

void Kernel::finish_thread(Thread& t, s32 status) {
    t.status = Thread::Status::Dead;
    t.wait = Thread::Wait::None;
    t.exit_status = status;
    for (auto& [uid, w] : threads_) {
        if (w->status == Thread::Status::Waiting && w->wait == Thread::Wait::ThreadEnd && w->wait_object == t.uid) {
            if (w->wait_out) memory_.write<s32>(w->wait_out, status);
            w->status = Thread::Status::Ready;
            w->wait = Thread::Wait::None;
        }
    }
}

Thread* Kernel::schedule() {
    // Libère les threads supprimés (jamais le thread en cours d'exécution).
    for (auto it = threads_.begin(); it != threads_.end();) {
        Thread& t = *it->second;
        if (t.delete_pending && &t != current_) {
            if (t.stack_size) memory_.unmap(t.stack_base, t.stack_size);
            if (t.tls) memory_.unmap(t.tls, kTlsBlockSize);
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }

    const u64 now = now_us();
    for (auto& [uid, t] : threads_) {
        if (t->status == Thread::Status::Waiting && (t->wait == Thread::Wait::Delay || t->wait == Thread::Wait::Vblank) &&
            now >= t->wake_time) {
            t->status = Thread::Status::Ready;
            t->wait = Thread::Wait::None;
        }
    }

    // Tourniquet : le prochain thread prêt après le dernier exécuté.
    Thread* pick = nullptr;
    for (auto it = threads_.upper_bound(last_scheduled_); it != threads_.end() && !pick; ++it)
        if (it->second->status == Thread::Status::Ready && !it->second->delete_pending) pick = it->second.get();
    for (auto it = threads_.begin(); it != threads_.end() && !pick; ++it)
        if (it->second->status == Thread::Status::Ready && !it->second->delete_pending) pick = it->second.get();
    if (pick) {
        last_scheduled_ = pick->uid;
        current_ = pick;
    }
    return pick;
}

u64 Kernel::next_wake_time() const {
    u64 next = std::numeric_limits<u64>::max();
    for (const auto& [uid, t] : threads_)
        if (t->status == Thread::Status::Waiting && (t->wait == Thread::Wait::Delay || t->wait == Thread::Wait::Vblank) &&
            t->wake_time < next)
            next = t->wake_time;
    return next;
}

bool Kernel::all_threads_finished() const {
    for (const auto& [uid, t] : threads_)
        if (!t->delete_pending && (t->status == Thread::Status::Ready || t->status == Thread::Status::Waiting)) return false;
    return true;
}

// --- État partagé avec l'interface ----------------------------------------------

FrameBuffer Kernel::framebuffer() const {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    return framebuffer_;
}

void Kernel::set_input(const InputState& input) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    input_ = input;
}

std::string Kernel::stdout_text() const {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    return console_;
}

void Kernel::write_console(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    console_.append(data, size);
}

void Kernel::mount(const std::string& device, const std::string& host_dir) { mounts_[device] = host_dir; }

void Kernel::register_builtin_modules() {
    register_processmgr();
    register_sysmem();
    register_threads();
    register_sync();
    register_io();
    register_display_ctrl();
}

}  // namespace vita
