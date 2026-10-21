/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * zl1_hw.h - hardware constants for the LeEco Le Pro 3 (zl1, MSM8996 Pro).
 *
 * EVERY value here carries a source tag. Three kinds:
 *
 *   [DT]   Read from a device tree in the LeEco / mainline tree. This is the
 *          SoC's documented address, so it is as good as the upstream source.
 *   [CFG]  Read from LineageOS board config (kernel base, page size...).
 *   [OBS]  To be confirmed on the phone by the probe. Nothing marked [OBS]
 *          may be relied on by a driver before the probe has printed it.
 *
 * The probe is the only program that may trust a [DT] value blindly: if a
 * [DT] address is right, the probe reads it; if it is wrong, the phone
 * either returns 0xFFFFFFFF or the interrupt fires. Either way the log says
 * which one happened, and that becomes evidence.
 */
#ifndef ZL1_HW_H
#define ZL1_HW_H

/* ------------------------------------------------------------------ */
/* Platform identity                                                   */
/* ------------------------------------------------------------------ */
#define ZL1_SOC                "MSM8996 Pro"   /* [CFG] LineageOS zl1 */
#define ZL1_PLATFORM_TAG       0x6c7a3170u     /* "zl1p" probe signature */

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
/* [DT] msm8996.dtsi: intc: interrupt-controller@9bc0000
 *      GICv3 with security extensions. Two regions: distributor, then the
 *      redistributor block at 0x09c00000 with a 0x40000 stride per core.
 *      NOTE: this is GICv3, NOT the GIC-400 the Galaxy S8 port uses. The
 *      register layout and the timer delivery differ (see ZL1_PPI_VIRT_TIMER).
 */
#define ZL1_GICD_BASE          0x09BC0000u
#define ZL1_GICD_SIZE          0x00010000u
#define ZL1_GICR_BASE          0x09C00000u
#define ZL1_GICR_SIZE          0x00100000u
#define ZL1_GICR_STRIDE        0x00040000u
/* [DT] msm8996.dtsi: timer { interrupts = <PPI 13>, <PPI 14>, <PPI 11>, <PPI 10> }
 *      indexes are: secure phys, non-secure phys, virtual, hypervisor.
 *      So the virtual timer is PPI 11 -> INTID 16 + 11 = 27.
 *      On the S8 (GIC-400) it was PPI 11 -> 27 as well, but delivered as a
 *      Group 1 interrupt through different banked registers; do not assume
 *      the S8 code paths transfer.
 */
#define ZL1_PPI_VIRT_TIMER     27
#define ZL1_PPI_PHYS_TIMER     30
#define ZL1_PPI_HYP_TIMER      26

/* [OBS] Platform expects the standard PSCI 1.0 SMC conduit. Method and
 *       function IDs must be confirmed on the phone (probe reads PSCI
 *       version through arm_smccc, see zl1_probe.c). */
#define ZL1_PSCI_VERSION       0x84000000u

/* ------------------------------------------------------------------ */
/* Clocks and timers                                                   */
/* ------------------------------------------------------------------ */
/* [OBS] The S8 port found CNTFRQ_EL0 == 0 at handoff and used the DT value
 *       instead. 8996 boots at 19.2 MHz in most vendor DTs; the probe prints
 *       whatever CNTFRQ_EL0 actually holds so we keep the reading, not a guess.
 *       Still unverified on zl1: TWRP's kernel has already programmed the
 *       counter by the time adb is reachable, so the host cannot see the
 *       bootloader handoff value.
 */
#define ZL1_TIMER_FALLBACK_HZ  19200000u

/* ------------------------------------------------------------------ */
/* Storage: UFS 2.0                                                    */
/* ------------------------------------------------------------------ */
/* [DT] mainline msm8996.dtsi: ufshc@624000, compatible "jedec,ufs-2.0".
 *      Unlike sweet/SDM845 (0x1d84000) the 8996 UFS host sits in the low
 *      "peripheral" window. Register offsets themselves are UFSHCI 2.0
 *      standard, so the sweet/shim UFS driver's offset table is reusable;
 *      only the base address, the PHY and the clock/RPMh setup are new.
 */
#define ZL1_UFS_BASE           0x00624000u
#define ZL1_UFS_SIZE           0x00002500u
/* [DT] ufsphy: phy@627000, compatible "qcom,msm8996-qmp-ufs-phy".
 * [VERIFIED 2026-10-21] /proc/iomem on the phone:
 *     00624000-006264ff : /soc/ufshc@624000
 *     00627000-00627da7 : phy_mem
 *     00630000-00637fff : /soc/ufsice@630000
 * The inline-crypto engine at 0x630000 is new to this port. The probe must
 * not touch it, but the XNU UFS driver will have to coexist with it.
 */
#define ZL1_UFS_PHY_BASE       0x00627000u
#define ZL1_UFS_PHY_SIZE       0x00001000u
#define ZL1_UFSICE_BASE        0x00630000u
#define ZL1_UFSICE_SIZE        0x00008000u

/* UFSHCI standard register offsets (JEDEC UFS 2.0 spec, section 6). These
 * are the same offsets sweet uses; repeated here so the probe does not
 * depend on the sweet tree. */
#define UFSHCI_CAP             0x00u
#define UFSHCI_VER             0x08u
#define UFSHCI_IS              0x20u
#define UFSHCI_IE              0x24u
#define UFSHCI_HCS             0x30u
#define UFSHCI_HCE             0x34u
#define UFSHCI_UTRLBA          0x50u
#define UFSHCI_UTRLBAU         0x54u
#define UFSHCI_UTRLDBR         0x58u
#define UFSHCI_UTRLCLR         0x5Cu
#define UFSHCI_UTRLRSR         0x60u
#define UFSHCI_UTMRLBA         0x70u

/* ------------------------------------------------------------------ */
/* Display: the continuous splash framebuffer left by the bootloader    */
/* ------------------------------------------------------------------ */
/* [DT] LeEco msm8996.dtsi: cont_splash_mem@83401000, size 0x23ff000.
 *      dfps_data_mem@83400000 sits directly below it (4 KiB of DSI timing),
 *      so the framebuffer base is 0x83401000 and the visible panel area is
 *      the first 1080*1920*4 bytes.
 *      [DT] panel: dsi-panel-s6d6fa1-1080p-video / r69338-*-1080p-video,
 *      width 1080, height 1920, framerate 57.
 *      The panel node also shows the Le Pro 3 ships several panels per HW
 *      revision (EVT/DVT/PVT/NA); all of the zl1 ones are 1080x1920 video
 *      mode, so the geometry is safe even before the probe confirms it.
 */
#define ZL1_FB_PHYS            0x83401000u
#define ZL1_FB_WIDTH           1080u
#define ZL1_FB_HEIGHT          1920u
#define ZL1_FB_STRIDE          (ZL1_FB_WIDTH * 4u)
#define ZL1_FB_BPP             32u
#define ZL1_SPLASH_REGION_BASE 0x83400000u
#define ZL1_SPLASH_REGION_SIZE 0x02400000u

/* ------------------------------------------------------------------ */
/* Memory layout                                                       */
/* ------------------------------------------------------------------ */
/* [DT] LeEco msm8996.dtsi reserved-memory:
 *        removed_regions        0x85800000 + 0x03000000  (no-map)
 *        peripheral_region      0x8ea00000 + 0x02d00000  (no-map)
 *        modem_region           0x88800000 + 0x06200000  (no-map)
 *      That is what the modem/firmware already occupies. The probe reports
 *      the live FDT memory nodes instead of trusting these, because the
 *      exact set changes with the bootloader and the carrier.
 */
#define ZL1_RESERVED_REMOVED   0x85800000u
#define ZL1_RESERVED_REMOVED_SZ 0x03000000u
#define ZL1_RESERVED_MODEM     0x88800000u
#define ZL1_RESERVED_MODEM_SZ  0x06200000u
#define ZL1_RESERVED_PERIPH    0x8ea00000u
#define ZL1_RESERVED_PERIPH_SZ 0x02d00000u

/* [CFG] LineageOS msm8996-common BoardConfig: BOARD_KERNEL_BASE 0x80000000,
 *       page size 4096.
 *
 * [VERIFIED 2026-10-21, LEX727] The real memory map is TWO banks, not one
 * high bank as originally assumed here:
 *
 *   bank 0  0x0000000080000000 + 0x0000000080000000  (2.00 GiB)
 *   bank 1  0x0000000100000000 + 0x000000007e550000  (1.97 GiB)
 *
 * read from /proc/device-tree/memory/reg and confirmed by /proc/iomem
 * ("80000000-857fffff System RAM", "91600000-17e54ffff System RAM").
 * Code must still read the FDT rather than trust these numbers, but the
 * two-bank shape is now a fact and the page-table plan has to allow for it.
 */
#define ZL1_KERNEL_BASE        0x80000000u
#define ZL1_KERNEL_PAGE_SIZE   4096u
#define ZL1_HIGH_BANK_MIN      0x100000000ull

/* ------------------------------------------------------------------ */
/* Boot chain                                                          */
/* ------------------------------------------------------------------ */
/* [DT] qcom,msm-imem@66bf000, restart_reason at +0x65c.
 *      The bootloader reads this dword to decide the next mode (recovery,
 *      fastboot, normal). Writing a value here and resetting is how an
 *      Android device asks for a specific reboot target without touching
 *      any partition. Our probe uses 0x77665501 (recovery) only after the
 *      log has been committed to the framebuffer.
 */
#define ZL1_IMEM_BASE          0x066BF000u
#define ZL1_IMEM_RESTART_REASON 0x0000065Cu
#define ZL1_RESTART_REASON_RECOVERY 0x77665501u
#define ZL1_RESTART_REASON_FASTBOOT 0x77665500u

/* [DT] PS_HOLD lives in PM8994 (SPMI SID 1). Written as an absolute address
 *      for the 8996 QSPMI arbiter map; the probe only reads PON interrupt
 *      latched status, never writes PS_HOLD.
 *      SPMI arbiter itself: [DT] spmi@400f000 (core), chnls 0x00600000.
 */
#define ZL1_SPMI_CORE_BASE     0x0400F000u
#define ZL1_SPMI_CHNLS_BASE    0x00600000u
#define ZL1_PM8994_SID         1u
#define ZL1_PMI8994_SID        2u

/* ------------------------------------------------------------------ */
/* GPIO / pinctrl                                                      */
/* ------------------------------------------------------------------ */
/* [DT] tlmm: pinctrl@1010000, 0x300000 bytes. */
#define ZL1_TLMM_BASE          0x01010000u
#define ZL1_TLMM_SIZE          0x00300000u

/* ------------------------------------------------------------------ */
/* Touch: Synaptics DSX, NOT Goodix                                    */
/* ------------------------------------------------------------------ */
/* [DT] LeEco leeco/letv-tp-synaptics-dsx-i2c-le_zl1.dtsi:
 *        synaptics_dsx@20 on i2c 0x75ba000 (BLSP2 I2C6)
 *        irq-gpio  85 -> tlmm 125
 *        reset-gpio 89 -> tlmm 89
 *        vcc_i2c supply PM8994 LDO14
 *      The EVT board file also lists an ft8716 at 0x38 on the same bus, so
 *      the first thing the touch bring-up must do is read 0x20 and see
 *      whether the Synaptics answers. sweet's Goodix GT9896 driver is of NO
 *      use here; this needs a fresh RMI/DSX driver.
 */
#define ZL1_TOUCH_I2C_BASE     0x075BA000u
#define ZL1_TOUCH_I2C_ADDR     0x20u
#define ZL1_TOUCH_IRQ_GPIO     125u
#define ZL1_TOUCH_RST_GPIO     89u
#define ZL1_FT_TOUCH_I2C_ADDR  0x38u

/* ------------------------------------------------------------------ */
/* GCC and RPMh equivalents                                            */
/* ------------------------------------------------------------------ */
/* [DT] gcc: clock-controller@300000 (qcom,gcc-msm8996), 0x90000 bytes.
 *      8996 predates RPMh: power votes go through the RPM (glink) which
 *      the bootloader already brought up. The probe must not touch the
 *      clock controller; it only records whether UFS/GCC gates are on.
 */
#define ZL1_GCC_BASE           0x00300000u
#define ZL1_MMCC_BASE          0x008C0000u
/* [DT] mdss: display-subsystem@900000, mdss_phys 0x1000, vbif 0x901000 */
#define ZL1_MDSS_BASE          0x00900000u
#define ZL1_MDSS_VBIF          0x00901000u

#endif /* ZL1_HW_H */