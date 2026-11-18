# FT Vita : fondations d'un émulateur PS Vita pour Android

Ce dépôt ne contient encore que les **fondations** : il charge les exécutables
Vita non chiffrés (eboot.bin de homebrew au format FSELF, ELF Vita relogeables),
exécute leur code (ARM, Thumb-2, flottants, NEON) et émule une première série
de fonctions système (mémoire, threads, synchronisation, fichiers, affichage
d'un tampon d'image, manette). Pas encore de jeux : il manque notamment le GPU
(SceGxm), l'audio et une grande partie des bibliothèques système. L'architecture permet ensuite de
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
    cpu/interpreter.h     Interpréteur ARMv7 de référence : ARM, Thumb-2, VFPv3 et NEON
    loader/elf_loader.h   Chargeur : FSELF (compressé ou non), ELF Vita relogeable, imports par NID
    loader/relocation.h   Relocations Vita (PT_SCE_RELA) et ARM
    hle/kernel.h          Émulation haut niveau (NID -> C++) : ordonnanceur de threads, objets
  src/hle/
    nids.h                NID émulés, avec leur source dans vita-headers
    modules_kernel.cpp    Processus, blocs mémoire, threads, mutex légers, sémaphores, temps
    modules_io.cpp        Fichiers (ux0:, app0:... associés à des dossiers de l'hôte)
    modules_display_ctrl.cpp  Tampon d'image, synchronisation verticale, manette
    gpu/renderer.h        Interface de rendu (NullRenderer, Vulkan à venir)
  tests/                  Tests unitaires et de bout en bout
    guest/                Programmes C exécutés nativement ET dans l'émulateur
      homebrew/           Homebrew de test empaqueté au format Vita
    tools/make_vita_elf.py  Équivalent minimal de vita-elf-create + vita-make-fself
```

### Choix techniques

| Composant de la Vita              | Approche retenue                                              |
|-----------------------------------|---------------------------------------------------------------|
| Calcul flottant de la Vita        | Règles ARM émulées au-dessus de l'IEEE de l'hôte (`core/src/cpu/fp.h`) : FPSCR de départ de la Vita (dénormaux à zéro, NaN par défaut), propagation des NaN, modes d'arrondi, drapeaux d'exception. NEON suit toujours les réglages standard ARM. |
| CPU ARM Cortex-A9 (ARMv7, Thumb-2, NEON) | `CpuBackend` abstrait. Interpréteur pour la mise au point, puis [dynarmic](https://github.com/lioncash/dynarmic) (recompilation ARMv7 vers ARM64) pour les performances. |
| 512 Mo de RAM, adresses 32 bits   | Réservation de 4 Gio côté hôte : adresse invitée = `base + offset`. |
| Firmware et modules système       | HLE : les imports (NID) sont remplacés par `svc #n` et implémentés en C++. Aucun fichier du firmware n'est nécessaire. Les NID viennent de la base `db/` de vita-headers. |
| Threads (CPU à 3 cœurs utilisateur) | Un seul cœur émulé : tourniquet entre les threads prêts, attentes bloquantes (délai, sémaphore, mutex, fin de thread, VBlank). Priorités et délais d'expiration pas encore gérés. |
| Affichage                         | En attendant le GPU, le tampon de `sceDisplaySetFrameBuf` est copié dans la fenêtre Android à chaque VBlank. |
| GPU SGX543MP4+ / API SceGxm       | Traduction vers Vulkan, avec recompilation des shaders GXP vers SPIR-V. |
| Écran 960×544                     | `SurfaceView` avec mise à l'échelle.                          |

## Compiler

**Cœur et tests (Linux / macOS) :**

```bash
cmake -S core -B core/build && cmake --build core/build -j && ctest --test-dir core/build
```

Si `clang` et `ld.lld` sont installés, les tests compilent aussi
`core/tests/guest/programs.c` pour un Cortex-A9, en ARM et en Thumb-2, avec
`-O0` à `-O3`, sans flottants, avec VFP ou avec NEON. Chaque programme exécuté
dans l'émulateur doit donner exactement le même résultat que le même code
compilé nativement, au bit près pour les flottants : tri, CRC32, tables de
sauts, calcul 64 bits, matrices 4×4, vecteurs 3D, Mandelbrot, physique, etc.
Un second homebrew (`tests/guest/system`) appelle les fonctions système par
leurs vrais NID : threads concurrents protégés par un mutex léger, ping-pong
par sémaphores, fichiers dans un `ux0:` temporaire, console, manette et
dégradé affiché via `sceDisplaySetFrameBuf`.

Un homebrew de test (`tests/guest/homebrew`) est empaqueté par
`tests/tools/make_vita_elf.py` en ELF Vita relogeable puis en FSELF (compressé
ou non), comme le ferait le VitaSDK, puis chargé, y compris à une autre adresse
que celle prévue pour exercer les relocations.

Les tests NEON (`programs_neon.c`) utilisent les intrinsics de `arm_neon.h`
côté Vita et une référence scalaire côté natif. `tests/fp_tests.cpp` vérifie
les règles flottantes ARM sur des valeurs calculées à la main.

**Application Android :** il faut le SDK Android et le NDK. Ouvrez le projet
dans Android Studio, ou lancez :

```bash
./gradlew assembleDebug
```

Seules les architectures 64 bits sont ciblées (`arm64-v8a`, `x86_64`), car la
réservation de 4 Gio d'espace d'adressage l'exige.

## Feuille de route

1. **CPU** : ~~Thumb-2~~, ~~VFPv3~~, ~~NEON~~, ~~réglages flottants de la Vita~~ (faits). Restent le moniteur exclusif multicœur et les performances (brancher dynarmic).
2. **Chargeur** : ~~FSELF, `ET_SCE_RELEXEC`, relocations, imports~~ (faits). Restent les imports de variables, les exports, les formats de relocation compacts (2 à 9) des SDK récents, l'ouverture des paquets VPK, et une validation sur de vrais binaires du VitaSDK.
3. **HLE** : premières fonctions faites (mémoire, threads, mutex légers, sémaphores, fichiers, tampon d'image, manette). Restent les priorités et délais d'expiration, les dossiers (sceIoDopen), les événements et mutex complets, l'audio (SceAudio), la date (SceRtc), les boîtes de dialogue (SceCommonDialog), et l'initialisation complète de la libc du VitaSDK.
4. **GPU** : `VulkanRenderer`, puis SceGxm et le traducteur de shaders.
5. **Interface** : bibliothèque de jeux, contrôles tactiles à l'écran, réglages.

## Aspects légaux

Ce projet ne contient et ne contiendra aucun code, firmware ni clé de Sony.
Les jeux commerciaux sont distribués sous forme de SELF chiffrés : le chargeur
les refuse. Pour lancer ses propres jeux, l'utilisateur devrait les extraire de
sa propre console. En attendant, testez avec des homebrews compilés avec le
[VitaSDK](https://vitasdk.org).
