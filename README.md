# Linux kernel for the Unihertz Titan Pocket

A Linux kernel and driver set for the **Unihertz Titan Pocket** (MediaTek
MT6771 / Helio P70) that runs a regular Linux distribution instead of
Android. It is MediaTek's **4.14.186** BSP kernel, plus:
- drivers for the board-specific hardware that Unihertz never released
  source for, recovered from the stock firmware;
- a GPU and display stack (Panfrost + a KMS driver);
- the MediaTek connectivity drivers (Wi-Fi, Bluetooth, FM) built from
  source.

Everything here is source code. No stock kernel modules are loaded, and
no vendor firmware is included.

This kernel boots Debian 12 (systemd) on the phone, with a GPU-accelerated
Wayland desktop.

## Contents

| Path | What |
|---|---|
| `kernel/` | the kernel tree: MediaTek 4.14.186 BSP for the Titan Pocket with our changes |
| `modules/connectivity/` | out-of-tree MediaTek CONNSYS drivers: WMT core, Wi-Fi, Bluetooth, FM radio |
| `docs/` | hardware reference, per-driver notes, and how the vendor drivers were reverse-engineered |

## Hardware support

| Hardware | Driver | State |
|---|---|---|
| Hardware keyboard (AW9523 matrix) | `drivers/misc/mediatek/aw9523/` | ✅ stock keymap, Alt symbol layer |
| Keyboard touchpad (Synaptics RMI4) | `drivers/input/misc/titanpocket-touchpad.c` + RMI4 | ✅ multitouch |
| Touchscreen (Goodix GT1151QM) | BSP | ⚠️ present, lightly tested |
| Display | `drivers/gpu/drm/mtkfb_kms/` (new) on the BSP `mtkfb` | ✅ DRM/KMS, 720×720 @ 60 Hz |
| GPU (Mali-G72 MP3) | `drivers/gpu/drm/panfrost/` (backported from 5.10) | ✅ OpenGL ES 3.1 with Mesa |
| Battery fuel gauge (O2Micro OZ8806) | `drivers/power/supply/oz8806_battery.c` (new) | ✅ voltage, current, temperature, capacity |
| Charging (MT6370) | BSP | ⚠️ works, not characterized |
| Speaker amplifier (FourSemi FS1860) | `sound/soc/codecs/fs1860.c` (new) | ✅ |
| Wi-Fi 2.4 + 5 GHz (CONNSYS gen3) | `modules/connectivity/` | ✅ |
| Bluetooth | `modules/connectivity/bt/` | ✅ raw HCI (`/dev/stpbt`); pairing untested |
| FM radio (MT6631) | `modules/connectivity/fm/` | ✅ `/dev/fm` |
| IR blaster | BSP `mtk_irtx_pwm` + MT6370 LDO fix | ✅ `/dev/irtx` |
| NFC (STMicro ST21NFC) | BSP `st21nfc` | ✅ `/dev/st21nfc` (NCI) |
| USB gadget (serial + network) | BSP | ✅ |
| CPU frequency scaling, suspend to RAM | BSP + fixes | ✅ |
| Microphones, earpiece | BSP | ❓ untested |
| GPS | CONNSYS | ❓ not built |
| Cellular modem | BSP CCCI | ❓ needs firmware loading + userspace |
| Cameras | — | ❌ no sensor driver source; the ISP stack is Android-only |

## Origin of the source

- **Kernel base:**
  [ubuntu-touch-unihertz-titan/kernel-alps-mt6771](https://github.com/ubuntu-touch-unihertz-titan/kernel-alps-mt6771),
  branch `halium-11.0`. That is MediaTek's alps 4.14.186 BSP for the Titan
  Pocket/Slim, with the AW9523 keyboard, the GT1151QM touchscreen and the
  panel driver.
- **Connectivity modules:** Motorola's published MediaTek connectivity
  sources and MediaTek's open-source Wi-Fi adaptor (details below).
- **Missing pieces:** reconstructed from the stock Android 11 firmware of
  the phone. Sources were the stock kernel image (with recovered symbols),
  the stock modules, the vendor HAL libraries and the device tree. The
  method and tools are in
  [docs/reverse-engineering.md](docs/reverse-engineering.md).
- **Reference:** OPPO's MT6771 4.14 kernel release, used to cross-check
  MediaTek code that Unihertz didn't publish.

## Changes to the kernel

All changes are relative to `halium-11.0`. Each is explained in
[docs/kernel.md](docs/kernel.md) and in the per-subsystem docs.

### New drivers

| Driver | Files | Doc |
|---|---|---|
| OZ8806 battery fuel gauge | `drivers/power/supply/oz8806_battery.c` | [docs/drivers/oz8806.md](docs/drivers/oz8806.md) |
| FS1860 speaker amplifier (ASoC codec) | `sound/soc/codecs/fs1860.[ch]` | [docs/drivers/fs1860.md](docs/drivers/fs1860.md) |
| Touchpad glue: reset + RMI4 instantiation | `drivers/input/misc/titanpocket-touchpad.c` | [docs/drivers/touchpad.md](docs/drivers/touchpad.md) |
| `mtkfb_kms`: minimal DRM/KMS device on the MediaTek framebuffer | `drivers/gpu/drm/mtkfb_kms/` | [docs/gpu.md](docs/gpu.md) |
| Panfrost (Linux 5.10) with MT6771 power glue | `drivers/gpu/drm/panfrost/` (`panfrost_mtk.c`) | [docs/gpu.md](docs/gpu.md) |
| DRM infrastructure backported from 5.10 | `drivers/gpu/drm/scheduler/`, `drm_gem_shmem_helper.c`, `include/drm/*`, `include/linux/{dma-resv,io-pgtable}.h` | [docs/gpu.md](docs/gpu.md) |
| `ARM_MALI_LPAE` IOMMU page-table format | `drivers/iommu/io-pgtable{,-arm}.[ch]` | [docs/gpu.md](docs/gpu.md) |

### Changes to existing drivers

| Area | Change | Doc |
|---|---|---|
| Keyboard (`aw9523_key.c`) | active at probe, idempotent suspend/resume, stock keymap recovered from the stock kernel | [docs/drivers/aw9523-keyboard.md](docs/drivers/aw9523-keyboard.md) |
| Pinctrl (`pinctrl-paris.c`) | `agold_gpio_get_in()` for board strap reads | [docs/hardware.md](docs/hardware.md) |
| Audio (`mtk-soc-speaker-amp`, `mtk-soc-afe-control.c`) | FS1860 amp hook; playback copy honours the ALSA buffer position | [docs/audio.md](docs/audio.md) |
| IR blaster (`mt6370_pmu_ldo.c`) | adds the `irtx_ldo` consumer so the IR LED gets power | [docs/ir.md](docs/ir.md) |
| USB boost (`usb_boost_plat.c`) | off by default; it pinned all CPUs at maximum frequency during USB traffic | [docs/power.md](docs/power.md) |
| USB gadget Ethernet (`u_ether.c`) | `eth_work` no longer requeues itself forever when the mtu3 ring is full | [docs/power.md](docs/power.md) |
| Panfrost power | runtime suspend (MFG clock + MTCMOS off), system suspend (GPU bucks off), balanced runtime PM | [docs/power.md](docs/power.md) |
| GPU frequency (`mtk_gpufreq_core.c`) | `mt_gpufreq_probed()` and exports so Panfrost can defer and drive power; kbase is no longer built | [docs/gpu.md](docs/gpu.md) |
| DRM core (`drm_gem.c`), `vgem` | skip NULL pages of sparse heap objects; clean caches before write-combined user mappings (arm64) | [docs/gpu.md](docs/gpu.md) |

### Fixes needed to boot and run

| File | Fix |
|---|---|
| `drivers/devfreq/helio-dvfsrc.c`, `helio-dvfsrc_v1.h` | include the local `mtk_dvfsrc_reg.h`. The angle-bracket include picked another SoC's header with absolute register addresses, so the display idle manager oopsed at ~19 s (bootloop) |
| `drivers/misc/mediatek/video/mt6771/videox/disp_helper.c` | display idle manager off; its "decouple" mode scans out a buffer only Android's SurfaceFlinger updates |
| `drivers/tty/vt/selection.c` | upstream fix for a duplicate `set_selection` that breaks `CONFIG_VT` |

### Building with GCC

The upstream tree is built with clang. About 90 files were changed so that
it builds with GCC 9.4: duplicate symbol definitions, include paths, and
warnings promoted to errors. One lesson from this: an include-path change
can silently select a different header with the same name. That is what
caused the DVFSRC bootloop.

### Configuration

`arch/arm64/configs/titanpocket_defconfig` (generated from the stock
`/proc/config.gz`) plus the fragment
`arch/arm64/configs/titanpocket_linux.config`. The fragment adds:
- the new drivers;
- what systemd and a generic distribution need (devtmpfs, cgroups/BPF,
  seccomp, inotify, ...);
- a VT text console on the framebuffer;
- USB gadget serial and network;
- the DRM stack: Panfrost as a module, `mtkfb_kms` as a module;
- debugfs and `/dev/mem` access, for bring-up.

The kernel version string ends in `-titanpocket`.

## Connectivity modules

The phone's Wi-Fi, Bluetooth, FM and GPS share MediaTek's on-die
**CONNSYS** subsystem with an MT6631 RF chip. The drivers build out of tree
against `kernel/`:

| Module | Device | Source |
|---|---|---|
| `wmt_drv.ko` | WMT core: chip power, STP, BTIF, patch download | Motorola `vendor-mediatek-kernel_modules-connectivity-common` (`android-11-release-rrq`) |
| `wmt_chrdev_wifi.ko` | `/dev/wmtWifi`: Wi-Fi on/off, NVRAM hand-over | `mediatek-opensource` wlan-adaptor (`alps-mp-r0.mp1`) |
| `wlan_drv_gen3.ko` | `wlan0` (cfg80211) | Motorola `...-connectivity-wlan-core-gen3` (`android-15-release-vvob35`) |
| `bt_drv.ko` | `/dev/stpbt`: raw H4 HCI over STP | Motorola `...-connectivity-bt-mt66xx` (`android-11-release-rrk`) |
| `fmradio_drv.ko` | `/dev/fm` (MT6631) | Motorola `...-connectivity-fmradio` (`android-11-release-rrq`) |

MediaTek never published two MT6771-specific files. They were
**reconstructed** from the stock modules:
- `common/common_main/platform/mt6771.c`: chip power, clocks, PMIC LDOs,
  EMI remap and MPU, pin mux. 31 hooks of the ops table, each annotated
  with the stock code address it came from.
- `wlan_gen3/os/linux/plat/mt6771/plat_priv.c`: the EMI MPU region for
  Wi-Fi. Without it the chip triggers MPU violations.

Smaller changes to the vendor sources (Motorola-specific config file
selection, newer-kernel API guards, firmware search path) are listed in
[docs/drivers/connectivity.md](docs/drivers/connectivity.md).

## Building

Tested with **native GCC 9.4 on arm64 (Ubuntu 20.04)**. Cross-compiling
with an `aarch64-linux-gnu-` GCC 9 toolchain should work but hasn't been
tested. Pass `CROSS_COMPILE=aarch64-linux-gnu-` to every `make`, because
the config sets an empty prefix. The source must be on a
**case-sensitive** filesystem.

```sh
cd kernel
export ARCH=arm64
O=../build                     # out-of-tree output directory

scripts/kconfig/merge_config.sh -O $O -m \
    arch/arm64/configs/titanpocket_defconfig \
    arch/arm64/configs/titanpocket_linux.config
make O=$O olddefconfig
make O=$O -j$(nproc) Image.gz modules
# -> $O/arch/arm64/boot/Image.gz and the in-tree modules
#    (panfrost.ko, mtkfb_kms.ko, ...)
```

Connectivity modules, against that build:

```sh
KDIR=$PWD/../build bash ../modules/connectivity/build.sh
# -> modules/connectivity/out/{wmt_drv,wmt_chrdev_wifi,wlan_drv_gen3,bt_drv,fmradio_drv}.ko
```

Rebuild the connectivity modules whenever the kernel configuration
changes, because symbol versions change with it.

## Booting it

> ⚠️ Flashing a boot image replaces the phone's boot partition. Back it up
> first, and keep a way back to stock (fastboot or SP Flash Tool). The
> bootloader must be unlocked.

- The stock LK bootloader expects a **boot image header v2** with a
  separate dtb. Repack the stock `boot.img` with the new `Image.gz`
  (e.g. with `magiskboot`). Keep its **stock dtb and command line**. This
  kernel is used with the stock base device tree; the tree's generic
  `mt6771.dts` is not what the phone boots with.
- The board devices are defined in the **stock dtbo partition**, which the
  bootloader applies at boot. It must stay untouched: the drivers bind
  against the stock nodes and compatibles.
- Supply your own initramfs and root filesystem. The UFS partition layout,
  and how a Linux root partition was made beside Android's, are in
  [docs/partitioning.md](docs/partitioning.md).
- Flash with `fastboot flash boot <image>`.

## Runtime requirements (not in this repository)

Some hardware needs files that only exist on the phone itself:

| Needed for | Files | Source |
|---|---|---|
| Wi-Fi / Bluetooth | `WMT_SOC.cfg`, `ROMv4_be_patch_1_0_hdr.bin`, `ROMv4_be_patch_1_1_hdr.bin`, `WIFI_RAM_CODE_6759` → `/lib/firmware` | stock vendor partition |
| FM radio | `mt6631_fm_v1_patch.bin`, `mt6631_fm_v1_coeff.bin`, `fm_cust.cfg` → `/lib/firmware` | stock vendor partition |
| Speaker amplifier | `fs16xx_01s_mono.preset` → `/lib/firmware` | stock vendor partition |
| Wi-Fi MAC, TX power, 5 GHz | factory NVRAM record `APCFG/APRDEB/WIFI` (first 1024 bytes, handed to `/dev/wmtWifi`) | the phone's `nvdata` partition, unique per unit |
| Bluetooth address | NVRAM record `APCFG/APRDEB/BT_Addr`, set with HCI vendor command `0xFC1A` | the phone's `nvdata` partition, unique per unit |

These files are not redistributable, and the NVRAM records are specific
to each phone. Copy them from your own device, and don't publish images
that contain them.

The drivers also need some userspace:
- **Wi-Fi/Bluetooth/FM:** the WMT core needs a launcher that powers the
  chip on and serves the patch download. This is Android's
  `wmt_loader`/`wmt_launcher`, or a replacement implementing the same
  protocol. Bluetooth needs a relay from `/dev/stpbt` to `/dev/vhci` to
  appear as a BlueZ `hci0`. See
  [docs/drivers/connectivity.md](docs/drivers/connectivity.md).
- **GPU:** Mesa 22.3 or later (Panfrost + `kmsro`). The KMS driver
  registers as `mediatek`, so Mesa pairs it with Panfrost.
- **NFC:** an NCI stack talking to `/dev/st21nfc`
  ([docs/nfc.md](docs/nfc.md)).

## Known limitations

- 4.14 vendor kernel. A mainline port would start from the closely
  related MT8183. The drivers here are source code, so they can move with
  it.
- Display scanout copies each frame into the framebuffer: no zero-copy
  yet, no vblank interrupts, no DPMS.
- GPU runs at a fixed frequency (no devfreq governor).
- With Wi-Fi associated, the connectivity chip wakes the SoC from deep
  sleep about every 0.5 s.
- Not done: cameras, modem, GPS, microphones/earpiece routing.

## Documentation

| Doc | Contents |
|---|---|
| [hardware.md](docs/hardware.md) | SoC, PMICs, device tree layout, I2C device map, GPIOs, speakers, battery |
| [reverse-engineering.md](docs/reverse-engineering.md) | how the vendor drivers were recovered from the stock firmware, and the method |
| [kernel.md](docs/kernel.md) | source tree, configuration fragment, every change and why |
| [partitioning.md](docs/partitioning.md) | UFS layout and adding a Linux root partition |
| [gpu.md](docs/gpu.md) | Panfrost backport to 4.14, bugs found, the `mtkfb_kms` display driver |
| [power.md](docs/power.md) | CPU frequency, idle, suspend to RAM, GPU power, wake sources |
| [audio.md](docs/audio.md) | speaker signal chain and the bugs fixed in it |
| [ir.md](docs/ir.md) | IR blaster: driver interface, bitstream format, the LED power fix |
| [nfc.md](docs/nfc.md) | ST21NFC: driver interface, NCI, tag reading and writing |
| [debugging.md](docs/debugging.md) | logs, debug interfaces, gotchas |
| [drivers/connectivity.md](docs/drivers/connectivity.md) | CONNSYS Wi-Fi/Bluetooth from source, reconstructing `mt6771.c` |
| [drivers/fm.md](docs/drivers/fm.md) | FM radio |
| [drivers/aw9523-keyboard.md](docs/drivers/aw9523-keyboard.md) | keyboard driver, keymap, Alt layer |
| [drivers/touchpad.md](docs/drivers/touchpad.md) | keyboard touchpad (Synaptics RMI4) |
| [drivers/oz8806.md](docs/drivers/oz8806.md) | battery fuel gauge |
| [drivers/fs1860.md](docs/drivers/fs1860.md) | speaker amplifier |

The docs are working notes from bringing up a real device. Some of them
mention userspace tools and build helpers that are not part of this
repository.

## License

- The kernel is **GPL-2.0** (`kernel/COPYING`). New drivers carry
  `SPDX-License-Identifier: GPL-2.0`.
- The connectivity modules are GPL-2.0 or "Dual BSD/GPL", as marked in
  each source file. The reconstructed platform files keep the GPL-2.0
  header of the MediaTek code they reimplement.
- No vendor firmware, calibration data or NVRAM content is included.
