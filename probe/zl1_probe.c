/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * zl1_probe.c - bare-metal, read-only fact collector for the Le Pro 3.
 *
 * Design rules, learned from the S8 and sweet bring-ups:
 *
 *   * This program runs from the RAM kernel slot before any OS. It writes
 *     NOTHING except the splash framebuffer, which the bootloader already
 *     owns and which nobody repaints while we spin here.
 *   * Every claim is one log line. A failed claim prints the failure instead
 *     of hanging, so one boot yields a complete report even when half the
 *     address-map guesses are wrong.
 *   * The display IS the log. The retail zl1 has no exposed UART and there
 *     is no ramoops before Android boots, so the splash buffer is the only
 *     channel that needs no driver we do not have yet.
 *
 * Line format is "name: value" so tools/zl1-log.py can diff reports from
 * this device against the Galaxy S8 / sweet reports we are borrowing code
 * and expectations from.
 */
#include <stdint.h>
#include "zl1_hw.h"
#include "font.h"

/* probe/mmu4k.c */
int zl1_mmu4k_enable(void);
void zl1_ufs_probe(void);
void zl1_mmu4k_disable(void);
uint64_t zl1_mmu4k_tcr(void);
uint64_t zl1_mmu4k_sctlr(void);

/* ---------------------------------------------------------------- */
/* Framebuffer console                                              */
/* ---------------------------------------------------------------- */
/* Glyph == 1 pa, 2x2 scaled on a 1080-wide panel: 216 columns x 60 rows at
 * 16x32 px per cell. Scaled because 1x on a 5.5" 1080p panel is unreadable
 * from a photo, and photos are how these logs come back. */
#define GLYPH_W 8u
#define GLYPH_H 16u
#define SCALE   2u
#define CELL_W  (GLYPH_W * SCALE)
#define CELL_H  (GLYPH_H * SCALE)
#define COLS    (ZL1_FB_WIDTH / CELL_W)
#define ROWS    (ZL1_FB_HEIGHT / CELL_H)

static volatile uint32_t *const fb = (volatile uint32_t *)ZL1_FB_PHYS;
static uint32_t cur_col, cur_row;
static const uint32_t FG = 0x00FFFFFFu;
static const uint32_t BG = 0x00000000u;

static void fb_px(uint32_t x, uint32_t y, uint32_t v)
{
    if (x < ZL1_FB_WIDTH && y < ZL1_FB_HEIGHT)
        fb[(uint64_t)y * ZL1_FB_WIDTH + x] = v;
}

static void con_clear(void)
{
    for (uint32_t y = 0; y < ZL1_FB_HEIGHT; y++) {
        volatile uint32_t *line = fb + (uint64_t)y * ZL1_FB_WIDTH;
        for (uint32_t x = 0; x < ZL1_FB_WIDTH; x++)
            line[x] = BG;
    }
    cur_col = cur_row = 0;
}

static void con_scroll(void)
{
    const uint64_t keep = (uint64_t)ZL1_FB_WIDTH * (ZL1_FB_HEIGHT - CELL_H);
    for (uint64_t i = 0; i < keep; i++)
        fb[i] = fb[i + (uint64_t)ZL1_FB_WIDTH * CELL_H];
    for (uint32_t y = ZL1_FB_HEIGHT - CELL_H; y < ZL1_FB_HEIGHT; y++)
        for (uint32_t x = 0; x < ZL1_FB_WIDTH; x++)
            fb[(uint64_t)y * ZL1_FB_WIDTH + x] = BG;
    cur_row = ROWS - 1;
}

static void con_putc(char c)
{
    if (c == '\n') {
        cur_col = 0;
        if (++cur_row >= ROWS)
            con_scroll();
        return;
    }
    if (c < 0x20 || c > 0x7e)
        c = '?';
    const unsigned char *g = font8x16[(int)c - 0x20];
    const uint32_t x0 = cur_col * CELL_W, y0 = cur_row * CELL_H;
    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            const uint32_t px = (g[gy] & (0x80u >> gx)) ? FG : BG;
            for (uint32_t sy = 0; sy < SCALE; sy++)
                for (uint32_t sx = 0; sx < SCALE; sx++)
                    fb_px(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, px);
        }
    }
    if (++cur_col >= COLS) {
        cur_col = 0;
        if (++cur_row >= ROWS)
            con_scroll();
    }
}

static void con_str(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void con_hex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    char b[18];
    int n = 0;
    con_str("0x");
    if (!v) {
        con_putc('0');
        return;
    }
    while (v) {
        b[n++] = d[v & 0xf];
        v >>= 4;
    }
    while (n--)
        con_putc(b[n]);
}

static void con_dec(uint64_t v)
{
    char b[22];
    int n = 0;
    if (!v) {
        con_putc('0');
        return;
    }
    while (v) {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n--)
        con_putc(b[n]);
}

/* Exported for probe/ufs.c. They are declared there rather than in a header
 * because ufs.c is the only other user and C has no need to be clever here. */
void probe_out(const char *s) { con_str(s); }
void probe_dec(uint64_t v) { con_dec(v); }

static void kv(const char *k, uint64_t v)
{
    con_str(k);
    con_str(": ");
    con_hex(v);
    con_putc('\n');
}

static void kvd(const char *k, uint64_t v)
{
    con_str(k);
    con_str(": ");
    con_dec(v);
    con_putc('\n');
}

static void kvs(const char *k, const char *v)
{
    con_str(k);
    con_str(": ");
    con_str(v);
    con_putc('\n');
}

/* Exported for probe/ufs.c, after the static helpers they wrap. */
void probe_kv(const char *k, uint64_t v) { kv(k, v); }
void probe_kvd(const char *k, uint64_t v) { kvd(k, v); }

/* ---------------------------------------------------------------- */
/* System registers                                                 */
/* ---------------------------------------------------------------- */
#define MRS_FN(fn, reg)                                             \
    static inline uint64_t fn(void)                                 \
    {                                                               \
        uint64_t v;                                                 \
        __asm__ volatile("isb; mrs %0, " reg : "=r"(v) : : "memory"); \
        return v;                                                   \
    }
MRS_FN(rd_currentel, "currentel")
MRS_FN(rd_sctlr, "sctlr_el1")
MRS_FN(rd_mpidr, "mpidr_el1")
MRS_FN(rd_midr, "midr_el1")
MRS_FN(rd_revidr, "revidr_el1")
MRS_FN(rd_aa64pfr0, "id_aa64pfr0_el1")
MRS_FN(rd_aa64pfr1, "id_aa64pfr1_el1")
MRS_FN(rd_aa64isar0, "id_aa64isar0_el1")
MRS_FN(rd_aa64isar1, "id_aa64isar1_el1")
MRS_FN(rd_aa64mmfr0, "id_aa64mmfr0_el1")
MRS_FN(rd_aa64mmfr1, "id_aa64mmfr1_el1")
MRS_FN(rd_aa64mmfr2, "id_aa64mmfr2_el1")
MRS_FN(rd_cntfrq, "cntfrq_el0")
MRS_FN(rd_cntpct, "cntpct_el0")
MRS_FN(rd_ctr, "ctr_el0")
MRS_FN(rd_dczid, "dczid_el0")
MRS_FN(rd_ccsidr, "ccsidr_el1")
MRS_FN(rd_clidr, "clidr_el1")

static inline uint32_t mmio_rd(uint64_t a)
{
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
    return v;
}

/* ---------------------------------------------------------------- */
/* Device tree, read-only, no libc                                  */
/* ---------------------------------------------------------------- */
#define FDT_MAGIC      0xd00dfeedu
#define FDT_BEGIN_NODE 0x1u
#define FDT_END_NODE   0x2u
#define FDT_PROP       0x3u
#define FDT_NOP        0x4u
#define FDT_END        0x9u

typedef struct {
    const uint8_t *base;
    uint32_t off_struct, off_strings, size_struct;
} fdt_t;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static int fdt_open(fdt_t *f, uintptr_t dtb)
{
    const uint8_t *b = (const uint8_t *)dtb;
    if (!dtb || (dtb & 7))
        return -1;
    if (be32(b) != FDT_MAGIC)
        return -2;
    f->base = b;
    f->off_struct = be32(b + 8);
    f->off_strings = be32(b + 12);
    f->size_struct = be32(b + 36);
    return 0;
}

/* Sum the /memory reg entries and split them into below/above 4 GiB. */
static uint64_t fdt_memory(const fdt_t *f, uint64_t *low, uint64_t *high)
{
    const uint8_t *p = f->base + f->off_struct;
    const uint8_t *end = p + f->size_struct;
    uint64_t total = 0, base = 0, size = 0;
    int depth = 0, in_mem = 0, addr_cells = 2, size_cells = 2;

    *low = *high = 0;
    while (p < end) {
        const uint32_t tok = be32(p);
        p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            while (*p)
                p++;
            p = (const uint8_t *)(((uintptr_t)p + 4) & ~(uintptr_t)3);
            depth++;
            in_mem = depth == 1 && name[0] == 'm' && name[1] == 'e' &&
                     name[2] == 'm' && name[3] == 'o' && name[4] == 'r' &&
                     name[5] == 'y';
            base = size = 0;
        } else if (tok == FDT_PROP) {
            const uint32_t len = be32(p), nameoff = be32(p + 4);
            const uint8_t *val = p + 8;
            const char *pn = (const char *)(f->base + f->off_strings + nameoff);
            if (in_mem) {
                if (pn[0] == 'r' && pn[1] == 'e' && pn[2] == 'g') {
                    if (addr_cells == 2 && len >= 16) {
                        base = be64(val);
                        size = be64(val + 8);
                    } else if (len >= 8) {
                        base = be32(val);
                        size = be32(val + 4);
                    }
                }
            } else if (pn[0] == '#' && pn[1] == 'a' && pn[2] == 'd') {
                addr_cells = (int)be32(val);
            } else if (pn[0] == '#' && pn[1] == 's') {
                size_cells = (int)be32(val);
            }
            (void)size_cells;
            p = (const uint8_t *)(((uintptr_t)val + len + 3) & ~(uintptr_t)3);
        } else if (tok == FDT_END_NODE) {
            if (in_mem) {
                if (base == 0)
                    *low = size;
                else if (base >= ZL1_HIGH_BANK_MIN)
                    *high = size;
                total += size;
            }
            in_mem = 0;
            depth--;
        } else if (tok == FDT_NOP) {
            continue;
        } else if (tok == FDT_END) {
            break;
        }
    }
    return total;
}

/* ---------------------------------------------------------------- */
/* start.S variables                                                */
/* ---------------------------------------------------------------- */
/* Reached through adrp/add in inline asm, for the same reason mmu4k.c
 * does it: a plain extern reference would go through the GOT, and a flat
 * kernel Image has none. The assembler spells C names with a leading
 * underscore, hence `_g_dtb` here versus `g_dtb` in the source. */
#define SYM_ADDR(sym) ({                                        \
    uintptr_t __a;                                              \
    __asm__ volatile("adrp %0, " #sym "@PAGE\n\t"              \
                     "add  %0, %0, " #sym "@PAGEOFF"            \
                     : "=r"(__a));                              \
    __a;                                                        \
})
#define g_dtb           (*(volatile uintptr_t *)SYM_ADDR(_g_dtb))
#define g_build_tag     (*(volatile uint64_t *)SYM_ADDR(_g_build_tag))
#define g_fault_count   (*(volatile uint64_t *)SYM_ADDR(_g_fault_count))
#define g_fault_last_far (*(volatile uint64_t *)SYM_ADDR(_g_fault_last_far))

void zl1_probe_main(void)
{
    con_clear();
    con_str("LeEco Le Pro 3 (zl1) probe\n");
    con_str("READ-ONLY. No partition is touched.\n\n");

    kv("build_tag", g_build_tag);
    kv("current_el", rd_currentel());
    kv("sctlr_at_entry", rd_sctlr());
    kv("mpidr", rd_mpidr());
    con_putc('\n');

    con_str("-- CPU identity --\n");
    kv("midr", rd_midr());
    kv("revidr", rd_revidr());
    kv("aa64pfr0", rd_aa64pfr0());
    kv("aa64pfr1", rd_aa64pfr1());
    kv("aa64isar0", rd_aa64isar0());
    kv("aa64isar1", rd_aa64isar1());
    kv("aa64mmfr0", rd_aa64mmfr0());
    kv("aa64mmfr1", rd_aa64mmfr1());
    kv("aa64mmfr2", rd_aa64mmfr2());
    con_putc('\n');

    const uint64_t isar0 = rd_aa64isar0();
    const uint64_t mmfr0 = rd_aa64mmfr0();
    con_str("-- ARMv8.0 traps --\n");
    kvs("LSE_atomics", ((isar0 >> 20) & 0xfU) ? "yes" : "NO, patch needed");
    kvs("RCpc", ((isar0 >> 16) & 0xfU) ? "yes" : "no");
    kvs("grain16K", ((mmfr0 >> 20) & 0xfU) ? "yes" : "NO, 4K only");
    kvs("PAN", ((mmfr0 >> 24) & 0xfU) ? "yes" : "no");
    con_putc('\n');

    con_str("-- clocks and caches --\n");
    kv("cntfrq", rd_cntfrq());
    kv("cntpct", rd_cntpct());
    kv("ctr_el0", rd_ctr());
    kv("dczid", rd_dczid());
    kv("ccsidr", rd_ccsidr());
    kv("clidr", rd_clidr());
    con_putc('\n');

    con_str("-- device tree --\n");
    fdt_t fdt;
    const int frc = fdt_open(&fdt, g_dtb);
    kv("fdt_at", g_dtb);
    kvd("fdt_open_rc", (uint64_t)(int64_t)frc);
    if (!frc) {
        uint64_t low = 0, high = 0;
        const uint64_t total = fdt_memory(&fdt, &low, &high);
        kvd("mem_low_mib", low >> 20);
        kvd("mem_high_mib", high >> 20);
        kvd("mem_total_mib", total >> 20);
    } else {
        con_str("  no usable DTB: memory unknown\n");
    }
    con_putc('\n');

    /* Full read-only UFS walk lives in probe/ufs.c: registers, TEST UNIT
     * READY, READ CAPACITY and the GPT partition list. It stops at the first
     * stage that does not work and says so, which is the whole point - the
     * answer decides whether storage bring-up is days or weeks.
     *
     * This runs BEFORE the MMU test on purpose. With the MMU off every
     * access is Device memory, so the command/PRDT structures the UFS host
     * reads by DMA are coherent without any cache maintenance. Running it
     * after zl1_mmu4k_enable() would require clean+invalidate around every
     * transfer, and getting that wrong corrupts the DMA silently. */
    zl1_ufs_probe();
    con_putc('\n');

    con_str("-- MMU 4K identity map --\n");
    const int mrc = zl1_mmu4k_enable();
    kvd("mmu4k_rc", (uint64_t)(int64_t)mrc);
    if (!mrc) {
        kv("tcr_after", zl1_mmu4k_tcr());
        kv("sctlr_after", zl1_mmu4k_sctlr());
        zl1_mmu4k_disable();
        kvs("mmu", "on then off again");
    } else {
        kvs("mmu", "not enabled, see rc");
    }
    con_putc('\n');

    con_str("-- GICv3 0x9bc0000 / 0x9c00000 --\n");
    kv("gicd_ctlr", mmio_rd(ZL1_GICD_BASE + 0x00));
    kv("gicd_typer", mmio_rd(ZL1_GICD_BASE + 0x04));
    kv("gicd_iidr", mmio_rd(ZL1_GICD_BASE + 0x08));
    kv("gicr_typer", mmio_rd(ZL1_GICR_BASE + 0x08));
    con_putc('\n');

    con_str("-- input --\n");
    kv("touch_i2c_reg0", mmio_rd(ZL1_TOUCH_I2C_BASE));
    con_putc('\n');

    con_str("-- faults taken --\n");
    kvd("fault_count", g_fault_count);
    kv("fault_last_far", g_fault_last_far);
    con_putc('\n');

    con_str("probe complete. power cycle to return.\n");
    for (;;)
        __asm__ volatile("wfe");
}