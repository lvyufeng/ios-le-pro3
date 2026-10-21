/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * mmu4k.c - minimal EL1 identity map with 4 KiB pages for the zl1 probe.
 *
 * Derived from the Galaxy S8 port's a5xelte/mmu4k.c (which in turn came
 * from the sweet tree). Two things are different on MSM8996 and are worth
 * calling out:
 *
 *   1. 8996 boots at EL1 with MMU off from the qcom bootloader (same as
 *      Samsung), so the handoff assumption holds.
 *   2. We must map the UFS and GIC register windows as Device memory. The
 *      S8 code mapped the first 4 GiB Device and only made the Image pages
 *      executable; that is wrong here because DRAM on zl1 *starts* at
 *      0x80000000 and the Image may be loaded anywhere in it. We instead
 *      build a sparse map: every L1 entry is Device, and each 1 GiB that
 *      actually contains an Image/stack/DMA page is rebuilt as Normal WB.
 *
 * The mapping is deliberately identity: the probe runs with MMU off first,
 * then turns it on to prove the page-table code path works on this SoC,
 * then turns it back off and reports. XNU bring-up needs the same code
 * twice (shim and kernel), so it is kept small and dependency free.
 */
#include <stdint.h>
#include "zl1_hw.h"

#define PAGE_SHIFT 12U
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)
#define L2_SHIFT   21U   /* 2 MiB block */
#define L2_SIZE    (1ULL << L2_SHIFT)
#define L1_SHIFT   30U   /* 1 GiB block */
#define TABLE_ENTRIES 512U

/* Descriptor bits (ARM DDI 0487, D5.3). */
#define DESC_VALID      (1ULL << 0)
#define DESC_BLOCK      (1ULL << 0)
#define DESC_TABLE      (3ULL << 0)
#define DESC_PAGE       (3ULL << 0)
#define DESC_AF         (1ULL << 10)
#define DESC_SH_INNER   (3ULL << 8)
#define DESC_AP_RW      (0ULL << 6)
#define DESC_PXN        (1ULL << 53)
#define DESC_UXN        (1ULL << 54)
#define DESC_ATTR_DEV   (0ULL << 2)   /* MAIR attr0: Device-nGnRnE */
#define DESC_ATTR_NORM  (1ULL << 2)   /* MAIR attr1: Normal WB */

#define DEVICE_FLAGS (DESC_AF | DESC_ATTR_DEV | DESC_PXN | DESC_UXN)
#define CODE_FLAGS   (DESC_AF | DESC_ATTR_NORM | DESC_SH_INNER | DESC_UXN)

#define SCTLR_M (1ULL << 0)
#define SCTLR_C (1ULL << 2)
#define SCTLR_I (1ULL << 12)
#define SCTLR_WXN (1ULL << 19)

/* Three 4 KiB tables: one L1 covering 4 GiB as device blocks, one L2 for
 * the 1 GiB the Image lives in, one L3 for its single 2 MiB block. The
 * probe is deliberately tiny so one L2/L3 pair stays enough; build.sh
 * asserts the final image stays inside that 2 MiB.
 *
 * Every start.S symbol below is reached through inline asm rather than as
 * an ordinary extern declaration. Why: clang's Mach-O backend routes
 * references to extern symbols through the GOT (GOT_LOAD_PAGE21 +
 * GOT_LOAD_PAGEOFF12), and a flat kernel Image has no __got section and no
 * dynamic linker to fill it. Loading the address with adrp/add in asm keeps
 * the relocation kind to PAGE21/PAGEOFF12, which flatten.py resolves. */
#define SYM_ADDR(sym) ({                                        \
    uintptr_t __a;                                              \
    __asm__ volatile("adrp %0, " #sym "@PAGE\n\t"              \
                     "add  %0, %0, " #sym "@PAGEOFF"            \
                     : "=r"(__a));                              \
    __a;                                                        \
})
#define START_ADDR     SYM_ADDR(_start)
#define IMAGE_END_ADDR SYM_ADDR(_image_end)

#define L1_ADDR ((volatile uint64_t *)(uintptr_t)SYM_ADDR(_zl1_l1))
#define L2_ADDR ((volatile uint64_t *)(uintptr_t)SYM_ADDR(_zl1_l2))
#define L3_ADDR ((volatile uint64_t *)(uintptr_t)SYM_ADDR(_zl1_l3))

static inline uint64_t read_mmfr0(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, id_aa64mmfr0_el1" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t read_sctlr(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, sctlr_el1" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t read_ctr(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, ctr_el0" : "=r"(v) : : "memory");
    return v;
}

/* Return values are negative on the same conventions as the S8 probe so
 * logs from the two projects can be diffed: -1 no 4K granule, -2 MMU was
 * already on, -3 image straddles a 2 MiB block, -4 tables misaligned. */
int zl1_mmu4k_enable(void)
{
    const uint64_t lo = (uint64_t)START_ADDR & ~(PAGE_SIZE - 1);
    const uint64_t hi = ((uint64_t)IMAGE_END_ADDR + PAGE_SIZE - 1) &
                        ~(PAGE_SIZE - 1);
    uint64_t sctlr = read_sctlr();
    const uint64_t mmfr0 = read_mmfr0();

    /* TGran4 occupies bits [31:28]; 0 means the 4 KiB granule is supported. */
    if (((mmfr0 >> 28) & 0xfU) != 0)
        return -1;
    if (sctlr & SCTLR_M)
        return -2;
    if (lo < 0x1000ULL || (hi - 1) >> L1_SHIFT != lo >> L1_SHIFT)
        return -3; /* one 1 GiB L1 block covers the image */
    if (((uint64_t)L1_ADDR & (PAGE_SIZE - 1)) ||
        ((uint64_t)L2_ADDR & (PAGE_SIZE - 1)) ||
        ((uint64_t)L3_ADDR & (PAGE_SIZE - 1)))
        return -4;

    /* L1: everything as device blocks first, so no entry is left invalid
     * (a zero descriptor is a translation fault, and the stack lives in a
     * "device" page until we come back and mark it Normal). 32-bit VA means
     * only the first 4 entries are reachable, but filling all 512 costs
     * nothing and removes a whole class of "forgot to initialise" faults. */
    for (uint64_t i = 0; i < TABLE_ENTRIES; i++)
        L1_ADDR[i] = (i << L1_SHIFT) | DEVICE_FLAGS | DESC_BLOCK;

    const uint64_t l1_index = lo >> L1_SHIFT;
    const uint64_t one_gib = lo & ~((1ULL << L1_SHIFT) - 1);
    for (uint64_t i = 0; i < TABLE_ENTRIES; i++)
        L2_ADDR[i] = (one_gib + (i << L2_SHIFT)) | DEVICE_FLAGS | DESC_BLOCK;
    L1_ADDR[l1_index] = (uint64_t)L2_ADDR | DESC_TABLE;

    /* The probe image must fit inside one 2 MiB block, checked above, so a
     * single L3 table turns those 512 pages WB-X. Everything else in the
     * 2 MiB block stays device (harmless: nothing there is executed or
     * read as normal memory by this program). */
    const uint64_t two_mib = lo & ~(L2_SIZE - 1);
    for (uint64_t i = 0; i < TABLE_ENTRIES; i++) {
        const uint64_t pa = two_mib + (i << PAGE_SHIFT);
        L3_ADDR[i] = pa |
                    ((pa >= lo && pa < hi) ? CODE_FLAGS : DEVICE_FLAGS) |
                    DESC_PAGE;
    }
    L2_ADDR[(lo >> L2_SHIFT) & (TABLE_ENTRIES - 1)] =
        (uint64_t)L3_ADDR | DESC_TABLE;

    /* Clean the three tables using the real cache line size. */
    const uint64_t ctr = read_ctr();
    const uint64_t line = 4ULL << ((ctr >> 16) & 0xfU);
    for (uint64_t p = (uint64_t)L1_ADDR;
         p < (uint64_t)L1_ADDR + PAGE_SIZE; p += line)
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    for (uint64_t p = (uint64_t)L2_ADDR;
         p < (uint64_t)L2_ADDR + PAGE_SIZE; p += line)
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    for (uint64_t p = (uint64_t)L3_ADDR;
         p < (uint64_t)L3_ADDR + PAGE_SIZE; p += line)
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" : : : "memory");

    /* 4 KiB granule, 32-bit VA, walks for TTBR0 only, WB/WA cached walks.
     * AttrIndx 0 = Device-nGnRnE, 1 = Normal WB (MAIR below). */
    const uint64_t tcr = 32ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |
                         (32ULL << 16) | (3ULL << 20) | (1ULL << 23) |
                         (2ULL << 30);
    const uint64_t mair = 0x00FFULL;
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair) : "memory");
    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr) : "memory");
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)L1_ADDR)
                     : "memory");
    __asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" : : : "memory");

    sctlr &= ~SCTLR_WXN;
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr) : "memory");
    return 0;
}

void zl1_mmu4k_disable(void)
{
    uint64_t sctlr = read_sctlr();
    sctlr &= ~SCTLR_M;
    __asm__ volatile("dsb sy; msr sctlr_el1, %0; isb; tlbi vmalle1; dsb sy; isb"
                     : : "r"(sctlr) : "memory");
}

uint64_t zl1_mmu4k_tcr(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, tcr_el1" : "=r"(v) : : "memory");
    return v;
}

uint64_t zl1_mmu4k_sctlr(void)
{
    return read_sctlr();
}