#include "vita/loader/elf_loader.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "vita/common/log.h"
#include "vita/loader/relocation.h"
#include "vita/memory/memory.h"

namespace vita {

namespace {

// --- Formats -----------------------------------------------------------------

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

// En-tête SELF (« SCE\0 »), d'après self.h du VitaSDK.
struct SceHeader {
    u32 magic;
    u32 version;
    u16 sdk_type;
    u16 header_type;
    u32 metadata_offset;
    u64 header_len;
    u64 elf_filesize;
    u64 self_filesize;
    u64 unknown;
    u64 self_offset;
    u64 appinfo_offset;
    u64 elf_offset;
    u64 phdr_offset;
    u64 shdr_offset;
    u64 section_info_offset;
    u64 sceversion_offset;
    u64 controlinfo_offset;
    u64 controlinfo_size;
    u64 padding;
};

struct SegmentInfo {
    u64 offset;
    u64 length;
    u64 compression;  // 1 : brut, 2 : zlib
    u64 encryption;   // 1 : chiffré, 2 : en clair
};

static_assert(sizeof(Elf32Ehdr) == 52);
static_assert(sizeof(Elf32Phdr) == 32);
static_assert(sizeof(SceHeader) == 128);
static_assert(sizeof(SegmentInfo) == 32);

constexpr u16 kEtExec = 2;
constexpr u16 kEtSceExec = 0xFE00;
constexpr u16 kEtSceRelExec = 0xFE04;
constexpr u16 kEmArm = 40;
constexpr u32 kPtLoad = 1;
constexpr u32 kPtSceRela = 0x60000000;
constexpr u32 kSceMagic = 0x00454353;  // « SCE\0 »

// Base des allocations pour un ELF relogeable qui ne peut pas aller à son adresse prévue.
constexpr GuestAddr kRelocatableBase = 0x81000000;

LoadResult fail(LoadStatus status, std::string message) {
    LOG_ERROR("Chargeur : %s", message.c_str());
    LoadResult r;
    r.status = status;
    r.message = std::move(message);
    return r;
}

template <typename T>
bool read_struct(const std::vector<u8>& data, u64 offset, T& out) {
    if (offset + sizeof(T) > data.size() || offset + sizeof(T) < offset) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

// --- Conteneur : ELF brut ou SELF non chiffré --------------------------------

struct Image {
    Elf32Ehdr ehdr;
    std::vector<Elf32Phdr> phdrs;
    std::vector<std::vector<u8>> segments;  // contenu (p_filesz octets) de chaque segment
    bool from_self = false;
};

LoadResult parse_container(const std::vector<u8>& data, Image& img) {
    u32 magic = 0;
    read_struct(data, 0, magic);

    u64 elf_offset = 0, phdr_offset;
    std::vector<SegmentInfo> infos;
    if (magic == kSceMagic) {
        SceHeader h;
        if (!read_struct(data, 0, h)) return fail(LoadStatus::NotElf, "En-tête SELF tronqué");
        if (h.version != 3 || h.header_type != 1) return fail(LoadStatus::UnsupportedType, "Version de SELF inconnue");
        img.from_self = true;
        elf_offset = h.elf_offset;
        if (!read_struct(data, elf_offset, img.ehdr)) return fail(LoadStatus::NotElf, "ELF du SELF tronqué");
        phdr_offset = h.phdr_offset;
        infos.resize(img.ehdr.e_phnum);
        for (u16 i = 0; i < img.ehdr.e_phnum; ++i)
            if (!read_struct(data, h.section_info_offset + i * sizeof(SegmentInfo), infos[i]))
                return fail(LoadStatus::NotElf, "Table des segments du SELF tronquée");
    } else {
        if (!read_struct(data, 0, img.ehdr)) return fail(LoadStatus::NotElf, "Ce n'est pas un fichier ELF");
        phdr_offset = img.ehdr.e_phoff;
    }

    const Elf32Ehdr& eh = img.ehdr;
    if (std::memcmp(eh.e_ident, "\x7F" "ELF", 4) != 0) return fail(LoadStatus::NotElf, "Ce n'est pas un fichier ELF");
    if (eh.e_ident[4] != 1 || eh.e_ident[5] != 1 || eh.e_machine != kEmArm)
        return fail(LoadStatus::WrongArch, "ELF attendu : ARM 32 bits little-endian");
    if (eh.e_type != kEtExec && eh.e_type != kEtSceExec && eh.e_type != kEtSceRelExec)
        return fail(LoadStatus::UnsupportedType, "Type d'ELF non pris en charge");
    if (eh.e_phentsize != sizeof(Elf32Phdr) || eh.e_phnum == 0)
        return fail(LoadStatus::NotElf, "Table des segments invalide");

    img.phdrs.resize(eh.e_phnum);
    img.segments.resize(eh.e_phnum);
    for (u16 i = 0; i < eh.e_phnum; ++i) {
        Elf32Phdr& ph = img.phdrs[i];
        if (!read_struct(data, phdr_offset + i * sizeof(Elf32Phdr), ph)) return fail(LoadStatus::NotElf, "Table des segments tronquée");
        if (ph.p_type != kPtLoad && ph.p_type != kPtSceRela) continue;
        std::vector<u8>& out = img.segments[i];

        if (!img.from_self) {
            if (static_cast<u64>(ph.p_offset) + ph.p_filesz > data.size()) return fail(LoadStatus::NotElf, "Segment hors du fichier");
            out.assign(data.begin() + ph.p_offset, data.begin() + ph.p_offset + ph.p_filesz);
            continue;
        }
        const SegmentInfo& si = infos[i];
        if (si.encryption != 2)
            return fail(LoadStatus::EncryptedSelf,
                        "SELF chiffré : le déchiffrement n'est pas pris en charge. Utilisez un homebrew (FSELF) "
                        "ou un exécutable déchiffré.");
        if (si.offset > data.size() || si.length > data.size() - si.offset)
            return fail(LoadStatus::NotElf, "Segment du SELF hors du fichier");
        if (si.compression == 2) {
            out.resize(ph.p_filesz);
            uLongf out_len = ph.p_filesz;
            if (uncompress(out.data(), &out_len, data.data() + si.offset, static_cast<uLong>(si.length)) != Z_OK ||
                out_len != ph.p_filesz)
                return fail(LoadStatus::NotElf, "Décompression d'un segment impossible");
        } else {
            if (si.length < ph.p_filesz) return fail(LoadStatus::NotElf, "Segment du SELF trop court");
            out.assign(data.begin() + si.offset, data.begin() + si.offset + ph.p_filesz);
        }
    }

    LoadResult ok;
    ok.status = LoadStatus::Ok;
    return ok;
}

// --- Module : sceModuleInfo et imports ---------------------------------------

std::string read_string(Memory& mem, GuestAddr addr, u32 max_len = 256) {
    std::string s;
    for (u32 i = 0; i < max_len; ++i) {
        const char c = static_cast<char>(mem.read<u8>(addr + i));
        if (!c || mem.has_fault()) break;
        s += c;
    }
    return s;
}

LoadResult parse_module_info(Memory& mem, const Image& img, LoadedModule& mod) {
    // e_entry : index du segment (2 bits de poids fort) et position de sceModuleInfo dans ce segment.
    const u32 seg = img.ehdr.e_entry >> 30, offset = img.ehdr.e_entry & 0x3FFFFFFF;
    if (seg >= mod.segments.size()) return fail(LoadStatus::BadModuleInfo, "sceModuleInfo : segment invalide");
    const GuestAddr base = mod.segments[seg].addr;
    const GuestAddr mi = base + offset;
    if (!mem.is_mapped(mi, 0x5C)) return fail(LoadStatus::BadModuleInfo, "sceModuleInfo hors du segment");

    // Champs de sceModuleInfo (0x5C octets) ; les positions sont relatives au segment.
    mod.name = read_string(mem, mi + 4, 27);
    const u32 import_top = mem.read<u32>(mi + 44), import_end = mem.read<u32>(mi + 48);
    const u32 tls_start = mem.read<u32>(mi + 56);
    mod.tls_filesz = mem.read<u32>(mi + 60);
    mod.tls_memsz = mem.read<u32>(mi + 64);
    const u32 module_start = mem.read<u32>(mi + 68);
    if (tls_start) mod.tls_start = base + tls_start;
    if (module_start == 0 || module_start == 0xFFFFFFFF) return fail(LoadStatus::BadModuleInfo, "Module sans module_start");
    mod.entry = base + module_start;
    mod.entry_is_module_start = true;

    // Tables d'imports : formes courte (0x24 octets) et longue (0x34 octets).
    for (GuestAddr p = base + import_top; p < base + import_end;) {
        const u32 size = mem.read<u8>(p);
        u32 lib_nid, lib_name, func_nids, func_entries, num_funcs = mem.read<u16>(p + 6), num_vars = mem.read<u16>(p + 8);
        if (size == 0x34) {
            lib_nid = mem.read<u32>(p + 16);
            lib_name = mem.read<u32>(p + 20);
            func_nids = mem.read<u32>(p + 28);
            func_entries = mem.read<u32>(p + 32);
        } else if (size == 0x24) {
            lib_nid = mem.read<u32>(p + 12);
            lib_name = mem.read<u32>(p + 16);
            func_nids = mem.read<u32>(p + 20);
            func_entries = mem.read<u32>(p + 24);
        } else {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "Table d'imports de taille inconnue (0x%x)", size);
            return fail(LoadStatus::BadModuleInfo, buf);
        }
        const std::string library = read_string(mem, lib_name);
        for (u32 i = 0; i < num_funcs; ++i)
            mod.imports.push_back({library, lib_nid, mem.read<u32>(func_nids + 4 * i), mem.read<u32>(func_entries + 4 * i)});
        mod.variable_imports += num_vars;
        p += size;
        if (mem.has_fault()) break;
    }
    if (mem.take_fault()) return fail(LoadStatus::BadModuleInfo, "Tables d'imports illisibles");

    LoadResult ok;
    ok.status = LoadStatus::Ok;
    return ok;
}

}  // namespace

LoadResult load_executable(Memory& memory, const std::vector<u8>& data) {
    Image img;
    LoadResult r = parse_container(data, img);
    if (!r.ok()) return r;

    const bool relocatable = img.ehdr.e_type == kEtSceRelExec;
    LoadedModule& mod = r.module;
    mod.from_self = img.from_self;

    // Segments chargeables. Pour un ELF relogeable, l'index de segment des
    // relocations correspond à l'index dans la table des segments.
    std::vector<RelocSegment> reloc_segments(img.phdrs.size(), RelocSegment{0});
    mod.segments.assign(img.phdrs.size(), LoadedSegment{0, 0, 0});
    bool any = false;
    for (size_t i = 0; i < img.phdrs.size(); ++i) {
        const Elf32Phdr& ph = img.phdrs[i];
        if (ph.p_type != kPtLoad || ph.p_memsz == 0) continue;
        if (ph.p_filesz > ph.p_memsz) return fail(LoadStatus::NotElf, "Segment incohérent");

        GuestAddr addr = ph.p_vaddr;
        if (!memory.is_free(addr, ph.p_memsz)) {
            if (!relocatable) return fail(LoadStatus::MapFailed, "Adresse du segment déjà occupée");
            addr = memory.find_free(ph.p_memsz, kRelocatableBase, ph.p_align > 0x1000 ? ph.p_align : 0x1000);
            if (!addr) return fail(LoadStatus::MapFailed, "Plus de mémoire pour le segment");
            mod.relocated = true;
        }
        if (!memory.map(addr, ph.p_memsz)) return fail(LoadStatus::MapFailed, "Impossible de mapper un segment");
        if (!img.segments[i].empty()) memory.copy_to_guest(addr, img.segments[i].data(), static_cast<u32>(img.segments[i].size()));
        mod.segments[i] = {addr, ph.p_vaddr, ph.p_memsz};
        reloc_segments[i].addr = addr;
        any = true;
        LOG_INFO("Segment %zu : 0x%08x -> 0x%08x (+0x%x)", i, ph.p_vaddr, addr, ph.p_memsz);
    }
    if (!any) return fail(LoadStatus::NotElf, "Aucun segment chargeable");

    if (img.ehdr.e_type == kEtExec) {  // ELF ARM ordinaire (programmes de test)
        mod.entry = img.ehdr.e_entry;
        mod.name = "elf";
    } else {
        if (relocatable) {
            for (size_t i = 0; i < img.phdrs.size(); ++i) {
                if (img.phdrs[i].p_type != kPtSceRela) continue;
                std::string error;
                if (!apply_sce_relocations(memory, img.segments[i].data(), img.segments[i].size(), reloc_segments, error))
                    return fail(LoadStatus::BadRelocation, error);
            }
        }
        LoadResult info = parse_module_info(memory, img, mod);
        if (!info.ok()) return info;
    }

    // Les segments vides (non chargeables) ne sont pas renvoyés.
    std::vector<LoadedSegment> loaded;
    for (const LoadedSegment& s : mod.segments)
        if (s.size) loaded.push_back(s);
    mod.segments.swap(loaded);

    char buf[160];
    std::snprintf(buf, sizeof(buf), "Module « %s » chargé (%zu imports%s%s)", mod.name.c_str(), mod.imports.size(),
                  mod.from_self ? ", FSELF" : "", mod.relocated ? ", relogé" : "");
    r.message = buf;
    return r;
}

LoadResult load_executable_file(Memory& memory, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(LoadStatus::FileError, "Impossible d'ouvrir " + path);
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return load_executable(memory, data);
}

}  // namespace vita
