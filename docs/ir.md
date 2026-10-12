# Infrared remote (IR blaster)

Status (2026-10-12): **working**. `titan-ir` (Rust) and the Remote app send
any signal from the Flipper Zero IR database through the phone's IR blaster.
Verified with a Flipper Zero (decodes NEC 0x04/0x08 as sent) and a camera.
Transmit only: there is no IR receiver, so codes can't be learned.

## Hardware and driver

The IR LED is driven by a PWM channel through MediaTek's IRTX driver,
already in the BSP and built: `drivers/misc/mediatek/irtx/mtk_irtx_pwm.c`,
`/dev/irtx`, DT node `irtx@1100d000`, `CONFIG_MTK_IRTX_PWM_SUPPORT=y`. The
stock Android HAL is `consumerir.mt6771.so`.

The PWM runs in "memory mode" and plays a bitstream from a DMA buffer:

| | |
|---|---|
| `ioctl IRTX_IOC_SET_CARRIER_FREQ` | `_IOW('R', 0, u32)` = `0x40045200`, carrier in Hz |
| `ioctl IRTX_IOC_SET_DUTY_CYCLE` | `_IOW('R', 2, u32)` = `0x40045202`, `duty << 16 \| cycle`; we use 1/3 |
| bit duration | `1 / (cycle × carrier)`; with cycle 3, a third of a carrier period (228 clocks of 26 MHz at 38 kHz) |
| mark | per carrier period the bits `1,0,0` (33 % duty) |
| space | zero bits |
| packing | 32-bit little-endian words, **LSB first** (as the stock HAL: shift-left by a running bit index) |
| last word | total duration in µs; the driver sleeps that long, then turns the PWM off |

Found by reading the driver and disassembling the stock HAL's transmit
function (`objdump -d consumerir.mt6771.so`: a `26e6 / (3 × carrier)`
period, an `lsl`/`orr` bit loop wrapping at 32, the total stored in the
final word).

### LED power: the missing `irtx_ldo` (kernel #29)

At first the PWM ran correctly (pin GPIO 90 muxed to `PWM_A`, PWM channel 0
enabled, buffer and timing as intended) but the LED stayed dark. The **IR
LED is powered by the MT6370 sub-PMIC's LDO**. `irtx_probe()` asks for it as
`regulator_get_optional(NULL, "irtx_ldo")`, sets 2.8 V, and
`switch_irtx_gpio()` enables it while transmitting. The BSP's MT6370 LDO
driver registered the LDO only as `mt6370_ldo`, so the lookup failed
(`irtx_ldo regulator not found` at boot) and the LED never got power.

Found by comparing with the stock kernel (`vmlinux.elf`): stock
`irtx_probe`/`switch_irtx_gpio`/`dev_char_write` call `regulator_get_optional`
with the same name, `regulator_set_voltage(2800000)`,
`regulator_enable/disable` and Unihertz's `agold_mt6370_ldo_disable`, and
stock's LDO driver contains the string `irtx_ldo`.

Fix: `drivers/misc/mediatek/pmic/mt6370/mt6370_pmu_ldo.c` adds the consumer
supply `REGULATOR_SUPPLY("irtx_ldo", NULL)` to the LDO's init data. The
panel driver (`R63308_CASCADEMDZ40`) also uses this LDO, as `mt6370_ldo`, at
the same 2.8 V, so the two consumers agree. Check:
`/sys/class/regulator/*/name` = `mt6370_ldo` reads `enabled` during a
transmission.

Phone cameras are unreliable for checking IR (most iPhone rear cameras filter
it); a Flipper Zero in *Infrared → Learn New Remote* shows exactly what was
received.

## The library: Flipper-IRDB

[Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB) (community
database, **CC0**), installed at `/usr/local/share/titan-ir/Flipper-IRDB`:
8,901 remote files, ~302,000 signals (112 MB). File format:

```text
name: Power
type: parsed
protocol: NECext
address: 04 00 00 00      # little-endian bytes
command: 08 00 00 00
#
name: Vol_up
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 9024 4512 579 ...   # mark/space microseconds, starting with a mark
```

## Protocols

Timings and the packing of address/command into bits follow the Flipper
firmware (`lib/infrared/encoder_decoder`, GPLv3; used as a reference only,
our encoder is our own code), so the database values mean the same thing:

| Protocol | Carrier | Frame |
|---|---|---|
| NEC, NECext, NEC42, NEC42ext | 38 kHz | 9000/4500 preamble; bits LSB first, 560 mark + 560/1690 space; NEC adds inverted address/command bytes, NECext sends 16-bit address + 16-bit command as-is |
| Samsung32 | 38 kHz | 4500/4500; address, address, command, ~command; 550 + 550/1650 |
| Kaseikyo | 38 kHz | 3456/1728; 48 bits: vendor id, vendor parity + genre1, genre2 + command, id + command, xor parity; 432 + 432/1296 |
| RCA | 38 kHz | 4000/4000; 4-bit address, 8-bit command, both inverted; 500 + 1000/2000 |
| Pioneer | 40 kHz | 8500/4225; NEC-style 33 bits; sent twice |
| SIRC, SIRC15, SIRC20 | 40 kHz | 2400/600; 7-bit command + 5/8/13-bit address LSB first, 1200/600 mark + 600 space; sent 3× in 45 ms frames |
| RC5, RC5X | 36 kHz | Manchester 889 µs, "1" = space then mark; S1, S2 (0 = RC5X), toggle, 5 address, 6 command bits MSB first |
| RC6 (mode 0) | 36 kHz | leader 2666/889; Manchester 444 µs, "1" = mark then space; start, mode 000, double-length toggle, 8 address, 8 command bits |
| raw | as given | timings exactly as given |

RC5/RC6 toggle bits alternate between presses (state in `/run/titan-ir.toggle`).

## Unmodified timings and long signals

Every code is sent with its timings exactly as encoded or captured. The PWM
buffer size register is 16 bits (words), so one write holds at most 65,536
words = 2,097,152 bits: ~19 s at 36 kHz, ~18 s at 38 kHz, ~12 s at 56 kHz
(larger values would silently wrap). Longer signals are split across
writes:
- a write ends after a mark, and the following space is timed by sleeping
  (exact, costs no buffer space);
- only a single mark longer than a whole buffer is cut, leaving the
  driver's ~1 ms per-write overhead inside it (seen only in 5 corrupt
  captures with ~2^30 µs marks; sent as-is, those drive the LED for about
  18 minutes).

## Streaming and halting

Series of codes (`stream`, `send-all`, `tvbgone`, and every single send) go
through one engine (`src/stream.rs`):
- `/dev/irtx` stays open; consecutive codes with the same carrier are
  concatenated and played in ~0.5 s writes. The carrier is switched between
  writes when it changes.
- Each code keeps its timings; if its last space is shorter than the
  inter-code gap (`-g`, default 50 ms), that trailing silence is extended so
  receivers see separate frames.
- `-p` prints `N/TOTAL label` as each code starts.
- **Halt**: Ctrl-C, SIGTERM or `titan-ir stop` (signals the PID in
  `/run/titan-ir.pid`). Sending stops after the current ~0.5 s write and the
  driver turns the PWM off. Verified: a full TV-B-Gone run stopped 3 s in,
  after code 22 of 102.

## TV-B-Gone

`titan-ir tvbgone [-p] [-g MS] [-n N] [--list]` sends, in order:
1. the curated universal list `Universal_TV_Remotes/UNIVERSAL_POWER_OFF_DEVICES.ir`
   (17 brands);
2. every distinct TV power code (`Power`, `Power_off`, `On/Off`, `Standby`,
   ...) in `TVs/` and `Universal_TV_Remotes/`, deduplicated by encoded
   signal and ordered by how many TV models share it.

Currently 102 codes, ~21 s per pass. The Remote app has a **TV-B-Gone**
button on its home page with progress and Stop.

## `titan-ir`

Source `userspace/titan-ir/` (Rust, no dependencies), built as a static
aarch64 musl binary by `userspace/titan-ir/build.sh` (also fetches the
database into `out/`). Rust and the musl target are installed in the build
container with rustup.

```sh
titan-ir find samsung tv                  # search the library by path
titan-ir list TVs/Samsung/Samsung_AA59-00443A.ir
titan-ir send TVs/Samsung/Samsung_AA59-00443A.ir Power
titan-ir send TVs/LG/LG_TV.ir Vol_up -n 3
titan-ir timings FILE BUTTON              # print the encoded timings
titan-ir proto NECext 0x04 0x08           # any protocol by hand
titan-ir raw 38000 9000 4500 560 560      # raw timings
titan-ir check                            # encode the whole library (3.8 s on the phone)
titan-ir send-all TVs/LG/LG_TV.ir -p      # every button of a remote
titan-ir stream -p codes.txt              # lines: send FILE BUTTON | all FILE |
                                          #        proto P ADDR CMD | raw FREQ T...
titan-ir tvbgone -p                       # universal TV power off
titan-ir stop                             # halt whatever is sending
```

`check` encodes all 301,915 signals unmodified (5 need several writes); the
largest single write is 256 KB. Root is needed for `/dev/irtx`.

## Remote app: `titan-remote`

`/usr/local/bin/titan-remote` (GTK 3, Python; sends through `titan-ir`), in
the dock as **Remote** (icon generated by `userspace/debian/icons/gen-icons.py`).

- **Home**: search all remotes by words in their path, favorites, recent
  remotes, category tiles.
- **Browse**: category → brand → model file.
- **Remote**: the file's buttons as a 3-column grid. Power (red), volume,
  mute and channel come first, then navigation and playback, then the rest
  in file order. A sent button flashes green. ☆/★ toggles a favorite.
- **Volume keys**: while a remote with volume buttons is shown and the
  window has focus, the phone's volume keys send that remote's Vol+/Vol-.
  The app creates `/run/titan-volume-keys.grab`, and `titan-powerkey` leaves
  the phone's volume alone while it exists.
- State: `~/.config/titan-remote/state.json`.

## Not done

- Holding a button to repeat (NEC repeat frames).
