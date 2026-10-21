/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * loader.c - be iBoot: load the XNU payload and jump to it.
 *
 * Adapted from ios-sweet's shim/loader.c. The differences that matter on
 * zl1 (MSM8996, ARMv8.0, 4 KiB pages) versus sweet (SM7150, ARMv8.2,
 * 16 KiB pages):
 *
 *   1. PAGE_4K, not PAGE_16K. Sizing boot_args and the ADT by a 16 KiB
 *      page would put them across a page the kernel maps differently.
 *   2. The framebuffer comes from the caller (the probe's constant), not a
 *      compile-time FB_BASE, because the splash address is a bootloader
 *      property this port has only seen in a device tree so far.
 *   3. Every address the payload lands on is checked against the live
 *      FDT's memory map *and* its no-map carve-outs. On sweet the payload
 *      places the kernel at a fixed physical base; doing that here without
 *      checking would eventually drop it on the modem.
 *
 * Everything the payload needs is precomputed by tools/mkpayload.py: the
 * chained fixups are already resolved, so this file only copies segments,
 * places the ADT and boot_args, and jumps.
 */
#include <stdint.h>

#include "fdt_read.h"
#include "loader.h"

/* Console hooks, supplied by the shim's main. */
void shim_out(const char *s);
void shim_kv(const char *key, uint64_t value);

#define PAGE_4K 0x1000ULL

/* Offsets the kernel depends on. If any of these tripped, the struct above
 * no longer matches pexpert/boot.h and the jump would hand XNU garbage. */
_Static_assert(__builtin_offsetof(boot_args_t, Video) == 40,
               "boot_args.Video must be at offset 40");
_Static_assert(__builtin_offsetof(boot_args_t, CommandLine) == 108,
               "boot_args.CommandLine must be at offset 108");
_Static_assert(sizeof(boot_args_t) == 736, "boot_args must be 736 bytes");

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* MMU off: everything is Device memory, so copy in 8-byte words. */
static void copy64(uint64_t dst, uint64_t src, uint64_t n)
{
    volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
    volatile const uint64_t *s = (volatile const uint64_t *)(uintptr_t)src;
    for (uint64_t i = 0; i < (n + 7) / 8; i++)
        d[i] = s[i];
}

static void zero64(uint64_t dst, uint64_t n)
{
    volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)dst;
    for (uint64_t i = 0; i < n / 8; i++)
        d[i] = 0;
}

static int magic_ok(const char *m)
{
    static const char want[PAYLOAD_MAGIC_LEN] = {'Z', 'L', '1', 'X', 'N', 'U', 0, 0};
    for (int i = 0; i < PAYLOAD_MAGIC_LEN; i++)
        if (m[i] != want[i])
            return 0;
    return 1;
}

static uint64_t align_down(uint64_t v, uint64_t a)
{
    return v & ~(a - 1);
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

const char *loader_boot(uint64_t dtb, uint64_t fb_phys, uint32_t fb_w,
                        uint32_t fb_h, uint32_t fb_stride)
{
    fdt_info_t f;
    const int orc = fdt_open(&f, dtb);
    shim_kv("fdt_open", (uint64_t)(int64_t)orc);
    if (orc)
        return "device tree did not open";
    fdt_read_memory(&f);
    fdt_read_reserved(&f);
    shim_kv("mem_banks", (uint64_t)f.mem_count);
    shim_kv("reserved_nomap", f.reserved_nomap_bytes);

    uint64_t rd_start = 0, rd_end = 0;
    rd_start = fdt_chosen_u64(&f, "linux,initrd-start");
    rd_end = fdt_chosen_u64(&f, "linux,initrd-end");
    shim_kv("initrd_start", rd_start);
    shim_kv("initrd_end", rd_end);
    if (!rd_start || rd_end <= rd_start)
        return "no initrd in /chosen";

    const payload_t *p = (const payload_t *)(uintptr_t)rd_start;
    if (!magic_ok(p->magic))
        return "no ZL1XNU payload";
    if (p->version != 1)
        return "payload version is not 1";
    if ((rd_start & 7) || (p->adt_off & 7))
        return "payload is misaligned";

    shim_kv("virt_base", p->virt_base);
    shim_kv("phys_base", p->phys_base);
    shim_kv("mem_size", p->mem_size);
    shim_kv("entry_va", p->entry_va);
    shim_kv("nsegs", p->nsegs);
    shim_kv("kernel_end", p->kernel_end);

    /* The window the kernel is about to own must be real RAM and must not
     * sit on a no-map carve-out. */
    if (!loader_range_is_usable(&f, p->phys_base, p->mem_size))
        return "kernel window is not usable RAM";

    /* The ramdisk must not be clobbered by the copy. */
    if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
        return "ramdisk overlaps kernel memory";

    const payload_seg_t *segs = (const payload_seg_t *)((uintptr_t)p + sizeof(*p));
    /* Header + segments must fit inside the ramdisk we were handed. */
    if (rd_start + sizeof(*p) + (uint64_t)p->nsegs * sizeof(*segs) > rd_end)
        return "payload segment table runs past the ramdisk";

    const uint64_t pa_of_va_base = p->phys_base - p->virt_base;

    for (uint32_t i = 0; i < p->nsegs; i++) {
        const payload_seg_t *s = &segs[i];
        if (rd_start + s->file_off + s->file_size > rd_end)
            return "payload segment runs past the ramdisk";
        const uint64_t dst = s->vmaddr + pa_of_va_base;
        if (!loader_range_is_usable(&f, align_down(dst, PAGE_4K),
                             align_up(s->vmsize, PAGE_4K)))
            return "payload segment lands outside usable RAM";
        copy64(dst, rd_start + s->file_off, s->file_size);
        if (s->vmsize > s->file_size) {
            const uint64_t written = (s->file_size + 7) & ~7ULL;
            zero64(dst + written, s->vmsize - written);
        }
    }
    shim_kv("segments_copied", p->nsegs);

    /* ADT and boot_args go behind the kernel image. */
    const uint64_t adt_pa = p->kernel_end + pa_of_va_base;
    if (!loader_range_is_usable(&f, align_down(adt_pa, PAGE_4K),
                         align_up(p->adt_size, PAGE_4K)))
        return "ADT would land outside usable RAM";
    copy64(adt_pa, rd_start + p->adt_off, p->adt_size);

    const uint64_t ba_pa = align_up(adt_pa + p->adt_size, PAGE_4K);
    const uint64_t top = ba_pa + PAGE_4K;
    if (!loader_range_is_usable(&f, ba_pa, PAGE_4K))
        return "boot_args would land outside usable RAM";

    zero64(ba_pa, PAGE_4K);
    volatile boot_args_t *ba = (volatile boot_args_t *)(uintptr_t)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = top;
    ba->Video.v_baseAddr = fb_phys;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = fb_stride;
    ba->Video.v_width = fb_w;
    ba->Video.v_height = fb_h;
    ba->Video.v_depth = 32;
    ba->machineType = 0;
    /* XNU reads the ADT with the MMU on, so this has to be the virtual
     * address, not the physical one the copy above used. */
    ba->deviceTreeP = adt_pa - pa_of_va_base;
    ba->deviceTreeLength = (uint32_t)p->adt_size;
    for (int i = 0; i < BOOT_LINE_LENGTH; i++)
        ba->CommandLine[i] = p->cmdline[i];
    ba->bootFlags = 0;
    ba->memSizeActual = p->mem_size;

    shim_kv("adt_pa", adt_pa);
    shim_kv("boot_args_pa", ba_pa);
    shim_kv("top_of_kernel_data", top);

    const uint64_t entry_pa = p->entry_va + pa_of_va_base;
    shim_kv("jump_to", entry_pa);
    shim_out("jumping to XNU\n");

    /* Writes were done with the MMU and caches off; drop stale I-cache. */
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" : : : "memory");
    ((xnu_entry_t)(uintptr_t)entry_pa)(ba_pa);
    return "XNU returned";
}