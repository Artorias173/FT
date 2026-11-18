/*
 * Programmes C portables : compilés nativement (valeurs attendues) et en
 * ARM/Thumb-2 pour l'émulateur. Pas de bibliothèque standard, pas de flottants,
 * pas de division par une variable (le Cortex-A9 n'a pas d'instruction de division).
 */
#include "programs.h"

typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;

/* Empêche le compilateur de tout calculer à la compilation. */
static volatile u32 g_seed = 12345;

static u32 lcg(u32* state) {
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

static u32 fib_recursive(u32 n) { return n < 2 ? n : fib_recursive(n - 1) + fib_recursive(n - 2); }

static u32 test_fibonacci(void) {
    u32 a = 0, b = 1;
    for (u32 i = 0; i < 40 + (g_seed & 1); ++i) {
        u32 t = a + b;
        a = b;
        b = t;
    }
    return a ^ fib_recursive(18 + (g_seed & 3));
}

static u32 test_sort(void) {
    u32 data[64];
    u32 state = g_seed;
    for (int i = 0; i < 64; ++i) data[i] = lcg(&state) % 1000;
    for (int i = 1; i < 64; ++i) {  /* tri par insertion */
        u32 key = data[i];
        int j = i - 1;
        while (j >= 0 && data[j] > key) {
            data[j + 1] = data[j];
            --j;
        }
        data[j + 1] = key;
    }
    u32 sum = 0;
    for (int i = 0; i < 64; ++i) {
        if (i > 0 && data[i - 1] > data[i]) return 0xBAD;
        sum = sum * 31 + data[i];
    }
    return sum;
}

static u32 test_crc32(void) {
    unsigned char buf[100];
    for (int i = 0; i < 100; ++i) buf[i] = (unsigned char)(i * 7 + g_seed);
    u32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < 100; ++i) {
        crc ^= buf[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static u32 dispatch(u32 op, u32 x) {
    switch (op) {  /* table de sauts (TBB/TBH en Thumb-2) */
        case 0: return x + 1;
        case 1: return x * 3;
        case 2: return x ^ 0x5555;
        case 3: return x << 2;
        case 4: return x >> 1;
        case 5: return ~x;
        case 6: return x - 7;
        case 7: return x | 0x100;
        case 8: return x & 0xFF0F;
        case 9: return x * x;
        case 10: return x + 1000;
        case 11: return (x << 16) | (x >> 16);
        default: return x;
    }
}

static u32 test_switch(void) {
    u32 x = g_seed;
    for (u32 i = 0; i < 200; ++i) x = dispatch((i * 7) % 13, x);
    return x;
}

struct Packed {
    u32 a : 5;
    u32 b : 11;
    u32 c : 3;
    u32 d : 13;
};

static u32 test_bitfields(void) {
    struct Packed p = {0, 0, 0, 0};
    u32 acc = 0;
    for (u32 i = 0; i < 50; ++i) {
        p.a = i + g_seed;
        p.b += i * 37;
        p.c ^= i;
        p.d = p.d * 3 + p.a;
        acc += p.a + p.b + p.c + p.d;
    }
    s32 v = (s32)(g_seed << 20) >> 25;  /* extraction signée */
    return acc ^ (u32)v;
}

static u32 popcount(u32 v) {
    u32 n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

static u32 test_bit_ops(void) {
    u32 acc = 0, x = g_seed * 2654435761u;
    for (int i = 0; i < 32; ++i) {
        x = x * 1664525u + 1013904223u;
        acc += (u32)__builtin_clz(x | 1) + popcount(x);
        acc ^= __builtin_bswap32(x);
        acc = (acc << 5) | (acc >> 27);  /* rotation */
    }
    return acc;
}

static u32 test_64bit(void) {
    u64 a = (u64)g_seed * 0x9E3779B97F4A7C15ull;
    u64 b = 0;
    s64 s = -(s64)g_seed * 1000003;
    for (int i = 0; i < 20; ++i) {
        a = a * 6364136223846793005ull + 1442695040888963407ull;
        b += a >> (i + 3);
        s = (s * 3) >> 1;
    }
    return (u32)(a ^ (a >> 32) ^ b ^ (b >> 29) ^ (u64)s);
}

static u32 test_signed_narrow(void) {
    s8 bytes[32];
    s16 shorts[32];
    u32 state = g_seed;
    for (int i = 0; i < 32; ++i) {
        bytes[i] = (s8)lcg(&state);
        shorts[i] = (s16)lcg(&state);
    }
    s32 acc = 0;
    for (int i = 0; i < 32; ++i) {
        acc += bytes[i] * 3 - shorts[i];
        if (bytes[i] < 0) acc ^= shorts[i];
    }
    return (u32)acc;
}

static u32 str_len(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

static u32 test_strings(void) {
    char buf[64];
    const char* src = "Bonjour depuis la PS Vita emulee !";
    u32 n = str_len(src);
    for (u32 i = 0; i < n; ++i) buf[i] = src[n - 1 - i];  /* inversion */
    buf[n] = 0;
    u32 h = 5381;
    for (u32 i = 0; buf[i]; ++i) {
        char c = buf[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        h = h * 33 + (u32)c;
    }
    return h + n;
}

typedef u32 (*op_fn)(u32);
static u32 op_double(u32 x) { return x * 2; }
static u32 op_square(u32 x) { return x * x; }
static u32 op_negate(u32 x) { return 0u - x; }
static u32 op_mix(u32 x) { return x ^ (x >> 7); }

static u32 test_function_pointers(void) {
    static op_fn const table[4] = {op_double, op_square, op_negate, op_mix};
    volatile u32 sel = g_seed;
    u32 x = 7;
    for (u32 i = 0; i < 64; ++i) x = table[(i + sel) & 3](x) + i;
    return x;
}

static u32 test_sieve(void) {
    unsigned char composite[1000];
    for (int i = 0; i < 1000; ++i) composite[i] = 0;
    u32 count = 0, sum = 0;
    for (u32 i = 2; i < 1000; ++i) {
        if (composite[i]) continue;
        ++count;
        sum += i;
        for (u32 j = i * i; j < 1000; j += i) composite[j] = 1;
    }
    return (count << 16) ^ sum;
}

/* Division logicielle (le compilateur ne doit pas appeler __aeabi_uidiv). */
static u32 soft_udiv(u32 n, u32 d, u32* rem) {
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    *rem = r;
    return q;
}

static u32 test_division(void) {
    u32 acc = 0;
    for (u32 i = 1; i < 60; ++i) {
        u32 rem;
        u32 q = soft_udiv(g_seed * 7919u + i * 104729u, i * 3 + 1, &rem);
        acc = acc * 17 + q + rem;
    }
    return acc;
}

static u32 test_mul_accumulate(void) {
    s32 a[16], b[16];
    u32 state = g_seed;
    for (int i = 0; i < 16; ++i) {
        a[i] = (s32)lcg(&state) - 0x400000;
        b[i] = (s32)lcg(&state) - 0x400000;
    }
    s64 dot = 0;
    u32 mla = 0;
    for (int i = 0; i < 16; ++i) {
        dot += (s64)a[i] * b[i];          /* SMLAL */
        mla = mla - (u32)a[i] * (u32)b[i];  /* MLS */
    }
    return (u32)dot ^ (u32)(dot >> 32) ^ mla;
}

struct Node {
    u32 value;
    struct Node* next;
};

static u32 test_linked_list(void) {
    struct Node nodes[20];
    struct Node* head = 0;
    for (u32 i = 0; i < 20; ++i) {  /* insertion en tête */
        nodes[i].value = i * i + g_seed;
        nodes[i].next = head;
        head = &nodes[i];
    }
    struct Node* prev = 0;  /* inversion de la liste */
    while (head) {
        struct Node* next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    u32 acc = 0;
    for (struct Node* n = prev; n; n = n->next) acc = acc * 7 + n->value;
    return acc;
}

typedef u32 (*test_fn)(void);
static test_fn const kTests[VITA_TEST_COUNT] = {
    test_fibonacci, test_sort,          test_crc32,    test_switch,
    test_bitfields, test_bit_ops,       test_64bit,    test_signed_narrow,
    test_strings,   test_function_pointers, test_sieve, test_division,
    test_mul_accumulate, test_linked_list,
};
static const char* const kNames[VITA_TEST_COUNT] = {
    "fibonacci", "tri", "crc32", "switch", "champs de bits", "opérations sur bits", "64 bits",
    "entiers signés courts", "chaînes", "pointeurs de fonction", "crible", "division", "multiplication-accumulation",
    "liste chaînée",
};

unsigned run_test(int index) { return kTests[index](); }
const char* test_name(int index) { return kNames[index]; }
