# FT Vita : fondations d'un émulateur PS Vita pour Android

Ce dépôt ne contient encore que les **fondations** : il exécute du code C
compilé pour la Vita (ARM et Thumb-2, en calcul entier), mais pas encore de jeux. L'architecture permet ensuite de
remplacer chaque brique (CPU, GPU, HLE) sans toucher aux autres.

## Architecture

```
app/                      Application Android (Kotlin + JNI)
  src/main/java/...       MainActivity (choix du fichier), EmulationActivity (SurfaceView)
  src/main/cpp/           jni_bridge.cpp : pont Kotlin <-> cœur C++
core/                     Cœur d'émulation C++17 portable (Android, Linux)
  include/vita/
    emulator.h            Orchestrateur : cycle de vie, thread d'émulation
    memory/memory.h       Espace d'adressage invité 4 Gio (mmap + mprotect)
    cpu/cpu.h             Interface CpuBackend (interpréteur aujourd'hui, dynarec demain)
    cpu/interpreter.h     Interpréteur ARMv7 de référence : ARM (A32) et Thumb-2 (T32)
    loader/elf_loader.h   Chargeur ELF ARM 32 bits non chiffré
    hle/kernel.h          Émulation haut niveau des modules système (NID -> C++)
    gpu/renderer.h        Interface de rendu (NullRenderer, Vulkan à venir)
  tests/                  Tests unitaires et de bout en bout
    guest/                Programmes C exécutés nativement ET dans l'émulateur
```

### Choix techniques

| Composant de la Vita              | Approche retenue                                              |
|-----------------------------------|---------------------------------------------------------------|
| CPU ARM Cortex-A9 (ARMv7, Thumb-2, NEON) | `CpuBackend` abstrait. Interpréteur pour la mise au point, puis [dynarmic](https://github.com/lioncash/dynarmic) (recompilation ARMv7 vers ARM64) pour les performances. |
| 512 Mo de RAM, adresses 32 bits   | Réservation de 4 Gio côté hôte : adresse invitée = `base + offset`. |
| Firmware et modules système       | HLE : les imports (NID) sont remplacés par `svc #n` et implémentés en C++. Aucun fichier du firmware n'est nécessaire. |
| GPU SGX543MP4+ / API SceGxm       | Traduction vers Vulkan, avec recompilation des shaders GXP vers SPIR-V. |
| Écran 960×544                     | `SurfaceView` avec mise à l'échelle.                          |

## Compiler

**Cœur et tests (Linux / macOS) :**

```bash
cmake -S core -B core/build && cmake --build core/build -j && ctest --test-dir core/build
```

Si `clang` et `ld.lld` sont installés, les tests compilent aussi
`core/tests/guest/programs.c` pour un Cortex-A9, en ARM et en Thumb-2, avec
`-O0`, `-O2` et `-Os`. Chaque programme exécuté dans l'émulateur doit donner
exactement le même résultat que le même code compilé nativement : tri, CRC32,
tables de sauts, champs de bits, calcul 64 bits, pointeurs de fonction, etc.

**Application Android :** il faut le SDK Android et le NDK. Ouvrez le projet
dans Android Studio, ou lancez :

```bash
./gradlew assembleDebug
```

Seules les architectures 64 bits sont ciblées (`arm64-v8a`, `x86_64`), car la
réservation de 4 Gio d'espace d'adressage l'exige.

## Feuille de route

1. **CPU** : ~~Thumb-2~~ (fait), VFP/NEON (calcul flottant, indispensable pour les jeux), moniteur exclusif multicœur. Brancher ensuite dynarmic pour les performances.
2. **Chargeur** : ELF relogeables `ET_SCE_RELEXEC`, résolution des imports et exports (`sceModuleInfo`), format VPK (homebrew).
3. **HLE** : threads (SceThreadmgr), fichiers (SceIofilemgr), affichage (SceDisplay), manettes (SceCtrl), audio (SceAudio).
4. **GPU** : `VulkanRenderer`, puis SceGxm et le traducteur de shaders.
5. **Interface** : bibliothèque de jeux, contrôles tactiles à l'écran, réglages.

## Aspects légaux

Ce projet ne contient et ne contiendra aucun code, firmware ni clé de Sony.
Les jeux commerciaux sont distribués sous forme de SELF chiffrés : le chargeur
les refuse. Pour lancer ses propres jeux, l'utilisateur devrait les extraire de
sa propre console. En attendant, testez avec des homebrews compilés avec le
[VitaSDK](https://vitasdk.org).
