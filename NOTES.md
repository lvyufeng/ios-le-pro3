# NOTES — raw engineering log

Reverse-chronological. Distinguish **observed on hardware** from **read from
a source** from **planned**. Only the first may be used as a claim.

---

## 2026-10-21 — repository created, probe built

### Observed on hardware

Nothing yet. No phone has run this code.

### Read from sources

- `msm8996.dtsi` (mainline): UFS `0x624000`, PHY `0x627000`, GICD
  `0x9bc0000`, GICR `0x9c00000` stride `0x40000`, virtual timer PPI 11
  (INTID 27).
- LeEco msm8996 kernel dts: splash `0x83401000` 1080x1920, touch Synaptics
  DSX on `0x75ba000` addr `0x20`, IRQ gpio125, reset gpio89, PM8994 LDO14;
  an FocalTech `0x38` also exists on the same bus for some HW revs.
- LineageOS msm8996-common: kernel base `0x80000000`, page 4096,
  `Image.gz-dtb`; zl1 sets tags `0x100`, ramdisk `0x01000000`.
- Mu-Silicium has no MSM8996 package (86 packages checked).

### Built

- `probe/`: 3 objects (start.S, mmu4k.c, zl1_probe.c) -> `tools/flatten.py`
  -> 53,248-byte flat Image with a valid arm64 header (verified: branch at 0,
  `ARMd` at 48 and 56, `image_size` patched, `text_offset` 0, flags 0).
- `tools/mkboot.py`: v0 Android boot image, 16,384 bytes, kernel gzip'd at
  page 0x1000, ramdisk (newc cpio, 1 file) at 0x3000. Verified by unpacking.

### Decisions and their reasons

- **Probe before kernel.** Two of the biggest unknowns (whether LSE exists,
  whether the bootloader leaves UFS running) change the shape of the work
  more than any code written now would.
- **Screen as the log.** The retail zl1 has no exposed UART and there is no
  ramoosa before Android boots. The splash framebuffer survives because
  nothing repaints it while the probe spins.
- **Self-written linker.** The dev host is stock macOS: no lld, no binutils.
  Clang assembles and compiles aarch64 fine, but ld64 cannot produce the
  flat layout the qcom bootloader wants. `tools/flatten.py` links the Mach-O
  objects directly and refuses relocations it cannot prove correct.
- **Avoid the GOT.** Any extern symbol reference in Mach-O goes through the
  GOT, which a flat image has no way to fill. All cross-file symbols are
  therefore reached with `adrp/add` in inline asm. This is enforced by
  `flatten.py` failing the build on a GOT relocation.

### Bugs found while building (all fixed)

1. `section_64` field offsets were wrong (`offset/align/reloff/nreloc` are
   at +48/+52/+56/+60 from the section struct base).
2. `n_value` in the nlist is an *object* address, not a section offset.
3. Symbols in sections that were never laid out (debug, unwind) crashed the
   address table.
4. `__cstring` was missing from the layout order.
5. `_image_end` in `start.o` truncated the executable range; it now lives in
   its own `__image_end` section laid out last.
6. The boot image header's kernel offset was not page-aligned.
7. The `ARMd` marker had to be at byte 56, not 48; the header is laid out to
   satisfy both.

### Next concrete action

1. Flash `probe/out/boot.img` with `fastboot boot` on an unlocked LEX727.
2. Read the screen; run `tools/zl1-log.py` and commit the capture under
   `hardware/observations/`.
3. Fill the `[OBS]` rows in `hardware/zl1-facts.md` from that capture.
4. Only then decide the shim design: with UFS already running and ARMv8.0
   confirmed, the shim can follow the sweet shape with the S8's LSE patches.

### Open risks

- `fastboot boot` may be refused by the stock LEX727 bootloader (it is on
  some LeEco builds). If so, the probe has to go into the `boot` or
  `recovery` partition instead, which changes the safety story.
- The splash framebuffer may not be readable from the kernel slot on this
  bootloader. The probe would then show nothing at all — in which case the
  first fallback is a UART header at the bottom of the phone, and the second
  is writing to `last_kmsg`-style storage.
- Whether the probe can even be entered depends on the Image header being
  what this particular LK accepts. The layout copied from the stock kernel
  is the best available evidence; it is not proof.