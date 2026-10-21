/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * loader.h - act as iBoot for XNU on zl1.
 */
#ifndef ZL1_LOADER_H
#define ZL1_LOADER_H

#include <stdint.h>

#include "fdt_read.h"

typedef struct {
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t file_off;
    uint64_t file_size;
} payload_seg_t;

#define PAYLOAD_MAGIC_LEN 8
#define BOOT_LINE_LENGTH  608

typedef struct {
    char magic[PAYLOAD_MAGIC_LEN];   /* "ZL1XNU\0\0" */
    uint32_t version;                /* 1 */
    uint32_t nsegs;
    uint64_t virt_base;
    uint64_t entry_va;
    uint64_t adt_off;
    uint64_t adt_size;
    uint64_t kernel_end;
    uint64_t phys_base;
    uint64_t mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    /* payload_seg_t segs[nsegs] follows the header */
} payload_t;

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2. Offsets are asserted in
 * loader.c against the values the kernel expects; do not reorder. */
typedef struct {
    uint64_t v_baseAddr;
    uint64_t v_display;
    uint64_t v_rowBytes;
    uint64_t v_width;
    uint64_t v_height;
    uint64_t v_depth;
} boot_video_t;

typedef struct {
    uint16_t Revision;
    uint16_t Version;
    uint64_t virtBase;
    uint64_t physBase;
    uint64_t memSize;
    uint64_t topOfKernelData;
    boot_video_t Video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char CommandLine[BOOT_LINE_LENGTH];
    uint64_t bootFlags;
    uint64_t memSizeActual;
} boot_args_t;

/* Exposed for the host test: is [addr, addr+len) inside a memory bank and
 * clear of every no-map carve-out? */
int loader_range_is_usable(const fdt_info_t *f, uint64_t addr, uint64_t len);

/* Copy the payload's segments into RAM, place the ADT and boot_args behind
 * the kernel and jump to it at EL1 with the MMU off. Returns only on an
 * error, with a human readable reason. */
const char *loader_boot(uint64_t dtb, uint64_t fb_phys, uint32_t fb_w,
                        uint32_t fb_h, uint32_t fb_stride);

#endif /* ZL1_LOADER_H */