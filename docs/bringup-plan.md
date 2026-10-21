# Bring-up plan

The order below is the one the S8 and sweet ports actually converged on,
adjusted for zl1's two handicaps: no UEFI port and ARMv8.0.

## Stage 0 — probe (this repository, done)

Read facts off the phone before writing anything that depends on them.

Exit criteria: a capture in `hardware/observations/` that fills every
`[OBS]` row of `hardware/zl1-facts.md`.

## Stage 1 — shim that prints the FDT and nothing else

Take `ios-fable`'s / S8's `shim/` shape:

- `start.S`: EL1 entry, MMU off, save x0, vectors, stack.
- Load the payload from the ramdisk, or just print and spin.
- Print the live FDT: `memory` nodes, `reserved-memory` nodes, `/soc`
  compatible, and the `chosen` node.
- Read the GPT through a **polled UFS driver** reusing sweet's UFSHCI
  offsets at the 8996 base `0x624000`.

Exit criteria: the phone prints the memory map and the partition table, and
`userdata` first/last LBAs match `BOARD_USERDATAIMAGE_PARTITION_SIZE`.

## Stage 2 — XNU kernel entry

Build the sweet XNU tree with a `ZL1` machine config:

- 4 KiB pages: sweet's patch set already has the 4 KiB path.
- No LSE: take the S8 port's LSE scan and `libfirehose_kernel` rebuild.
- GICv3: take the **fajita/SDM845** GICv3 code, not the S8's GIC-400 code.
- Virtual timer: INTID 27, Group 1.
- Platform expert: match `qcom,zl1` in the ADT.

Exit criteria: the kernel prints its version line and reaches
`pmap_startup` without faulting.

## Stage 3 — storage and root

- UFS block device in XNU, root on `userdata`.
- HFS+ root image built from the iPhone X 16.7.16 IPSW (never committed).
- AMFI stub, AppleKeyStore stub, permissive sandbox - exactly as sweet.

Exit criteria: `launchd` runs.

## Stage 4 — display and input

- `AppleCLCD` presenting the splash framebuffer.
- Synaptics DSX driver: this is a new driver, not a port of sweet's Goodix
  one. Start with a polled RMI read of the product ID to identify the part.
- Power key from PM8994 PON; volume keys once the GPIOs are confirmed.

Exit criteria: SpringBoard reaches the lock screen.

## Decisions already taken

- **Screen first, UART never** unless the probe shows the framebuffer is not
  usable. The retail zl1 has no exposed serial port.
- **`userdata` for the iOS root.** Android data will be erased at that
  point; that step needs explicit confirmation before it is run.
- **No `fastboot flash` until Stage 3.** Stages 0-2 all use `fastboot boot`.

## What is most likely to kill this

1. `fastboot boot` refused by the stock LeEco bootloader.
2. No usable display from the kernel slot (splash not kept).
3. UFS clock/power not left running by the bootloader, requiring a full
   GCC + PHY + RPM bring-up before any block I/O - weeks of work on its own.
4. ARMv8.0 instruction gaps beyond LSE (RCpc, PAuth) turning up one at a
   time in userland.

Each of 1-3 is answered by the probe in Stage 0. That is why it exists.