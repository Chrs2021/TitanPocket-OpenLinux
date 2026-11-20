# Linux 6.12 for the Unihertz Titan Pocket

A port of the Titan Pocket (MediaTek MT6771 / Helio P70) to Linux
**6.12.118**, as a patch series against the kernel.org release. It
replaces the 4.14 BSP kernel in `kernel/`: upstream drivers wherever
the MT6771's blocks match the MT8183's (they mostly do), and our own
drivers where the hardware is board-specific or MediaTek never
upstreamed it. It boots the same Debian 12 system as the 4.14 kernel,
with its Wayland desktop.

The work is in progress; see the table below for what works.

## Contents

| Path | What |
|---|---|
| `patches/` | the port, one patch per subsystem, for `git am` on v6.12.118 |
| `tools/build.sh` | configures and builds `Image.gz` and the board's device tree |
| `tools/mkboot.sh` | packs an Android v2 boot image that the phone's LK bootloader accepts |
| `tools/ramdisk-init` | `/init` of the boot image's small ramdisk: USB gadget, then Debian |

The CONNSYS drivers (Wi-Fi, Bluetooth, FM, GPS) for 6.12 are out of
tree in `modules/connectivity/linux612/`, built with
`modules/connectivity/build612.sh`; the camera driver is
`modules/camera/linux612/`, built with `modules/camera/build612.sh`. It
runs on the kernel's media core, which `titanpocket.config` builds as
modules (`videodev`, `mc`, `videobuf2-*`): install those with it.

## Patches

| Patch | What |
|---|---|
| 0001 soc-mediatek | MCDI (CPU power-down with the SSPM), SPM (resource requests, suspend through the SPM firmware, the PLLs' sleep control, unused PLLs off), SCP sensor hub (IIO), LK framebuffer handover |
| 0002 cpu-power | MT6771 CPU and CCI frequency tables, SVS |
| 0003 iommu | MT6771 data with the VPU's fixed IOVA window |
| 0004 pmdomain | the CONN power domain stays on through suspend |
| 0005 pmic | MT6358 VA09 regulator; MT6397 RTC reboot modes as LK reads them; MT6358 AUD/MISC interrupts dispatched (XPP bit); accdet cell |
| 0006 ufs | MT6771 UFS host (UniPro 1.6) |
| 0007 usb | forced VBUS session that follows the Type-C role; USB set up again after a deep-sleep reset |
| 0008 watchdog | the watchdog stops in suspend when the bootloader left it running |
| 0009 i2c | the multi-channel I2C controller's mode restored through ATF |
| 0010 keyboard | AW9523 and matrix-keypad locking fixes for I2C GPIO expanders |
| 0011 power-supply | OZ8806 fuel gauge; MT6370 charging policy |
| 0012 display | firmware-framebuffer handover, DSI interrupt order, Synaptics R63308 panel |
| 0013 ir | IR transmitter on the PWM block |
| 0014 audio | machine driver, FS1860 amplifier, AFE hardware gain and FM paths, modem voice links (PCM 2), PCM rate fix, MT6358 jack detection |
| 0015 dts | `mt6771.dtsi` and `mt6771-unihertz-titan-pocket.dts` |
| 0016 config | `titanpocket.config`, a fragment over arm64 `defconfig` |
| 0017 leds-flash | MT6370 flash: strobe timeout written at probe |
| 0018 pmdomain-md1 | the MT6771 power controller with the modem's domain (MD1) |
| 0019 modem | the MT6771 modem: CCCI over CCIF, the AT mux as WWAN ports, network data over CLDMA (`ccmniN`), exception handshake and devcoredump |
| 0020 vdec | the MT6771 H.264 decoder (V4L2 stateless) and encoder (V4L2 stateful) |
| 0021 panfrost | the GPU clock unprepared for system suspend, so its PLL stops |
| 0022 pmic-keys | the MT6358's reset register with the MT6331's layout |
| 0023 spidev | spidev for the fingerprint sensor on SPI0 |

## Building

```sh
git clone --branch v6.12.118 --depth 1 \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux-6.12
cd linux-6.12
git am /path/to/kernel-6.12/patches/*.patch
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig titanpocket.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) \
    Image.gz mediatek/mt6771-unihertz-titan-pocket.dtb
```

`tools/build.sh` does the same inside our build container and reports
config options that Kconfig dropped; `tools/build.sh modules` also builds
and stages the kernel's modules, which the out-of-tree drivers link
against.

## Boot image

LK boots only an Android v2 boot image, and reads the header's device
tree as an Android DT table. `tools/mkboot.sh` repacks the phone's own
boot image with `magiskboot` (so the header and stock command line stay
as LK expects), replacing the kernel, the device tree (wrapped in a DT
table) and the ramdisk. The tools assume our workspace's layout: the
stock boot image, `magiskboot`, a static busybox and `udhcpd.conf`;
adjust the paths at the top of the script.

The command line gains:

| Option | Why |
|---|---|
| `clk_ignore_unused pd_ignore_unused` | some clocks and power domains LK leaves on have no consumer in the device tree yet |
| `titan.root` (`DEBIAN=1`) | `ramdisk-init` mounts the Debian root filesystem and switches to it |
| `mem_sleep_default=deep` (`DEBIAN=1`) | the power key's suspend is deep sleep through the SPM firmware; `SLEEP=s2idle` gives `mem_sleep_default=s2idle` instead |
| `mt6771_spm.deep=1` (`DEBIAN=1` with deep) | offers `deep` in `/sys/power/mem_sleep`: starts the SPM firmware and suspends through it |
| `mt6771_spm.wake_mask=0xe87d5ef` (`DEBIAN=1` with deep) | the SPM's default wake sources less the sensor hub's (bit 11): the SCP's request would end every suspend at once; the modem's interrupts stay wake sources |
| `mt6771_scp.sleep=1` (`DEBIAN=1` with deep) | leaves the SCP's own sleep on (the vendor switches it off at boot); the sensors still work |
| `mt6771_spm.pcm_flags1=0x300` (`DEBIAN=1` with deep) | `BIG_BUCK_OFF_ENABLE` and `_ON`: the SPM cuts the A73 cluster's buck (`vproc11`) in sleep |

Flash it from LK's fastboot (the bootloader must be unlocked; keep a
copy of the stock boot partition):

```sh
fastboot flash boot boot-6.12.img
fastboot reboot
```

`ramdisk-init` starts a USB gadget (serial and network) and boots Debian
with `titan.root`. Without it, or when the root cannot be mounted, it
stays in a rescue mode: a short report of what the kernel found, telnet
on 172.16.42.1 and a shell on the serial port, and back to fastboot after
90 s when no host configures the gadget.

## Hardware support

| Hardware | Driver | State |
|---|---|---|
| CPUs (power-down, frequency scaling, thermal throttling) | PSCI cpuidle + `mtk-mcdi`, `mediatek-cpufreq`, `mtk-cci-devfreq` | ✅ |
| UFS (root filesystem) | `ufs-mediatek` | ✅ with runtime PM |
| USB device | `mtu3`, `phy-mtk-tphy` | ✅ the Type-C port controller (MT6370, TCPM) decides the role; host mode untried |
| Charging (MT6370) | `mt6370-charger` | ✅ input limit from the Type-C source and BC1.2, termination, charge LED, stops outside 0-59 °C |
| Fuel gauge (OZ8806) | `oz8806_battery` | ✅ |
| Display (720×720 MIPI-DSI) | MediaTek DRM + `panel-synaptics-r63308` | ✅ |
| GPU (Mali-G72 MP3) | Panfrost | ✅ |
| Keyboard (AW9523 matrix) | `pinctrl-aw9523` + `gpio-matrix-keypad` | ✅ |
| Touchpad (RMI4) | `rmi4_i2c` | ✅ |
| Touchscreen (GT1151QM) | `goodix` | ✅ |
| Side keys, power key | `mt6779-keypad`, `mt6358-keys` | ✅ |
| Audio (speaker, microphones, headphones) | MT8183 AFE + MT6358 + `fs1860` | ✅ |
| Wi-Fi, Bluetooth, FM | `modules/connectivity/linux612/` | ✅ Bluetooth SCO through `mtk-btcvsd` (no call tried) |
| GPS | `modules/connectivity/linux612/` | ✅ the chip's raw data on `/dev/stpgps`; positions from a receiver program in userspace, no MediaTek library ([docs/gnss-open.md](../docs/gnss-open.md)) |
| Sensors (accelerometer, gyroscope, magnetometer, light, proximity) | `mt6771-scp` (SCP sensor hub), IIO | ✅ |
| IR transmitter | `mtk-pwm-ir-tx` | ✅ `/dev/lirc0` |
| Vibration motor | `regulator-haptic` | ✅ |
| Suspend to idle | | ✅ ~120 mA with the screen off |
| Deep sleep (SPM) | `mt6771-spm` | ✅ with `mt6771_spm.deep=1`, the Debian default: suspends and resumes with display, GPU, USB, Wi-Fi, Bluetooth and keyboard working; 23 mA asleep with the modem stopped, 28.5 mA with it registered and VoLTE up (#151, one 20-minute run; 37.5 mA on #136), with the SCP asleep and the A73 buck cut. ⚠️ The occasional reset was a hung wake (CPU 0 not restarting after a sleep of a minute or more); with the CPU PLLs under the hardware's sleep control across each sleep (`mt6771_spm.pll_sleep=2`, the default) 0 resets in 100 long sleeps against 1 in 45 without it, being confirmed in daily use. Unused PLLs (TVDPLL, the audio PLLs, the GPU's in system suspend) are off; what the SPM firmware does: `docs/spm-firmware.md` |
| Screen-on idle (SODI) | `mt6771-spm`, `mtk-mcdi` | ✅ the SPM firmware's SODI from the last core standing, with the screen on (`mt6771_spm.sodi=1`, the Debian default); with the screen off the display and GPU are powered down |
| Cameras (OV16880, OV8865) | `modules/camera/linux612/` (CAMSV, CAM A statistics, the DIP) | ✅ both at 30 fps through the hardware ISP, auto exposure and white balance, libcamera; the flash is not enabled |
| Video decoder (H.264) | `mt6771-vdec` (patch 0020) | ✅ V4L2 stateless decoder (GStreamer `v4l2slh264dec`), bit-exact up to 1080p High; no firmware |
| Video encoder (H.264) | `mt6771-venc` (patch 0020) | ✅ V4L2 stateful encoder (GStreamer `v4l2h264enc`) up to 1080p, the hardware's rate control; its rate-control code comes from the phone's vendor library ([docs/video-encoder.md](../docs/video-encoder.md)) |
| NFC (ST21NFC) | `modules/nfc/linux612/` (`/dev/st21nfc`) | ✅ tags read; the controller and its 26 MHz clock buffer are off while the device is closed |
| Cellular modem | `drivers/net/wwan/mt6771` (`titan_modem`, patch 0019) | ✅ registers on LTE under ModemManager, IPv6 data over CLDMA, IPv4 through 464XLAT; VoLTE calls, SMS and USSD with an IMS client in userspace; a call wakes the phone from deep sleep. Emergency calls are not supported |

## Firmware

No firmware is included. LK loads the SCP's firmware from the phone's
`scp` partition before the kernel starts, and the SPM firmware comes
with the phone's ATF (the kernel starts it through SiP calls); the CONNSYS
firmware (Wi-Fi, Bluetooth patch) comes from the phone's vendor
partition, as for the 4.14 kernel. The video encoder's rate-control code
(`titan/venc_rc.bin`) is extracted from the phone's vendor encoder
library, and the GNSS chip's DSP patch from its `libmnl.so`; neither
is distributed.
