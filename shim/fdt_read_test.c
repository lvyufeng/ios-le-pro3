/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * fdt_read_test.c - host test for shim/fdt_read.c.
 *
 * The reader is compiled unchanged for the host and pointed at a DTB built
 * by tools/mkfdt.py. This catches the thing that would otherwise only show
 * up on the phone: a walk that silently returns nothing, or worse, wanders
 * off the end of the blob.
 *
 * Build and run:  shim/test_fdt.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fdt_read.h"

static int failures;

#define CHECK(cond, ...)                                                \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
            printf(__VA_ARGS__);                                       \
            printf("\n");                                              \
            failures++;                                                \
        }                                                              \
    } while (0)

static unsigned char *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "short read on %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <tree.dtb>\n", argv[0]);
        return 2;
    }
    size_t len = 0;
    unsigned char *blob = load(argv[1], &len);

    /* The reader takes an absolute address. On the host that means the
     * heap pointer, which is fine: it only ever dereferences within
     * [_base, _base + totalsize). */
    fdt_info_t f;
    int rc = fdt_open(&f, (uint64_t)(uintptr_t)blob);
    CHECK(rc == 0, "fdt_open returned %d on a valid tree", rc);
    if (rc)
        return 1;

    CHECK(f.totalsize == len, "totalsize %u != file length %zu",
          f.totalsize, len);

    /* --- memory ---------------------------------------------------- */
    int banks = fdt_read_memory(&f);
    CHECK(banks == 1, "expected 1 memory bank, got %d", banks);
    if (banks == 1) {
        CHECK(f.mem[0].base == 0x80000000ULL,
              "bank base %llx, expected 0x80000000",
              (unsigned long long)f.mem[0].base);
        CHECK(f.mem[0].size == 0x20000000ULL,
              "bank size %llx, expected 0x20000000",
              (unsigned long long)f.mem[0].size);
    }
    CHECK(fdt_total_memory(&f) == 0x20000000ULL,
          "total memory %llx", (unsigned long long)fdt_total_memory(&f));

    /* --- reserved-memory ------------------------------------------- */
    fdt_read_reserved(&f);
    /* removed_regions (0x3000000) + modem (0x6200000) are no-map; the
     * splash region is not, so it must not be counted. */
    uint64_t want = 0x3000000ULL + 0x6200000ULL;
    CHECK(f.reserved_nomap_bytes == want,
          "no-map total %llx, expected %llx",
          (unsigned long long)f.reserved_nomap_bytes,
          (unsigned long long)want);

    /* --- cpus ------------------------------------------------------ */
    fdt_read_cpus(&f);
    CHECK(f.cpu_count == 4, "expected 4 cpus, got %d", f.cpu_count);
    if (f.cpu_count == 4) {
        CHECK(f.mpidr[0] == 0x0, "cpu0 mpidr %llx",
              (unsigned long long)f.mpidr[0]);
        CHECK(f.mpidr[1] == 0x100, "cpu1 mpidr %llx",
              (unsigned long long)f.mpidr[1]);
        CHECK(f.mpidr[2] == 0x200, "cpu2 mpidr %llx",
              (unsigned long long)f.mpidr[2]);
        CHECK(f.mpidr[3] == 0x300, "cpu3 mpidr %llx",
              (unsigned long long)f.mpidr[3]);
    }

    /* --- /chosen --------------------------------------------------- */
    char args[256];
    int n = fdt_chosen_bootargs(&f, args, sizeof(args));
    CHECK(n > 0, "bootargs lookup returned %d", n);
    CHECK(strstr(args, "androidboot.hardware=qcom") != NULL,
          "bootargs missing hardware=, got %s", args);

    /* --- negative cases -------------------------------------------- */
    fdt_info_t bad;
    CHECK(fdt_open(&bad, 0) != 0, "null address accepted");
    CHECK(fdt_open(&bad, (uint64_t)(uintptr_t)blob) == 0,
          "valid tree rejected on second open");

    /* Truncating totalsize must be rejected, not walked. */
    unsigned char *copy = malloc(len);
    memcpy(copy, blob, len);
    copy[7] = 0x10;                    /* totalsize low byte -> 16 */
    CHECK(fdt_open(&bad, (uint64_t)(uintptr_t)copy) != 0,
          "a 16-byte totalsize was accepted");
    free(copy);

    /* A wrong magic must be rejected. */
    copy = malloc(len);
    memcpy(copy, blob, len);
    copy[0] = 0x00;
    CHECK(fdt_open(&bad, (uint64_t)(uintptr_t)copy) != 0,
          "a bad magic was accepted");
    free(copy);

    free(blob);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("fdt_read: all checks passed\n");
    return 0;
}