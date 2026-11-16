# Linux kernel for the Unihertz Titan Pocket

A Linux kernel and driver set for the **Unihertz Titan Pocket** (MediaTek
MT6771 / Helio P70) that runs a regular Linux distribution instead of
Android. It is MediaTek's **4.14.186** BSP kernel, plus:
- drivers for the board-specific hardware that Unihertz never released
  source for, recovered from the stock firmware;
- a GPU and display stack (Panfrost + a KMS driver);
- the MediaTek connectivity drivers (Wi-Fi, Bluetooth, FM, GPS) built
  from source;
- V4L2 support for the cameras (the camera drivers themselves are still
  being brought up out of tree, see [docs/camera.md](docs/camera.md)).

Everything here is source code. No stock kernel modules are loaded, and
no vendor firmware is included.

This kernel boots Debian 12 (systemd) on the phone, with a GPU-accelerated
Wayland desktop.

**Linux 6.12.** [`kernel-6.12/`](kernel-6.12/README.md) ports the phone to
Linux 6.12.115 as a patch series against the kernel.org release: upstream
drivers wherever the MT6771 matches the MT8183 (it mostly does), our own
where it doesn't. It boots the same Debian with the desktop, touch, keys,
sound, Wi-Fi, Bluetooth, FM, sensors, charging and deep sleep (37.5 mA in
everyday standby with the modem registered; the fix for its occasional
hung wake is being confirmed in daily use); the cameras are ported but not
yet run. Its status table and build steps are in that README.

## Contents

| Path | What |
|---|---|
| `kernel/` | the kernel tree: MediaTek 4.14.186 BSP for the Titan Pocket with changes needed to support this device. |
| `kernel-6.12/` | the Linux 6.12 port: patches against v6.12.115, build and boot-image tools, status |
| `modules/connectivity/` | out-of-tree MediaTek CONNSYS drivers: WMT core, Wi-Fi, Bluetooth, FM radio, GPS (`linux612/` and `build612.sh`: the 6.12 build) |
| `modules/camera/` | the camera driver (`v4l2/`: V4L2 + media controller; `linux612/`: its 6.12 version), the pass-2 ISP (`dip/`), bring-up tools (`camprobe/`, `camcap/`) and the ISP register map (`isp/`) |
| `docs/` | hardware reference, per-driver notes, and how the vendor drivers were reverse-engineered |

## Hardware support

| Hardware | Driver | State |
|---|---|---|
| Hardware keyboard (AW9523 matrix) | `drivers/misc/mediatek/aw9523/` | ✅ stock keymap, Alt symbol layer |
| Keyboard touchpad (Synaptics RMI4) | `drivers/input/misc/titanpocket-touchpad.c` + RMI4 | ✅ multitouch |
| Touchscreen (Goodix GT1151QM) | BSP | ✅ the desktop and apps are used by touch |
| Display | `drivers/gpu/drm/mtkfb_kms/` (new) on the BSP `mtkfb` | ✅ DRM/KMS, 720×720 @ 60 Hz |
| GPU (Mali-G72 MP3) | `drivers/gpu/drm/panfrost/` (backported from 5.10) | ✅ OpenGL ES 3.1 with Mesa |
| Battery fuel gauge (O2Micro OZ8806) | `drivers/power/supply/oz8806_battery.c` (new) | ✅ voltage, current, temperature, capacity |
| Charging (MT6370) | BSP | ⚠️ works, not characterized |
| Speaker amplifier (FourSemi FS1860) | `sound/soc/codecs/fs1860.c` (new) | ✅ |
| Wi-Fi 2.4 + 5 GHz (CONNSYS gen3) | `modules/connectivity/` | ✅ sleeps with the SoC (power save, suspend mode); monitor mode with frame injection |
| Bluetooth | `modules/connectivity/bt/` | ✅ BlueZ via raw HCI (`/dev/stpbt`); LE scanning and GATT peripheral connections work (with the supported-commands fix in the HCI relay); controller sleep enabled, so it no longer keeps the connectivity chip awake; classic pairing and A2DP audio through PipeWire work |
| FM radio (MT6631) | `modules/connectivity/fm/` | ✅ `/dev/fm`: tuning, seek, scan, RDS (station name, call letters, programme type, radio text) |
| Headphone jack (MT6358 accdet) | BSP | ✅ plug detection (`SW_HEADPHONE_INSERT`); sound and FM switch to the headphones with their own volume ([docs/audio.md](docs/audio.md#headphones)) |
| IR blaster | BSP `mtk_irtx_pwm` + MT6370 LDO fix | ✅ `/dev/irtx` |
| NFC (STMicro ST21NFC) | BSP `st21nfc` | ✅ `/dev/st21nfc` (NCI) |
| USB gadget (serial + network) | BSP | ✅ plus a mass-storage LUN that lends the microSD card to a computer on request |
| microSD card | BSP (`msdc1`) | ✅ |
| CPU frequency scaling, suspend to RAM | BSP + fixes | ⚠️ works; a power domain left on at suspend now refuses the suspend instead of an oops and a reset; a rare hang on resume is open ([docs/power.md](docs/power.md)) |
| Microphones (two analog MEMS, MT6358 codec) | BSP | ✅ DCC mode, ALSA device 1 ([docs/audio.md](docs/audio.md#microphones)) |
| Earpiece, headset mic | BSP | ❓ untested |
| Accelerometer, gyroscope, magnetometer | BSP sensor hub (SCP) | ✅ readings in SI units; reported uncalibrated, offsets learned in userspace ([docs/hardware.md](docs/hardware.md#motion-sensors)) |
| Vibration motor | BSP | ✅ |
| GPS / GNSS (MT6631) | `modules/connectivity/gps/` | ✅ 3D fix outdoors (GPS, GLONASS, BeiDou, SBAS). The driver is source; positioning runs on MediaTek's closed `libmnl.so` from the phone ([docs/gnss.md](docs/gnss.md)) |
| Cellular modem | BSP CCCI | ❓ needs firmware loading + userspace |
| Cameras (OV16880 rear, OV8865 front) | `modules/camera/v4l2/` (new, out of tree) | ✅ both stream at 30 fps through V4L2 (MIPI receiver + CAMSV DMA) and libcamera (software ISP debayering on the GPU; `cam`, GStreamer); Camera app with autofocus, fed by MediaTek's ISP AF statistics (pass 1, run beside the capture path). The rest of the hardware ISP isn't used yet ([docs/camera.md](docs/camera.md)) |

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
| `udmabuf` (backported from 4.20): memfd pages as dma-bufs, for libcamera's software ISP | `drivers/dma-buf/udmabuf.c`, `include/uapi/linux/udmabuf.h` | [docs/camera.md](docs/camera.md) |

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
| V4L2 (`drivers/media/v4l2-core/Kconfig`) | `VIDEOBUF2_DMA_SG` gets a prompt, so an out-of-tree V4L2 driver can use it without an in-tree driver selecting it | [docs/camera.md](docs/camera.md) |
| videobuf2 (`videobuf2-core.c`) | Linux 5.0's buffer orphaning: `REQBUFS(0)` frees the queue while buffers are still mapped or exported (libcamera relies on it) | [docs/camera.md](docs/camera.md) |
| V4L2 formats (`v4l2-ioctl.c`) | the name of 4.18's 14-bit Bayer format (`BG14`) | [docs/camera.md](docs/camera.md) |
| DRM core (`drm_gem.c`), `vgem` | skip NULL pages of sparse heap objects; clean caches before write-combined user mappings (arm64) | [docs/gpu.md](docs/gpu.md) |
| Suspend (`clk-mt6771-pg.c`, `spm_v4/mtk_sleep.c`) | a power domain still on at suspend refuses the suspend (`-EBUSY`) instead of `BUG_ON`, which reset the phone | [docs/power.md](docs/power.md) |
| Logging (`spm_v4/mtk_spm_sodi.c`, `pmic/mt6358/v1/pmic_irq.c`, `mmc/host/mediatek/ComboA/sd.c`) | no SODI line for every non-timer wake (a summary once a minute); PMIC IRQ enable and SD clock lines at debug level | [docs/power.md](docs/power.md) |

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
- V4L2 with the media controller and subdevice API, and videobuf2
  scatter-gather buffers, for the camera drivers (other media areas off);
- `/dev/udmabuf`;
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
| `gps_drv.ko` | `/dev/stpgps`: raw GNSS data over STP | MediaTek `vendor-mediatek-kernel_modules-connectivity-gps` (android-11) |

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
# -> modules/connectivity/out/{wmt_drv,wmt_chrdev_wifi,wlan_drv_gen3,bt_drv,fmradio_drv,gps_drv}.ko
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
| GPS | MediaTek's `libmnl.so` (positioning engine, carries the GNSS DSP firmware patch) and the 32-bit Android runtime it links against: 8 files in a chroot | stock system image and runtime APEX |
| Wi-Fi MAC, TX power, 5 GHz | factory NVRAM record `APCFG/APRDEB/WIFI` (first 1024 bytes, handed to `/dev/wmtWifi`) | the phone's `nvdata` partition, unique per unit |
| Bluetooth address, sleep parameters | NVRAM record `APCFG/APRDEB/BT_Addr`: the address (vendor command `0xFC1A`) and bytes 18-24, the controller's sleep parameters (`0xFC7A`, re-sent after every HCI_Reset) | the phone's `nvdata` partition, unique per unit |

These files are not redistributable, and the NVRAM records are specific
to each phone. Copy them from your own device, and don't publish images
that contain them.

The drivers also need userspace:
- **Wi-Fi/Bluetooth/FM:** the WMT core needs a launcher that powers the
  chip on and serves the patch download. Bluetooth needs a relay from
  `/dev/stpbt` to `/dev/vhci` so it appears as BlueZ `hci0` (set to the
  factory address from NVRAM with vendor command `0xFC1A`). Wi-Fi needs the
  factory NVRAM record handed to the driver before power-on
  ([docs/drivers/connectivity.md](docs/drivers/connectivity.md)).
- **Speaker:** the loudspeaker path (I2S clock, then the FS1860 amp) has to
  be powered while a playback device is open
  ([docs/audio.md](docs/audio.md)).
- **GPS:** `libmnl.so` needs a host process that loads it and feeds it
  `/dev/stpgps`, and something to turn its output into NMEA for gpsd. The
  interface (17 callbacks, the `/dev/stpgps` framing) is documented in
  [docs/gnss.md](docs/gnss.md); the host programs aren't in this
  repository yet.
- **GPU:** Mesa 22.3 or later (Panfrost + `kmsro`). The KMS driver
  registers as `mediatek`, so Mesa pairs it with Panfrost.
- **NFC:** an NCI stack talking to `/dev/st21nfc`
  ([docs/nfc.md](docs/nfc.md)).

## Userspace

The userspace this was brought up with (the daemons that replace Android's
vendor services, the desktop and the apps) is a separate project, not yet
released. The docs describe what the hardware needs from it in general
terms; where they name a program, it belongs to that project.

**tinyalsa:** in read/write mode, tinyalsa 2.0.0 let `SYNC_PTR` overwrite
the kernel's application pointer, so the stream rewound on this kernel and
audio got overwritten; the userspace carries a one-line fix.

## Known limitations

- 4.14 vendor kernel. A mainline port would start from the closely
  related MT8183. The drivers here are source code, so they can move with
  it.
- Display scanout copies each frame into the framebuffer: no zero-copy
  yet, no vblank interrupts, no DPMS.
- GPU runs at a fixed frequency (no devfreq governor).
- Sleep current: ~12 mA in forced sleep with Wi-Fi connected and Bluetooth
  on, but in real use it can step up to ~80 mA after a while, for a reason
  not found yet (docs/power.md).
- GPS depends on MediaTek's closed `libmnl.so`, and runs without
  assistance data (no AGPS/EPO yet), so cold starts take minutes.
- Cameras run on libcamera's software ISP (a patched v0.7.2, see
  docs/camera.md), not MediaTek's hardware ISP; no tuning files yet.
- Not done: modem, microphones/earpiece routing, FM on headphones and RDS.

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
| [gnss.md](docs/gnss.md) | GPS: the `/dev/stpgps` protocol, the libmnl interface, running it without Android's mnld |
| [camera.md](docs/camera.md) | cameras: sensors, power sequence, MIPI receiver, CAMSV DMA, raw capture |
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
mention programs and build helpers from the userspace project, which is not
part of this repository.

## License

- The kernel is **GPL-2.0** (`kernel/COPYING`). New drivers carry
  `SPDX-License-Identifier: GPL-2.0`.
- The connectivity modules are GPL-2.0 or "Dual BSD/GPL", as marked in
  each source file. The reconstructed platform files keep the GPL-2.0
  header of the MediaTek code they reimplement.
- No vendor firmware, calibration data or NVRAM content is included.
