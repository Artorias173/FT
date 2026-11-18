#pragma once
// Outils de décodage partagés par les fichiers de l'interpréteur.

#include "vita/common/types.h"

namespace vita::arm {

constexpr u32 bits(u32 v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
constexpr bool bit(u32 v, int n) { return (v >> n) & 1; }
constexpr u32 ror(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
constexpr u32 sign_extend(u32 v, int width) {
    const u32 m = 1u << (width - 1);
    v &= (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
    return (v ^ m) - m;
}

enum ShiftType : u32 { kLsl = 0, kLsr = 1, kAsr = 2, kRor = 3, kRrx = 4 };

struct Shifted {
    u32 value;
    bool carry;
};

// Décalage complet (montant de 0 à 255, comme pour un décalage par registre).
inline Shifted shift_c(u32 v, u32 type, u32 amount, bool carry_in) {
    if (type == kRrx) return {(v >> 1) | (static_cast<u32>(carry_in) << 31), bit(v, 0)};
    if (amount == 0) return {v, carry_in};
    switch (type) {
        case kLsl:
            if (amount < 32) return {v << amount, bit(v, 32 - amount)};
            return {0, amount == 32 && bit(v, 0)};
        case kLsr:
            if (amount < 32) return {v >> amount, bit(v, amount - 1)};
            return {0, amount == 32 && bit(v, 31)};
        case kAsr:
            if (amount < 32) return {static_cast<u32>(static_cast<s32>(v) >> amount), bit(v, amount - 1)};
            return {bit(v, 31) ? 0xFFFFFFFFu : 0u, bit(v, 31)};
        default: {  // ROR
            const u32 r = ror(v, amount);
            return {r, bit(r, 31)};
        }
    }
}

// Décalage encodé dans l'instruction (DecodeImmShift) : 0 signifie 32 pour LSR/ASR, RRX pour ROR.
inline Shifted shift_imm_c(u32 v, u32 type, u32 imm5, bool carry_in) {
    switch (type) {
        case kLsl: return shift_c(v, kLsl, imm5, carry_in);
        case kLsr: return shift_c(v, kLsr, imm5 ? imm5 : 32, carry_in);
        case kAsr: return shift_c(v, kAsr, imm5 ? imm5 : 32, carry_in);
        default: return imm5 ? shift_c(v, kRor, imm5, carry_in) : shift_c(v, kRrx, 1, carry_in);
    }
}

// Immédiat modifié des instructions Thumb-2 (ThumbExpandImm_C).
inline Shifted thumb_expand_imm_c(u32 imm12, bool carry_in) {
    if (bits(imm12, 11, 10) == 0) {
        const u32 imm8 = imm12 & 0xFF;
        switch (bits(imm12, 9, 8)) {
            case 0: return {imm8, carry_in};
            case 1: return {imm8 | (imm8 << 16), carry_in};
            case 2: return {(imm8 << 8) | (imm8 << 24), carry_in};
            default: return {imm8 * 0x01010101u, carry_in};
        }
    }
    const u32 value = ror(0x80 | (imm12 & 0x7F), bits(imm12, 11, 7));
    return {value, bit(value, 31)};
}

inline u32 byte_swap(u32 v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
inline u32 rev16(u32 v) { return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00); }
inline u32 revsh(u32 v) { return sign_extend(((v & 0xFF) << 8) | ((v >> 8) & 0xFF), 16); }
inline u32 bit_reverse(u32 v) {
    u32 r = 0;
    for (int i = 0; i < 32; ++i) r |= ((v >> i) & 1) << (31 - i);
    return r;
}
inline u32 count_leading_zeros(u32 v) { return v ? static_cast<u32>(__builtin_clz(v)) : 32; }

// Moitié haute ou basse d'un registre, avec extension de signe (SMULxy...).
inline s64 half(u32 v, bool top) { return top ? static_cast<s16>(v >> 16) : static_cast<s16>(v & 0xFFFF); }

// Extraction / insertion de champs de bits (UBFX, SBFX, BFI, BFC).
inline u32 ubfx(u32 v, u32 lsb, u32 width) {
    return width >= 32 ? v >> lsb : (v >> lsb) & ((1u << width) - 1);
}
inline u32 sbfx(u32 v, u32 lsb, u32 width) { return sign_extend(ubfx(v, lsb, width), width); }
inline u32 bfi(u32 dst, u32 src, u32 lsb, u32 msb) {
    if (msb < lsb) return dst;  // encodage imprévisible : on ne change rien
    const u32 width = msb - lsb + 1;
    const u32 mask = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1)) << lsb;
    return (dst & ~mask) | ((src << lsb) & mask);
}

// Extensions avec rotation (SXTB, UXTH, SXTAB...). kind : 0 SXTH, 1 UXTH, 4 SXTB, 5 UXTB.
inline bool extend(u32 kind, u32 value, u32 rotation, u32& out) {
    const u32 v = ror(value, rotation);
    switch (kind) {
        case 0: out = sign_extend(v, 16); return true;
        case 1: out = v & 0xFFFF; return true;
        case 4: out = sign_extend(v, 8); return true;
        case 5: out = v & 0xFF; return true;
        default: return false;  // variantes 16 bits doubles (SXTB16...) non gérées
    }
}

}  // namespace vita::arm
