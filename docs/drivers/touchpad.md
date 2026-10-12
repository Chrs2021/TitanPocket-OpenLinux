# Keyboard touchpad

The keyboard's touch surface (used on Android for scrolling and cursor
movement) is a **Synaptics RMI4** controller. We drive it with the mainline
RMI4 stack plus a small board glue driver.

| | |
|---|---|
| Bus / address | i2c6, **0x20** (the stock dtbo's `cap_tkpd@21` node is a red herring) |
| Interrupt | EINT 2 (GPIO 2), level low — from the stock `touch_pad` dtbo node |
| Reset | GPIO 9, active low |
| RMI4 functions | F34 flash (page 0), F01 control (page 0), **F12 2D** (page 0), F54 analog (page 1), F55 sensor tuning (page 3) |
| Glue driver | `drivers/input/misc/titanpocket-touchpad.c` (`CONFIG_INPUT_TITANPOCKET_TOUCHPAD`) |
| Core driver | `drivers/input/rmi4` (`RMI4_CORE`, `RMI4_I2C`, `RMI4_2D_SENSOR`, `RMI4_F11`, `RMI4_F12`) |
| Input device | `Synaptics 63022-001` (product id from F01), `INPUT_PROP_POINTER` |

## How it was found

1. Stock symbols showed `drivers/misc/mediatek/agold/touchpad/` (`mtk_pad.c`,
   a clone of MediaTek's touch-panel framework) on top of
   `synaptics_1403/` (Synaptics DSX driver).
2. `pad_probe` only builds the input device (720x360 multitouch).
3. Stock `synaptics_rmi4_probe` reads `rst-gpio`/`int-gpio` from DT (absent in
   the dtbo; only printed), then does
   `agold_gpio_set(9, 1); msleep(200); agold_gpio_set(9, 0); msleep(20);
   agold_gpio_set(9, 1); msleep(200);` and maps the IRQ from the
   `mediatek,touch_pad` node.
4. On our kernel GPIO 9 was an output driven low. Releasing it by hand
   (`devmem` on the GPIO data register) made a device appear at 0x20
   (`i2cdetect -y -r 6`), and reading the RMI4 Page Description Table
   (descending from 0xE9 on each page) gave the function list above.

## What the glue driver does

Binds `compatible = "mediatek,touch_pad"`, maps its interrupt, replays the
stock reset pulse, and instantiates `rmi4_i2c` at 6-0020 with
`rmi_device_platform_data.sensor_pdata.sensor_type = rmi_sensor_touchpad`, so
the input device is reported as an indirect pointer (touchpad) rather than a
touchscreen. Probe is asynchronous because the reset pulse sleeps ~420 ms.

## Verified behaviour

Swiping for 15 s gave ~310 interrupts and 2,243 input events across 8
touches: multitouch slots with tracking IDs, `ABS_MT_POSITION_X/Y` covering
101–683 / 120–360 (stock reported the surface as 720x360), pressure,
touch major/minor, plus `BTN_TOUCH` and `BTN_TOOL_FINGER`. Recording:
`logs/touchpad_events.bin` (raw `struct input_event`, 24 bytes each).

## F55 must stay disabled

With `CONFIG_RMI4_F55=y` the probe fails: right after the F55 handler probes
(page 3), the PDT read of the next entry (0x03E3) is NAKed (`-121`) and the
whole RMI4 device is torn down. Byte reads of the same address work later, so
it is transient after F55's own register access. F55 (sensor electrode
mapping) is not needed for reporting touches.

## Manual bring-up / debugging

```sh
# release reset by hand (bit 9 of GPIO data register 0)
v=$(devmem 0x10005100 32); devmem 0x10005100 32 $(( v | 0x200 ))
i2cdetect -y -r 6                   # 0x20 should appear
i2cset -f -y 6 0x20 0xff 0          # select RMI4 page 0
i2cget -f -y 6 0x20 0xee            # 0x34 = first PDT entry is F34
grep -i rmi /proc/interrupts; cat /proc/bus/input/devices
```

## Not done yet

- Gestures/modes the stock driver had (`pad_switch_*_mode`, wake gesture,
  calibration via `agold_touchpad_enable_cali`).
- Suspend/resume coordination with the keyboard (stock resumes the pad from
  the AW9523 key handler when `touchpad_flag` is set).
- F54 (needs V4L2), F34 firmware update and F55 are not enabled.
- Pointer feel under a desktop (speed, palm handling) still needs tuning.

## Desktop use (libinput)

libinput sees a 65×28 mm internal touchpad: tap-to-click, two-finger and
edge scrolling, adaptive acceleration. Because the surface is the keyboard
itself, **disable-while-typing** matters: brushing the keys while typing
would otherwise move the pointer. DWT needs the keyboard to be tagged as an
internal keyboard (udev rule + libinput quirk, see
[aw9523-keyboard.md](aw9523-keyboard.md)). `libinput debug-events --verbose`
then logs `dwt activated with Synaptics 63022-001<->aw9523-key`.

**Pressure quirk**: a normal finger reports pressure ~190/255 (recorded:
10th percentile 90, median 190). libinput treats touchpad touches above 130
as palms by default (for any device with `ABS_MT_PRESSURE`), so it ignored
almost every swipe. `/etc/libinput/local-overrides.quirks` sets
`AttrPalmPressureThreshold=0` (pressure-based palm detection off) and
`AttrPressureRange=40:25` for `Synaptics 63022-001`. Check it with
`libinput quirks list /dev/input/event3`; compositors read it when the
device is added (restart Weston). The remaining question is a design
one: pointer, or scroll-only as on Android. libinput can't do scroll-only by
itself; it would take a small daemon or compositor support.
