#pragma once

#include <string>
#include <vector>

#include "vita/common/types.h"

namespace vita {

class Memory;

// Codes de relocation ARM (ELF for the ARM Architecture) utilisés par la Vita.
enum RelocationCode : u32 {
    kRelNone = 0,
    kRelAbs32 = 2,
    kRelRel32 = 3,
    kRelThmCall = 10,
    kRelCall = 28,
    kRelJump24 = 29,
    kRelThmJump24 = 30,
    kRelTarget1 = 38,
    kRelV4bx = 40,
    kRelTarget2 = 41,
    kRelPrel31 = 42,
    kRelMovwAbsNc = 43,
    kRelMovtAbs = 44,
    kRelThmMovwAbsNc = 47,
    kRelThmMovtAbs = 48,
    kRelRBase = 255,
};

struct RelocSegment {
    GuestAddr addr;  // adresse de chargement du segment
};

// Applique une relocation : S = symbole, A = addend, P = emplacement à corriger.
bool apply_relocation(Memory& memory, u32 code, GuestAddr place, u32 symbol, u32 addend);

// Applique une table de relocations Vita (segment PT_SCE_RELA). Formats 0
// (12 octets) et 1 (8 octets) ; les formats compacts 2 à 9 des SDK récents
// ne sont pas encore pris en charge.
bool apply_sce_relocations(Memory& memory, const u8* data, size_t size, const std::vector<RelocSegment>& segments,
                           std::string& error);

}  // namespace vita
