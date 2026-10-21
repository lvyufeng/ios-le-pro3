/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * fdt_read.h - read-only Flattened Device Tree access for the zl1 shim.
 *
 * The probe's parser (probe/zl1_probe.c) only needed /memory. The shim
 * needs more: reserved regions (so the XNU payload never lands on the
 * modem or the firmware carve-outs), the CPU list, and the /chosen bootargs
 * the bootloader already set.
 *
 * Everything here is bounds-checked against the header's totalsize. A
 * corrupt or truncated DTB must yield "not found", never a walk off the
 * end - the shim runs before any exception handling beyond the probe's
 * vectors exists.
 */
#ifndef ZL1_FDT_READ_H
#define ZL1_FDT_READ_H

#include <stdint.h>

#define FDT_MAGIC      0xd00dfeedu
#define FDT_BEGIN_NODE 0x00000001u
#define FDT_END_NODE   0x00000002u
#define FDT_PROP       0x00000003u
#define FDT_NOP        0x00000004u
#define FDT_END        0x00000009u

#define FDT_MAX_DEPTH  32
#define FDT_MAX_MEM    8

typedef struct {
    uint64_t base;
    uint64_t size;
} fdt_mem_t;

typedef struct {
    const uint8_t *base;     /* start of the blob */
    uint32_t totalsize;
    uint32_t off_struct;
    uint32_t size_struct;
    uint32_t off_strings;
    uint32_t size_strings;

    /* Filled by fdt_read_memory(). */
    fdt_mem_t mem[FDT_MAX_MEM];
    int mem_count;

    /* Filled by fdt_read_reserved(): total bytes under no-map. */
    uint64_t reserved_nomap_bytes;
    int reserved_count;

    /* Filled by fdt_read_cpus(): the MPIDRs listed under /cpus. */
    uint64_t mpidr[8];
    int cpu_count;
} fdt_info_t;

/* Validate the header and fill the read-only fields of `out`.
 * Returns 0 on success, negative on a bad header. `out` is zeroed first. */
int fdt_open(fdt_info_t *out, uint64_t fdt_addr);

/* Walk /memory and /memory@* and record each reg entry. Returns the number
 * of banks recorded (0 if none). */
int fdt_read_memory(fdt_info_t *f);

/* Walk /reserved-memory and total the regions marked no-map. */
void fdt_read_reserved(fdt_info_t *f);

/* Walk /cpus/cpu@* and record each reg value as an MPIDR. */
void fdt_read_cpus(fdt_info_t *f);

/* Return the first 64-bit /chosen property `name`, or 0 if absent. */
uint64_t fdt_chosen_u64(const fdt_info_t *f, const char *name);

/* Copy /chosen/bootargs into buf (NUL terminated). Returns the length
 * copied, or -1 on overflow, 0 if the property is absent. */
int fdt_chosen_bootargs(const fdt_info_t *f, char *buf, uint32_t cap);

/* Total RAM in bytes across all recorded banks. */
uint64_t fdt_total_memory(const fdt_info_t *f);

#endif /* ZL1_FDT_READ_H */