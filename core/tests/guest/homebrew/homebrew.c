/*
 * Calculs volontairement riches en pointeurs absolus : chaînes, tables de
 * fonctions, structures chaînées et données globales. Une fois le programme
 * chargé ailleurs qu'à son adresse prévue, tout repose sur les relocations.
 */
#include "homebrew.h"

typedef unsigned int u32;

static const char* const kWords[] = {"bonjour", "vita", "homebrew", "relocation"};  /* données -> code */
static int g_counter = 41;                                                          /* segment de données */
static u32 g_zeroed[8];                                                             /* .bss */

struct Node {
    u32 value;
    const struct Node* next;  /* données -> données */
};
static const struct Node kNodes[3] = {{10, &kNodes[1]}, {20, &kNodes[2]}, {30, 0}};
static struct Node g_mutable[2] = {{1, &g_mutable[1]}, {2, &kNodes[0]}};

static u32 twice(u32 x) { return x * 2; }
static u32 square(u32 x) { return x * x; }
static u32 (*const kOps[])(u32) = {twice, square};  /* pointeurs de fonctions Thumb */
static u32 (*g_op)(u32) = square;

static u32 str_hash(const char* s) {
    u32 h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static float g_scale = 1.5f;

unsigned hb_value(int index) {
    switch (index) {
        case 0: {
            u32 h = 0;
            for (unsigned i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i) h ^= str_hash(kWords[i]) + i;
            return h;
        }
        case 1: return (u32)++g_counter;
        case 2: {
            u32 s = 0;
            for (int i = 0; i < 8; ++i) s |= g_zeroed[i];
            g_zeroed[3] = 7;
            return s + g_zeroed[3];
        }
        case 3: {
            u32 s = 0;
            for (const struct Node* n = &kNodes[0]; n; n = n->next) s = s * 3 + n->value;
            for (const struct Node* n = &g_mutable[0]; n; n = n->next) s = s * 5 + n->value;
            return s;
        }
        case 4: return kOps[0](21) + kOps[1](12) + g_op(3);
        case 5: return (u32)(g_scale * 1000.0f) + (u32)(&kNodes[2] - &kNodes[0]);
        default: return (u32)(kWords[2][3]) + (u32)(g_mutable[1].next == &kNodes[0]);
    }
}
