// Tests du cœur, sans dépendance externe. Lancement : ctest ou ./vita_core_tests
#include <cstdio>
#include <cstring>
#include <vector>

#include "vita/cpu/interpreter.h"
#include "vita/emulator.h"

using namespace vita;

static int g_failures = 0;

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
    CHECK(load_elf(mem, {'S', 'C', 'E', 0, 1, 2, 3}).status == LoadStatus::EncryptedSelf);
    CHECK(load_elf(mem, {1, 2, 3, 4}).status == LoadStatus::NotElf);

    auto elf = make_elf({0xE1A00000});
    elf[18] = 3;  // EM_386 : mauvaise architecture
    CHECK(load_elf(mem, elf).status == LoadStatus::WrongArch);
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

    // Instruction suivante = 0 (ANDEQ r0,r0,r0) ; Z=1 donc elle s'exécute sans effet.
    // Une instruction Thumb doit en revanche s'arrêter proprement.
    cpu.state().cpsr |= CpuState::kFlagT;
    CHECK(cpu.run(1) == CpuExit::Undefined);
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

}  // namespace

int main() {
    test_memory();
    test_loader_rejects();
    test_flags();
    test_end_to_end();
    test_threaded_run();

    if (g_failures) {
        std::fprintf(stderr, "%d vérification(s) en échec\n", g_failures);
        return 1;
    }
    std::printf("Tous les tests du cœur passent.\n");
    return 0;
}
