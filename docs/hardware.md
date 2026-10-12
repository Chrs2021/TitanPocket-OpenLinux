# Hardware reference

Facts about the Unihertz Titan Pocket that matter for kernel work. Everything
here was read from the device or its stock firmware (kernel binary, dtb, dtbo,
vendor partition), not from datasheets, unless marked otherwise.

## Platform

| | |
|---|---|
| SoC | MediaTek MT6771V/CT (Helio P70), 4x Cortex-A73 + 4x Cortex-A53 |
| GPU | Mali-G72 MP3 |
| PMIC | MT6358 (main), MT6370 sub-PMIC (charger, flash LED, backlight bias, Type-C) |
| Storage | UFS (`H9HQ15AECMBDAR`, 128 GB; LU2 `/dev/sdc`, 4 KiB sectors) — see [partitioning.md](partitioning.md) |
| Stock kernel | 4.14.186, clang 11, built 2022-01-13 (`4.14.186-g9600a6ddd-dirty`) |
| Stock Android | 11, security patch 2021-12 |
| Bootloader | MediaTek LK, unlocked (`seccfg` lock_state = 3) |
| Boot image | header v2, separate dtb in the header, `Image.gz` kernel |
| ODM | Agold (board names "G66"/"A66" appear throughout the vendor code) |

## Device tree

The board devices are **not** in the base dtb. The bootloader applies the
**dtbo** partition (`Firmware/dtbo-verified.img`) on top of the base dtb, and
the dtbo carries all I2C peripherals, pin states and audio pin muxing. Our
boot images keep the stock base dtb and the stock dtbo partition is never
touched, so drivers bind against the stock nodes and compatibles.

Decompiled copies: `stock_dt/dtbo0.dts` (overlay), `extracted_boot/device_tree.dts` (base).

## I2C devices

From the stock dtbo. "Driver" is the driver that binds in our kernel.

| Bus | Addr | Compatible | Device | Driver |
|---|---|---|---|---|
| i2c0 | 0x5d | `mediatek,cap_touch` | Goodix GT1151QM touchscreen | `drivers/input/touchscreen/mediatek/GT1151` |
| i2c1 | 0x0c | `mediatek,msensor` | magnetometer | BSP |
| i2c1 | 0x51 | `mediatek,alsps` | ambient light / proximity | BSP |
| i2c1 | 0x77 | `mediatek,barometer` | barometer | BSP |
| i2c2 | 0x1a/0x72/0x50 | `mediatek,camera_main*` | OV16880 rear camera, AF, EEPROM | none (see roadmap) |
| i2c2 | 0x28 | `mediatek,camera_sub_two` | | none |
| i2c3 | 0x08 | `mediatek,nfc` | ST21NFC | BSP |
| i2c3 | 0x63 | `mediatek,flashlights_lm3643_i2c` | flash LED driver | BSP |
| i2c3 | **0x34** | `mediatek,speaker_amp` | **FourSemi FS1860 smart amp** | `sound/soc/codecs/fs1860.c` (new) |
| i2c4 | **0x58** | `awinic,aw9523-key` | **AW9523 keyboard matrix** | `drivers/misc/mediatek/aw9523` |
| i2c4 | 0x10/0x35/... | `mediatek,camera_*` | OV8865 front camera etc. | none |
| i2c5 | 0x4e | `mediatek,usb_type_c` | Type-C controller | BSP |
| i2c5 | 0x34 | `mediatek,subpmic_pmu` | MT6370 | BSP |
| i2c6 | **0x2f** | `mediatek,coulomb` | **O2Micro OZ8806 fuel gauge** | `drivers/power/supply/oz8806_battery.c` (new) |
| i2c6 | 0x4b | `mediatek,slave_charger` / `richtek,rt9465` | secondary charger | BSP |
| i2c6 | 0x21 | `mediatek,cap_tkpd` | stock placeholder node; nothing answers here | |
| i2c6 | **0x20** | (not in dtbo) | **Synaptics RMI4 keyboard touchpad** (F01/F12/F34/F54/F55) | `rmi4_i2c` via `drivers/input/misc/titanpocket-touchpad.c` (new) |
| i2c6 | 0x61 | `mediatek,idt` | wireless charging (unused) | |
| i2c6 | 0x50 | `mediatek,remote_ctrl` | IR blaster | |

Device names are as they appear under `/sys/bus/i2c/devices` (e.g. `3-0034`,
renamed to `fs1860` by the amp driver).

## GPIO notes

- GPIO numbers below are SoC pin numbers; Linux gpiolib numbering adds the
  pinctrl base (320 on this kernel; see `/sys/devices/platform/1000b000.pinctrl/mt_gpio`).
- **GPIO 5** is a board strap: high selects a 24 kOhm NTC pull-up for the
  battery thermistor (this unit), low selects 120 kOhm. Read via
  `agold_gpio_get_in(5)`.
- Keyboard: AW9523 interrupt on EINT 0, reset GPIO from the dtbo
  (`awinic,reset-gpio`).
- Touchpad: interrupt on EINT 2 / GPIO 2, level low (`touch_pad` node).
  **Reset on GPIO 9** (active low; hard-coded in the stock driver). Our
  kernel boots with GPIO 9 driven low, so the controller is invisible on the
  bus until reset is released.

### Speaker amp I2S pins

From the dtbo pin state `aud_pins_smartpa_on`, resolved against
`drivers/pinctrl/mediatek/pinctrl-mtk-mt6771.h`:

| Pin | Mode 2 function | Use |
|---|---|---|
| GPIO170 | I2S3_BCK | bit clock to FS1860 |
| GPIO171 | I2S3_LRCK | frame clock to FS1860 |
| GPIO172 | I2S3_DO | audio data to FS1860 |
| GPIO174 | I2S0_DI | feedback/echo data from FS1860 |

These are switched to mode 2 by the `Audio_i2s0_SideGen_Switch` mixer control
(via `AudDrv_GPIO_SMARTPA_Select`).

## Audio outputs

There are two transducers:

- **Loudspeaker** (bottom): FS1860 smart amp on I2S3, controlled over i2c3/0x34.
- **Earpiece** (top): driven by the MT6358 internal codec (`Voice_Amp_Switch`,
  `RCV_PATH_INT=yes` in the vendor audio config).

Both can be fed from the same AFE memory interface (DL1), so during bring-up
check which end sound comes from. See [audio.md](audio.md).

## Battery

- 4250 mAh design capacity, 4.40 V charge voltage, 3.45 V cut-off
  (stock O2Micro config struct).
- Sense resistor 10 mOhm.
- OZ8806 keeps its coulomb counter powered from the battery, so remaining
  capacity survives reboots and kernel changes.
