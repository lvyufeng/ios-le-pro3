# zl1 hardware facts

Every row cites where the value came from. **Only rows marked `verified`
have been read off a phone.** Nothing in this project may treat an
unverified value as a fact in code without a comment saying so.

Sources used:

- **DT-8996** — `arch/arm64/boot/dts/qcom/msm8996.dtsi`, mainline Linux tree
  (`torvalds/linux`, master).
- **DT-LE** — LeEco's released msm8996 kernel as carried by LineageOS
  (`LineageOS/android_kernel_leeco_msm8996`, branch `lineage-18.1`), files
  under `arch/arm/boot/dts/qcom/` and `.../qcom/leeco/`.
- **LIN** — `LineageOS/android_device_leeco_msm8996-common` and
  `android_device_leeco_zl1`.
- **OBS** — to be filled in from the probe log.

## SoC and CPU

| Item | Value | Source | Status |
|---|---|---|---|
| SoC | MSM8996 Pro (Snapdragon 821) | LIN `TARGET_KERNEL_SOURCE` | verified by name |
| Cores | 2x Kryo (big) + 2x Kryo (little), 4 total | DT-LE cpu nodes | dt |
| Little MPIDR | `0x000`, `0x100` | DT-LE `cpu@0`, `cpu@100` | dt |
| Big MPIDR | `0x200`, `0x300` | DT-8996 `cpu@200` | dt |
| PSCI | 1.0, `smc` conduit | DT-8996 `psci { compatible = "arm,psci-1.0"; method = "smc"; }` | dt |
| ARM level | ARMv8.0 (no LSE, no 16 KiB granules on the A53-class cores) | S8 port's finding on equivalent silicon; must be confirmed | **probe** |

Note: the Le Pro 3 is one of the MSM8996 variants where **all four cores can
be started**; the B-version MSM8996 on some other phones could not. The
probe reports `MPIDR` and the PSCI version so this can be checked.

## Interrupts and timers

| Item | Value | Source | Status |
|---|---|---|---|
| GIC type | **GICv3** with security extensions | DT-8996 `intc: interrupt-controller@9bc0000` | dt |
| GIC distributor | `0x09BC0000`, 64 KiB | DT-8996 | dt |
| GIC redistributor block | `0x09C00000`, 1 MiB | DT-8996 `reg` second entry | dt |
| Redistributor stride | `0x00040000` | DT-8996 `redistributor-stride` | dt |
| Virtual timer | **INTID 27** (PPI 11) | DT-8996 `timer` interrupts list | dt |
| Physical timer | INTID 30 (PPI 14) | DT-8996 | dt |
| Counter frequency | 19.2 MHz nominal | qcom convention; **probe reads `CNTFRQ_EL0`** | **probe** |

Warning: the Galaxy S8 port uses a **GIC-400**, and its timer is reached
through banked GIC registers. None of that code transfers. The GICv3 driver
path in `ios-fajita` (SDM845) is the right donor.

## Storage

| Item | Value | Source | Status |
|---|---|---|---|
| UFS host (UFSHCI) | `0x00624000`, 0x2500 bytes | DT-8996 `ufshc@624000` | dt |
| UFS PHY | `0x00627000` (QMP) | DT-8996 `phy@627000` | dt |
| UFS version | 2.0, likely 2 lanes | DT-8996 `jedec,ufs-2.0` | dt |
| Partition picker | `userdata` | LIN fstab | dt |

The 8996 UFS host sits at a completely different address than sweet/SDM845
(`0x1d84000`). The register *offsets* are standard UFSHCI 2.0, so the
sweet/shim driver's offset table is reusable; the base, PHY and clock setup
are not.

## Display

| Item | Value | Source | Status |
|---|---|---|---|
| Splash framebuffer | `0x83401000` | DT-LE `cont_splash_mem@83401000` | dt |
| Splash region | `0x83400000`, 0x02400000 (36 MiB) | DT-LE `dfps_data_mem` + `cont_splash_mem` | dt |
| Panel | 1080 x 1920 | DT-LE `dsi-panel-s6d6fa1-1080p-video` (`qcom,mdss-dsi-panel-width/height`) | dt |
| Panel mode | DSI video mode, 57 fps | DT-LE panel node | dt |
| Panels per HW rev | several (EVT/DVT/PVT/NA all 1080x1920) | DT-LE `leeco/` panel files | dt |

The Foreground display is **not** iPhone X shaped. Any final presentation
will need a scaled or letterboxed mode; the sweet port's "present as
1125x2436" trick does not fit this panel aspect ratio.

## Input

| Item | Value | Source | Status |
|---|---|---|---|
| Touch controller | **Synaptics DSX** (RMI-compatible) | DT-LE `letv-tp-synaptics-dsx-i2c-le_zl1.dtsi` | dt |
| Touch I2C bus | `0x075BA000` (BLSP2 I2C6) | DT-LE | dt |
| Touch I2C address | `0x20` | DT-LE `synaptics_dsx@20` | dt |
| Touch IRQ GPIO | 125 (TLMM) | DT-LE | dt |
| Touch reset GPIO | 89 (TLMM) | DT-LE | dt |
| Touch VDD | PM8994 LDO14 | DT-LE `vcc_i2c-supply` | dt |
| Alternate touch | FocalTech `0x38` on the same bus (some HW revs) | DT-LE `msm8996-le_zl1-evt.dtsi` | dt |
| Power key | PM8994 PON `kpdpwr` | DT-LE `msm-pm8994.dtsi` | dt |
| Volume keys | not found in the DT-LE board files; likely TLMM `gpio126` per `tlmm_gpio_key` pinctrl | incomplete | **probe** |

The touch controller being Synaptics rather than Goodix is the single
biggest input-side difference from `ios-sweet`. Its GT9896 driver is of
zero use here.

## Memory and boot

| Item | Value | Source | Status |
|---|---|---|---|
| Kernel base | `0x80000000` | LIN `BOARD_KERNEL_BASE` | cfg |
| Page size | 4096 | LIN `BOARD_KERNEL_PAGESIZE` | cfg |
| Image name | `Image.gz-dtb` | LIN `BOARD_KERNEL_IMAGE_NAME` | cfg |
| Kernel addr in boot.img | `0x80008000` | derived (base + 0x800) | **probe** |
| Tags addr | `0x80000100` | LIN `BOARD_KERNEL_TAGS_OFFSET` | cfg |
| Ramdisk addr | `0x81000000` | LIN `BOARD_RAMDISK_OFFSET` | cfg |
| IMEM base | `0x066BF000` | DT-LE `qcom,msm-imem@66bf000` | dt |
| Restart reason | IMEM + `0x65c` | DT-LE `restart_reason@65c` | dt |
| Reserved (no-map) | `0x85800000`+48 MiB, `0x88800000`+98 MiB, `0x8ea00000`+45 MiB | DT-LE reserved-memory | dt |
| RAM size | 4 GiB (LEX727) | device spec | **probe** |

## Open questions the probe answers

1. `CNTFRQ_EL0`: 19.2 MHz, or 0 like the S8 saw?
2. `ID_AA64ISAR0`: is LSE really absent? (It decides whether XNU needs the
   patch the S8 port added.)
3. Real memory layout: how much of the 4 GiB is usable above the firmware
   carve-outs, and does the bootloader hand us the high bank?
4. Does the 4 KiB identity map enable and disable cleanly on this SoC?
5. Is the UFS host left running by the bootloader (`HCS` DP+UTRLRDY), so a
   polled driver can issue commands without re-initialising it?
6. Which touch controller is actually populated on this unit: Synaptics or
   FocalTech?
7. Volume key GPIOs.