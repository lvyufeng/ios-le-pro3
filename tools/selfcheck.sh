#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# selfcheck.sh - build the probe from scratch and verify the result.
#
# This is the same check the CI workflow runs. It lives here as a script so
# it can be run on the development machine without a network round trip,
# and so the repository has a working check even before CI is configured
# (pushing .github/workflows/ needs a token with the `workflow` scope).
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

echo "== toolchain"
clang --version | head -1
python3 --version

echo
echo "== clean build"
rm -rf probe/out
./probe/build.sh

echo
echo "== no GOT references"
for o in probe/out/*.o; do
    if xcrun llvm-objdump --macho --reloc "$o" 2>/dev/null | grep -q GOT; then
        echo "FAIL: $o has a GOT relocation (a flat Image cannot resolve it)"
        exit 1
    fi
    echo "  ok   $o"
done

echo
echo "== Image header"
python3 - <<'PY'
import struct, sys
d = open('probe/out/Image', 'rb').read()
errs = []
if (struct.unpack_from('<I', d, 0)[0] & 0xFC000000) != 0x14000000:
    errs.append('offset 0 is not a branch')
if d[56:60] != b'ARMd':
    errs.append(f'ARMd marker missing at 56 (got {d[56:60]!r})')
if struct.unpack_from('<I', d, 4)[0] != 0:
    errs.append('text_offset is not 0')
if struct.unpack_from('<Q', d, 8)[0] != len(d):
    errs.append('image_size does not match the file length')
if struct.unpack_from('<Q', d, 16)[0] != 0:
    errs.append('flags are not 0')
if len(d) > 0x200000:
    errs.append(f'image is {len(d)} bytes, over the 2 MiB L3 mapping limit')
print('\n'.join('FAIL: ' + e for e in errs) if errs else f'  ok   {len(d)} bytes')
sys.exit(1 if errs else 0)
PY

echo
echo "== boot image"
python3 - <<'PY'
import gzip, struct
d = open('probe/out/boot.img', 'rb').read()
f = struct.unpack_from('<8s10I16s512s', d, 0)
assert f[0] == b'ANDROID!', 'bad magic'
page, ksize, kaddr = f[8], f[1], f[2]
rsize, raddr = f[3], f[4]
assert d[page:page+2] == b'\x1f\x8b', 'kernel is not gzip at the page offset'
raw = gzip.decompress(d[page:page+ksize])
assert raw[56:60] == b'ARMd', 'kernel lost its ARMd marker'
roff = page + ((ksize + page - 1) // page) * page
assert d[roff:roff+6] == b'070701', 'ramdisk is not newc cpio'
print(f'  ok   {len(d)} bytes, kernel {ksize} gz @ {kaddr:#x}, '
      f'ramdisk {rsize} @ {raddr:#x}')
PY

echo
echo "all checks passed"
