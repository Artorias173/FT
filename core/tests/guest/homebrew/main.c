/* Point d'entrée du homebrew de test, comme celui d'un programme VitaSDK. */
#include "homebrew.h"

/* Fonctions système importées par NID (stubs dans stubs.S). */
void sceKernelExitProcess(int code);
void vitaTestReport(unsigned index, unsigned value);
unsigned vitaTestUnimplemented(void);

int module_start(unsigned args, const void* argp) {
    vitaTestReport(100, args);
    vitaTestReport(101, (unsigned)argp);
    vitaTestReport(102, vitaTestUnimplemented());  /* fonction inconnue : doit renvoyer 0 */
    unsigned sum = 0;
    for (int i = 0; i < HB_VALUE_COUNT; ++i) {
        const unsigned v = hb_value(i);
        vitaTestReport((unsigned)i, v);
        sum = sum * 31 + v;
    }
#ifdef HB_RETURN_FROM_START
    return (int)(sum & 0x7FFFFFFF);  /* le processus se termine au retour de module_start */
#else
    sceKernelExitProcess((int)(sum & 0x7FFFFFFF));
    return 0;
#endif
}

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
