/*
 * Homebrew de test des modules système : mémoire, threads, synchronisation,
 * temps, fichiers, console, commandes et affichage. Chaque résultat est
 * rapporté par vitaTestReport(index, valeur) et vérifié par core_tests.cpp.
 */
typedef int SceUID;
typedef long long SceOff;

typedef struct {
    unsigned size;
    void* base;
    unsigned pitch, pixelformat, width, height;
} SceDisplayFrameBuf;

typedef struct {
    unsigned long long timeStamp;
    unsigned buttons;
    unsigned char lx, ly, rx, ry;
    unsigned char rest[16];
} SceCtrlData;

void sceKernelExitProcess(int code);
SceUID sceKernelGetStdout(void);
long long sceKernelGetProcessTimeWide(void);
unsigned sceKernelGetProcessTimeLow(void);
SceUID sceKernelAllocMemBlock(const char* name, int type, unsigned size, void* opt);
int sceKernelFreeMemBlock(SceUID uid);
int sceKernelGetMemBlockBase(SceUID uid, void** base);
SceUID sceKernelCreateThread(const char* name, int (*entry)(unsigned, void*), int priority, unsigned stack_size,
                             unsigned attr, int affinity, const void* option);
int sceKernelStartThread(SceUID thid, unsigned arglen, void* argp);
int sceKernelExitDeleteThread(int status);
int sceKernelWaitThreadEnd(SceUID thid, int* stat, unsigned* timeout);
int sceKernelDelayThread(unsigned usec);
int sceKernelGetThreadId(void);
int sceKernelCreateLwMutex(void* work, const char* name, unsigned attr, int init_count, const void* opt);
int sceKernelLockLwMutex(void* work, int count, unsigned* timeout);
int sceKernelUnlockLwMutex(void* work, int count);
int sceKernelDeleteLwMutex(void* work);
SceUID sceKernelCreateSema(const char* name, unsigned attr, int init, int max, void* opt);
int sceKernelWaitSema(SceUID sema, int signal, unsigned* timeout);
int sceKernelSignalSema(SceUID sema, int signal);
int sceKernelDeleteSema(SceUID sema);
SceUID sceIoOpen(const char* path, int flags, int mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void* buf, unsigned size);
int sceIoWrite(SceUID fd, const void* buf, unsigned size);
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence);
int sceIoRemove(const char* path);
int sceIoMkdir(const char* path, int mode);
int sceDisplaySetFrameBuf(const SceDisplayFrameBuf* fb, int sync);
int sceDisplayGetFrameBuf(SceDisplayFrameBuf* fb, int sync);
int sceDisplayWaitVblankStart(void);
int sceCtrlSetSamplingMode(int mode);
int sceCtrlPeekBufferPositive(int port, SceCtrlData* data, int count);
void vitaTestReport(unsigned index, unsigned value);

#define R(i, v) vitaTestReport((i), (unsigned)(v))
#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400

static int g_shared;
static unsigned long long g_mutex_work[4];  /* SceKernelLwMutexWork : 32 octets */
static SceUID g_ping, g_pong;
static char g_log[16];
static int g_log_len;

static int worker(unsigned arglen, void* argp) {
    const unsigned* args = (const unsigned*)argp;
    if (arglen != 8) return -1;
    for (unsigned i = 0; i < args[1]; ++i) {
        sceKernelLockLwMutex(g_mutex_work, 1, 0);
        const int v = g_shared;
        sceKernelDelayThread(0);  /* cède la main dans la section critique */
        g_shared = v + 1;
        sceKernelUnlockLwMutex(g_mutex_work, 1);
    }
    return 1000 + (int)args[0];
}

static int ponger(unsigned arglen, void* argp) {
    (void)arglen; (void)argp;
    for (int i = 0; i < 5; ++i) {
        sceKernelWaitSema(g_ping, 1, 0);
        g_log[g_log_len++] = 'B';
        sceKernelSignalSema(g_pong, 1);
    }
    sceKernelExitDeleteThread(7);
    return 0;
}

static int returner(unsigned arglen, void* argp) {
    (void)argp;
    return 70 + (int)arglen;  /* retour simple : équivaut à sceKernelExitThread */
}

static int str_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

int module_start(unsigned args, const void* argp) {
    (void)args; (void)argp;

    /* 1. Blocs mémoire. */
    SceUID block = sceKernelAllocMemBlock("test", 0x0C20D060, 0x10000, 0);
    R(0, block > 0);
    unsigned* base = 0;
    sceKernelGetMemBlockBase(block, (void**)&base);
    unsigned sum = 0;
    for (unsigned i = 0; i < 0x4000; ++i) base[i] = i * 3;
    for (unsigned i = 0; i < 0x4000; ++i) sum += base[i];
    R(1, sum);
    R(2, sceKernelFreeMemBlock(block));
    R(3, sceKernelFreeMemBlock(block) < 0);

    /* 2. Threads et mutex léger. */
    sceKernelCreateLwMutex(g_mutex_work, "compteur", 0, 0, 0);
    SceUID workers[3];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned wargs[2] = {i, 200};
        workers[i] = sceKernelCreateThread("worker", worker, 0x10000100, 0x4000, 0, 0, 0);
        sceKernelStartThread(workers[i], sizeof(wargs), wargs);  /* copiés sur la pile du thread */
    }
    int status_sum = 0;
    for (int i = 0; i < 3; ++i) {
        int st = -1;
        sceKernelWaitThreadEnd(workers[i], &st, 0);
        status_sum += st;
    }
    R(4, status_sum);
    R(5, g_shared);
    sceKernelDeleteLwMutex(g_mutex_work);

    /* 3. Ping-pong par sémaphores. */
    g_ping = sceKernelCreateSema("ping", 0, 0, 1, 0);
    g_pong = sceKernelCreateSema("pong", 0, 0, 1, 0);
    SceUID pong_thread = sceKernelCreateThread("ponger", ponger, 0x10000100, 0x4000, 0, 0, 0);
    sceKernelStartThread(pong_thread, 0, 0);
    for (int i = 0; i < 5; ++i) {
        g_log[g_log_len++] = 'A';
        sceKernelSignalSema(g_ping, 1);
        sceKernelWaitSema(g_pong, 1, 0);
    }
    R(6, str_eq(g_log, "ABABABABAB", 10) && g_log_len == 10);
    R(7, sceKernelWaitThreadEnd(pong_thread, 0, 0) < 0);  /* supprimé par sceKernelExitDeleteThread */
    R(8, sceKernelSignalSema(g_ping, 2) < 0);              /* dépasse le maximum */
    sceKernelDeleteSema(g_ping);
    sceKernelDeleteSema(g_pong);

    /* 4. Temps. */
    const long long t0 = sceKernelGetProcessTimeWide();
    const unsigned low0 = sceKernelGetProcessTimeLow();
    sceKernelDelayThread(3000);
    R(9, sceKernelGetProcessTimeWide() - t0 >= 3000);
    R(10, sceKernelGetProcessTimeLow() - low0 >= 3000);
    R(11, sceKernelGetThreadId() > 0);

    /* 5. Fichiers. */
    R(12, sceIoMkdir("ux0:data/ft", 0777));
    SceUID fd = sceIoOpen("ux0:/data/ft/test.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    R(13, sceIoWrite(fd, "Bonjour Vita", 12));
    sceIoClose(fd);
    fd = sceIoOpen("ux0:data/ft/test.txt", SCE_O_RDONLY, 0);
    R(14, (unsigned)sceIoLseek(fd, 8, 0));
    char buf[8] = {0};
    R(15, sceIoRead(fd, buf, 8) == 4 && str_eq(buf, "Vita", 4));
    R(16, (unsigned)sceIoLseek(fd, 0, 2));
    R(17, sceIoClose(fd));
    R(18, sceIoClose(fd));                                /* déjà fermé */
    R(19, sceIoRemove("ux0:data/ft/test.txt"));
    R(20, sceIoOpen("ux0:data/ft/absent.txt", SCE_O_RDONLY, 0));
    R(21, sceIoOpen("inconnu0:fichier", SCE_O_RDONLY, 0));
    R(22, sceIoOpen("ux0:../../etc/passwd", SCE_O_RDONLY, 0));

    /* 6. Console. */
    static const char msg[] = "Bonjour depuis la Vita !\n";
    R(23, sceIoWrite(sceKernelGetStdout(), msg, sizeof(msg) - 1));

    /* 7. Commandes (état fixé par le test avant l'exécution). */
    R(24, sceCtrlSetSamplingMode(1));
    SceCtrlData pad[2];
    R(25, sceCtrlPeekBufferPositive(0, pad, 2));
    R(26, pad[1].buttons);
    R(27, pad[0].lx | (pad[0].ly << 8) | (pad[0].rx << 16) | ((unsigned)pad[0].ry << 24));

    /* 8. Affichage : un dégradé dans un tampon en mémoire vidéo. */
    SceUID vram = sceKernelAllocMemBlock("fb", 0x09408060, 0x200000, 0);
    unsigned* fb = 0;
    sceKernelGetMemBlockBase(vram, (void**)&fb);
    for (unsigned y = 0; y < 544; ++y)
        for (unsigned x = 0; x < 960; ++x) fb[y * 960 + x] = 0xFF330000u | ((y & 0xFF) << 8) | (x & 0xFF);
    SceDisplayFrameBuf desc = {sizeof(SceDisplayFrameBuf), fb, 960, 0, 960, 544};
    R(28, sceDisplaySetFrameBuf(&desc, 1));
    SceDisplayFrameBuf got = {sizeof(SceDisplayFrameBuf), 0, 0, 0, 0, 0};
    sceDisplayGetFrameBuf(&got, 1);
    R(29, got.width == 960 && got.height == 544 && got.base == fb);
    desc.size = 12;
    R(30, sceDisplaySetFrameBuf(&desc, 1) < 0);
    sceDisplayWaitVblankStart();
    sceDisplayWaitVblankStart();

    /* 9. Retour simple d'une fonction de thread. */
    SceUID rt = sceKernelCreateThread("returner", returner, 0x10000100, 0x4000, 0, 0, 0);
    sceKernelStartThread(rt, 5, (void*)"abcde");
    int rstat = 0;
    sceKernelWaitThreadEnd(rt, &rstat, 0);
    R(31, rstat);

    sceKernelExitProcess(123);
    return 0;
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
