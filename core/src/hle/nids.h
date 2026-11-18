#pragma once
// NID des fonctions émulées, relevés dans la base du VitaSDK
// (dépôt vitasdk/vita-headers, fichiers db/360/<module>.yml indiqués).

#include "vita/common/types.h"

namespace vita::nid {

// SceProcessmgr.yml
constexpr u32 sceKernelExitProcess = 0x7595D9AA;
constexpr u32 sceKernelGetStdout = 0xE5AA625C;
constexpr u32 sceKernelGetStderr = 0xFA5E3ADA;

// SceLibKernel.yml
constexpr u32 sceKernelGetProcessTimeWide = 0xB110C123;
constexpr u32 sceKernelGetProcessTimeLow = 0xE9F973B1;
constexpr u32 sceKernelCreateThread = 0xC5C11EE7;
constexpr u32 sceKernelStartThread = 0xF08DE149;
constexpr u32 sceKernelGetThreadId = 0x0FB972F9;
constexpr u32 sceKernelWaitThreadEnd = 0xDDB395A9;
constexpr u32 sceKernelCreateLwMutex = 0xDA6EC8EF;
constexpr u32 sceKernelLockLwMutex = 0x46E7BE7B;
constexpr u32 sceKernelUnlockLwMutex = 0x91FA6614;
constexpr u32 sceKernelTryLockLwMutex = 0xA6A2C915;
constexpr u32 sceKernelDeleteLwMutex = 0x244E76D2;
constexpr u32 sceKernelCreateSema = 0x1BD67366;
constexpr u32 sceKernelWaitSema = 0x0C7B834B;
constexpr u32 sceIoOpen = 0x6C60AC61;
constexpr u32 sceIoLseek = 0x99BA173E;
constexpr u32 sceIoRemove = 0xE20ED0F3;
constexpr u32 sceIoMkdir = 0x9670D39F;

// SceKernelThreadMgr.yml (bibliothèque SceThreadmgr)
constexpr u32 sceKernelExitThread = 0x0C8A38E1;
constexpr u32 sceKernelExitDeleteThread = 0x1D17DECF;
constexpr u32 sceKernelDelayThread = 0x4B675D05;
constexpr u32 sceKernelDeleteThread = 0x1BBDE3D9;
constexpr u32 sceKernelSignalSema = 0xE6B761D1;
constexpr u32 sceKernelDeleteSema = 0xDB32948A;

// SceIofilemgr.yml
constexpr u32 sceIoClose = 0xC70B8886;
constexpr u32 sceIoRead = 0xFDB32293;
constexpr u32 sceIoWrite = 0x34EFD876;

// SceSysmem.yml
constexpr u32 sceKernelAllocMemBlock = 0xB9D5EBDE;
constexpr u32 sceKernelFreeMemBlock = 0xA91E15EE;
constexpr u32 sceKernelGetMemBlockBase = 0xB8EF5818;

// SceDriverUser.yml (enveloppes de SceDisplay) et SceDisplay.yml
constexpr u32 sceDisplaySetFrameBuf = 0x7A410B64;
constexpr u32 sceDisplayGetFrameBuf = 0x42AE6BBC;
constexpr u32 sceDisplayWaitVblankStart = 0x5795E898;

// SceCtrl.yml
constexpr u32 sceCtrlSetSamplingMode = 0xA497B150;
constexpr u32 sceCtrlPeekBufferPositive = 0xA9C3CED6;
constexpr u32 sceCtrlReadBufferPositive = 0x67E7AB83;

}  // namespace vita::nid
