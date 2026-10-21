#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""mkboot.py - wrap the probe Image in an Android boot image for `fastboot boot`.

Everything here is host-side and offline. The tool never opens a block
device, never talks to the phone, and writes exactly one file.

Pieces (from the LineageOS leeco msm8996-common board config):
    base address    0x80000000   BOARD_KERNEL_BASE
    page size       4096         BOARD_KERNEL_PAGESIZE
    ramdisk offset  0x01000000   BOARD_RAMDISK_OFFSET
    tags offset     0x00000100   BOARD_KERNEL_TAGS_OFFSET
    image name      Image.gz-dtb BOARD_KERNEL_IMAGE_NAME

`Image.gz-dtb` means the bootloader wants a gzip'd kernel with the DTB
appended. For the probe there is no DTB to append: the zl1 bootloader hands
the probe the live one in x0, exactly as it does for Android. We therefore
ship `Image.gz` with an empty (but valid) ramdisk, which is what the kernel
expects for a "no initramfs" boot.

The ramdisk is a minimal cpio archive with a single file, so the address
math and the bootloader's parsing path are exercised on the phone before a
real payload exists.
"""
import argparse
import gzip
import hashlib
import struct
import sys
from pathlib import Path


def hexint(s: str) -> int:
    return int(s, 0)


def cpio_newc(files: dict) -> bytes:
    """Build a minimal newc cpio archive. Keys are paths, values are bytes."""
    out = bytearray()

    def entry(name: str, data: bytes, mode: int, ino: int) -> None:
        namez = name.encode() + b"\0"
        fields = [
            110,           # ino
            mode,          # mode
            0, 0,          # uid, gid
            1,             # nlink
            0,             # mtime
            len(data),     # filesize
            0, 0,          # devmajor, devminor
            0, 0,          # rdevmajor, rdevminor
            len(namez),    # namesize
            0,             # check
        ]
        out.extend(b"070701")
        out.extend(b"".join(b"%08X" % f for f in fields))
        out.extend(namez)
        out.extend(b"\0" * ((-len(out)) % 4))
        out.extend(data)
        out.extend(b"\0" * ((-len(out)) % 4))

    ino = 1
    for path, data in files.items():
        entry(path, data, 0o100644, ino)
        ino += 1
    entry("TRAILER!!!", b"", 0, 0)
    return bytes(out)


def build_header_v0(page_size: int, kernel: bytes, ramdisk: bytes,
                    second: bytes, name: bytes, cmdline: bytes,
                    kernel_addr: int, ramdisk_addr: int, second_addr: int,
                    tags_addr: int) -> bytes:
    """Android boot image header, version 0 (the format Android 5-8 used)."""
    kernel_size = len(kernel)
    ramdisk_size = len(ramdisk)
    second_size = len(second)
    if len(name) > 16:
        raise SystemExit("board name must be <= 16 bytes")
    if len(cmdline) > 512:
        raise SystemExit("cmdline must be <= 512 bytes")

    # The v0 header is 1632 bytes: the fields below, then 32 bytes of id[].
    hdr = bytearray(1632)
    struct.pack_into("<8s10I16s512s", hdr, 0,
                     b"ANDROID!",
                     kernel_size, kernel_addr,     # kernel_size, kernel_addr
                     ramdisk_size, ramdisk_addr,   # ramdisk_size, ramdisk_addr
                     second_size, second_addr,     # second_size, second_addr
                     tags_addr,                    # tags_addr
                     page_size,                    # page_size
                     0,                            # header_version / os_version
                     0,                            # os_version
                     name.ljust(16, b"\0"),
                     cmdline.ljust(512, b"\0"))

    def pad(b: bytes) -> bytes:
        return b + b"\0" * ((-len(b)) % page_size)

    body = bytearray()
    digest = hashlib.sha1()
    for part in (kernel, ramdisk, second):
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    hdr[576:596] = digest.digest()      # id[32]: first 20 bytes of the SHA-1
    # The header itself is padded to a page boundary before the kernel.
    body.extend(pad(bytes(hdr)))
    body.extend(pad(kernel))
    body.extend(pad(ramdisk))
    body.extend(pad(second))
    return bytes(body)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, help="flat arm64 Image from probe/build.sh")
    ap.add_argument("output", type=Path, help="boot.img to write")
    ap.add_argument("--page-size", type=hexint, default=4096)
    ap.add_argument("--kernel-addr", type=hexint, default=0x80008000)
    ap.add_argument("--ramdisk-addr", type=hexint, default=0x81000000)
    ap.add_argument("--tags-addr", type=hexint, default=0x80000100)
    ap.add_argument("--second-addr", type=hexint, default=0x80F00000)
    ap.add_argument("--cmdline", default="console=ttyMSM0,115200n8")
    ap.add_argument("--name", default="zl1-probe")
    ap.add_argument("--gzip", dest="gz", action="store_true", default=True,
                    help="gzip the kernel (default; matches Image.gz-dtb)")
    ap.add_argument("--no-gzip", dest="gz", action="store_false")
    args = ap.parse_args()

    image = args.image.read_bytes()
    if image[56:60] != b"ARMd":
        sys.exit(f"{args.image}: missing ARMd marker at offset 56 "
                 "- was this built by probe/build.sh?")
    kernel = gzip.compress(image, compresslevel=9) if args.gz else image

    # The probe needs no userspace; a one-file cpio is enough for the
    # bootloader to accept the image and keeps the ramdisk address math real.
    ramdisk = cpio_newc({"probe-readme.txt":
                         b"zl1 probe: no userspace. See github.com/lvyufeng/ios-le-pro3\n"})

    boot = build_header_v0(
        page_size=args.page_size,
        kernel=kernel,
        ramdisk=ramdisk,
        second=b"",
        name=args.name.encode(),
        cmdline=args.cmdline.encode(),
        kernel_addr=args.kernel_addr,
        ramdisk_addr=args.ramdisk_addr,
        second_addr=args.second_addr,
        tags_addr=args.tags_addr,
    )

    if len(boot) > 64 * 1024 * 1024:
        sys.exit(f"boot image is {len(boot)} bytes; the zl1 boot partition is 64 MiB")

    args.output.write_bytes(boot)
    print(f"kernel  : {len(image)} raw -> {len(kernel)} gz")
    print(f"ramdisk : {len(ramdisk)} bytes (newc cpio, 1 file)")
    print(f"boot.img: {len(boot)} bytes -> {args.output}")
    print(f"sha256  : {hashlib.sha256(boot).hexdigest()}")


if __name__ == "__main__":
    main()