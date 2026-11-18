#include "vita/loader/relocation.h"

#include <cstdio>
#include <cstring>

#include "vita/memory/memory.h"

namespace vita {

namespace {

// Champ immédiat 16 bits des MOVW/MOVT ARM : imm4 (bits 19:16) et imm12 (bits 11:0).
u32 arm_mov_imm(u32 instr, u32 v) { return (instr & 0xFFF0F000) | ((v >> 12) & 0xF) << 16 | (v & 0xFFF); }

// MOVW/MOVT Thumb-2 : imm4 et i dans le premier demi-mot, imm3 et imm8 dans le second.
void thumb_mov_imm(u16& hw1, u16& hw2, u32 v) {
    hw1 = static_cast<u16>((hw1 & 0xFBF0) | ((v >> 12) & 0xF) | (((v >> 11) & 1) << 10));
    hw2 = static_cast<u16>((hw2 & 0x8F00) | (((v >> 8) & 7) << 12) | (v & 0xFF));
}

// BL / BLX / B.W Thumb-2 : déplacement signé de 25 bits.
void thumb_branch(u16& hw1, u16& hw2, u32 x) {
    const u32 s = (x >> 24) & 1, i1 = (x >> 23) & 1, i2 = (x >> 22) & 1;
    const u32 j1 = (i1 ^ 1) ^ s, j2 = (i2 ^ 1) ^ s;
    hw1 = static_cast<u16>((hw1 & 0xF800) | (s << 10) | ((x >> 12) & 0x3FF));
    hw2 = static_cast<u16>((hw2 & 0xD000) | (j1 << 13) | (j2 << 11) | ((x >> 1) & 0x7FF));
}

}  // namespace

bool apply_relocation(Memory& memory, u32 code, GuestAddr p, u32 s, u32 a) {
    const u32 value = s + a;
    switch (code) {
        case kRelNone: case kRelV4bx: case kRelRBase:
            return true;
        case kRelAbs32: case kRelTarget1:
            memory.write<u32>(p, value);
            break;
        case kRelRel32: case kRelTarget2:
            memory.write<u32>(p, value - p);
            break;
        case kRelPrel31:  // le bit 31 du mot est conservé
            memory.write<u32>(p, (memory.read<u32>(p) & 0x80000000u) | ((value - p) & 0x7FFFFFFFu));
            break;
        case kRelCall: case kRelJump24:
            memory.write<u32>(p, (memory.read<u32>(p) & 0xFF000000u) | (((value - p) >> 2) & 0x00FFFFFFu));
            break;
        case kRelThmCall: case kRelThmJump24: {
            u16 hw1 = memory.read<u16>(p), hw2 = memory.read<u16>(p + 2);
            thumb_branch(hw1, hw2, value - p);
            memory.write<u16>(p, hw1);
            memory.write<u16>(p + 2, hw2);
            break;
        }
        case kRelMovwAbsNc: memory.write<u32>(p, arm_mov_imm(memory.read<u32>(p), value & 0xFFFF)); break;
        case kRelMovtAbs: memory.write<u32>(p, arm_mov_imm(memory.read<u32>(p), value >> 16)); break;
        case kRelThmMovwAbsNc: case kRelThmMovtAbs: {
            u16 hw1 = memory.read<u16>(p), hw2 = memory.read<u16>(p + 2);
            thumb_mov_imm(hw1, hw2, code == kRelThmMovwAbsNc ? value & 0xFFFF : value >> 16);
            memory.write<u16>(p, hw1);
            memory.write<u16>(p + 2, hw2);
            break;
        }
        default:
            return false;
    }
    return !memory.take_fault();
}

bool apply_sce_relocations(Memory& memory, const u8* data, size_t size, const std::vector<RelocSegment>& segments,
                           std::string& error) {
    char buf[128];
    auto word = [&](size_t off) {
        u32 w;
        std::memcpy(&w, data + off, 4);
        return w;
    };
    auto segment = [&](u32 index, GuestAddr& out) {
        if (index == 15) { out = 0; return true; }  // adresse absolue
        if (index >= segments.size()) return false;
        out = segments[index].addr;
        return true;
    };

    size_t off = 0;
    while (off + 4 <= size) {
        const u32 w1 = word(off);
        const u32 format = w1 & 0xF;
        u32 symseg, code, datseg, addend, offset, code2 = 0, dist2 = 0;
        if (format == 0) {  // forme longue (12 octets), celle qu'émet le VitaSDK
            if (off + 12 > size) break;
            symseg = (w1 >> 4) & 0xF;
            code = (w1 >> 8) & 0xFF;
            datseg = (w1 >> 16) & 0xF;
            code2 = (w1 >> 20) & 0xFF;
            dist2 = (w1 >> 28) & 0xF;
            addend = word(off + 4);
            offset = word(off + 8);
            off += 12;
        } else if (format == 1) {  // forme courte (8 octets), répartition des bits reprise de Vita3K
            if (off + 8 > size) break;
            const u32 w2 = word(off + 4);
            symseg = (w1 >> 4) & 0xF;
            code = (w1 >> 8) & 0xFF;
            datseg = (w1 >> 16) & 0xF;
            offset = ((w1 >> 20) & 0xFFF) | ((w2 & 0x3FF) << 12);
            addend = w2 >> 10;
            off += 8;
        } else {
            std::snprintf(buf, sizeof(buf), "Format de relocation %u non pris en charge (octet 0x%zx)", format, off);
            error = buf;
            return false;
        }

        GuestAddr s, base;
        if (!segment(symseg, s) || datseg == 15 || !segment(datseg, base)) {
            std::snprintf(buf, sizeof(buf), "Relocation vers un segment inexistant (octet 0x%zx)", off);
            error = buf;
            return false;
        }
        const GuestAddr place = base + offset;
        if (!apply_relocation(memory, code, place, s, addend) ||
            (code2 && !apply_relocation(memory, code2, place + dist2 * 2, s, addend))) {
            std::snprintf(buf, sizeof(buf), "Relocation de type %u impossible à 0x%08x", code2 && code ? code2 : code, place);
            error = buf;
            return false;
        }
    }
    if (off != size) {
        error = "Table de relocations tronquée";
        return false;
    }
    return true;
}

}  // namespace vita
