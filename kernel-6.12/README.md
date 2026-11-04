# Linux 6.12 for the Unihertz Titan Pocket

A port of the Titan Pocket (MediaTek MT6771 / Helio P70) to Linux
**6.12.115**, as a patch series against the kernel.org release. It
replaces the 4.14 BSP kernel in `kernel/`: upstream drivers wherever
the MT6771's blocks match the MT8183's (they mostly do), and our own
drivers where the hardware is board-specific or MediaTek never
upstreamed it. It boots the same Debian 12 system as the 4.14 kernel,
with the Weston desktop.

The work is in progress; see the table below for what works.

## Contents

| Path | What |
|---|---|
| `patches/` | the port, one patch per subsystem, for `git am` on v6.12.115 |
| `tools/build.sh` | configures and builds `Image.gz` and the board's device tree |
| `tools/mkboot.sh` | packs an Android v2 boot image that the phone's LK bootloader accepts |
| `tools/firstlight-init` | `/init` of the boot image's small ramdisk: USB gadget, then Debian |

The CONNSYS drivers (Wi-Fi, Bluetooth, FM, GPS) for 6.12 are out of
tree in `modules/connectivity/linux612/`, built with
`modules/connectivity/build612.sh`; the camera driver is
`modules/camera/linux612/`, built with `modules/camera/build612.sh`. It
runs on the kernel's media core, which `titanpocket.config` builds as
modules (`videodev`, `mc`, `videobuf2-*`): install those with it.

## Patches

| Patch | What |
|---|---|
| 0001 soc-mediatek | MCDI (CPU power-down with the SSPM), SPM (resource requests, suspend through the SPM firmware), SCP sensor hub (IIO), LK framebuffer handover |
| 0002 cpu-power | MT6771 CPU and CCI frequency tables |
| 0003 pmdomain | the CONN power domain stays on through suspend |
| 0004 pmic | MT6358 VA09 regulator; MT6397 RTC reboot modes as LK reads them; MT6358 AUD/MISC interrupts dispatched (XPP bit); accdet cell |
| 0005 ufs | MT6771 UFS host (UniPro 1.6) |
| 0006 usb | forced VBUS session that follows the Type-C role; USB set up again after a deep-sleep reset |
| 0007 watchdog | the watchdog stops in suspend when the bootloader left it running |
| 0008 i2c | the multi-channel I2C controller's mode restored through ATF |
| 0009 keyboard | AW9523 and matrix-keypad locking fixes for I2C GPIO expanders |
| 0010 power-supply | OZ8806 fuel gauge; MT6370 charging policy |
| 0011 display | firmware-framebuffer handover, DSI interrupt order, Synaptics R63308 panel |
| 0012 ir | IR transmitter on the PWM block |
| 0013 audio | machine driver, FS1860 amplifier, AFE hardware gain and FM paths, modem voice links (PCM 2), PCM rate fix, MT6358 jack detection |
| 0014 dts | `mt6771.dtsi` and `mt6771-unihertz-titan-pocket.dts` |
| 0015 config | `titanpocket.config`, a fragment over arm64 `defconfig` |
| 0016 leds-flash | MT6370 flash: strobe timeout written at probe |

## Building

```sh
git clone --branch v6.12.115 --depth 1 \
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
| `titan.root` (`DEBIAN=1`) | `firstlight-init` mounts the Debian root filesystem and switches to it |
| `mem_sleep_default=s2idle` (`DEBIAN=1`) | suspend to idle unless deep sleep is enabled (below) |
| `mt6771_spm.deep=1` (optional, `EXTRA_CMDLINE`) | offers `deep` in `/sys/power/mem_sleep`: suspend through the SPM firmware |

Flash it from LK's fastboot (the bootloader must be unlocked; keep a
copy of the stock boot partition):

```sh
fastboot flash boot boot-6.12.img
fastboot reboot
```

`firstlight-init` starts a USB gadget (serial and network, telnet on
172.16.42.1) and boots Debian from it with `titan.root`; without it, it
logs what the kernel found and returns to fastboot.

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
| GPS | `modules/connectivity/linux612/` | ⚠️ powers on; positioning not tried on 6.12 |
| Sensors (accelerometer, gyroscope, magnetometer, light, proximity) | `mt6771-scp` (SCP sensor hub), IIO | ✅ |
| IR transmitter | `mtk-pwm-ir-tx` | ✅ `/dev/lirc0` |
| Vibration motor | `regulator-haptic` | ✅ |
| Suspend to idle | | ✅ ~120 mA with the screen off |
| Deep sleep (SPM) | `mt6771-spm` | ⚠️ with `mt6771_spm.deep=1`: suspends and resumes with display, GPU, USB, Wi-Fi, Bluetooth and keyboard working; about 31 mA (4.14 reached 12 mA); the SCP sensor hub stays awake unless `mt6771_scp.sleep=1` (untested); what the SPM firmware does: `docs/spm-firmware.md` |
| Cameras (OV16880, OV8865) | `modules/camera/linux612/` (CAMSV, CAM A statistics, the DIP) | ✅ both at 30 fps through the hardware ISP, auto exposure and white balance, libcamera; the flash is not enabled |
| NFC (ST21NFC) | `modules/nfc/linux612/` (`/dev/st21nfc`) | ✅ tags read; the controller and its 26 MHz clock buffer are off while the device is closed |
| Cellular modem | `modules/modem/linux612/` (`titan_modem`) | ✅ registers on LTE under ModemManager, IPv6 data over CLDMA, IPv4 through 464XLAT, IMS registration and first calls with our own client (`userspace/sbin/titan-ims`) (`tools/modem/ltedata.py`); SMS, USSD and emergency calls not done |

## Firmware

No firmware is included. LK loads the SCP's firmware from the phone's
`scp` partition before the kernel starts, and the SPM firmware comes
with the phone's ATF (the kernel starts it through SiP calls); the CONNSYS
firmware (Wi-Fi, Bluetooth patch) comes from the phone's vendor
partition, as for the 4.14 kernel.
