#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# test_fdt.sh - host test for the shim's FDT reader.
#
# Builds a zl1-shaped DTB with tools/mkfdt.py, compiles shim/fdt_read.c for
# the host without modification, and runs the checks in fdt_read_test.c.
# This is the one part of the shim that can be proven before the phone is
# available, so it runs on every selfcheck.
set -eu
cd "$(dirname "$0")"
OUT=out
mkdir -p "$OUT"

python3 ../tools/mkfdt.py "$OUT/zl1-test.dtb"

cc -std=c99 -Wall -Wextra -Werror -O1 -I. \
   fdt_read.c fdt_read_test.c -o "$OUT/fdt_read_test"
"$OUT/fdt_read_test" "$OUT/zl1-test.dtb"

# loader_place.c has no device dependency, so its placement rules are proven
# here rather than discovered when a kernel lands on the modem.
cc -std=c99 -Wall -Wextra -Werror -O1 -I. \
   loader_place.c loader_range_test.c -o "$OUT/loader_range_test"
"$OUT/loader_range_test"
