#pragma once

#include <string>
#include <vector>

#include "vita/common/types.h"

namespace vita {

class Memory;

enum class LoadStatus {
    Ok,
    FileError,
    NotElf,
    EncryptedSelf,    // SELF chiffré : nécessite des clés que l'on ne fournit pas
    UnsupportedType,  // par exemple ET_SCE_RELEXEC (relocations à implémenter)
    WrongArch,
    MapFailed,
};

struct LoadedSegment {
    GuestAddr addr;
    u32 size;
};

struct LoadedModule {
    GuestAddr entry = 0;
    std::vector<LoadedSegment> segments;
};

struct LoadResult {
    LoadStatus status = LoadStatus::FileError;
    std::string message;
    LoadedModule module;

    bool ok() const { return status == LoadStatus::Ok; }
};

// Charge un ELF ARM 32 bits non chiffré (homebrew compilé avec le VitaSDK
// avant l'étape vita-elf-create, ou ELF de test) dans la mémoire invitée.
LoadResult load_elf(Memory& memory, const std::vector<u8>& data);
LoadResult load_elf_file(Memory& memory, const std::string& path);

}  // namespace vita
