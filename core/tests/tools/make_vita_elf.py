#!/usr/bin/env python3
"""
Fabrique un exécutable Vita de test à partir d'un ELF ARM lié avec
`ld.lld -q` (relocations conservées), comme le font vita-elf-create puis
vita-make-fself dans le VitaSDK :

  1. ELF Vita relogeable (ET_SCE_RELEXEC) : sceModuleInfo, tables d'imports
     par NID et segment de relocations PT_SCE_RELA (format 0, 12 octets) ;
  2. enveloppe SELF non chiffrée (FSELF), segments compressés ou non.

Les structures suivent les définitions du VitaSDK (sce-elf-defs.h, self.h).
Seule la bibliothèque standard Python est utilisée.

Usage : make_vita_elf.py entrée.elf nids.json sortie.velf [--fself f.self] [--compress] [--encrypted]
"""
import json
import struct
import sys
import zlib

SHT_SYMTAB, SHT_REL = 2, 9
SHF_ALLOC = 2
PT_LOAD, PT_SCE_RELA = 1, 0x60000000
ET_SCE_RELEXEC = 0xFE04
R_ARM_NONE, R_ARM_ABS32, R_ARM_REL32, R_ARM_THM_CALL = 0, 2, 3, 10
R_ARM_CALL, R_ARM_JUMP24, R_ARM_THM_JUMP24, R_ARM_TARGET1 = 28, 29, 30, 38
R_ARM_V4BX, R_ARM_TARGET2, R_ARM_PREL31 = 40, 41, 42
PC_RELATIVE = {R_ARM_REL32, R_ARM_THM_CALL, R_ARM_CALL, R_ARM_JUMP24, R_ARM_THM_JUMP24, R_ARM_TARGET2, R_ARM_PREL31}


def fail(msg):
    sys.exit("make_vita_elf : " + msg)


class Elf:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            fail("ELF ARM 32 bits little-endian attendu")
        (self.type, self.machine, _, self.entry, phoff, shoff, self.flags, _, phentsize, phnum,
         shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        self.phdrs = [struct.unpack_from("<8I", data, phoff + i * phentsize) for i in range(phnum)]
        self.shdrs = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
        strtab = self.shdrs[shstrndx]
        self.names = [self.cstr(strtab[4] + s[0]) for s in self.shdrs]
        self.symbols = []
        for s in self.shdrs:
            if s[1] == SHT_SYMTAB:
                names = self.shdrs[s[6]]
                for off in range(s[4], s[4] + s[5], 16):
                    name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, off)
                    self.symbols.append((self.cstr(names[4] + name), value, shndx))

    def cstr(self, off):
        return self.data[off:self.data.index(b"\0", off)].decode()

    def section(self, name):
        for i, n in enumerate(self.names):
            if n == name:
                return i, self.shdrs[i]
        fail("section %s introuvable" % name)

    def symbol(self, name):
        for s in self.symbols:
            if s[0] == name:
                return s
        fail("symbole %s introuvable" % name)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opts = {a for a in argv if a.startswith("--") and a != "--fself"}
    fself_path = argv[argv.index("--fself") + 1] if "--fself" in argv else None
    if fself_path:
        args.remove(fself_path)
    if len(args) != 3:
        fail(__doc__)
    elf = Elf(open(args[0], "rb").read())
    nids = json.load(open(args[1]))

    loads = [p for p in elf.phdrs if p[0] == PT_LOAD and p[5] > 0]
    seg_bytes = [bytearray(elf.data[p[1]:p[1] + p[4]]) for p in loads]

    def seg_of(addr, allow_end=False):
        for i, p in enumerate(loads):
            if p[2] <= addr < p[2] + p[5] or (allow_end and addr == p[2] + p[5]):
                return i
        return None

    relocs = []  # (symseg, code, datseg, addend, offset)

    def abs32(place, target):
        ds, ts = seg_of(place), seg_of(target, allow_end=True)
        relocs.append((ts, R_ARM_ABS32, ds, target - loads[ts][2], place - loads[ds][2]))

    # 1. Relocations conservées par l'éditeur de liens (forme REL : addend implicite).
    for idx, sh in enumerate(elf.shdrs):
        if sh[1] != SHT_REL or not (elf.shdrs[sh[7]][2] & SHF_ALLOC):
            continue
        for off in range(sh[4], sh[4] + sh[5], 8):
            r_offset, r_info = struct.unpack_from("<II", elf.data, off)
            rtype, symidx = r_info & 0xFF, r_info >> 8
            if rtype in (R_ARM_NONE, R_ARM_V4BX):
                continue
            ds = seg_of(r_offset)
            if ds is None:
                continue
            name, value, shndx = elf.symbols[symidx]
            if rtype in (R_ARM_ABS32, R_ARM_TARGET1):
                v = struct.unpack_from("<I", seg_bytes[ds], r_offset - loads[ds][2])[0]
                ts = seg_of(value & ~1, allow_end=True) if shndx not in (0, 0xFFF1) else None
                if ts is None:
                    relocs.append((15, R_ARM_ABS32, ds, v, r_offset - loads[ds][2]))
                else:
                    relocs.append((ts, R_ARM_ABS32, ds, (v - loads[ts][2]) & 0xFFFFFFFF, r_offset - loads[ds][2]))
            elif rtype in PC_RELATIVE:
                if seg_of(value & ~1, allow_end=True) != ds:
                    fail("relocation relative au PC entre segments (type %d) non gérée" % rtype)
            else:
                fail("type de relocation %d non géré (compiler avec -mno-movt)" % rtype)

    # 2. sceModuleInfo et tables d'imports, écrits dans la zone réservée .sce.reserved.
    _, res = elf.section(".sce.reserved")
    base = res[3]
    rs = seg_of(base)
    out = bytearray()

    def here():
        return base + len(out)

    def align4():
        while len(out) % 4:
            out.append(0)

    wanted = {name for lib in nids["libraries"] for name in lib["functions"]}
    stubs = {s[0]: s[1] & ~1 for s in elf.symbols if s[0] in wanted and s[2] != 0}
    libs = []
    for lib in nids["libraries"]:
        funcs = [(name, int(nid, 16)) for name, nid in lib["functions"].items() if name in stubs]
        if funcs:
            libs.append((lib["name"], int(lib["nid"], 16), funcs))

    out += b"\0" * 0x5C  # sceModuleInfo, rempli plus bas
    imports_top = here()
    out += b"\0" * (0x34 * len(libs))
    pointers = []  # (emplacement, cible) : pointeurs absolus à reloger
    for i, (name, lib_nid, funcs) in enumerate(libs):
        name_addr = here()
        out += name.encode() + b"\0"
        align4()
        nid_table = here()
        for _, nid in funcs:
            out += struct.pack("<I", nid)
        entry_table = here()
        for fname, _ in funcs:
            pointers.append((here(), stubs[fname]))
            out += struct.pack("<I", stubs[fname])
        entry = struct.pack("<BBHHHHH10I", 0x34, 0, 1, 0, len(funcs), 0, 0, 0, lib_nid, name_addr, 0,
                            nid_table, entry_table, 0, 0, 0, 0)
        at = imports_top - base + 0x34 * i
        out[at:at + 0x34] = entry
        for field_off, target in ((20, name_addr), (28, nid_table), (32, entry_table)):
            pointers.append((imports_top + 0x34 * i + field_off, target))
    imports_end = imports_top + 0x34 * len(libs)

    seg_base = loads[rs][2]
    module_start = elf.symbol("module_start")[1]
    info = struct.pack("<HH27sB15I", 0, 0x0101, nids["module_name"].encode()[:26], 6, 0,
                       0, 0, imports_top - seg_base, imports_end - seg_base, 0x12345678, 0, 0, 0,
                       module_start - seg_base, 0xFFFFFFFF, 0, 0, 0, 0)
    assert len(info) == 0x5C
    out[0:0x5C] = info
    if len(out) > res[5]:
        fail("zone .sce.reserved trop petite (%d octets nécessaires)" % len(out))
    seg_bytes[rs][base - seg_base:base - seg_base + len(out)] = out
    for place, target in pointers:
        abs32(place, target)

    # 3. Segment de relocations (format 0 : 12 octets par entrée).
    rela = b"".join(struct.pack("<III", 0 | (s << 4) | (c << 8) | (d << 16), a, o) for s, c, d, a, o in relocs)

    # 4. ELF Vita : en-tête, segments chargeables puis PT_SCE_RELA.
    phnum = len(loads) + 1
    offset = 52 + 32 * phnum
    phdrs, blobs = [], []
    for p, data in zip(loads, seg_bytes):
        offset = (offset + 15) & ~15
        phdrs.append((PT_LOAD, offset, p[2], p[3], len(data), p[5], p[6], p[7]))
        blobs.append((offset, bytes(data)))
        offset += len(data)
    offset = (offset + 15) & ~15
    phdrs.append((PT_SCE_RELA, offset, 0, 0, len(rela), 0, 0, 16))
    blobs.append((offset, rela))
    e_entry = (rs << 30) | (base - seg_base)
    ehdr = elf.data[:16] + struct.pack("<HHIIIIIHHHHHH", ET_SCE_RELEXEC, 40, 1, e_entry, 52, 0, 0x05000000, 52, 32,
                                       phnum, 40, 0, 0)
    velf = bytearray(ehdr + b"".join(struct.pack("<8I", *p) for p in phdrs))
    for off, data in blobs:
        velf += b"\0" * (off - len(velf)) + data
    open(args[2], "wb").write(velf)

    # 5. FSELF (disposition de vita-make-fself).
    if fself_path:
        header_len, elf_offset = 0x1000, 0xA0
        phdr_offset = (elf_offset + 52 + 15) & ~15
        info_offset = phdr_offset + 32 * phnum
        version_offset = info_offset + 32 * phnum
        body, infos, pos = bytearray(), [], header_len
        for p, (_, data) in zip(phdrs, blobs):
            pos = (pos + 15) & ~15
            body += b"\0" * (pos - header_len - len(body))
            stored = zlib.compress(data, 9) if "--compress" in opts else data
            stored += b"\0" * (-len(stored) % 4)
            infos.append(struct.pack("<4Q", pos, len(stored), 2 if "--compress" in opts else 1,
                                     1 if "--encrypted" in opts else 2))
            body += stored
            pos += len(stored)
        total = header_len + len(body)
        sce = struct.pack("<IIHHIQQQQQQQQQQQQQQ", 0x454353, 3, 0xC0, 1, 0x600, header_len, len(velf), total, 0, 4,
                          0x80, elf_offset, phdr_offset, 0, info_offset, version_offset, version_offset + 16, 0, 0)
        appinfo = struct.pack("<QIIQQ", 0x2F00000000000001, 0, 8, 0x0001000000000000, 0)
        head = bytearray(sce + appinfo)
        head += b"\0" * (elf_offset - len(head)) + ehdr
        head += b"\0" * (phdr_offset - len(head)) + b"".join(struct.pack("<8I", *p) for p in phdrs)
        head += b"".join(infos) + b"\0" * 16
        head += b"\0" * (header_len - len(head))
        open(fself_path, "wb").write(head + body)


if __name__ == "__main__":
    main(sys.argv[1:])
