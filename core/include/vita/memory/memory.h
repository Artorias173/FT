#pragma once

#include <cstring>
#include <type_traits>
#include <vector>

#include "vita/common/types.h"

namespace vita {

// Espace d'adressage invité de 4 Gio.
//
// On réserve 4 Gio de mémoire virtuelle côté hôte (sans les allouer) et on
// n'active que les pages mappées par l'invité. Une adresse invitée se traduit
// en `base + adresse`, ce qui garde la traduction triviale et rapide. C'est
// aussi ce qu'attend un recompilateur dynamique (dynarec) comme dynarmic.
class Memory {
public:
    static constexpr u64 kAddressSpaceSize = 1ull << 32;
    static constexpr u32 kPageSize = 0x1000;

    Memory() = default;
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    bool init();
    void shutdown();

    // Mappe [addr, addr + size) en lecture/écriture, pages mises à zéro.
    bool map(GuestAddr addr, u32 size);
    void unmap(GuestAddr addr, u32 size);
    bool is_mapped(GuestAddr addr, u32 size = 1) const;

    // Pointeur hôte vers une adresse invitée, ou nullptr si elle n'est pas mappée.
    u8* host_ptr(GuestAddr addr, u32 size = 1);
    u8* base() const { return base_; }

    template <typename T>
    T read(GuestAddr addr) {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (u8* p = checked(addr, sizeof(T))) std::memcpy(&value, p, sizeof(T));
        return value;
    }

    template <typename T>
    void write(GuestAddr addr, T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (u8* p = checked(addr, sizeof(T))) std::memcpy(p, &value, sizeof(T));
    }

    bool copy_to_guest(GuestAddr dst, const void* src, u32 size);

    // Vrai si un accès invalide a eu lieu depuis le dernier appel.
    bool take_fault(GuestAddr* fault_addr = nullptr);

private:
    u8* checked(GuestAddr addr, u32 size);

    u8* base_ = nullptr;
    std::vector<bool> pages_;  // une entrée par page de 4 Kio
    bool fault_ = false;
    GuestAddr fault_addr_ = 0;
};

}  // namespace vita
