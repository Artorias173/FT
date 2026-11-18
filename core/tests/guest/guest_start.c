/*
 * Point d'entrée des programmes de test dans l'émulateur. Les appels système
 * passent par SVC : #0 = sceKernelExitProcess, #1 = rapport d'un résultat.
 */
#include "programs.h"

__attribute__((naked)) static void report(unsigned index, unsigned value) {
    __asm__ volatile("svc #1\n\tbx lr");
}

__attribute__((naked, noreturn)) static void exit_process(int code) {
    __asm__ volatile("svc #0\n\tb .");
}

void _start(void) {
    unsigned checksum = 0;
    for (int i = 0; i < VITA_TEST_COUNT; ++i) {
        unsigned value = run_test(i);
        report((unsigned)i, value);
        checksum = checksum * 31 + value;
    }
#ifdef VITA_GUEST_FLOAT
    for (int i = 0; i < VITA_FLOAT_TEST_COUNT; ++i) {
        unsigned value = run_float_test(i);
        report((unsigned)(VITA_TEST_COUNT + i), value);
        checksum = checksum * 31 + value;
    }
#endif
#ifdef VITA_GUEST_NEON
    for (int i = 0; i < VITA_NEON_TEST_COUNT; ++i) {
        unsigned value = run_neon_test(i);
        report((unsigned)(VITA_TEST_COUNT + VITA_FLOAT_TEST_COUNT + i), value);
        checksum = checksum * 31 + value;
    }
#endif
    exit_process((int)checksum);
}

/* Le compilateur peut générer des appels à memcpy/memset même sans bibliothèque. */
void* memcpy(void* dst, const void* src, unsigned n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memset(void* dst, int c, unsigned n) {
    unsigned char* d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void* memmove(void* dst, const void* src, unsigned n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}
