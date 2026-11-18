#include "vita/loader/elf_loader.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include "vita/common/log.h"
#include "vita/memory/memory.h"

namespace vita {

namespace {

// Structures ELF32 définies ici pour ne pas dépendre de <elf.h>.
struct Elf32Ehdr {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct Elf32Phdr {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
};

static_assert(sizeof(Elf32Ehdr) == 52);
static_assert(sizeof(Elf32Phdr) == 32);

constexpr u16 kEtExec = 2;
constexpr u16 kEtSceRelExec = 0xFE04;  // exécutable relogeable propre à la Vita
constexpr u16 kEmArm = 40;
constexpr u32 kPtLoad = 1;

LoadResult fail(LoadStatus status, std::string message) {
    LOG_ERROR("Chargeur : %s", message.c_str());
    LoadResult r;
    r.status = status;
    r.message = std::move(message);
    return r;
}

}  // namespace

LoadResult load_elf(Memory& memory, const std::vector<u8>& data) {
    if (data.size() >= 4 && std::memcmp(data.data(), "SCE\0", 4) == 0) {
        return fail(LoadStatus::EncryptedSelf,
                    "Fichier SELF (eboot.bin signé) : le déchiffrement n'est pas pris en charge. "
                    "Utilisez un ELF homebrew non chiffré.");
    }
    if (data.size() < sizeof(Elf32Ehdr) || std::memcmp(data.data(), "\x7F" "ELF", 4) != 0)
        return fail(LoadStatus::NotElf, "Ce n'est pas un fichier ELF");

    Elf32Ehdr eh;
    std::memcpy(&eh, data.data(), sizeof(eh));

    if (eh.e_ident[4] != 1 || eh.e_ident[5] != 1 || eh.e_machine != kEmArm)
        return fail(LoadStatus::WrongArch, "ELF attendu : ARM 32 bits little-endian");
    if (eh.e_type == kEtSceRelExec)
        return fail(LoadStatus::UnsupportedType,
                    "ELF Vita relogeable (ET_SCE_RELEXEC) : relocations pas encore implémentées");
    if (eh.e_type != kEtExec)
        return fail(LoadStatus::UnsupportedType, "Type d'ELF non pris en charge");
    if (eh.e_phentsize != sizeof(Elf32Phdr) ||
        static_cast<u64>(eh.e_phoff) + static_cast<u64>(eh.e_phnum) * sizeof(Elf32Phdr) > data.size())
        return fail(LoadStatus::NotElf, "Table des segments invalide");

    LoadResult result;
    for (u16 i = 0; i < eh.e_phnum; ++i) {
        Elf32Phdr ph;
        std::memcpy(&ph, data.data() + eh.e_phoff + i * sizeof(Elf32Phdr), sizeof(ph));
        if (ph.p_type != kPtLoad || ph.p_memsz == 0) continue;

        if (ph.p_filesz > ph.p_memsz || static_cast<u64>(ph.p_offset) + ph.p_filesz > data.size())
            return fail(LoadStatus::NotElf, "Segment hors du fichier");
        if (!memory.map(ph.p_vaddr, ph.p_memsz))
            return fail(LoadStatus::MapFailed, "Impossible de mapper un segment");
        // Les pages fraîchement mappées sont à zéro : la partie .bss est déjà prête.
        if (ph.p_filesz && !memory.copy_to_guest(ph.p_vaddr, data.data() + ph.p_offset, ph.p_filesz))
            return fail(LoadStatus::MapFailed, "Copie du segment impossible");

        result.module.segments.push_back({ph.p_vaddr, ph.p_memsz});
        LOG_INFO("Segment chargé : 0x%08x (+0x%x)", ph.p_vaddr, ph.p_memsz);
    }

    if (result.module.segments.empty()) return fail(LoadStatus::NotElf, "Aucun segment chargeable");

    result.status = LoadStatus::Ok;
    result.module.entry = eh.e_entry;
    result.message = "ELF chargé";
    return result;
}

LoadResult load_elf_file(Memory& memory, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(LoadStatus::FileError, "Impossible d'ouvrir " + path);
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return load_elf(memory, data);
}

}  // namespace vita
