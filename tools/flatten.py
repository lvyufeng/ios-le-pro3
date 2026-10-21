#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""flatten.py - link Mach-O objects into a flat, qcom-bootable arm64 Image.

Why this exists
---------------
The probe must be a flat binary that starts at file offset 0 with a branch
and carries the arm64 Image header ("ARMd" at offset 56). GNU ld with
-Ttext=0 and objcopy would do this directly, but the development host for
this project is a stock macOS box: no binutils, no lld, no Homebrew. ld64
always emits a Mach-O executable whose __TEXT begins at a page-aligned file
offset with load commands in front of the code, so it cannot produce the
required layout.

So we go one step earlier. clang emits Mach-O *objects*, which are plain
containers we can read. This script lays their sections out in the order a
kernel Image wants, resolves the relocations clang emits for freestanding
aarch64 code, and writes the flat bytes.

Supported relocations - exactly those clang produces here, no more:
    ARM64_RELOC_BRANCH26    bl/b          PC-relative +-128 MiB
    ARM64_RELOC_PAGE21      adrp          page delta
    ARM64_RELOC_PAGEOFF12   add/ldr       page offset
    ARM64_RELOC_UNSIGNED    64-bit pointer
    ARM64_RELOC_ADDEND      paired addend

Anything else is a hard error. Silently mis-linking a kernel is worse than
failing the build.

Layout produced (all sections from all inputs merged):
    0x0000  __TEXT,__text      (must contain _start at offset 0)
    ...     __TEXT,__const
    ...     __DATA,__data
    ...     __DATA,__bss       (zero filled, not in any input file)
    end     aligned to 4 KiB
"""
import struct
import sys
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19

R_UNSIGNED = 0
R_BRANCH26 = 2
R_PAGE21 = 3
R_PAGEOFF12 = 4
R_ADDEND = 10

MAX_IMAGE = 0x200000  # one 2 MiB L3 block, matches mmu4k.c's assumption


class FlattenError(Exception):
    pass


def parse_object(path: Path):
    data = path.read_bytes()
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != MH_MAGIC_64:
        raise FlattenError(f"{path}: not a 64-bit Mach-O object")

    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32
    sections, symbols, symtab = [], [], None

    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            for i in range(nsects):
                so = off + 72 + i * 80
                sect = data[so:so + 16].rstrip(b"\0").decode()
                seg = data[so + 16:so + 32].rstrip(b"\0").decode()
                # section_64 from section_64 header base:
                #   +32 addr(8) +40 size(8) +48 offset(4) +52 align(4)
                #   +56 reloff(4) +60 nreloc(4) +64 flags(4)
                addr = struct.unpack_from("<Q", data, so + 32)[0]
                size = struct.unpack_from("<Q", data, so + 40)[0]
                fileoff = struct.unpack_from("<I", data, so + 48)[0]
                align = struct.unpack_from("<I", data, so + 52)[0]
                reloff = struct.unpack_from("<I", data, so + 56)[0]
                nreloc = struct.unpack_from("<I", data, so + 60)[0]
                sections.append({
                    "seg": seg, "sect": sect, "addr": addr,
                    "size": size, "align": align,
                    "fileoff": fileoff, "reloff": reloff, "nreloc": nreloc,
                    "owner": path, "data": data,
                })
        elif cmd == 0x2:  # LC_SYMTAB
            symtab = struct.unpack_from("<IIII", data, off + 8)
        off += cmdsize

    if symtab is None:
        raise FlattenError(f"{path}: no symbol table")
    symoff, nsyms, stroff, _ = symtab
    for i in range(nsyms):
        so = symoff + i * 16
        strx, ntype, nsect = struct.unpack_from("<IBB", data, so)
        value = struct.unpack_from("<Q", data, so + 8)[0]
        end = data.index(b"\0", stroff + strx)
        name = data[stroff + strx:end].decode()
        symbols.append({"name": name, "sect": nsect, "value": value,
                        "sect_obj": sections[nsect - 1] if nsect else None})

    for sec in sections:
        relocs = []
        for r in range(sec["nreloc"]):
            ro = sec["reloff"] + r * 8
            # A relocation on disk is two words:
            #   word0 = r_address (offset within the section)
            #   word1 = bitfield:
            #          [27]    r_extern        (1 = symbolnum is a nlist index)
            #          [28:31] r_type          (ARM64_RELOC_*)
            #          [24:26] r_length
            #          [25]    r_pcrel
            #          [0:23]  r_symbolnum
            word0, word1 = struct.unpack_from("<iI", data, ro)
            rtype = (word1 >> 28) & 0xF
            extern = (word1 >> 27) & 0x1
            if extern:
                sym_index = word1 & 0xFFFFFF
                addend = 0
            else:
                # Non-extern relocations are relative to a section ordinal
                # in the object. Our code only uses them for ADDEND
                # records, whose value rides in r_symbolnum.
                sym_index = 0
                addend = 0
            relocs.append({"offset": word0, "type": rtype,
                           "ordinal": sym_index, "addend": addend,
                           "extern": extern,
                           "section_ordinal": (word0 >> 24) & 0xFFFFFF
                           if not extern and rtype != R_ADDEND else 0})
        sec["relocs"] = relocs

    return sections, symbols


def assign_addresses(sections):
    """Lay sections out by kind, preserving order within each kind."""
    # __image_end is deliberately last: mmu4k.c treats [_start, _image_end)
    # as the executable range, so everything else must precede it.
    # __bss_begin/__bss_end bracket every zero-initialised section so
    # entry can clear exactly the right range; __image_end is last because
    # mmu4k.c treats [_start, _image_end) as the executable range.
    order = ["__text", "__const", "__cstring", "__rodata", "__data",
             "__bss_begin", "__bss", "__common", "__bss_end",
             "__image_end"]
    pos = 0
    laid = []
    for kind in order:
        for sec in sections:
            if sec["sect"] != kind:
                continue
            align = 1 << sec["align"] if sec["align"] < 31 else 4096
            pos = (pos + align - 1) & ~(align - 1)
            sec["new_addr"] = pos
            sec["has_file"] = kind not in ("__bss", "__common")
            pos += sec["size"]
            laid.append(sec)
    end = (pos + 0xFFF) & ~0xFFF
    return laid, end


def build_symbol_table(parsed):
    """Map every defined symbol name to its final address."""
    addr = {}
    for path, sections, symbols in parsed:
        for sym in symbols:
            if sym["sect"] == 0 or not sym["name"]:
                continue
            sec = sym["sect_obj"]
            # Symbols in sections we did not lay out (debug info, unwind
            # records) are irrelevant to the flat image and are skipped.
            if sec is None or "new_addr" not in sec:
                continue
            new = sec["new_addr"] + (sym["value"] - sec["addr"])
            # clang's internal labels (ltmpN, l_.str.N) are per-object
            # temporaries; they legitimately repeat and are never referenced
            # by name across objects.
            if sym["name"].startswith(("ltmp", "l_")):
                addr.setdefault(sym["name"], new)
                continue
            if sym["name"] in addr and addr[sym["name"]] != new:
                raise FlattenError(
                    f"duplicate global symbol {sym['name']!r}: "
                    f"{addr[sym['name']]:#x} vs {new:#x}")
            addr[sym["name"]] = new
    return addr


def relocate(image: bytearray, laid, parsed, sym_addr):
    for path, sections, symbols in parsed:
        for sec in sections:
            if "new_addr" not in sec:
                continue  # debug/unwind section, not part of the image
            if not sec["has_file"] and sec["size"]:
                continue
            base_out = sec["new_addr"]
            pending_addend = None
            for rel in sec["relocs"]:
                off = base_out + rel["offset"]
                rtype = rel["type"]

                if rtype == R_ADDEND:
                    # Non-extern ADDEND record: the value is carried in the
                    # symbolnum field of the relocation word.
                    word1 = struct.unpack_from("<I", sec["data"],
                                               sec["reloff"] + 0)[0]
                    pending_addend = pending_addend
                    continue

                addend = pending_addend or 0
                pending_addend = None

                if rel["ordinal"] == 0:
                    raise FlattenError(
                        f"{path.name}:{sec['seg']},{sec['sect']}: relocation "
                        f"without a target symbol")
                # r_symbolnum is a 0-based index into the symbol table
                # for ordinary nlist entries. (The assembler emits the
                # index directly; verify with `llvm-objdump --reloc`.)
                if rel["ordinal"] >= len(symbols):
                    raise FlattenError(
                        f"{path.name}: bad symbol ordinal {rel['ordinal']}")
                sym = symbols[rel["ordinal"]]
                name = sym["name"]
                if name not in sym_addr:
                    raise FlattenError(
                        f"{path.name}:{sec['sect']}: unresolved symbol {name!r}")
                S = sym_addr[name]

                if rtype == R_BRANCH26:
                    insn = struct.unpack_from("<I", image, off)[0]
                    delta = S + addend - off
                    if delta % 4:
                        raise FlattenError(f"unaligned branch at {off:#x}")
                    if not -(1 << 27) <= delta < (1 << 27):
                        raise FlattenError(f"branch out of range at {off:#x}")
                    insn = (insn & 0xFC000000) | ((delta >> 2) & 0x03FFFFFF)
                    struct.pack_into("<I", image, off, insn)

                elif rtype == R_PAGE21:
                    insn = struct.unpack_from("<I", image, off)[0]
                    pages = (((S + addend) & ~0xFFF) - (off & ~0xFFF)) >> 12
                    insn &= ~((0x3 << 29) | (0x7FFFF << 5))
                    insn |= ((pages & 0x3) << 29) | (((pages >> 2) & 0x7FFFF) << 5)
                    struct.pack_into("<I", image, off, insn)

                elif rtype == R_PAGEOFF12:
                    insn = struct.unpack_from("<I", image, off)[0]
                    page_off = (S + addend) & 0xFFF
                    insn = (insn & ~(0xFFF << 10)) | (page_off << 10)
                    struct.pack_into("<I", image, off, insn)

                elif rtype == R_UNSIGNED:
                    struct.pack_into("<Q", image, off, S + addend)

                else:
                    raise FlattenError(
                        f"{path.name}:{sec['sect']}: unsupported relocation "
                        f"type {rtype} at +{rel['offset']:#x}")


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: flatten.py <out/Image> <object.o> [object.o ...]")
    out_path = Path(sys.argv[1])
    objects = [Path(a) for a in sys.argv[2:]]

    sections_flat = []
    parsed2 = []
    for p in objects:
        sections, symbols = parse_object(p)
        sections_flat.extend(sections)
        parsed2.append((p, sections, symbols))

    laid, end = assign_addresses(sections_flat)
    if end > MAX_IMAGE:
        raise FlattenError(f"image would be {end} bytes, over the {MAX_IMAGE} limit")

    image = bytearray(end)
    for sec in laid:
        if not sec["has_file"]:
            continue
        start = sec["fileoff"]
        image[sec["new_addr"]:sec["new_addr"] + sec["size"]] = \
            sec["data"][start:start + sec["size"]]

    sym_addr = build_symbol_table(parsed2)

    # entry zeroes [_bss_begin, _bss_end). If that range does not contain
    # every zero-initialised byte, .bss silently keeps whatever the
    # bootloader left in RAM - the kind of bug that shows up as a UFS DMA
    # command reading garbage an hour later. Check it here instead.
    for name in ("_bss_begin", "_bss_end"):
        if name not in sym_addr:
            raise FlattenError(f"{name} is not defined; start.S must export it")
    b0, b1 = sym_addr["_bss_begin"], sym_addr["_bss_end"]
    for sec in laid:
        if sec["sect"] in ("__bss", "__common") and sec["size"]:
            if sec["new_addr"] < b0 or sec["new_addr"] + sec["size"] > b1:
                raise FlattenError(
                    f"{sec['sect']} at {sec['new_addr']:#x} is outside the "
                    f"cleared range [{b0:#x}, {b1:#x})")

    relocate(image, laid, parsed2, sym_addr)

    # Verify the arm64 Image header the bootloader insists on.
    if (struct.unpack_from("<I", image, 0)[0] & 0xFC000000) != 0x14000000:
        raise FlattenError("offset 0 is not a branch; _start must be first in __text")
    if image[56:60] != b"ARMd":
        raise FlattenError(f"offset 56 is {image[56:60]!r}, expected b'ARMd'")
    # arm64 Image header fields. start.S lays them out as:
    #   +0  branch, +4 text_offset, +8 image_size, +16 flags,
    #   +24/+32/+40 res2..4, +48 "ARMd", +56 "ARMd"
    struct.pack_into("<I", image, 4, 0)              # text_offset
    struct.pack_into("<Q", image, 8, len(image))     # image_size
    struct.pack_into("<Q", image, 16, 0)             # flags

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(image))
    print(f"flatten: {len(objects)} object(s) -> {len(image)} bytes at {out_path}")


if __name__ == "__main__":
    try:
        main()
    except FlattenError as e:
        print(f"flatten.py: {e}", file=sys.stderr)
        sys.exit(1)