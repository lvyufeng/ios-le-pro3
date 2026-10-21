/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * loader_place.c - where may the payload go?
 *
 * Split out of loader.c so it can be compiled for the host and tested
 * (shim/loader_range_test.c). This is the function that decides whether a
 * kernel segment may be written to an address; it must not depend on
 * anything that only exists on the phone.
 */
#include "loader.h"

/*
 * True when [addr, addr+len) lies inside one of the FDT's memory banks and
 * does not overlap any no-map carve-out. This is the check that keeps the
 * payload off the modem, the firmware regions and the secure world: the
 * payload was built on a host and knows nothing about what this particular
 * bootloader set aside.
 */
int loader_range_is_usable(const fdt_info_t *f, uint64_t addr, uint64_t len)
{
    if (!len)
        return 1;
    const uint64_t end = addr + len;
    if (end < addr)                 /* overflow */
        return 0;

    int in_bank = 0;
    for (int i = 0; i < f->mem_count; i++) {
        const uint64_t b0 = f->mem[i].base;
        const uint64_t b1 = b0 + f->mem[i].size;
        if (addr >= b0 && end <= b1) {
            in_bank = 1;
            break;
        }
    }
    if (!in_bank)
        return 0;

    for (int i = 0; i < f->resv_count; i++) {
        const uint64_t r0 = f->resv[i].base;
        const uint64_t r1 = r0 + f->resv[i].size;
        if (addr < r1 && r0 < end)
            return 0;       /* overlaps a no-map region */
    }
    return 1;
}
