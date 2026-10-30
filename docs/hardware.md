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
| i2c1 | 0x51 | `mediatek,alsps` | ambient light / proximity: nothing answers here; the sensor hub drives them ([below](#light-and-proximity-sensors)) | none |
| i2c1 | 0x77 | `mediatek,barometer` | **not fitted**: nothing answers at 0x77 or 0x76 (2026-10-29), the stock kernel builds no barometer driver, the hub has no barometer | none |
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

## Vibration motor

- **Supply:** the MT6358 PMIC's VIBR LDO (`RG_LDO_VIBR_EN`). Its voltage
  is `RG_VIBR_VOSEL` = the device tree's `vib_vol` (11).
- **Driver:** the BSP's (`drivers/misc/mediatek/vibrator/`, platform
  `mt6771`) works as is. It is exposed through the LED class at
  `/sys/class/leds/vibrator`:
  - write a time in ms to `duration`, then 1 to `activate`; a kernel
    timer ends the pulse;
  - `activate` reads 1 while it runs (`state` is just a stored value);
  - pulses shorter than `vib_timer` (25 ms) are stretched to 25 ms, and
    pulses are capped at 15 s;
  - an over-current interrupt (`INT_VIBR_OC`) stops it.
- **`titan-vibrate`** (Rust, `userspace/titan-vibrate/`, static musl):
  - with no argument, a 25 ms tick;
  - `MS` for one pulse;
  - `ON,OFF,ON,…` for a pattern in ms;
  - the names `tick`, `click`, `double`, `heartbeat`, `notify`, `long`;
  - `stop`.
  
  Patterns are timed in userspace and the command returns when they end
  (`double` takes 246 ms, `notify` 457 ms).

## Motion sensors

The accelerometer, gyroscope and magnetometer (i2c1 0x0c) are read by the
SCP's sensor hub; the kernel's MediaTek `sensors-1.0` framework
(`accelhub`, `gyrohub`, `maghub`) passes the readings on. For each, with
`<s>` = `acc`, `gyro` or `mag`:

- `/dev/m_<s>_misc`: packed `struct sensor_event` records (int64 timestamp,
  handle, flush_action, status, reserved, int32 word[6]); x, y, z in
  word[0..2], `flush_action` 0 for data.
- `/sys/class/sensor/m_<s>_misc/<s>batch`: `handle,flag,period_ns,latency_ns`.
- `/sys/class/sensor/m_<s>_misc/<s>active`: write 1/0. **Reading it gives
  the divisor to SI units:** acc 1000 (m/s²), gyro 7505747 (rad/s), mag 100
  (µT).

Axes are Android's: x right, y to the top of the screen, z out of it. The
accelerometer reads +9.8 on the axis pointing up. Measured rates: acc ~60 Hz
at a 50 Hz request, gyro ~93 Hz at 100 Hz, mag 50 Hz.

**Calibration (2026-10-18):** on Linux both the gyro and the magnetometer
report status 0 (uncalibrated); Android's HAL loaded the factory
calibration from NVRAM.
- The gyro's first ~0.5 s after enabling swing by up to 1 rad/s. In one test
  it then sat at (−4.4, −7.8, −0.8) °/s at rest; later the offset was near
  zero, so the hub seems to correct it after a while.
- The magnetometer read 96 µT at first (a hard-iron offset), later a
  plausible 40 µT, still with status 0. The hub seems to start applying its
  own calibration during a session without saying so.

The magnetometer's axes match the accelerometer's: turned flat in the
hand, its heading tracked the gyro's yaw in the right direction. Avoid the
fusion device (`/dev/m_fusion_misc`, sensor IDs such as 13 for the
uncalibrated magnetometer and 2 for orientation): enabling those left the
SCP's IPI busy (`scp_ipi_send: ... mutex busy`) and the writer blocked in
`nanohub_ipi_read` until it was killed.

`titan_sensors.py` (`/usr/local/lib/python3.11/dist-packages`) wraps all of
this for our apps: `Sensor` (start, read in SI units, stop; `keep_alive()`
turns a sensor back on when another program, e.g. the lock screen, switched
it off), `GyroBias` (learns the offset whenever the phone lies still) and
`MagCal` (a least-squares sphere fit of readings from many directions; kept
only when tight, and used only when it explains the field better than the
raw readings). Both are saved in `~/.local/share/titan-sensors/`.
**Maze** and **Compass** use it (desktop.md).

## Light and proximity sensors

Both are read by the SCP's sensor hub (`alspshub`, MediaTek's alsps
framework), like the motion sensors: `/dev/m_als_misc` and
`/dev/m_ps_misc` carry the same `struct sensor_event` records, with
`/sys/class/sensor/m_als_misc/als{batch,active}` and `m_ps_misc/ps{...}`.
The device tree's `alsps` entry at i2c1 0x51 doesn't answer: the chip is
on the hub's own bus. Two differences from the motion sensors (2026-10-29):

- The batch line needs the real handle: `ID_LIGHT` 4, `ID_PROXIMITY` 7
  (`"4,0,200000000,0"`); with 0 nothing reports.
- `alsactive` takes `handle,enable` (`"4,1"`), `psactive` a bare `1`/`0`.
  A bare `1` to `alsactive` is accepted and does nothing.

Both report only on change. **Light** is word[0] in lux: about 440 in a
room in daylight, 11-58 in a hand's shadow, 0-8 covered by a finger, and
flat at 20 000 under a lamp (saturated). The divisor `alsactive` reads back
(100) is never set by the hub driver and doesn't apply. Right after
enabling, the framework sends value + 1 once to force a first update.
**Proximity** is word[0] = state + 1: 1 near, 2 far (so 0/1 after the
framework's +1); every pass of a hand ~2-3 cm over the top edge gave a
near/far pair. `titan_sensors.py` handles all of this: `Sensor("als")`
gives lux, `Sensor("ps")` 0 near / 1 far.

## microSD card

`/dev/mmcblk0` (28.8 GB, one FAT32 partition, label `TITAN`; formatted by
Android as portable storage, its empty Android folders kept). It holds
**Pictures**: `titan-usb-storage.service` mounts it at `/media/sdcard` and
binds its `Pictures` folder on `~/Pictures`, and lends it to a computer as a
USB drive on request (desktop.md).

