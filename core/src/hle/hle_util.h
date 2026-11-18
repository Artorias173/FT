#pragma once
// Outils communs aux implémentations des fonctions système.

#include <string>

#include "vita/cpu/cpu.h"
#include "vita/memory/memory.h"

namespace vita::hle {

// Argument numéro i selon l'AAPCS : r0-r3, puis la pile.
inline u32 arg(Memory& m, const CpuState& s, int i) {
    return i < 4 ? s.r[i] : m.read<u32>(s.r[13] + 4 * static_cast<u32>(i - 4));
}

inline std::string read_cstr(Memory& m, GuestAddr addr, u32 max_len = 1024) {
    std::string out;
    if (!addr) return out;
    for (u32 i = 0; i < max_len; ++i) {
        const char c = static_cast<char>(m.read<u8>(addr + i));
        if (!c || m.has_fault()) break;
        out += c;
    }
    m.take_fault();
    return out;
}

inline void ret64(CpuState& s, u64 v) {
    s.r[0] = static_cast<u32>(v);
    s.r[1] = static_cast<u32>(v >> 32);
}

}  // namespace vita::hle
