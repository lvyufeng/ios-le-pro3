/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * fdt_read.c - bounds-checked FDT walk for the zl1 shim.
 *
 * No libc. The shim is freestanding and, on this build host, everything is
 * compiled into a flat Image where a stray call to memcpy would drag in the
 * whole runtime. So: no memcpy/memcmp/strcmp, only local loops.
 *
 * The walk is a single pass over the structure block keeping a depth stack
 * of node names. Anything that does not parse is treated as "stop here and
 * report nothing", because a second pass after a bounds error would read
 * memory we have no claim on.
 */
#include "fdt_read.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void zero(void *dst, uint32_t n)
{
    uint8_t *b = dst;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int str_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

int fdt_open(fdt_info_t *out, uint64_t addr)
{
    zero(out, sizeof(*out));
    if (!addr || (addr & 7))
        return -1;

    const uint8_t *b = (const uint8_t *)(uintptr_t)addr;
    if (be32(b) != FDT_MAGIC)
        return -2;

    const uint32_t totalsize = be32(b + 4);
    const uint32_t off_struct = be32(b + 8);
    const uint32_t off_strings = be32(b + 12);
    const uint32_t size_struct = be32(b + 36);
    const uint32_t size_strings = be32(b + 32);

    /* Every sub-block must live inside the blob. totalsize is what the
     * bootloader says it mapped; if a header field exceeds it, the tree is
     * lying and we refuse to walk it. */
    if (totalsize < 64 || totalsize > (16u << 20))
        return -3;
    if (off_struct > totalsize || size_struct > totalsize - off_struct)
        return -4;
    if (off_strings > totalsize || size_strings > totalsize - off_strings)
        return -5;

    out->base = b;
    out->totalsize = totalsize;
    out->off_struct = off_struct;
    out->size_struct = size_struct;
    out->off_strings = off_strings;
    out->size_strings = size_strings;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Generic walk                                                     */
/* ---------------------------------------------------------------- */
/* One handler, reused by every reader below. depth 1 is the root node, so
 * "/memory" is a child at depth 2 and "/cpus/cpu@0" at depth 3.
 *
 * `on_node` is called after the node's properties have been seen (i.e. at
 * the closing tag), which is when a node's full property set is known.
 * Each node keeps its own small property stash in `depth_props[]`.
 */
typedef struct {
    uint64_t addr_cells;
    uint64_t size_cells;
    uint64_t reg[16];       /* raw cells, at most 16 */
    int reg_len;
    int has_nomap;
    int has_device_type;
} node_props_t;

typedef void (*node_cb_t)(const char *path, const node_props_t *props,
                          void *ctx);

static void collect(fdt_info_t *f, node_cb_t cb, void *ctx)
{
    const uint8_t *p = f->base + f->off_struct;
    const uint8_t *end = p + f->size_struct;
    const uint8_t *strs = f->base + f->off_strings;
    const uint8_t *strs_end = strs + f->size_strings;

    char path[256];
    uint32_t path_len = 0;
    int depth = 0;
    uint64_t addr_cells = 2, size_cells = 2;   /* inherited defaults */
    uint64_t addr_cells_stack[FDT_MAX_DEPTH] = {0};
    uint64_t size_cells_stack[FDT_MAX_DEPTH] = {0};
    node_props_t props;
    zero(&props, sizeof(props));

    while (p + 4 <= end) {
        const uint32_t tok = be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            uint32_t n = 0;
            while (p < end && *p) {
                p++;
                n++;
            }
            if (p >= end)
                return;                     /* unterminated name */
            p++;                            /* skip NUL */
            p = (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);

            /* Push the new path segment. Every node's path starts with '/';
             * the root's name is empty, so it becomes "/" and its children
             * become "/name". Getting this wrong makes every lookup miss
             * silently, which is exactly how this bug first showed up. */
            if (depth >= FDT_MAX_DEPTH)
                return;
            if (n == 0) {
                path[0] = '/';
                path[1] = 0;
                path_len = 1;
            } else {
                if (path_len + n + 2 > sizeof(path))
                    return;
                if (path_len == 0)
                    path[path_len++] = '/';
                else if (path[path_len - 1] != '/')
                    path[path_len++] = '/';
                for (uint32_t i = 0; i < n; i++)
                    path[path_len++] = name[i];
                path[path_len] = 0;
            }
            addr_cells_stack[depth] = addr_cells;
            size_cells_stack[depth] = size_cells;
            depth++;
            zero(&props, sizeof(props));
        } else if (tok == FDT_PROP) {
            if (p + 8 > end)
                return;
            const uint32_t len = be32(p);
            const uint32_t nameoff = be32(p + 4);
            const uint8_t *val = p + 8;
            if (val + len > end)
                return;
            p = (const uint8_t *)(((uintptr_t)(val + len) + 3) & ~(uintptr_t)3);

            if (nameoff >= f->size_strings)
                continue;
            const char *pname = (const char *)strs + nameoff;
            if ((const uint8_t *)pname >= strs_end)
                continue;

            if (str_eq(pname, "#address-cells") && len >= 4) {
                addr_cells = be32(val);
            } else if (str_eq(pname, "#size-cells") && len >= 4) {
                size_cells = be32(val);
            } else if (str_eq(pname, "reg")) {
                /* Keep the first entries; a node never needs more than a
                 * handful for what the shim decides. */
                uint64_t cells = len / 4;
                if (cells > 16)
                    cells = 16;
                for (uint64_t i = 0; i < cells; i++)
                    props.reg[i] = be32(val + i * 4);
                props.reg_len = (int)cells;
                props.addr_cells = addr_cells;
                props.size_cells = size_cells;
            } else if (str_eq(pname, "no-map")) {
                props.has_nomap = 1;
            } else if (str_eq(pname, "device_type")) {
                props.has_device_type = 1;
            }
        } else if (tok == FDT_END_NODE) {
            if (depth <= 0)
                return;
            depth--;
            /* The node's properties are complete now; hand it back. */
            if (cb)
                cb(path, &props, ctx);
            /* Restore the inherited cell sizes of the parent. */
            addr_cells = addr_cells_stack[depth];
            size_cells = size_cells_stack[depth];
            /* Trim the path back to the parent, keeping its leading '/'. */
            while (path_len > 1 && path[path_len - 1] != '/')
                path_len--;
            if (path_len > 1)
                path_len--;     /* drop the separator */
            path[path_len] = 0;
            zero(&props, sizeof(props));
        } else if (tok == FDT_NOP) {
            continue;
        } else if (tok == FDT_END) {
            return;
        } else {
            return;                         /* unknown token: stop */
        }
    }
}

/* ---------------------------------------------------------------- */
/* Section handlers                                                 */
/* ---------------------------------------------------------------- */
typedef struct {
    fdt_info_t *f;
} mem_ctx_t;

static void memory_node(const char *path, const node_props_t *props, void *ctx)
{
    mem_ctx_t *c = ctx;
    if (!(str_eq(path, "/memory") || str_prefix(path, "/memory@")))
        return;
    if (props->reg_len < 2 || c->f->mem_count >= FDT_MAX_MEM)
        return;

    /* 8996 uses 2/2 cells (64-bit addresses). Fall back to 1/1 for trees
     * that set them that way; the DTB says which, so trust the node. */
    uint64_t base, size;
    if (props->addr_cells >= 2) {
        base = props->reg[0] << 32 | props->reg[1];
        size = props->size_cells >= 2 ? props->reg[2] << 32 | props->reg[3]
                                      : props->reg[2];
    } else {
        base = props->reg[0];
        size = props->reg[1];
    }
    if (!size)
        return;
    c->f->mem[c->f->mem_count].base = base;
    c->f->mem[c->f->mem_count].size = size;
    c->f->mem_count++;
}

typedef struct {
    fdt_info_t *f;
} res_ctx_t;

static void reserved_node(const char *path, const node_props_t *props, void *ctx)
{
    res_ctx_t *c = ctx;
    if (!str_prefix(path, "/reserved-memory/"))
        return;
    /* Children of /reserved-memory carry the regions. Count only those the
     * firmware marked no-map: the reusable ones may be handed to the OS. */
    uint32_t slashes = 0;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            slashes++;
    if (slashes != 2)
        return;
    if (!props->reg_len)
        return;
    c->f->reserved_count++;
    if (props->has_nomap) {
        uint64_t size;
        if (props->addr_cells >= 2)
            size = props->size_cells >= 2
                       ? (props->reg[2] << 32 | props->reg[3])
                       : props->reg[2];
        else
            size = props->reg[1];
        c->f->reserved_nomap_bytes += size;
    }
}

static void cpu_node(const char *path, const node_props_t *props, void *ctx)
{
    fdt_info_t *c = ctx;
    if (!str_prefix(path, "/cpus/cpu@"))
        return;
    if (c->cpu_count >= 8 || !props->reg_len)
        return;
    /* cpu@N's reg is the MPIDR in this tree generation: one 32-bit cell. */
    uint64_t mpidr = 0;
    for (int i = 0; i < props->reg_len; i++)
        mpidr = mpidr << 32 | props->reg[i];
    c->mpidr[c->cpu_count++] = mpidr;
}

int fdt_read_memory(fdt_info_t *f)
{
    mem_ctx_t c = {f};
    f->mem_count = 0;
    collect(f, memory_node, &c);
    return f->mem_count;
}

void fdt_read_reserved(fdt_info_t *f)
{
    res_ctx_t c = {f};
    f->reserved_count = 0;
    f->reserved_nomap_bytes = 0;
    collect(f, reserved_node, &c);
}

void fdt_read_cpus(fdt_info_t *f)
{
    f->cpu_count = 0;
    collect(f, cpu_node, f);
}

/* ---------------------------------------------------------------- */
/* /chosen lookups                                                  */
/* ---------------------------------------------------------------- */
/* Re-walk just for /chosen, because the generic collector above records
 * only properties it has a field for. A dedicated pass keeps the common
 * path small and the rare lookups obvious. */
static int chosen_lookup(const fdt_info_t *f, const char *name, uint64_t *out,
                         char *strbuf, uint32_t strcap)
{
    const uint8_t *p = f->base + f->off_struct;
    const uint8_t *end = p + f->size_struct;
    const uint8_t *strs = f->base + f->off_strings;
    int depth = 0, in_chosen = 0, found = 0;

    while (p + 4 <= end) {
        const uint32_t tok = be32(p);
        p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *n = (const char *)p;
            uint32_t len = 0;
            while (p < end && *p) {
                p++;
                len++;
            }
            p++;
            p = (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
            depth++;
            in_chosen = depth == 2 && len == 6 && n[0] == 'c' && n[1] == 'h' &&
                        n[2] == 'o' && n[3] == 's' && n[4] == 'e' && n[5] == 'n';
        } else if (tok == FDT_PROP) {
            const uint32_t len = be32(p);
            const uint32_t nameoff = be32(p + 4);
            const uint8_t *val = p + 8;
            if (val + len > end)
                return 0;
            p = (const uint8_t *)(((uintptr_t)(val + len) + 3) & ~(uintptr_t)3);
            if (!in_chosen || nameoff >= f->size_strings)
                continue;
            const char *pn = (const char *)strs + nameoff;
            if (!str_eq(pn, name))
                continue;
            found = 1;
            if (out) {
                if (len >= 8)
                    *out = be64(val);
                else if (len >= 4)
                    *out = be32(val);
                else
                    *out = 0;
            }
            if (strbuf && strcap) {
                uint32_t n2 = len < strcap - 1 ? len : strcap - 1;
                for (uint32_t i = 0; i < n2; i++)
                    strbuf[i] = (char)val[i];
                strbuf[n2] = 0;
            }
            break;
        } else if (tok == FDT_END_NODE) {
            depth--;
            in_chosen = 0;
        } else if (tok == FDT_NOP) {
            continue;
        } else {
            break;
        }
    }
    return found;
}

uint64_t fdt_chosen_u64(const fdt_info_t *f, const char *name)
{
    uint64_t v = 0;
    chosen_lookup(f, name, &v, 0, 0);
    return v;
}

int fdt_chosen_bootargs(const fdt_info_t *f, char *buf, uint32_t cap)
{
    if (!cap)
        return -1;
    buf[0] = 0;
    chosen_lookup(f, "bootargs", 0, buf, cap);
    return (int)str_len(buf);
}

uint64_t fdt_total_memory(const fdt_info_t *f)
{
    uint64_t total = 0;
    for (int i = 0; i < f->mem_count; i++)
        total += f->mem[i].size;
    return total;
}
