/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * ufs.c - read-only UFS probe for zl1 (MSM8996), 4 KiB blocks.
 *
 * Shape copied from ios-sweet's shim/ufs.c, with the parts that differ
 * between a 2019 phone and this one called out:
 *
 *   1. Base address. 8996 puts the UFSHCI at 0x00624000, not 0x01d84000.
 *      (hardware/zl1-facts.md, DT-8996 ufshc@624000.)
 *   2. Block size. zl1's UFS is 4096-byte LUNs; sweet uses 4096 too, but the
 *      GPT LBA math below assumes whatever READ CAPACITY reports, so check
 *      the log rather than trusting an inherited constant.
 *   3. Clock/power. sweet's chip leaves the host running because ABL read
 *      the boot image from it. The bootloader here also reads boot from UFS,
 *      so the same should hold - but 8996's GCC/RPM is a different beast, and
 *      whether it stays on is exactly what this probe measures.
 *
 * The safety rule from sweet is kept verbatim: the doorbell is only rung
 * after the host has *proven* it is using our request list. Otherwise it
 * would execute whatever the bootloader left in its own slot 0, which could
 * be a write.
 *
 * There is no SCSI WRITE anywhere in this file, by construction: `scsi_dma`
 * takes a direction flag and the only reader dispatches 0x28 (READ(10)) and
 * 0x25 (READ CAPACITY(10)).
 */
#include <stdint.h>
#include "zl1_hw.h"

/* UFSHCI register offsets: JEDEC UFS 2.0 / JESD223. Same table as sweet. */
#define REG_CAP       0x00u
#define REG_VER       0x08u
#define REG_IS        0x20u
#define REG_IE        0x24u
#define REG_HCS       0x30u
#define REG_HCE       0x34u
#define REG_UTRLBA    0x50u
#define REG_UTRLBAU   0x54u
#define REG_UTRLDBR   0x58u
#define REG_UTRLCLR   0x5cu
#define REG_UTRLRSR   0x60u

#define HCS_DP      (1u << 0)   /* device present */
#define HCS_UTRLRDY (1u << 1)   /* transfer request list ready */

#define BLOCK 4096u
/* One 2 MiB L3 block covers the image, so keep DMA buffers small and
 * static. 128 GPT entries x 128 bytes = 16 KiB fits in four blocks. */
#define GPT_MAX_ENTRIES 128u

#define MMC_RD10 0x28u
#define MMC_RDCAP10 0x25u

static inline uint32_t ufs_rd(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(ZL1_UFS_BASE + off);
}

static inline void ufs_wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(ZL1_UFS_BASE + off) = v;
}

/* UTP transfer request descriptor, 32 bytes. */
typedef struct {
    uint32_t dw0;       /* [31:28] CT, [26:25] DD, [24] I */
    uint32_t dw1;
    uint32_t dw2;       /* [7:0] OCS */
    uint32_t dw3;
    uint32_t ucdba;
    uint32_t ucdbau;
    uint32_t resp;      /* [31:16] response UPIU offset in DWORDs, [15:0] len */
    uint32_t prdt;      /* [31:16] PRDT offset in DWORDs, [15:0] entry count */
} utrd_t;

/* UTP command descriptor: command UPIU, response UPIU, then the PRDT. */
typedef struct {
    uint8_t cmd[512];
    uint8_t resp[512];
    struct {
        uint32_t addr_lo;
        uint32_t addr_hi;
        uint32_t rsvd;
        uint32_t dbc;   /* data byte count - 1 */
    } prdt[1];
} ucd_t;

/* DMA memory. Static so the linker keeps it inside the image's 2 MiB block,
 * where the probe's 4 KiB mapping marks it as normal writable memory. */
static utrd_t g_utrl[2] __attribute__((aligned(1024)));
static ucd_t g_ucd __attribute__((aligned(128)));
static uint8_t g_dma[4 * BLOCK] __attribute__((aligned(4096)));

/* Console hooks, provided by zl1_probe.c. */
void probe_out(const char *s);
void probe_dec(uint64_t v);
void probe_kv(const char *key, uint64_t value);
void probe_kvd(const char *key, uint64_t value);

static uint64_t ticks(void)
{
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t) : : "memory");
    return t;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void memzero(void *dst, uint32_t n)
{
    volatile uint8_t *b = dst;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

/*
 * One SCSI command in request slot 0, no data or data-in only.
 * Returns 0 on a clean completion, non-zero to name the stage that failed.
 */
static uint32_t scsi(uint32_t lun, const uint8_t *cdb, uint32_t cdb_len,
                     void *buf, uint32_t len, uint32_t *ocs_out,
                     uint32_t *status_out)
{
    static uint8_t tag;
    memzero(&g_utrl[0], sizeof(g_utrl[0]));
    memzero(&g_ucd, sizeof(g_ucd));

    /* Command UPIU: 12-byte header, 4-byte expected transfer length, CDB. */
    g_ucd.cmd[0] = 0x01;                 /* transaction type: COMMAND */
    g_ucd.cmd[1] = len ? 0x40 : 0x00;    /* flags: R = data into host */
    g_ucd.cmd[2] = (uint8_t)lun;
    g_ucd.cmd[3] = ++tag;
    g_ucd.cmd[12] = (uint8_t)(len >> 24);
    g_ucd.cmd[13] = (uint8_t)(len >> 16);
    g_ucd.cmd[14] = (uint8_t)(len >> 8);
    g_ucd.cmd[15] = (uint8_t)len;
    for (uint32_t i = 0; i < cdb_len; i++)
        g_ucd.cmd[16 + i] = cdb[i];

    const uint64_t ucd_pa = (uint64_t)(uintptr_t)&g_ucd;
    g_utrl[0].dw0 = (1u << 28) |                       /* CT: UFS storage */
                    ((len ? 2u : 0u) << 25) |          /* DD: device -> host */
                    (1u << 24);                        /* I: interrupt */
    g_utrl[0].dw2 = 0x0f;                              /* OCS invalid yet */
    g_utrl[0].ucdba = (uint32_t)ucd_pa;
    g_utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
    g_utrl[0].resp = (512u / 4) << 16 | (512u / 4);    /* offset, length */
    if (len) {
        const uint64_t buf_pa = (uint64_t)(uintptr_t)buf;
        g_ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
        g_ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
        g_ucd.prdt[0].dbc = len - 1;
        g_utrl[0].prdt = (1024u / 4) << 16 | 1;        /* offset, entries */
    }
    __asm__ volatile("dsb sy" : : : "memory");

    ufs_wr(REG_UTRLDBR, 1);
    const uint64_t start = ticks();
    while (ufs_rd(REG_UTRLDBR) & 1u) {
        /* Two seconds at 19.2 MHz. A too-short timeout turns a slow first
         * command into a false "UFS is broken" report. */
        if (ticks() - start > 2u * ZL1_TIMER_FALLBACK_HZ) {
            ufs_wr(REG_UTRLCLR, ~1u);       /* clear slot 0 */
            return 1;
        }
    }
    __asm__ volatile("dsb sy" : : : "memory");

    *ocs_out = g_utrl[0].dw2 & 0xffu;
    *status_out = g_ucd.resp[7];
    if (*ocs_out)
        return 2;
    if (g_ucd.resp[0] != 0x21)              /* RESPONSE UPIU */
        return 3;
    return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
    /* READ(10): LBA is 32-bit big endian, transfer length 16-bit BE. */
    const uint8_t cdb[10] = {
        MMC_RD10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8), (uint8_t)lba,
        0, 0,
        (uint8_t)(count >> 8), (uint8_t)count,
    };
    uint32_t ocs = 0, st = 0;
    return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

/*
 * Public entry point. Prints a self-contained section so the screen report
 * says, per line, what worked and where it stopped.
 */
void zl1_ufs_probe(void)
{
    probe_out("-- UFS 0x624000 (read only) --\n");

    const uint32_t cap = ufs_rd(REG_CAP);
    const uint32_t ver = ufs_rd(REG_VER);
    const uint32_t hcs = ufs_rd(REG_HCS);
    const uint32_t hce = ufs_rd(REG_HCE);

    probe_kv("ver", ver);
    probe_kv("cap", cap);
    probe_kv("hce", hce);
    probe_kv("hcs", hcs);
    probe_kv("is", ufs_rd(REG_IS));
    probe_kv("rsr", ufs_rd(REG_UTRLRSR));
    probe_kv("dbr", ufs_rd(REG_UTRLDBR));
    probe_kvd("nutrs", (cap & 0x1fu) + 1u);

    if (!(hce & 1u)) {
        probe_out("  host disabled (HCE=0): needs GCC+PHY bring-up\n");
        return;
    }
    if (!(hcs & HCS_DP)) {
        probe_out("  no device present (HCS.DP=0)\n");
        return;
    }
    if (!(hcs & HCS_UTRLRDY)) {
        probe_out("  request list not ready (HCS.UTRLRDY=0)\n");
        return;
    }
    if (ufs_rd(REG_UTRLDBR)) {
        probe_out("  bootloader requests still queued: stop\n");
        return;
    }

    /* Take over the request list. */
    ufs_wr(REG_UTRLRSR, 0);
    const uint64_t list = (uint64_t)(uintptr_t)g_utrl;
    ufs_wr(REG_UTRLBA, (uint32_t)list);
    ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
    ufs_wr(REG_UTRLRSR, 1);
    probe_kv("ours", list);
    probe_kv("rsr", ufs_rd(REG_UTRLRSR));

    /* Do not ring the doorbell unless the host really is using our list. */
    if (ufs_rd(REG_UTRLBA) != (uint32_t)list ||
        ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
        !(ufs_rd(REG_UTRLRSR) & 1u)) {
        probe_out("  list base not taken: stop\n");
        return;
    }

    uint32_t ocs = 0, st = 0;
    uint32_t rc = 1;
    const uint8_t tur[6] = {0x00};     /* TEST UNIT READY */
    for (int i = 0; i < 3 && rc; i++)
        rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
    probe_kvd("tur_rc", rc);
    probe_kv("tur_ocs_st", (uint64_t)ocs << 8 | st);

    const uint8_t rdcap[10] = {MMC_RDCAP10};
    rc = scsi(0, rdcap, sizeof(rdcap), g_dma, 8, &ocs, &st);
    probe_kvd("rdcap_rc", rc);
    if (!rc) {
        probe_kvd("last_lba", be32(g_dma));
        probe_kvd("block_size", be32(g_dma + 4));
    } else {
        probe_out("  READ CAPACITY failed; the rest needs a real UFS init\n");
        return;
    }

    /* READ(10) LBA 0: protective MBR at 0x1FE and the GPT header at 4 KiB. */
    rc = read_blocks(0, 2, g_dma);
    probe_kvd("read_rc", rc);
    if (rc) {
        probe_out("  READ(10) LBA0 failed\n");
        return;
    }
    probe_kv("mbr_sig", (uint64_t)g_dma[510] | (uint64_t)g_dma[511] << 8);

    const uint8_t *gpt = g_dma + BLOCK;
    if (!(gpt[0] == 'E' && gpt[1] == 'F' && gpt[2] == 'I' && gpt[3] == ' ')) {
        probe_out("  no GPT signature at LBA1\n");
        return;
    }
    probe_out("  GPT signature found\n");

    const uint32_t entry_lba = be32(gpt + 72);
    const uint32_t nent = be32(gpt + 80);
    const uint32_t esz = be32(gpt + 84);
    probe_kvd("gpt_entry_lba", entry_lba);
    probe_kvd("gpt_entries", nent);
    probe_kvd("gpt_entry_size", esz);

    if (esz != 128u || nent == 0 || nent > GPT_MAX_ENTRIES) {
        probe_out("  unexpected GPT shape: stop\n");
        return;
    }

    const uint32_t blocks = (nent * 128u + BLOCK - 1u) / BLOCK;
    if (read_blocks(entry_lba, blocks, g_dma)) {
        probe_out("  GPT entry array read failed\n");
        return;
    }

    probe_out("  partitions (>=64 MiB):\n");
    for (uint32_t i = 0; i < nent; i++) {
        const uint8_t *e = g_dma + i * 128u;
        const uint64_t first = le64(e + 32);
        const uint64_t last = le64(e + 40);
        if (!first || last < first)
            continue;
        if ((last - first + 1) >> 8 < 64)   /* 4 KiB blocks: 256 blocks = 1 MiB */
            continue;
        char name[17];
        int n = 0;
        for (int c = 0; c < 16; c++) {
            const uint8_t ch = e[56 + 2 * c];
            if (!ch)
                break;
            name[n++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
        }
        name[n] = 0;
        if (!n)
            continue;
        probe_out("   ");
        probe_out(name);
        probe_out(" first=");
        probe_dec(first);
        probe_out("\n");
    }
    probe_out("  UFS probe done\n");
}