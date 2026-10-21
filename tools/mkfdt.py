#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""mkfdt.py - build a small test DTB without dtc.

The shim's FDT reader needs to be tested against trees shaped like real
zl1 ones: 2/2 cells, /memory with a 64-bit base, /reserved-memory with
no-map children, /cpus/cpu@N carrying MPIDRs, and /chosen/bootargs. `dtc`
is not installed on the development host, and pulling in libfdt for a test
fixture would make the test depend on the thing under test.

This builds a DTB with exactly the FDT format (Devicetree Specification
v0.4), which also means the reader gets exercised on the format rather than
on a Python dict.

Usage:
    tools/mkfdt.py out.dtb               # the default zl1-shaped tree
    tools/mkfdt.py --soc8996 out.dtb     # same thing, named for clarity
"""
import struct
import sys
from pathlib import Path

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_END = 9


class Node:
    def __init__(self, name: str):
        self.name = name
        self.props = []          # (name, bytes)
        self.children = []

    def prop(self, name: str, data: bytes):
        self.props.append((name, data))
        return self

    def u32(self, name: str, *vals: int):
        return self.prop(name, b"".join(struct.pack(">I", v) for v in vals))

    def string(self, name: str, s: str):
        return self.prop(name, s.encode() + b"\0")

    def empty(self, name: str):
        return self.prop(name, b"")

    def child(self, name: str):
        n = Node(name)
        self.children.append(n)
        return n


def serialize(root: Node):
    strings = bytearray()
    string_offsets = {}

    def str_off(s: str) -> int:
        if s not in string_offsets:
            string_offsets[s] = len(strings)
            strings.extend(s.encode() + b"\0")
        return string_offsets[s]

    struct_block = bytearray()

    def emit(node: Node):
        struct_block.extend(struct.pack(">I", FDT_BEGIN_NODE))
        struct_block.extend(node.name.encode() + b"\0")
        while len(struct_block) % 4:
            struct_block.append(0)
        for pname, pdata in node.props:
            struct_block.extend(struct.pack(">III", FDT_PROP, len(pdata),
                                            str_off(pname)))
            struct_block.extend(pdata)
            while len(struct_block) % 4:
                struct_block.append(0)
        for c in node.children:
            emit(c)
        struct_block.extend(struct.pack(">I", FDT_END_NODE))

    emit(root)
    struct_block.extend(struct.pack(">I", FDT_END))

    # Memory reservation block: the spec requires an empty entry at minimum.
    rsv = struct.pack(">QQ", 0, 0)

    header_size = 40
    off_rsv = header_size
    off_struct = off_rsv + len(rsv)
    while off_struct % 4:
        off_struct += 1
    pad_rsv = b"\0" * (off_struct - (off_rsv + len(rsv)))
    off_strings = off_struct + len(struct_block)
    totalsize = off_strings + len(strings)

    hdr = struct.pack(
        ">10I",
        FDT_MAGIC, totalsize,
        off_struct, off_strings,
        off_rsv,
        17,          # version
        16,          # last_comp_version
        0,           # boot_cpuid_phys
        len(strings),
        len(struct_block),
    )
    return hdr + rsv + pad_rsv + bytes(struct_block) + bytes(strings)


def zl1_tree() -> Node:
    root = Node("")
    root.u32("#address-cells", 2).u32("#size-cells", 2)
    root.string("compatible", "qcom,msm8996-v3")
    root.string("model", "Qualcomm Technologies, Inc. MSM 8996 v3 + PMI8996 QRD SKU")

    chosen = root.child("chosen")
    chosen.string("bootargs",
                  "androidboot.hardware=qcom androidboot.console=ttyHSL0 "
                  "androidboot.selinux=enforcing")
    chosen.string("stdout-path", "/soc/serial@7570000")

    memory = root.child("memory")
    memory.string("device_type", "memory")
    memory.u32("reg", 0x0, 0x80000000, 0x0, 0x20000000)   # 512 MiB @ 2 GiB

    rsv = root.child("reserved-memory")
    rsv.u32("#address-cells", 2).u32("#size-cells", 2)
    rsv.empty("ranges")

    removed = rsv.child("removed_regions@85800000")
    removed.u32("reg", 0x0, 0x85800000, 0x0, 0x3000000)
    removed.empty("no-map")

    modem = rsv.child("modem_region@88800000")
    modem.u32("reg", 0x0, 0x88800000, 0x0, 0x6200000)
    modem.empty("no-map")

    splash = rsv.child("cont_splash_mem@83401000")
    splash.u32("reg", 0x0, 0x83401000, 0x0, 0x23FF000)
    # deliberately no no-map: reusable

    cpus = root.child("cpus")
    cpus.u32("#address-cells", 1).u32("#size-cells", 0)
    for name, mpidr in (("cpu@0", 0x0), ("cpu@100", 0x100),
                        ("cpu@200", 0x200), ("cpu@300", 0x300)):
        c = cpus.child(name)
        c.string("device_type", "cpu")
        c.u32("reg", mpidr)

    soc = root.child("soc")
    soc.u32("#address-cells", 1).u32("#size-cells", 1)
    soc.empty("ranges")
    ufshc = soc.child("ufshc@624000")
    ufshc.string("compatible", "qcom,msm8996-ufshc")
    ufshc.u32("reg", 0x624000, 0x2500)
    return root


def main() -> None:
    out = Path(sys.argv[-1])
    out.write_bytes(serialize(zl1_tree()))
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()