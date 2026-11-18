// Tests des relocations : on corrige une instruction puis on l'exécute avec
// l'interpréteur (dont le décodage est validé par de vrais programmes compilés).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vita/cpu/interpreter.h"
#include "vita/loader/relocation.h"
#include "vita/memory/memory.h"

using namespace vita;

extern int g_failures;

#define LD_CHECK(cond)                                                                 \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

namespace {

constexpr GuestAddr kCode = 0x82000000;

struct Fixture {
    Memory mem;
    Interpreter cpu{mem};
    Fixture() {
        mem.init();
        mem.map(kCode, 0x01000000);  // 16 Mio pour tester des branchements lointains
    }
    // Exécute `count` instructions à partir de `pc`.
    void run(GuestAddr pc, bool thumb, u64 count) {
        cpu.state().pc() = pc;
        if (thumb) cpu.state().cpsr |= CpuState::kFlagT; else cpu.state().cpsr &= ~CpuState::kFlagT;
        cpu.run(count);
    }
};

void test_branches() {
    for (const s32 distance : {0x40, -0x40, 0x3F0000, -0x2F0000, 0xFFFFE}) {
        // BL Thumb : addend -4 (biais du PC), comme le calcule vita-elf-create.
        Fixture f;
        const GuestAddr p = kCode + 0x800000, target = p + distance;
        f.mem.write<u16>(p, 0xF000);
        f.mem.write<u16>(p + 2, 0xF800);
        LD_CHECK(apply_relocation(f.mem, kRelThmCall, p, target, static_cast<u32>(-4)));
        f.run(p, true, 1);
        LD_CHECK(f.cpu.state().pc() == target);
        LD_CHECK(f.cpu.state().lr() == ((p + 4) | 1));

        // B.W Thumb (R_ARM_THM_JUMP24).
        f.mem.write<u16>(p, 0xF000);
        f.mem.write<u16>(p + 2, 0xB800);
        LD_CHECK(apply_relocation(f.mem, kRelThmJump24, p, target, static_cast<u32>(-4)));
        f.run(p, true, 1);
        LD_CHECK(f.cpu.state().pc() == target);

        // BL ARM : addend -8.
        const GuestAddr arm_target = p + (distance & ~3);
        f.mem.write<u32>(p, 0xEB000000);
        LD_CHECK(apply_relocation(f.mem, kRelCall, p, arm_target, static_cast<u32>(-8)));
        f.run(p, false, 1);
        LD_CHECK(f.cpu.state().pc() == arm_target);
    }
}

void test_moves_and_data() {
    for (const u32 value : {0x12345678u, 0xFFFF0001u, 0x00008000u, 0x81000ABCu}) {
        Fixture f;
        // MOVW/MOVT ARM (r3).
        f.mem.write<u32>(kCode, 0xE3003000);
        f.mem.write<u32>(kCode + 4, 0xE3403000);
        LD_CHECK(apply_relocation(f.mem, kRelMovwAbsNc, kCode, value - 0x10, 0x10));
        LD_CHECK(apply_relocation(f.mem, kRelMovtAbs, kCode + 4, value - 0x10, 0x10));
        f.run(kCode, false, 2);
        LD_CHECK(f.cpu.state().r[3] == value);

        // MOVW/MOVT Thumb-2 (r5).
        const GuestAddr t = kCode + 0x100;
        f.mem.write<u16>(t, 0xF240); f.mem.write<u16>(t + 2, 0x0500);
        f.mem.write<u16>(t + 4, 0xF2C0); f.mem.write<u16>(t + 6, 0x0500);
        LD_CHECK(apply_relocation(f.mem, kRelThmMovwAbsNc, t, value, 0));
        LD_CHECK(apply_relocation(f.mem, kRelThmMovtAbs, t + 4, value, 0));
        f.run(t, true, 2);
        LD_CHECK(f.cpu.state().r[5] == value);
    }

    Fixture f;
    const GuestAddr p = kCode + 0x200;
    LD_CHECK(apply_relocation(f.mem, kRelAbs32, p, 0x81000000, 0x1235));
    LD_CHECK(f.mem.read<u32>(p) == 0x81001235);
    LD_CHECK(apply_relocation(f.mem, kRelRel32, p, p + 0x100, 4));
    LD_CHECK(f.mem.read<u32>(p) == 0x104);
    f.mem.write<u32>(p, 0x80000000);  // PREL31 : le bit 31 est conservé
    LD_CHECK(apply_relocation(f.mem, kRelPrel31, p, p - 8, 0));
    LD_CHECK(f.mem.read<u32>(p) == 0xFFFFFFF8);
    LD_CHECK(!apply_relocation(f.mem, 99, p, 0, 0));  // type inconnu
}

void push_words(std::vector<u8>& out, std::initializer_list<u32> words) {
    for (u32 w : words) {
        const u8* b = reinterpret_cast<const u8*>(&w);
        out.insert(out.end(), b, b + 4);
    }
}

void test_sce_table() {
    Fixture f;
    // Deux segments : code à kCode, données à kCode + 0x10000.
    const std::vector<RelocSegment> segs = {{kCode}, {kCode + 0x10000}};
    f.mem.write<u32>(kCode + 0x40, 0xE3000000);  // MOVW r0
    f.mem.write<u32>(kCode + 0x44, 0xE3400000);  // MOVT r0

    std::vector<u8> table;
    // Format 0 : ABS32 dans le segment 1 (offset 8) vers segment 0 + 0x20.
    push_words(table, {0u | (0u << 4) | (kRelAbs32 << 8) | (1u << 16), 0x20, 8});
    // Format 0 avec seconde relocation : MOVW à +0x40 puis MOVT à +0x44 (dist2 = 2, soit 4 octets).
    push_words(table, {0u | (1u << 4) | (kRelMovwAbsNc << 8) | (0u << 16) | (kRelMovtAbs << 20) | (2u << 28), 0x1234, 0x40});
    // Format 1 : ABS32 au segment 1 + 0x1004, vers segment 0 + 0x7, offset sur 22 bits.
    const u32 off = 0x1004;
    push_words(table, {1u | (0u << 4) | (kRelAbs32 << 8) | (1u << 16) | ((off & 0xFFF) << 20), (off >> 12) | (0x7u << 10)});
    // Format 0, segment 15 : valeur absolue.
    push_words(table, {0u | (15u << 4) | (kRelAbs32 << 8) | (1u << 16), 0xCAFE0000, 0x10});

    std::string error;
    LD_CHECK(apply_sce_relocations(f.mem, table.data(), table.size(), segs, error));
    LD_CHECK(f.mem.read<u32>(kCode + 0x10000 + 8) == kCode + 0x20);
    LD_CHECK(f.mem.read<u32>(kCode + 0x10000 + 0x1004) == kCode + 7);
    LD_CHECK(f.mem.read<u32>(kCode + 0x10000 + 0x10) == 0xCAFE0000);
    f.run(kCode + 0x40, false, 2);
    LD_CHECK(f.cpu.state().r[0] == kCode + 0x10000 + 0x1234);

    // Erreurs : format compact non pris en charge, segment inexistant, table tronquée.
    std::vector<u8> bad;
    push_words(bad, {5u});
    LD_CHECK(!apply_sce_relocations(f.mem, bad.data(), bad.size(), segs, error));
    LD_CHECK(error.find("Format de relocation 5") != std::string::npos);
    bad.clear();
    push_words(bad, {0u | (7u << 4) | (kRelAbs32 << 8), 0, 0});
    LD_CHECK(!apply_sce_relocations(f.mem, bad.data(), bad.size(), segs, error));
    bad.resize(10);
    LD_CHECK(!apply_sce_relocations(f.mem, bad.data(), bad.size(), segs, error));
}

}  // namespace

void run_loader_tests() {
    test_branches();
    test_moves_and_data();
    test_sce_table();
}
