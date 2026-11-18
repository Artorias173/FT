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
    UnsupportedType,
    WrongArch,
    MapFailed,
    BadRelocation,
    BadModuleInfo,
};

struct LoadedSegment {
    GuestAddr addr;   // adresse de chargement
    u32 vaddr;        // adresse prévue dans l'ELF
    u32 size;
};

// Fonction importée d'une bibliothèque système, par NID.
struct FunctionImport {
    std::string library;
    u32 library_nid;
    u32 nid;
    GuestAddr stub;  // appel à rediriger vers l'implémentation émulée
};

struct LoadedModule {
    std::string name;
    GuestAddr entry = 0;
    bool entry_is_module_start = false;  // module_start(args, argp) plutôt qu'un _start nu
    std::vector<LoadedSegment> segments;
    std::vector<FunctionImport> imports;
    u32 variable_imports = 0;   // imports de variables, pas encore résolus
    GuestAddr tls_start = 0;
    u32 tls_filesz = 0, tls_memsz = 0;
    bool from_self = false;
    bool relocated = false;     // chargé ailleurs qu'à l'adresse prévue
};

struct LoadResult {
    LoadStatus status = LoadStatus::FileError;
    std::string message;
    LoadedModule module;

    bool ok() const { return status == LoadStatus::Ok; }
};

// Charge un exécutable : ELF ARM brut (ET_EXEC), ELF Vita (ET_SCE_EXEC ou
// ET_SCE_RELEXEC, avec relocations et imports), éventuellement dans une
// enveloppe SELF non chiffrée (FSELF, segments compressés ou non).
LoadResult load_executable(Memory& memory, const std::vector<u8>& data);
LoadResult load_executable_file(Memory& memory, const std::string& path);

}  // namespace vita
