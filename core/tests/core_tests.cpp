// Tests du cœur, sans dépendance externe. Lancement : ctest ou ./vita_core_tests
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "guest/homebrew/homebrew.h"
#include "guest/programs.h"

#include "vita/cpu/interpreter.h"
#include <unistd.h>

#include "vita/emulator.h"

using namespace vita;

int g_failures = 0;
void run_fp_tests();
void run_loader_tests();

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

namespace {

constexpr GuestAddr kBase = 0x81000000;
constexpr u32 kStubOffset = 0x100;

// Construit un ELF ARM 32 bits minimal contenant `code` à l'adresse kBase.
std::vector<u8> make_elf(const std::vector<u32>& code, u32 mem_size = 0x200) {
    constexpr u32 kCodeOffset = 0x80;
    std::vector<u8> elf(kCodeOffset + code.size() * 4, 0);
    auto put16 = [&](size_t off, u16 v) { std::memcpy(&elf[off], &v, 2); };
    auto put32 = [&](size_t off, u32 v) { std::memcpy(&elf[off], &v, 4); };

    std::memcpy(elf.data(), "\x7F" "ELF", 4);
    elf[4] = 1;  // ELFCLASS32
    elf[5] = 1;  // little-endian
    elf[6] = 1;  // version
    put16(16, 2);       // ET_EXEC
    put16(18, 40);      // EM_ARM
    put32(20, 1);
    put32(24, kBase);   // e_entry
    put32(28, 52);      // e_phoff
    put16(40, 52);      // e_ehsize
    put16(42, 32);      // e_phentsize
    put16(44, 1);       // e_phnum

    const size_t ph = 52;
    put32(ph + 0, 1);                                 // PT_LOAD
    put32(ph + 4, kCodeOffset);                       // p_offset
    put32(ph + 8, kBase);                             // p_vaddr
    put32(ph + 16, static_cast<u32>(code.size() * 4)); // p_filesz
    put32(ph + 20, mem_size);                         // p_memsz

    std::memcpy(&elf[kCodeOffset], code.data(), code.size() * 4);
    return elf;
}

void test_memory() {
    Memory mem;
    CHECK(mem.init());
    CHECK(!mem.is_mapped(0x10000));
    CHECK(mem.map(0x10000, 0x2000));
    CHECK(mem.is_mapped(0x10000, 0x2000));
    CHECK(!mem.is_mapped(0x11FFE, 4));  // déborde sur une page non mappée

    mem.write<u32>(0x10010, 0xDEADBEEF);
    CHECK(mem.read<u32>(0x10010) == 0xDEADBEEF);
    CHECK(mem.read<u8>(0x10010) == 0xEF);  // little-endian
    CHECK(!mem.take_fault());

    CHECK(mem.read<u32>(0x50000) == 0);
    GuestAddr fault = 0;
    CHECK(mem.take_fault(&fault));
    CHECK(fault == 0x50000);

    mem.unmap(0x10000, 0x2000);
    CHECK(!mem.is_mapped(0x10000));
}

void test_loader_rejects() {
    Memory mem;
    CHECK(mem.init());
    CHECK(load_executable(mem, {'S', 'C', 'E', 0, 1, 2, 3}).status == LoadStatus::NotElf);  // SELF tronqué
    CHECK(load_executable(mem, {1, 2, 3, 4}).status == LoadStatus::NotElf);

    auto elf = make_elf({0xE1A00000});
    elf[18] = 3;  // EM_386 : mauvaise architecture
    CHECK(load_executable(mem, elf).status == LoadStatus::WrongArch);
}

void test_flags() {
    Memory mem;
    CHECK(mem.init());
    CHECK(mem.map(kBase, 0x1000));
    const u32 program[] = {
        0xE3A00001,  // MOV  r0, #1
        0xE2500002,  // SUBS r0, r0, #2   -> -1 : N=1, C=0 (emprunt)
        0xE3E01000,  // MVN  r1, #0       -> 0xFFFFFFFF
        0xE2911001,  // ADDS r1, r1, #1   -> 0 : Z=1, C=1
        0xE1A02101,  // MOV  r2, r1, LSL #2
    };
    mem.copy_to_guest(kBase, program, sizeof(program));

    Interpreter cpu(mem);
    cpu.state().pc() = kBase;
    CHECK(cpu.run(2) == CpuExit::BudgetExhausted);
    CHECK(cpu.state().r[0] == 0xFFFFFFFF);
    CHECK(cpu.state().cpsr & CpuState::kFlagN);
    CHECK(!(cpu.state().cpsr & CpuState::kFlagC));

    CHECK(cpu.run(3) == CpuExit::BudgetExhausted);
    CHECK(cpu.state().r[1] == 0);
    CHECK(cpu.state().cpsr & CpuState::kFlagZ);
    CHECK(cpu.state().cpsr & CpuState::kFlagC);
    CHECK(cpu.state().pc() == kBase + 20);

    // Une instruction non définie (UDF) doit arrêter proprement l'exécution.
    mem.write<u32>(cpu.state().pc(), 0xE7F000F0);
    CHECK(cpu.run(1) == CpuExit::Undefined);
    CHECK(!cpu.last_error().empty());
}

// Programme complet : calcul, pile, conditions, BL vers une fonction système émulée.
void test_end_to_end() {
    const std::vector<u32> program = {
        0xE3A00005,  // 0x00 MOV   r0, #5
        0xE2800003,  // 0x04 ADD   r0, r0, #3
        0xE3011234,  // 0x08 MOVW  r1, #0x1234
        0xE34A1BCD,  // 0x0C MOVT  r1, #0xABCD
        0xE52D0004,  // 0x10 STR   r0, [sp, #-4]!
        0xE49D2004,  // 0x14 LDR   r2, [sp], #4
        0xE3520008,  // 0x18 CMP   r2, #8
        0x13A03001,  // 0x1C MOVNE r3, #1
        0x03A03002,  // 0x20 MOVEQ r3, #2
        0xEB000035,  // 0x24 BL    stub (0x100) -> sceKernelExitProcess(r0)
        0xEAFFFFFE,  // 0x28 B     .  (ne doit pas être atteint)
    };

    Emulator emu;
    CHECK(emu.init());
    LoadResult r = emu.load(make_elf(program));
    CHECK(r.ok());
    CHECK(emu.state() == EmuState::Loaded);
    CHECK(emu.kernel().patch_import(kBase + kStubOffset, 0x7595D9AA));

    CHECK(emu.run_slice(1000) == CpuExit::Halted);
    CpuState& s = emu.cpu_state();
    CHECK(s.r[0] == 8);
    CHECK(s.r[1] == 0xABCD1234);
    CHECK(s.r[2] == 8);
    CHECK(s.r[3] == 2);
    CHECK(s.sp() == Emulator::kStackTop);
    CHECK(s.lr() == kBase + 0x28);
    CHECK(s.tpidruro == Emulator::kTlsAddr);
    CHECK(emu.state() == EmuState::Stopped);
    CHECK(emu.status().find("code 8") != std::string::npos);
}

// Même programme, mais sur le thread d'émulation.
void test_threaded_run() {
    Emulator emu;
    CHECK(emu.init());
    CHECK(emu.load(make_elf({0xE3A0002A, 0xEB00003D})).ok());  // MOV r0,#42 ; BL 0x100
    CHECK(emu.kernel().patch_import(kBase + kStubOffset, 0x7595D9AA));
    emu.start();
    for (int i = 0; i < 200 && emu.state() == EmuState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    emu.stop();
    CHECK(emu.state() == EmuState::Stopped);
    CHECK(emu.cpu_state().r[0] == 42);
}

// Programmes C compilés pour Cortex-A9 (ARM et Thumb-2) : chaque résultat
// doit être identique à celui du même code compilé nativement.
void test_guest_programs() {
#ifndef VITA_GUEST_DIR
    std::printf("Programmes invités ignorés (clang ou ld.lld introuvable)\n");
#else
    constexpr int kWithFloat = VITA_TEST_COUNT + VITA_FLOAT_TEST_COUNT;
    constexpr int kTotal = kWithFloat + VITA_NEON_TEST_COUNT;
    u32 expected[kTotal];
    for (int i = 0; i < VITA_TEST_COUNT; ++i) expected[i] = run_test(i);
    for (int i = 0; i < VITA_FLOAT_TEST_COUNT; ++i) expected[VITA_TEST_COUNT + i] = run_float_test(i);
    for (int i = 0; i < VITA_NEON_TEST_COUNT; ++i) expected[kWithFloat + i] = run_neon_test(i);
    auto name_of = [](int i) {
        if (i < VITA_TEST_COUNT) return test_name(i);
        if (i < kWithFloat) return float_test_name(i - VITA_TEST_COUNT);
        return neon_test_name(i - kWithFloat);
    };

    constexpr u32 kExitNid = 0x7595D9AA;    // sceKernelExitProcess
    constexpr u32 kReportNid = 0xFFFF0001;  // fonction propre aux tests

    for (const char* variant : {"thumb_O0", "thumb_O2", "thumb_Os", "arm_O0", "arm_O2", "arm_Os",
                                "thumb_O0_vfp", "thumb_O2_vfp", "arm_O2_vfp", "thumb_O0_neon", "thumb_O2_neon",
                                "thumb_O3_neon", "arm_O2_neon", "arm_O3_neon"}) {
        const int count = std::strstr(variant, "_neon") ? kTotal
                          : std::strstr(variant, "_vfp") ? kWithFloat
                                                         : VITA_TEST_COUNT;
        Emulator emu;
        CHECK(emu.init());
        const std::string path = std::string(VITA_GUEST_DIR) + "/" + variant + ".elf";
        if (!emu.load(path).ok()) {
            std::fprintf(stderr, "ÉCHEC : impossible de charger %s\n", path.c_str());
            ++g_failures;
            continue;
        }

        std::vector<u32> got(count, 0xDEADDEAD);
        emu.kernel().register_function(kReportNid, "test_report", [&](CpuState& s) {
            if (s.r[0] < got.size()) got[s.r[0]] = s.r[1];
        });
        CHECK(emu.kernel().bind_svc(kExitNid) == 0);
        CHECK(emu.kernel().bind_svc(kReportNid) == 1);

        const auto start = std::chrono::steady_clock::now();
        const CpuExit exit = emu.run_slice(200'000'000);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        if (exit != CpuExit::Halted) {
            std::fprintf(stderr, "ÉCHEC [%s] : arrêt inattendu : %s\n", variant, emu.status().c_str());
            ++g_failures;
        }
        int ok = 0;
        for (int i = 0; i < count; ++i) {
            if (got[i] == expected[i]) {
                ++ok;
            } else {
                std::fprintf(stderr, "ÉCHEC [%s] %s : obtenu 0x%08x, attendu 0x%08x\n", variant, name_of(i),
                             got[i], expected[i]);
                ++g_failures;
            }
        }
        std::printf("  %-13s %2d/%d programmes corrects (%.1f ms)\n", variant, ok, count, ms);
    }
#endif
}

// Homebrew au format Vita : ELF relogeable (ET_SCE_RELEXEC) et FSELF, avec
// sceModuleInfo, imports par NID et relocations, chargé puis exécuté.
void test_vita_homebrew() {
#if !defined(VITA_GUEST_DIR) || !defined(VITA_HOMEBREW_TESTS)
    std::printf("Homebrew Vita ignoré (clang, ld.lld ou Python introuvable)\n");
#else
    u32 expected[HB_VALUE_COUNT];
    u32 checksum = 0;
    for (int i = 0; i < HB_VALUE_COUNT; ++i) {
        expected[i] = hb_value(i);
        checksum = checksum * 31 + expected[i];
    }
    const std::string code = "code " + std::to_string(checksum & 0x7FFFFFFF) + ")";

    struct Case {
        const char* file;
        bool fself;
        bool occupy;  // occupe l'adresse prévue pour forcer le chargement ailleurs
    };
    const Case cases[] = {
        {"hb_exit.velf", false, false}, {"hb_exit.self", true, false},   {"hb_exit_z.self", true, false},
        {"hb_exit_z.self", true, true}, {"hb_return_z.self", true, false}, {"hb_return.velf", false, true},
    };
    for (const Case& c : cases) {
        Emulator emu;
        CHECK(emu.init());
        if (c.occupy) CHECK(emu.memory().map(0x81000000, 0x20000));
        std::vector<u32> got(103, 0xDEADDEAD);
        emu.kernel().register_function(0xFFFF0001, "vitaTestReport", [&](CpuState& s) {
            if (s.r[0] < got.size()) got[s.r[0]] = s.r[1];
        });
        const LoadResult r = emu.load(std::string(VITA_GUEST_DIR) + "/" + c.file);
        if (!r.ok()) {
            std::fprintf(stderr, "ÉCHEC [%s] chargement : %s\n", c.file, r.message.c_str());
            ++g_failures;
            continue;
        }
        const LoadedModule& m = emu.module();
        CHECK(m.name == "vita_test_homebrew");
        CHECK(m.entry_is_module_start);
        CHECK(m.imports.size() == 3);
        CHECK(m.from_self == c.fself);
        CHECK(m.relocated == c.occupy);

        const CpuExit exit = emu.run_slice(10'000'000);
        int ok = 0;
        for (int i = 0; i < HB_VALUE_COUNT; ++i) {
            if (got[i] == expected[i]) ++ok;
            else std::fprintf(stderr, "ÉCHEC [%s] valeur %d : 0x%08x au lieu de 0x%08x\n", c.file, i, got[i], expected[i]);
        }
        g_failures += HB_VALUE_COUNT - ok;
        CHECK(exit == CpuExit::Halted);
        CHECK(got[100] == 0 && got[101] == 0);  // module_start(0, NULL)
        CHECK(got[102] == 0);                   // fonction non implémentée : renvoie 0
        CHECK(emu.kernel().unimplemented_calls() == 1);
        CHECK(emu.state() == EmuState::Stopped);
        if (emu.status().find(code) == std::string::npos) {
            std::fprintf(stderr, "ÉCHEC [%s] état final : %s\n", c.file, emu.status().c_str());
            ++g_failures;
        }
        std::printf("  %-17s %d/%d valeurs correctes%s\n", c.file, ok, HB_VALUE_COUNT, c.occupy ? " (relogé)" : "");
    }

    // Un SELF chiffré doit être refusé proprement.
    Emulator emu;
    CHECK(emu.init());
    CHECK(emu.load(std::string(VITA_GUEST_DIR) + "/hb_exit_encrypted.self").status == LoadStatus::EncryptedSelf);
#endif
}

// Modules système : un homebrew appelle mémoire, threads, synchronisation,
// temps, fichiers, console, commandes et affichage par leurs vrais NID.
void test_system_modules() {
#if !defined(VITA_GUEST_DIR) || !defined(VITA_HOMEBREW_TESTS)
    std::printf("Modules système ignorés (clang, ld.lld ou Python introuvable)\n");
#else
    namespace fs = std::filesystem;
    const fs::path ux0 = fs::temp_directory_path() / ("ft_vita_ux0_" + std::to_string(::getpid()));
    fs::remove_all(ux0);
    fs::create_directories(ux0 / "data");

    Emulator emu;
    CHECK(emu.init(std::make_unique<NullRenderer>(true)));
    emu.mount("ux0:", ux0.string());
    InputState input;
    input.buttons = 0x4010;  // CROIX + HAUT
    input.lx = 10; input.ly = 20; input.rx = 200; input.ry = 250;
    emu.set_input(input);

    std::vector<u32> got(32, 0xDEADDEAD);
    emu.kernel().register_function(0xFFFF0001, "vitaTestReport", [&](CpuState& s) {
        if (s.r[0] < got.size()) got[s.r[0]] = s.r[1];
    });
    const LoadResult r = emu.load(std::string(VITA_GUEST_DIR) + "/system.self");
    CHECK(r.ok());
    CHECK(emu.module().imports.size() == 34);

    const auto start = std::chrono::steady_clock::now();
    CHECK(emu.run_slice(200'000'000) == CpuExit::Halted);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    u32 block_sum = 0;
    for (u32 i = 0; i < 0x4000; ++i) block_sum += i * 3;
    const char* const msg = "Bonjour depuis la Vita !\n";
    const u32 expected[32] = {
        1, block_sum, 0, 1,                        // blocs mémoire
        3003, 600,                                 // threads + mutex léger
        1, 1, 1,                                   // sémaphores
        1, 1, 1,                                   // temps, identifiant de thread
        0, 12, 8, 1, 12, 0, sce_error::kBadFile, 0,  // fichiers
        sce_error::kNoEntry, sce_error::kNoDevice, sce_error::kInvalid,
        static_cast<u32>(std::strlen(msg)),        // console
        0, 2, 0x4010, 10u | (20u << 8) | (200u << 16) | (250u << 24),  // commandes
        0, 1, 1,                                   // affichage
        75,                                        // retour d'un thread
    };
    static const char* const kLabels[32] = {
        "alloc", "somme du bloc", "free", "double free", "statuts des threads", "compteur protégé", "ping-pong",
        "thread supprimé", "sémaphore pleine", "temps (64 bits)", "temps (32 bits)", "id de thread", "mkdir", "write",
        "lseek", "read", "lseek fin", "close", "double close", "remove", "fichier absent", "périphérique inconnu",
        "chemin « .. »", "console", "mode d'échantillonnage", "nombre d'échantillons", "boutons", "sticks",
        "SetFrameBuf", "GetFrameBuf", "SetFrameBuf invalide", "retour de thread"};
    int ok = 0;
    for (int i = 0; i < 32; ++i) {
        if (got[i] == expected[i]) {
            ++ok;
        } else {
            std::fprintf(stderr, "ÉCHEC [système] %s : 0x%08x au lieu de 0x%08x\n", kLabels[i], got[i], expected[i]);
            ++g_failures;
        }
    }
    CHECK(emu.status().find("code 123") != std::string::npos);
    CHECK(emu.kernel().unimplemented_calls() == 0);
    CHECK(emu.kernel().stdout_text() == msg);
    CHECK(fs::is_directory(ux0 / "data" / "ft"));
    CHECK(!fs::exists(ux0 / "data" / "ft" / "test.txt"));

    // Image : le dégradé dessiné par le programme a été présenté.
    const auto* renderer = static_cast<NullRenderer*>(emu.renderer());
    CHECK(renderer->frames() >= 1);
    CHECK(renderer->last_width() == 960 && renderer->last_height() == 544);
    if (renderer->last_frame().size() == 960u * 544u) {
        for (const auto [x, y] : {std::pair<u32, u32>{0, 0}, {959, 543}, {300, 200}})
            CHECK(renderer->last_frame()[y * 960 + x] == (0xFF330000u | ((y & 0xFF) << 8) | (x & 0xFF)));
    }
    fs::remove_all(ux0);
    std::printf("  system.self       %d/32 résultats corrects, %zu threads créés (%.1f ms)\n", ok,
                emu.kernel().thread_count(), ms);
#endif
}

}  // namespace

int main() {
    test_memory();
    test_loader_rejects();
    test_flags();
    test_end_to_end();
    test_threaded_run();
    run_fp_tests();
    run_loader_tests();
    test_guest_programs();
    test_vita_homebrew();
    test_system_modules();

    if (g_failures) {
        std::fprintf(stderr, "%d vérification(s) en échec\n", g_failures);
        return 1;
    }
    std::printf("Tous les tests du cœur passent.\n");
    return 0;
}
