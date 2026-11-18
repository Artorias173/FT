#pragma once
// Arithmétique flottante aux règles ARM (VFPv3 et NEON).
//
// L'hôte fournit les opérations IEEE 754 de base, dans le mode d'arrondi
// demandé. Ce module y ajoute ce qui distingue l'ARM du x86 :
//   - mise à zéro des dénormaux en entrée et en sortie (FPSCR.FZ) ;
//   - NaN par défaut (FPSCR.DN) et règles ARM de propagation des NaN
//     (le x86 produit par exemple un NaN par défaut négatif) ;
//   - drapeaux d'exception cumulés du FPSCR (IOC, DZC, OFC, UFC, IXC, IDC) ;
//   - conversions saturantes, virgule fixe, demi-précision (IEEE ou AHP),
//     estimations d'inverse et d'inverse de racine (VRECPE, VRSQRTE).
//
// Limite connue : l'ARM détecte les résultats minuscules avant arrondi, le
// x86 après. Un résultat qui s'arrondit exactement au plus petit normal peut
// donc être mis à zéro sur ARM et pas ici, et le drapeau UFC peut différer.

#include "vita/common/types.h"

namespace vita::fp {

// Bits du FPSCR.
constexpr u32 kIOC = 1u << 0;  // opération invalide
constexpr u32 kDZC = 1u << 1;  // division par zéro
constexpr u32 kOFC = 1u << 2;  // dépassement
constexpr u32 kUFC = 1u << 3;  // soupassement
constexpr u32 kIXC = 1u << 4;  // résultat inexact
constexpr u32 kIDC = 1u << 7;  // dénormal en entrée mis à zéro
constexpr u32 kExceptionMask = kIOC | kDZC | kOFC | kUFC | kIXC | kIDC;
constexpr u32 kQC = 1u << 27;  // saturation (NEON)
constexpr u32 kFZ = 1u << 24;
constexpr u32 kDN = 1u << 25;
constexpr u32 kAHP = 1u << 26;

enum Rounding : u32 { kNearest = 0, kPlusInf = 1, kMinusInf = 2, kZero = 3 };

// Contexte d'une opération : réglages lus dans le FPSCR et exceptions levées.
struct Ctx {
    bool fz = false;
    bool dn = false;
    bool ahp = false;
    u32 rmode = kNearest;
    u32 exceptions = 0;

    static Ctx from_fpscr(u32 fpscr) {
        Ctx c;
        c.fz = fpscr & kFZ;
        c.dn = fpscr & kDN;
        c.ahp = fpscr & kAHP;
        c.rmode = (fpscr >> 22) & 3;
        return c;
    }
    // NEON utilise toujours ces réglages, quel que soit le FPSCR.
    static Ctx standard(u32 fpscr) {
        Ctx c;
        c.fz = c.dn = true;
        c.ahp = fpscr & kAHP;
        return c;
    }
};

template <typename T> T add(T a, T b, Ctx& c);
template <typename T> T sub(T a, T b, Ctx& c);
template <typename T> T mul(T a, T b, Ctx& c);
template <typename T> T div(T a, T b, Ctx& c);
template <typename T> T sqrt(T a, Ctx& c);
template <typename T> T neg(T a);  // inverse le signe, même d'un NaN (aucune exception)
template <typename T> T abs(T a);
template <typename T> T max(T a, T b, Ctx& c);  // VMAX/VMIN : max(+0, -0) = +0
template <typename T> T min(T a, T b, Ctx& c);

// Comparaison : renvoie les drapeaux NZCV (sur 4 bits). `quiet_nan_signals` pour VCMPE.
template <typename T> u32 compare(T a, T b, bool quiet_nan_signals, Ctx& c);
// Comparaisons NEON (vrai = tous les bits à 1). VCEQ ne signale que les NaN signalants.
template <typename T> bool compare_eq(T a, T b, Ctx& c);
template <typename T> bool compare_ge(T a, T b, Ctx& c);
template <typename T> bool compare_gt(T a, T b, Ctx& c);

// Conversions entre précisions.
float to_single(double v, Ctx& c);
double to_double(float v, Ctx& c);

// Flottant -> entier 32 bits saturé (rmode explicite : VCVT tronque, VCVTR suit le FPSCR).
template <typename T> u32 to_int(T v, bool is_signed, u32 rmode, Ctx& c);
template <typename T> T from_int(u32 v, bool is_signed, Ctx& c);

// Virgule fixe sur `size` bits (16 ou 32) avec `frac_bits` bits après la virgule.
// Le résultat est étendu (signe ou zéros) à la taille du registre.
template <typename T> u64 to_fixed(T v, u32 size, u32 frac_bits, bool is_unsigned, u32 rmode, Ctx& c);
template <typename T> T from_fixed(u64 v, u32 size, u32 frac_bits, bool is_unsigned, Ctx& c);

// Demi-précision : format IEEE ou alternatif (AHP, sans infini ni NaN).
u16 to_half(float v, Ctx& c);
float from_half(u16 v, Ctx& c);

// Estimations NEON (VRECPE, VRSQRTE) et pas de Newton-Raphson (VRECPS, VRSQRTS).
float recip_estimate(float v, Ctx& c);
float rsqrt_estimate(float v, Ctx& c);
u32 recip_estimate_u32(u32 v);
u32 rsqrt_estimate_u32(u32 v);
float recip_step(float a, float b, Ctx& c);   // 2 - a*b
float rsqrt_step(float a, float b, Ctx& c);   // (3 - a*b) / 2

}  // namespace vita::fp
