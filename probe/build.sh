#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# build.sh - build the zl1 probe Image and pack it into a boot image.
#
# Two host profiles are supported:
#
#   * macOS with Xcode's clang only (no Homebrew, no binutils, no lld).
#     clang emits Mach-O objects; tools/flatten.py links them into the flat
#     arm64 Image the qcom bootloader expects. This is the path exercised on
#     the development machine and in CI.
#   * Linux with an aarch64 cross toolchain, where the objects could be
#     linked with ld directly. flatten.py is still used so both hosts
#     produce byte-identical output; it is the layout authority.
#
# Output:
#   probe/out/Image     flat arm64 Image (link address 0)
#   probe/out/boot.img  Android boot image v0 for `fastboot boot`
set -eu
cd "$(dirname "$0")"
OUT=out
mkdir -p "$OUT"
ROOT=$(cd .. && pwd)

# --- toolchain discovery -------------------------------------------------
if [ -z "${CC:-}" ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        CC=aarch64-linux-gnu-gcc
        TARGET_FLAGS="-march=armv8-a"
    else
        CC=${CC:-clang}
        TARGET_FLAGS="-target aarch64-apple-darwin"
    fi
fi

# Freestanding, no libc, no PIC. -mstrict-align matters because MMIO reads
# must stay single 4-byte loads (the fault handler steps over exactly one).
CFLAGS="$TARGET_FLAGS -ffreestanding -fno-builtin -fno-stack-protector
        -fno-pic -fno-pie -nostdlib -O2 -march=armv8-a -mgeneral-regs-only
        -mstrict-align -mno-outline-atomics -Wall -Wextra -Werror -I."

echo "CC = $CC"
for src in start.S mmu4k.c zl1_probe.c ufs.c; do
    base=$(basename "$src")
    obj="$OUT/${base%.*}.o"
    "$CC" $CFLAGS -c "$src" -o "$obj"
    echo "  $src -> $obj"
done

# A GOT relocation means a flat Image cannot work: there is no dynamic
# linker to fill it. Fail loudly instead of producing a kernel that hangs.
if command -v llvm-objdump >/dev/null 2>&1 && command -v xcrun >/dev/null 2>&1; then
    for o in "$OUT"/*.o; do
        if xcrun llvm-objdump --macho --reloc "$o" 2>/dev/null | grep -q GOT; then
            echo "build.sh: $o has a GOT relocation; use adrp/add via SYM_ADDR" >&2
            exit 1
        fi
    done
fi

python3 "$ROOT/tools/flatten.py" "$OUT/Image" \
    "$OUT/start.o" "$OUT/mmu4k.o" "$OUT/zl1_probe.o" "$OUT/ufs.o"

python3 "$ROOT/tools/mkboot.py" "$OUT/Image" "$OUT/boot.img"

echo
echo "ready: probe/$OUT/boot.img"
echo "flash: fastboot boot probe/$OUT/boot.img    (nothing is written)"