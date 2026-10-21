# ios-le-pro3

**iOS 16 (XNU 8796) on the LeEco Le Pro 3 — `zl1`, Qualcomm MSM8996 Pro (Snapdragon 821), LEX727.**

> Experimental bring-up, very early. Nothing here boots iOS yet. Today the
> repository contains the hardware facts and a read-only probe that you can
> actually run on the phone to check them.

## Why this device is hard

The Le Pro 3 is a 2016 phone. Every public iOS-on-Android port so far ran on
something newer, and that gap shows up in the bring-up order:

| | Le Pro 3 (this) | Redmi Note 10 Pro (`ios-sweet`) | Galaxy S8 (`ios-galaxy-s8`) |
|---|---|---|---|
| SoC | MSM8996 Pro | SM7150 | Exynos 8895 |
| Year | 2016 | 2019 | 2017 |
| ARM level | **ARMv8.0** | ARMv8.2 | ARMv8.0 |
| 16 KiB pages | **no** | yes | no |
| LSE atomics | **no** | yes | no |
| UEFI port in Mu-Silicium | **none** | `sweet_k6aPkg` style | `dreamltePkg` |
| Display | 1080x1920 LCD | 1080x2400 | 1440x2960 |
| Touch | Synaptics DSX | Goodix GT9896 | S6SY761 |

The two things that matter most:

1. **No UEFI port exists.** [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
   ships 86 platform packages and not one of them targets MSM8996 or anything
   from that era. A UniXNU-style boot chain needs one.
2. **The CPU is ARMv8.0.** XNU 8796 expects LSE atomics and (for some paths)
   16 KiB pages. Both have to be worked around, exactly as the Galaxy S8 port
   had to do.

That is why the plan starts with a probe rather than with a kernel.

## What this repository will contain

Following [UniXNU](https://github.com/UniXNU/UniXNU)'s structure, but built up
in the same order the S8 and sweet ports were:

```
hardware/     verified values from device trees and board configs, with sources
probe/        read-only fact collector that runs on the phone  <- done
shim/         the XNU loader (later)
tools/        host-side offline tools
xnu/patches/  the XNU patch series (later)
```

## The probe

`probe/` builds a flat arm64 Image that runs from the RAM kernel slot before
any OS. It reads CPU registers, the live device tree's memory nodes, the
MMU, and a handful of peripheral identification registers, draws the whole
report on the splash framebuffer, and spins. It writes no partition.

```sh
tools/selfcheck.sh                               # clean build + all header checks
fastboot boot probe/out/boot.img                 # nothing is flashed
```

`tools/selfcheck.sh` builds the Image and the boot image from scratch and
verifies the qcom boot header, the absence of GOT relocations and the boot
image layout. It is the same set of checks the GitHub Actions workflow
performs; the workflow is written but not yet pushed, because uploading
files under `.github/workflows/` needs a token with the `workflow` scope.

Read the result off the screen and file it with `tools/zl1-log.py`, which
turns a photo or pasted text into a diffable report. Every value in
`probe/zl1_hw.h` is tagged with where it came from:

| Tag | Meaning |
|---|---|
| `[DT]` | read from a device tree (LeEco kernel or mainline) |
| `[CFG]` | read from a LineageOS board config |
| `[OBS]` | **not yet observed**: the probe must confirm it |

## Build notes

The host toolchain assumed here is a plain macOS install of Xcode: no
Homebrew, no binutils, no lld. Clang can compile and assemble aarch64, but
there is no ELF linker to produce the flat Image the qcom bootloader wants.
`tools/flatten.py` therefore links the Mach-O objects itself - see its
docstring for the exact relocations it supports and why it rejects anything
else. On a Linux host with binutils, `probe/build.sh` can use the cross
toolchain instead.

## Safety

- Nothing here is flashed. The probe is loaded with `fastboot boot` and
  disappears on reboot.
- The bootloader must be unlocked. Keep a full backup of `boot`, `recovery`,
  `persist`, `modemst*` and `xbl`/`abl`/`tz` before anything else.
- iOS bring-up will install into `userdata` and **erase Android's data**.
  That is a separate, later, explicit step.

## Credits

The probe's structure, font and 4K MMU approach are adapted from
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) and
[Floppikrob/ios-galaxy-s8](https://github.com/Floppikrob/ios-galaxy-s8). The
device facts come from LeEco's released kernel sources (via LineageOS) and
mainline `msm8996.dtsi`.

Apple's code is licensed, and not distributed here.