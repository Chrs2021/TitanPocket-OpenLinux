# Linux 6.12 for the Unihertz Titan Pocket

The **Unihertz Titan Pocket** (MediaTek MT6771 / Helio P70) on Linux
**6.12.118**, to run a regular Linux distribution instead of Android. The
kernel is a patch series against the kernel.org release: upstream drivers
wherever the MT6771's blocks match the MT8183's (they mostly do), and our
own drivers where the hardware is board-specific or MediaTek never
upstreamed it. Drivers that live out of tree (the connectivity chip, the
cameras, NFC, the video codecs) are here too, as source.

It boots Debian 12 (systemd) on the phone's own storage, with a
GPU-accelerated Wayland desktop. The keyboard, touchpad, touchscreen,
display and GPU, sound, Wi-Fi, Bluetooth, FM, GPS, the sensors, both
cameras, NFC, charging, deep sleep and the cellular modem (LTE data, and
VoLTE calls and SMS through an IMS client in userspace) work; the table
below has the details.

This branch, `mainline-6.12`, is where the work is. The MediaTek 4.14 BSP
kernel the phone was first brought up on is still in `kernel/`, for
reference; `main` is that kernel's branch.

## Who this is for

This is source for developers, not an installable system: there are no
images and no installer.

- **Mainline-based distributions** (postmarketOS and similar): this
  branch. The patches, device tree and config fragment give a kernel whose
  drivers speak standard Linux interfaces (DRM, ALSA, V4L2, IIO, input,
  WWAN, cfg80211); the connectivity chip and the sound card also need
  small userspace helpers, in `userspace/` ("Userspace" below).
- **Android and Halium-based ROMs** (LineageOS, Ubuntu Touch and the
  like): MediaTek's vendor HALs expect the 4.14 BSP's interfaces, which
  this kernel doesn't have. The `main` branch is the useful one there: the
  driver sources Unihertz never released (fuel gauge, speaker amplifier,
  keyboard keymap, touchpad, the connectivity platform code) on the 4.14
  BSP, and the docs on how the hardware works.

> **⚠️ Emergency calls are not supported.** Do not rely on a Titan Pocket
> running this kernel to call an emergency number. Calls go over VoLTE
> through an IMS client in userspace, whose emergency calling has never
> been tested on a network.

## Contents

| Path | What |
|---|---|
| `kernel-6.12/` | the port: `patches/` (one per subsystem, for `git am` on v6.12.118), `tools/` (build, boot image, the ramdisk's `/init`) and a [README](kernel-6.12/README.md) with every patch, the command line and the per-driver status |
| `modules/connectivity/` | MediaTek's CONNSYS drivers (WMT core, Wi-Fi, Bluetooth, FM, GPS) built from source; `build612.sh` builds them for 6.12 (`linux612/`: the 6.12 glue) |
| `modules/camera/` | the camera driver (`linux612/`: CAMSV capture, the ISP's statistics and the pass-2 ISP, DIP), its bring-up tools and the ISP register map |
| `modules/nfc/` | the ST21NFC character driver (`linux612/`) |
| `modules/modem/` | build script and a debugging module for the modem driver (the driver itself is in patch 0019) |
| `modules/vdec/`, `modules/venc/` | out-of-tree builds of the H.264 decoder and encoder drivers (their source is in patch 0020) |
| `userspace/` | the userspace the hardware needs: the connectivity chip's launcher, Wi-Fi and Bluetooth bring-up and FM tuner; the sound card's routes, PipeWire sink and headphone switching ([README](userspace/README.md)) |
| `tools/` | test and analysis tools: `spm/` (the SPM firmware), `vdec/`, `venc/`; the firmware extractors `gnss/extract-patch.py` and `re/emu_venc.py` (below) |
| `docs/` | hardware reference, per-subsystem notes and how the vendor drivers were reverse-engineered |
| `kernel/` | the earlier kernel: MediaTek's 4.14.186 BSP with our drivers (see "The 4.14 kernel" below) |

## Hardware support

| Hardware | Driver | State |
|---|---|---|
| CPUs: power-down, frequency scaling, thermal throttling | PSCI cpuidle + `mtk-mcdi`, `mediatek-cpufreq`, `mtk-cci-devfreq`, `mtk-svs` | ✅ |
| Storage (UFS) | `ufs-mediatek` | ✅ with runtime PM |
| Display (720×720 MIPI-DSI) | MediaTek DRM + `panel-synaptics-r63308` | ✅ 60 Hz, through the IOMMU |
| GPU (Mali-G72 MP3) | Panfrost | ✅ OpenGL ES 3.1 with Mesa |
| Keyboard (AW9523 matrix) | `pinctrl-aw9523` + `gpio-matrix-keypad` | ✅ |
| Keyboard touchpad (Synaptics RMI4) | `rmi4_i2c` | ✅ |
| Touchscreen (Goodix GT1151QM) | `goodix` | ✅ |
| Side keys, power key | `mt6779-keypad`, `mt6358-keys` | ✅ |
| Speaker, microphones, earpiece, headphones | MT8183 AFE + MT6358 codec + `fs1860` | ✅ with jack detection |
| Wi-Fi 2.4 + 5 GHz, Bluetooth, FM radio | `modules/connectivity/` | ✅ Bluetooth audio through PipeWire |
| GPS / GNSS (MT6631) | `modules/connectivity/` | ✅ the driver carries the chip's raw data; positions come from a receiver program in userspace (GPS, GLONASS, BeiDou), with no MediaTek library |
| Sensors: accelerometer, gyroscope, magnetometer, light, proximity | `mt6771-scp` (the SCP sensor hub), IIO | ✅ |
| Cameras (OV16880 rear, OV8865 front) | `modules/camera/linux612/` | ✅ both at 30 fps through MediaTek's hardware ISP, auto exposure and white balance, libcamera; the flash is not enabled |
| Video decoder (H.264) | `mt6771-vdec` (patch 0020) | ✅ V4L2 stateless decoder (GStreamer `v4l2slh264dec`), bit-exact up to 1080p High; no firmware |
| Video encoder (H.264) | `mt6771-venc` (patch 0020) | ✅ V4L2 stateful encoder (GStreamer `v4l2h264enc`), up to 1080p, the hardware's rate control; needs rate-control code from your own phone (below) |
| Cellular modem | `drivers/net/wwan/mt6771` (patch 0019) | ✅ LTE under ModemManager, IPv6 data, IPv4 through 464XLAT; VoLTE calls, SMS and USSD with an IMS client in userspace; a call wakes the phone from deep sleep. Emergency calls: not supported |
| NFC (ST21NFC) | `modules/nfc/linux612/` | ✅ `/dev/st21nfc` (NCI), tags read; the controller is off while the device is closed |
| IR transmitter | `mtk-pwm-ir-tx` | ✅ `/dev/lirc0` |
| Charging (MT6370) | `mt6370-charger` | ✅ input limit from the Type-C source and BC1.2, termination, charge LED, temperature limits |
| Fuel gauge (OZ8806) | `oz8806_battery` | ✅ |
| USB device (serial + network) | `mtu3`, `phy-mtk-tphy` | ✅ the Type-C controller decides the role; host mode untried |
| Vibration motor | `regulator-haptic` | ✅ |
| Idle and sleep | `mt6771-spm`, `mtk-mcdi` | ✅ deep sleep through the SPM firmware: 23 mA asleep with the modem off, 28.5 mA with it registered and VoLTE up (one 20-minute run); the SoC idle states (SODI) with the screen on, and the display and GPU powered down with it off. ⚠️ A rare hung wake has a fix being confirmed in daily use |
| Fingerprint (Sunwave, SPI0) | `spidev` | 🚧 the bus is there; no driver yet |

The states above were measured on the phone running 6.12.115, the
previous stable release; the series applies to 6.12.118 unchanged and builds
there. Per-driver notes, and what each patch changes, are in
[kernel-6.12/README.md](kernel-6.12/README.md).

## Getting started

> ⚠️ Flashing replaces the phone's boot partition. Back it up first and
> keep a way back to stock (fastboot or SP Flash Tool). The bootloader
> must be unlocked.

**Kernel.** On a case-sensitive filesystem:

```sh
git clone --branch v6.12.118 --depth 1 \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux-6.12
cd linux-6.12
git am /path/to/kernel-6.12/patches/*.patch
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig titanpocket.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) \
    Image.gz modules mediatek/mt6771-unihertz-titan-pocket.dtb
```

**Out-of-tree drivers.** Each `modules/*/build612.sh` builds against that
kernel build: the connectivity drivers, the camera, NFC, and the video
codecs. They use the kernel's media and networking cores, which
`titanpocket.config` builds as modules; install those with them.

**Boot image.** The phone's LK bootloader boots only an Android v2 boot
image and reads its device tree as an Android DT table.
`kernel-6.12/tools/mkboot.sh` repacks the phone's own boot image with the
new kernel, device tree and a small ramdisk whose `/init`
(`kernel-6.12/tools/ramdisk-init`) starts a USB gadget (serial and
network) and switches to the root filesystem; without one it stays in a
rescue mode with a shell over USB. Flash it from LK's fastboot:

```sh
fastboot flash boot boot-6.12.img
fastboot reboot
```

The command line it adds (deep sleep, the SPM's wake sources and the
rest) is explained in [kernel-6.12/README.md](kernel-6.12/README.md).
For a root filesystem beside Android's partitions, see
[docs/partitioning.md](docs/partitioning.md).

## Firmware and per-phone data

This repository contains no firmware and no data from any phone. Some
hardware needs files that only exist on your own phone:

| Needed for | What | Where it comes from |
|---|---|---|
| Sensor hub, sleep | the SCP's firmware; the SPM firmware | LK loads the SCP's from the `scp` partition before the kernel starts; the SPM's comes with the phone's ATF. Nothing to copy |
| Modem | the modem image | LK loads `md1img` from its partition. Nothing to copy |
| Wi-Fi, Bluetooth | `WMT_SOC.cfg`, `ROMv4_be_patch_1_0_hdr.bin`, `ROMv4_be_patch_1_1_hdr.bin`, `WIFI_RAM_CODE_6759` → `/lib/firmware` | the stock vendor partition |
| FM radio | `mt6631_fm_v1_patch.bin`, `mt6631_fm_v1_coeff.bin`, `fm_cust.cfg` → `/lib/firmware` | the stock vendor partition |
| Speaker amplifier | `fs16xx_01s_mono.preset` → `/lib/firmware` | the stock vendor partition |
| GNSS | the GNSS chip's DSP patch → `/lib/firmware/titan/mt6631-gnss-patch.bin` | copied out of the phone's own `libmnl.so` by `tools/gnss/extract-patch.py` ([docs/gnss-open.md](docs/gnss-open.md)); the library itself is not needed to run |
| Video encoder | the encoder's rate-control code → `/lib/firmware/titan/venc_rc.bin` | `tools/re/emu_venc.py` runs the phone's own `libvcodecdrv.so` in an emulator and writes what it loads (Python with `unicorn`; [docs/video-encoder.md](docs/video-encoder.md)) |
| Wi-Fi MAC, TX power, 5 GHz | factory NVRAM record `APCFG/APRDEB/WIFI` | the phone's `nvdata` partition, unique per unit |
| Bluetooth address, sleep parameters | NVRAM record `APCFG/APRDEB/BT_Addr` | the phone's `nvdata` partition, unique per unit |

These files are not redistributable, and the NVRAM records are specific
to each phone: copy them from your own device, and don't publish images
that contain them.

## Userspace

The drivers expose standard Linux interfaces (DRM, ALSA, V4L2, IIO,
input, WWAN, cfg80211), but some hardware needs programs that replace
Android's vendor services. Those for the connectivity chip and the sound
card are in [`userspace/`](userspace/README.md): a launcher that powers
the chip and serves its patch download, Wi-Fi bring-up with the factory
NVRAM record, a relay that puts Bluetooth on BlueZ, an FM tuner, the
sound card's routes, a PipeWire speaker sink and headphone switching.
Still outside this repository: the modem's file service and bring-up,
the GNSS receiver program and the IMS client for calls and SMS, which
belong to a separate project with the desktop and the apps; the docs
describe what the hardware needs from them, and where they name such a
program, it belongs to that project. Mesa 22.3 or later drives the GPU.

## Known limitations

- **Emergency calls** are not supported (above).
- **Sleep:** 28.5 mA asleep with the modem registered, against about
  12 mA on the 4.14 kernel without it; screen-off use averages more,
  because of wakes (the modem's above all). A rare hung wake has a fix
  being confirmed in daily use.
- **Cameras:** no flash yet; lens shading and noise tuning are open.
- **Video:** H.264 only; no HEVC, interlaced decode or Main/High encode.
- **Fingerprint** sensor: no driver yet.
- USB host mode and USB Power Delivery are untried.

## The 4.14 kernel

`kernel/` is MediaTek's 4.14.186 BSP for the Titan Pocket (from
[ubuntu-touch-unihertz-titan/kernel-alps-mt6771](https://github.com/ubuntu-touch-unihertz-titan/kernel-alps-mt6771),
branch `halium-11.0`), with the drivers Unihertz never released source
for recovered from the stock firmware, Panfrost and a KMS driver
backported, and the connectivity drivers built from source. It was the
first kernel to boot Debian on the phone and is no longer developed here;
its own README and build steps are on the `main` branch, and
[docs/kernel.md](docs/kernel.md) lists its changes. Several drivers of
the 6.12 port (the fuel gauge, the speaker amplifier, the connectivity
platform code) started there.

## Origin of the source

- **Kernel:** Linux 6.12.118 from kernel.org, plus the patches in
  `kernel-6.12/patches/`.
- **Connectivity drivers:** Motorola's published MediaTek connectivity
  sources and MediaTek's open-source Wi-Fi adaptor. Two MT6771-specific
  files MediaTek never published (the chip's platform code and the
  Wi-Fi memory-protection region) were reconstructed from the stock
  modules ([docs/drivers/connectivity.md](docs/drivers/connectivity.md)).
- **Board drivers and register-level knowledge:** recovered from the
  stock Android 11 firmware (kernel image, modules, vendor libraries,
  device tree); method and tools in
  [docs/reverse-engineering.md](docs/reverse-engineering.md).

## Documentation

| Doc | Contents |
|---|---|
| [kernel-6.12/README.md](kernel-6.12/README.md) | the 6.12 port: patches, building, boot image, command line, per-driver status |
| [hardware.md](docs/hardware.md) | SoC, PMICs, device tree layout, I2C device map, GPIOs, speakers, battery |
| [power.md](docs/power.md) | CPU frequency, idle, suspend, sleep current, wake sources, the power key |
| [spm-firmware.md](docs/spm-firmware.md) | what the SPM firmware does in each sleep scenario |
| [modem.md](docs/modem.md) | the modem: CCCI, the AT mux, data, IMS, calls and SMS |
| [audio.md](docs/audio.md) | speaker signal chain, microphones, headphones, the bugs fixed |
| [camera.md](docs/camera.md) | cameras: sensors, MIPI receiver, CAMSV, the hardware ISP |
| [video-encoder.md](docs/video-encoder.md) | the H.264 encoder: registers, the V4L2 driver, results |
| [gnss-open.md](docs/gnss-open.md) | GNSS without MediaTek's library: the chip's protocol and measurements |
| [gnss.md](docs/gnss.md) | GNSS on the 4.14 kernel with MediaTek's library: the `/dev/stpgps` protocol |
| [ir.md](docs/ir.md) | IR blaster |
| [nfc.md](docs/nfc.md) | ST21NFC: driver interface, NCI, tags |
| [partitioning.md](docs/partitioning.md) | UFS layout and adding a Linux root partition |
| [reverse-engineering.md](docs/reverse-engineering.md) | how the vendor drivers were recovered from the stock firmware |
| [debugging.md](docs/debugging.md) | logs, debug interfaces, gotchas |
| [drivers/connectivity.md](docs/drivers/connectivity.md) | CONNSYS Wi-Fi, Bluetooth, FM and GPS from source |
| [drivers/fm.md](docs/drivers/fm.md), [drivers/fs1860.md](docs/drivers/fs1860.md), [drivers/oz8806.md](docs/drivers/oz8806.md) | FM radio, speaker amplifier, fuel gauge |
| [kernel.md](docs/kernel.md), [gpu.md](docs/gpu.md), [drivers/aw9523-keyboard.md](docs/drivers/aw9523-keyboard.md), [drivers/touchpad.md](docs/drivers/touchpad.md) | the 4.14 kernel: its changes, the Panfrost backport, its keyboard and touchpad drivers |

The docs are working notes from bringing up a real device.

## License

- The kernel patches are **GPL-2.0**, as the kernel; new drivers carry
  `SPDX-License-Identifier: GPL-2.0`.
- The connectivity drivers are GPL-2.0 or "Dual BSD/GPL", as marked in
  each file; the reconstructed platform files keep the GPL-2.0 header of
  the MediaTek code they reimplement.
- No vendor firmware, calibration data or NVRAM content is included.
