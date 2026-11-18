// Modules noyau : processus, mémoire, threads et synchronisation.
#include <cstring>

#include "hle_util.h"
#include "nids.h"
#include "vita/common/log.h"
#include "vita/hle/kernel.h"
#include "vita/memory/memory.h"

namespace vita {

using hle::arg;
using hle::read_cstr;

namespace {
// Descripteurs des sorties standard (valeurs propres à l'émulateur).
constexpr u32 kStdoutFd = 0x3E000001;
constexpr u32 kStderrFd = 0x3E000002;
}  // namespace

void Kernel::register_processmgr() {
    register_function(nid::sceKernelExitProcess, "sceKernelExitProcess", [this](CpuState& s) {
        LOG_INFO("sceKernelExitProcess(%d)", static_cast<s32>(s.r[0]));
        request_exit(static_cast<s32>(s.r[0]));
    });
    register_function(nid::sceKernelGetStdout, "sceKernelGetStdout", [](CpuState& s) { s.r[0] = kStdoutFd; });
    register_function(nid::sceKernelGetStderr, "sceKernelGetStderr", [](CpuState& s) { s.r[0] = kStderrFd; });
    register_function(nid::sceKernelGetProcessTimeWide, "sceKernelGetProcessTimeWide",
                      [this](CpuState& s) { hle::ret64(s, now_us()); });
    register_function(nid::sceKernelGetProcessTimeLow, "sceKernelGetProcessTimeLow",
                      [this](CpuState& s) { s.r[0] = static_cast<u32>(now_us()); });
}

void Kernel::register_sysmem() {
    // SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize size, void *opt)
    register_function(nid::sceKernelAllocMemBlock, "sceKernelAllocMemBlock", [this](CpuState& s) {
        const u32 size = s.r[2];
        if (size == 0) { s.r[0] = sce_error::kInvalid; return; }
        // Les blocs en mémoire vidéo (CDRAM, type 0x09408060) sont alignés sur 256 Kio.
        const u32 align = s.r[1] == 0x09408060 ? 0x40000 : Memory::kPageSize;
        const GuestAddr base = alloc_area(size, kHeapArea, align);
        if (!base) { s.r[0] = sce_error::kNoMemory; return; }
        const u32 uid = new_uid();
        memblocks_[uid] = {read_cstr(memory_, s.r[0], 32), base, (size + Memory::kPageSize - 1) & ~(Memory::kPageSize - 1)};
        s.r[0] = uid;
    });
    register_function(nid::sceKernelFreeMemBlock, "sceKernelFreeMemBlock", [this](CpuState& s) {
        auto it = memblocks_.find(s.r[0]);
        if (it == memblocks_.end()) { s.r[0] = sce_error::kGeneric; return; }
        memory_.unmap(it->second.base, it->second.size);
        memblocks_.erase(it);
        s.r[0] = 0;
    });
    register_function(nid::sceKernelGetMemBlockBase, "sceKernelGetMemBlockBase", [this](CpuState& s) {
        auto it = memblocks_.find(s.r[0]);
        if (it == memblocks_.end()) { s.r[0] = sce_error::kGeneric; return; }
        memory_.write<u32>(s.r[1], it->second.base);
        s.r[0] = memory_.take_fault() ? sce_error::kInvalid : 0;
    });
}

void Kernel::register_threads() {
    // SceUID sceKernelCreateThread(name, entry, priority, stack_size, attr, cpu_affinity, option)
    register_function(nid::sceKernelCreateThread, "sceKernelCreateThread", [this](CpuState& s) {
        const u32 stack_size = s.r[3] < 0x4000 ? 0x4000 : (s.r[3] + 0xFFF) & ~0xFFFu;
        const GuestAddr stack = alloc_area(stack_size, kThreadStackArea, Memory::kPageSize);
        const GuestAddr tls = stack ? alloc_area(kTlsBlockSize, kThreadStackArea, Memory::kPageSize) : 0;
        if (!stack || !tls) { s.r[0] = sce_error::kNoMemory; return; }
        auto t = std::make_unique<Thread>();
        t->uid = new_uid();
        t->name = read_cstr(memory_, s.r[0], 32);
        t->entry = s.r[1];
        t->priority = static_cast<s32>(s.r[2]);
        t->stack_base = stack;
        t->stack_size = stack_size;
        t->tls = tls;
        t->status = Thread::Status::Dormant;
        s.r[0] = t->uid;
        threads_[t->uid] = std::move(t);
    });
    // int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp) : argp est copié sur la pile du thread.
    register_function(nid::sceKernelStartThread, "sceKernelStartThread", [this](CpuState& s) {
        Thread* t = thread(s.r[0]);
        if (!t || t->status != Thread::Status::Dormant) { s.r[0] = sce_error::kGeneric; return; }
        const u32 arglen = s.r[1], argp = s.r[2];
        CpuState& st = t->state;
        st = CpuState{};
        u32 sp = t->stack_base + t->stack_size;
        u32 args = argp;
        if (argp && arglen && arglen < t->stack_size / 2) {
            sp = (sp - arglen) & ~7u;
            for (u32 i = 0; i < arglen; ++i) memory_.write<u8>(sp + i, memory_.read<u8>(argp + i));
            args = sp;
        }
        st.r[0] = arglen;
        st.r[1] = args;
        st.sp() = sp;
        st.lr() = thread_return_trap_;
        st.pc() = t->entry & ~1u;
        if (t->entry & 1) st.cpsr |= CpuState::kFlagT;
        st.fpscr = s.fpscr & 0x03C00000;  // mêmes réglages flottants que le créateur
        st.tpidruro = t->tls;
        t->status = Thread::Status::Ready;
        s.r[0] = memory_.take_fault() ? sce_error::kInvalid : 0;
    });
    register_function(nid::sceKernelExitThread, "sceKernelExitThread", [this](CpuState& s) {
        finish_thread(*current_, static_cast<s32>(s.r[0]));
        yield();
    });
    register_function(nid::sceKernelExitDeleteThread, "sceKernelExitDeleteThread", [this](CpuState& s) {
        finish_thread(*current_, static_cast<s32>(s.r[0]));
        current_->delete_pending = true;
        yield();
    });
    register_function(nid::sceKernelDeleteThread, "sceKernelDeleteThread", [this](CpuState& s) {
        Thread* t = thread(s.r[0]);
        if (!t || (t->status != Thread::Status::Dormant && t->status != Thread::Status::Dead)) {
            s.r[0] = sce_error::kGeneric;
            return;
        }
        t->delete_pending = true;
        s.r[0] = 0;
    });
    // int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout) ; délai non géré.
    register_function(nid::sceKernelWaitThreadEnd, "sceKernelWaitThreadEnd", [this](CpuState& s) {
        Thread* t = thread(s.r[0]);
        if (!t || t == current_) { s.r[0] = sce_error::kGeneric; return; }
        if (t->status == Thread::Status::Dead) {
            if (s.r[1]) memory_.write<s32>(s.r[1], t->exit_status);
            s.r[0] = 0;
            return;
        }
        s.r[0] = 0;
        block(Thread::Wait::ThreadEnd, t->uid, 0, 0, s.r[1]);
    });
    register_function(nid::sceKernelDelayThread, "sceKernelDelayThread", [this](CpuState& s) {
        const u64 wake = now_us() + s.r[0];
        s.r[0] = 0;
        block(Thread::Wait::Delay, 0, 0, wake);
    });
    register_function(nid::sceKernelGetThreadId, "sceKernelGetThreadId",
                      [this](CpuState& s) { s.r[0] = current_ ? current_->uid : 0; });
}

void Kernel::register_sync() {
    // Mutex légers : identifiés par l'adresse de leur zone de travail (SceKernelLwMutexWork).
    // Le verrouillage récursif par le propriétaire est toujours accepté.
    register_function(nid::sceKernelCreateLwMutex, "sceKernelCreateLwMutex", [this](CpuState& s) {
        const s32 init = static_cast<s32>(s.r[3]);
        lwmutexes_[s.r[0]] = LwMutex{init > 0 && current_ ? current_->uid : 0, init > 0 ? init : 0};
        s.r[0] = 0;
    });
    register_function(nid::sceKernelLockLwMutex, "sceKernelLockLwMutex", [this](CpuState& s) {
        auto it = lwmutexes_.find(s.r[0]);
        const s32 count = static_cast<s32>(s.r[1]) > 0 ? static_cast<s32>(s.r[1]) : 1;
        if (it == lwmutexes_.end()) { s.r[0] = sce_error::kGeneric; return; }
        LwMutex& m = it->second;
        if (m.owner == 0 || m.owner == current_->uid) {
            m.owner = current_->uid;
            m.count += count;
            s.r[0] = 0;
            return;
        }
        s.r[0] = 0;
        block(Thread::Wait::LwMutex, it->first, count, 0);
    });
    register_function(nid::sceKernelTryLockLwMutex, "sceKernelTryLockLwMutex", [this](CpuState& s) {
        auto it = lwmutexes_.find(s.r[0]);
        const s32 count = static_cast<s32>(s.r[1]) > 0 ? static_cast<s32>(s.r[1]) : 1;
        if (it == lwmutexes_.end() || (it->second.owner && it->second.owner != current_->uid)) {
            s.r[0] = sce_error::kGeneric;
            return;
        }
        it->second.owner = current_->uid;
        it->second.count += count;
        s.r[0] = 0;
    });
    register_function(nid::sceKernelUnlockLwMutex, "sceKernelUnlockLwMutex", [this](CpuState& s) {
        auto it = lwmutexes_.find(s.r[0]);
        if (it == lwmutexes_.end() || it->second.owner != current_->uid) { s.r[0] = sce_error::kGeneric; return; }
        LwMutex& m = it->second;
        m.count -= static_cast<s32>(s.r[1]) > 0 ? static_cast<s32>(s.r[1]) : 1;
        if (m.count <= 0) {
            m.owner = 0;
            m.count = 0;
            // Le premier thread en attente devient propriétaire.
            Thread* next = nullptr;
            for (auto& [uid, t] : threads_)
                if (t->status == Thread::Status::Waiting && t->wait == Thread::Wait::LwMutex && t->wait_object == it->first &&
                    (!next || t->wait_order < next->wait_order))
                    next = t.get();
            if (next) {
                m.owner = next->uid;
                m.count = next->wait_count;
                next->status = Thread::Status::Ready;
                next->wait = Thread::Wait::None;
            }
        }
        s.r[0] = 0;
    });
    register_function(nid::sceKernelDeleteLwMutex, "sceKernelDeleteLwMutex", [this](CpuState& s) {
        if (!lwmutexes_.erase(s.r[0])) { s.r[0] = sce_error::kGeneric; return; }
        for (auto& [uid, t] : threads_)
            if (t->status == Thread::Status::Waiting && t->wait == Thread::Wait::LwMutex && t->wait_object == s.r[0]) {
                t->state.r[0] = sce_error::kGeneric;  // l'attente échoue
                t->status = Thread::Status::Ready;
                t->wait = Thread::Wait::None;
            }
        s.r[0] = 0;
    });

    // Sémaphores : SceUID sceKernelCreateSema(name, attr, init, max, option).
    register_function(nid::sceKernelCreateSema, "sceKernelCreateSema", [this](CpuState& s) {
        const s32 init = static_cast<s32>(s.r[2]), max = static_cast<s32>(s.r[3]);
        if (init < 0 || max <= 0 || init > max) { s.r[0] = sce_error::kGeneric; return; }
        const u32 uid = new_uid();
        semaphores_[uid] = {read_cstr(memory_, s.r[0], 32), init, max};
        s.r[0] = uid;
    });
    // int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout) ; délai non géré.
    register_function(nid::sceKernelWaitSema, "sceKernelWaitSema", [this](CpuState& s) {
        auto it = semaphores_.find(s.r[0]);
        const s32 need = static_cast<s32>(s.r[1]);
        if (it == semaphores_.end() || need <= 0 || need > it->second.max) { s.r[0] = sce_error::kGeneric; return; }
        bool queued = false;  // FIFO : on ne double pas les threads déjà en attente
        for (auto& [uid, t] : threads_)
            if (t->status == Thread::Status::Waiting && t->wait == Thread::Wait::Sema && t->wait_object == it->first) queued = true;
        if (!queued && it->second.count >= need) {
            it->second.count -= need;
            s.r[0] = 0;
            return;
        }
        const u32 uid = it->first;
        s.r[0] = 0;
        block(Thread::Wait::Sema, uid, need, 0);
    });
    register_function(nid::sceKernelSignalSema, "sceKernelSignalSema", [this](CpuState& s) {
        auto it = semaphores_.find(s.r[0]);
        const s32 add = static_cast<s32>(s.r[1]);
        if (it == semaphores_.end() || add <= 0 || it->second.count + add > it->second.max) {
            s.r[0] = sce_error::kGeneric;
            return;
        }
        it->second.count += add;
        wake_sema_waiters(it->first);
        s.r[0] = 0;
    });
    register_function(nid::sceKernelDeleteSema, "sceKernelDeleteSema", [this](CpuState& s) {
        if (!semaphores_.erase(s.r[0])) { s.r[0] = sce_error::kGeneric; return; }
        for (auto& [uid, t] : threads_)
            if (t->status == Thread::Status::Waiting && t->wait == Thread::Wait::Sema && t->wait_object == s.r[0]) {
                t->state.r[0] = sce_error::kGeneric;
                t->status = Thread::Status::Ready;
                t->wait = Thread::Wait::None;
            }
        s.r[0] = 0;
    });
}

void Kernel::wake_sema_waiters(u32 uid) {
    Semaphore& sem = semaphores_[uid];
    while (true) {
        Thread* first = nullptr;
        for (auto& [id, t] : threads_)
            if (t->status == Thread::Status::Waiting && t->wait == Thread::Wait::Sema && t->wait_object == uid &&
                (!first || t->wait_order < first->wait_order))
                first = t.get();
        if (!first || sem.count < first->wait_count) return;
        sem.count -= first->wait_count;
        first->status = Thread::Status::Ready;
        first->wait = Thread::Wait::None;
    }
}

}  // namespace vita
