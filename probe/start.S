/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * start.S - entry point, exception vectors and data for the zl1 probe.
 *
 * Handoff contract we assume (and the probe verifies):
 *   the qcom LK/ABL loads the kernel Image at BOARD_KERNEL_BASE
 *   (0x80000000), enters at EL1 with the MMU off, DTB address in x0, and
 *   requires the arm64 Image header ("ARMd" at offset 56) to accept it.
 *
 * The vector table earns its place: half of this probe's job is to read
 * addresses copied from a device tree, and a wrong address on a qcom SoC
 * data-aborts rather than returning 0xFFFFFFFF. Catching the abort, bumping
 * a counter and stepping over the load turns a wrong guess into one line of
 * log instead of a dead phone with no output.
 *
 * Mach-O notes: this object is assembled by the same clang that compiles
 * the C, so section directives use the Mach-O form and addresses are formed
 * with adrp/add (there is no short-form adr for external symbols).
 */
    .section __TEXT,__text,regular,pure_instructions
    .globl _start
    .p2align 3
_start:
    /* arm64 Image header, exactly as the kernel build lays it out.
     *   0   branch to the entry point
     *   4   text_offset   (0 for a flat Image)
     *   8   image_size    (patched by flatten.py)
     *   16  flags         (0: little endian, 4 KiB pages)
     *   24  res2, 32 res3, 40 res4
     *   48  "ARMd"        (kept for tools that read the header early)
     *   56  "ARMd"        (the 64-bit marker LK actually checks)
     *   60  reserved */
    b       entry                       /* offset 0 */
    .long   0                           /* offset 4:  text_offset */
    .quad   0                           /* offset 8:  image_size (patched) */
    .quad   0                           /* offset 16: flags */
    .quad   0                           /* offset 24: res2 */
    .quad   0                           /* offset 32: res3 */
    .quad   0                           /* offset 40: res4 */
    .byte   0x41, 0x52, 0x4d, 0x64      /* offset 48: "ARMd" */
    .long   0                           /* offset 52: reserved */
    .byte   0x41, 0x52, 0x4d, 0x64      /* offset 56: "ARMd" (the one LK checks) */
    .long   0                           /* offset 60: reserved */

entry:
    mov     x19, x0                     /* DTB pointer from the bootloader */
    msr     daifset, #0xf               /* the probe never takes interrupts */

    adrp    x20, _g_dtb@PAGE
    add     x20, x20, _g_dtb@PAGEOFF
    str     x19, [x20]

    mrs     x21, currentel
    adrp    x20, _g_entry_el@PAGE
    add     x20, x20, _g_entry_el@PAGEOFF
    str     x21, [x20]

    /* Install vectors before the first MMIO read. */
    adrp    x22, _zl1_vectors@PAGE
    add     x22, x22, _zl1_vectors@PAGEOFF
    msr     vbar_el1, x22
    isb

    /* Stack top, linked right after the Image's data. */
    adrp    x22, _stack_top@PAGE
    add     x22, x22, _stack_top@PAGEOFF
    mov     sp, x22

    /* Zero .bss: fault counters and the page tables get written lazily. */
    adrp    x22, _bss_begin@PAGE
    add     x22, x22, _bss_begin@PAGEOFF
    adrp    x23, _bss_end@PAGE
    add     x23, x23, _bss_end@PAGEOFF
1:  cmp     x22, x23
    b.hs    2f
    str     xzr, [x22], #8
    b       1b
2:
    /* Build tag: identifies this image in a photo of the screen. */
    movz    x23, #0x7a6c                /* "zl" */
    movk    x23, #0x3101, lsl #16       /* "1", build 1 */
    adrp    x22, _g_build_tag@PAGE
    add     x22, x22, _g_build_tag@PAGEOFF
    str     x23, [x22]

    bl      _zl1_probe_main
3:  wfe
    b       3b

/* ------------------------------------------------------------------ */
/* Exception vectors                                                   */
/* ------------------------------------------------------------------ */
/* 16 slots of 128 bytes: 4 exception classes x 4 origins. Only
 * "synchronous, current EL, SPx" (slot 1) does work; every other slot
 * parks so an unexpected exception stays visible instead of sliding by. */
    .p2align 11
    .globl _zl1_vectors
_zl1_vectors:
    .p2align 7
    b       park                        /* 0x000 sync, cur EL SP0 */
    .p2align 7
    b       sync_handler                /* 0x200 sync, cur EL SPx */
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park
    .p2align 7
    b       park

sync_handler:
    mrs     x0, esr_el1
    ubfx    x1, x0, #26, #6             /* EC */
    cmp     x1, #0x24                   /* data abort, same EL */
    b.eq    resume_after_read
    cmp     x1, #0x25                   /* data abort, same EL, no stage-2 */
    b.eq    resume_after_read
    b       park

resume_after_read:
    adrp    x2, _g_fault_count@PAGE
    add     x2, x2, _g_fault_count@PAGEOFF
    ldr     x3, [x2]
    add     x3, x3, #1
    str     x3, [x2]

    mrs     x4, far_el1
    adrp    x2, _g_fault_last_far@PAGE
    add     x2, x2, _g_fault_last_far@PAGEOFF
    str     x4, [x2]

    adrp    x2, _g_fault_last_esr@PAGE
    add     x2, x2, _g_fault_last_esr@PAGEOFF
    str     x0, [x2]

    /* ELR_EL1 names the faulting load; every MMIO read in zl1_probe.c is a
     * single 4-byte ldr, so stepping 4 bytes resumes after it. */
    mrs     x5, elr_el1
    add     x5, x5, #4
    msr     elr_el1, x5
    eret

park:
    wfe
    b       park

    .ltorg

/* ------------------------------------------------------------------ */
/* Data                                                                */
/* ------------------------------------------------------------------ */
    .section __DATA,__data
    .p2align 3
    .globl _g_dtb
_g_dtb:
    .quad   0
    .globl _g_entry_el
_g_entry_el:
    .quad   0
    .globl _g_build_tag
_g_build_tag:
    .quad   0

/* The 4 KiB identity-map tables: one L1 (4 GiB as device blocks), one L2
 * for the 1 GiB the Image lives in, one L3 for its single 2 MiB block. */
    .p2align 12
    .globl _zl1_l1
_zl1_l1:
    .space  4096
    .p2align 12
    .globl _zl1_l2
_zl1_l2:
    .space  4096
    .p2align 12
    .globl _zl1_l3
_zl1_l3:
    .space  4096

/* 16 KiB stack; _stack_top is the AAPCS64-mandated 16-byte aligned top. */
    .p2align 12
    .space  16384
    .globl _stack_top
_stack_top:

/* ------------------------------------------------------------------ */
/* Bss                                                                */
/* ------------------------------------------------------------------ */
    .section __DATA,__bss
    .p2align 3
    .globl _g_fault_count
_g_fault_count:
    .quad   0
    .globl _g_fault_last_far
_g_fault_last_far:
    .quad   0
    .globl _g_fault_last_esr
_g_fault_last_esr:
    .quad   0



/* ------------------------------------------------------------------ */
/* End-of-image marker                                                */
/* ------------------------------------------------------------------ */
/* mmu4k.c maps [_start, _image_end) as executable Normal memory, so this
 * label must sit after ALL code from every object. flatten.py lays text
 * sections in object order start.o -> mmu4k.o -> zl1_probe.o, so a marker
 * in start.o would cut the range short. Putting it here and letting
 * flatten.py patch the symbol is not possible cross-object, so instead the
 * value is computed at build time: flatten.py rewrites the 16 bytes at
 * _image_end to hold the final image size. mmu4k.c reads it back. */
    .section __TEXT,__image_end,regular,pure_instructions
    .p2align 3
    .globl _image_end
_image_end:
    .quad   0

/* ------------------------------------------------------------------ */
/* Bss extent markers                                                 */
/* ------------------------------------------------------------------ */
/* .bss is spread across several objects, so a marker inside start.S
 * would only cover its own slice. flatten.py lays these two sections
 * immediately before and after every __bss/__common section, which makes
 * [_bss_begin, _bss_end) the true extent that entry zeroes. */
    .section __DATA,__bss_begin
    .p2align 3
    .globl _bss_begin
_bss_begin:

    .section __DATA,__bss_end
    .p2align 3
    .globl _bss_end
_bss_end:
