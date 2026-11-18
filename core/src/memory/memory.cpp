#include "vita/memory/memory.h"

#include <sys/mman.h>

#include "vita/common/log.h"

namespace vita {

namespace {
constexpr u64 page_index(u64 addr) { return addr / Memory::kPageSize; }
}  // namespace

Memory::~Memory() { shutdown(); }

bool Memory::init() {
    if (base_) return true;
    void* p = mmap(nullptr, kAddressSpaceSize, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOG_ERROR("Impossible de réserver l'espace d'adressage invité de 4 Gio");
        return false;
    }
    base_ = static_cast<u8*>(p);
    pages_.assign(page_index(kAddressSpaceSize), false);
    return true;
}

void Memory::shutdown() {
    if (!base_) return;
    munmap(base_, kAddressSpaceSize);
    base_ = nullptr;
    pages_.clear();
}

bool Memory::map(GuestAddr addr, u32 size) {
    if (!base_ || size == 0) return false;
    const u64 start = page_index(addr) * kPageSize;
    const u64 end = (static_cast<u64>(addr) + size + kPageSize - 1) / kPageSize * kPageSize;
    if (end > kAddressSpaceSize) return false;

    if (mprotect(base_ + start, end - start, PROT_READ | PROT_WRITE) != 0) {
        LOG_ERROR("mprotect a échoué pour 0x%08llx", static_cast<unsigned long long>(start));
        return false;
    }
    for (u64 i = page_index(start); i < page_index(end); ++i) pages_[i] = true;
    return true;
}

void Memory::unmap(GuestAddr addr, u32 size) {
    if (!base_ || size == 0) return;
    const u64 start = page_index(addr) * kPageSize;
    const u64 end = (static_cast<u64>(addr) + size + kPageSize - 1) / kPageSize * kPageSize;
    // Rend les pages au système puis interdit l'accès.
    madvise(base_ + start, end - start, MADV_DONTNEED);
    mprotect(base_ + start, end - start, PROT_NONE);
    for (u64 i = page_index(start); i < page_index(end); ++i) pages_[i] = false;
}

bool Memory::is_mapped(GuestAddr addr, u32 size) const {
    if (!base_ || size == 0) return false;
    const u64 last = static_cast<u64>(addr) + size - 1;
    if (last >= kAddressSpaceSize) return false;
    for (u64 i = page_index(addr); i <= page_index(last); ++i)
        if (!pages_[i]) return false;
    return true;
}

bool Memory::is_free(GuestAddr addr, u32 size) const {
    if (!base_ || size == 0) return false;
    const u64 last = static_cast<u64>(addr) + size - 1;
    if (last >= kAddressSpaceSize) return false;
    for (u64 i = page_index(addr); i <= page_index(last); ++i)
        if (pages_[i]) return false;
    return true;
}

GuestAddr Memory::find_free(u32 size, GuestAddr start, u32 align) const {
    if (align < kPageSize) align = kPageSize;
    for (u64 a = (static_cast<u64>(start) + align - 1) / align * align; a + size <= kAddressSpaceSize; a += align)
        if (is_free(static_cast<GuestAddr>(a), size)) return static_cast<GuestAddr>(a);
    return 0;
}

u8* Memory::host_ptr(GuestAddr addr, u32 size) {
    return is_mapped(addr, size) ? base_ + addr : nullptr;
}

u8* Memory::checked(GuestAddr addr, u32 size) {
    u8* p = host_ptr(addr, size);
    if (!p && !fault_) {
        fault_ = true;
        fault_addr_ = addr;
    }
    return p;
}

bool Memory::copy_to_guest(GuestAddr dst, const void* src, u32 size) {
    u8* p = host_ptr(dst, size);
    if (!p) return false;
    std::memcpy(p, src, size);
    return true;
}

bool Memory::take_fault(GuestAddr* fault_addr) {
    if (!fault_) return false;
    if (fault_addr) *fault_addr = fault_addr_;
    fault_ = false;
    return true;
}

}  // namespace vita
