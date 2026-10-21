/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * loader_range_test.c - host test for the loader's placement safety check.
 *
 * `loader_range_is_usable` is the function that decides whether the XNU
 * payload may be copied to an address. Getting it wrong drops a kernel onto
 * the modem's memory and the phone dies in a way that looks like a power
 * problem. It has no hardware dependency, so it is cheaper to prove here
 * than to diagnose on the device.
 *
 * Build and run: shim/test_fdt.sh
 */
#include <stdio.h>
#include <string.h>

#include "fdt_read.h"
#include "loader.h"

static int failures;

#define CHECK(cond, msg)                                                \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);       \
            failures++;                                                \
        }                                                              \
    } while (0)

/* The zl1-shaped map: 512 MiB of DRAM at 2 GiB, with the modem carve-out
 * and the removed_regions block from the LeEco device tree sitting in it. */
static void zl1_map(fdt_info_t *f)
{
    memset(f, 0, sizeof(*f));
    f->mem[0].base = 0x80000000ULL;
    f->mem[0].size = 0x20000000ULL;
    f->mem_count = 1;

    f->resv[0].base = 0x85800000ULL;      /* removed_regions */
    f->resv[0].size = 0x03000000ULL;
    f->resv[1].base = 0x88800000ULL;      /* modem */
    f->resv[1].size = 0x06200000ULL;
    f->resv_count = 2;
}

int main(void)
{
    fdt_info_t f;
    zl1_map(&f);

    /* Plain RAM, far from both carve-outs. */
    CHECK(loader_range_is_usable(&f, 0x90000000ULL, 0x100000ULL),
          "plain RAM below the carve-outs was rejected");

    /* Inside the modem region: must be refused. */
    CHECK(!loader_range_is_usable(&f, 0x88800000ULL, 0x1000ULL),
          "a range starting inside the modem carve-out was accepted");
    CHECK(!loader_range_is_usable(&f, 0x88900000ULL, 0x1000ULL),
          "a range deep inside the modem carve-out was accepted");

    /* Straddling the modem's start. */
    CHECK(!loader_range_is_usable(&f, 0x887FF000ULL, 0x2000ULL),
          "a range straddling the modem start was accepted");

    /* Straddling the end of removed_regions. */
    CHECK(!loader_range_is_usable(&f, 0x887FF000ULL, 0x2000ULL),
          "a range straddling removed_regions/modems was accepted");

    /* Immediately after a carve-out is fine. */
    CHECK(loader_range_is_usable(&f, 0x8EA00000ULL, 0x1000ULL),
          "RAM right after the modem carve-out was rejected");

    /* Below the memory bank. */
    CHECK(!loader_range_is_usable(&f, 0x70000000ULL, 0x1000ULL),
          "a range below DRAM was accepted");

    /* Past the end of the bank. */
    CHECK(!loader_range_is_usable(&f, 0x9FF00000ULL, 0x200000ULL),
          "a range running past the end of DRAM was accepted");

    /* Exactly the last page of the bank. */
    CHECK(loader_range_is_usable(&f, 0x9FFFF000ULL, 0x1000ULL),
          "the last page of DRAM was rejected");

    /* Overflow: base+len wraps. */
    CHECK(!loader_range_is_usable(&f, 0xFFFFFFFFFFFFF000ULL, 0x2000ULL),
          "an overflowing range was accepted");

    /* Zero length is a no-op and must be allowed (segments can be bss-only
     * if a caller asks, and a mid-range check with len 0 is harmless). */
    CHECK(loader_range_is_usable(&f, 0x90000000ULL, 0),
          "a zero-length range was rejected");

    /* A tree with no carve-out recorded (fdt_read_reserved not called). */
    fdt_info_t bare;
    zl1_map(&bare);
    bare.resv_count = 0;
    CHECK(loader_range_is_usable(&bare, 0x88800000ULL, 0x1000ULL),
          "with no carve-outs recorded the check should pass");

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("loader_range: all checks passed\n");
    return 0;
}