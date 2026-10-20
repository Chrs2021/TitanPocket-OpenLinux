# FM radio (MT6631 via CONNSYS)

Status: **working** (2026-10-11). Tuning, station scan and sound on the
loudspeaker. The test channel is 99.5 MHz.

## Pieces

| Piece | Where | Origin |
|---|---|---|
| `fmradio_drv.ko`: `/dev/fm`, MT6631 RX over WMT/STP | `modules/connectivity/fm/` (`CFG_FM_CHIP=mt6631`, `plat/legacy_wmt.c`) | Motorola's `vendor-mediatek-kernel_modules-connectivity-fmradio` (`android-11-release-rrq`) |
| Firmware: `mt6631_fm_v1_patch.bin`, `mt6631_fm_v1_coeff.bin`, `fm_cust.cfg` | `/lib/firmware` | vendor partition (not redistributable) |
| `titan-fm`: tuner CLI | `userspace/src/titan-fm.c` | new |
| `titan-radio`: audio routing + `titan-fm` | `userspace/debian/overlay/usr/local/bin/` | new |
| `titan-radio-gui`: the **Radio** app (GTK 3) | `userspace/debian/overlay/usr/local/bin/` | new |

The driver needs the WMT core (`titan-wmt.service`) to be running. Its
module depends on `wmt_drv`, so `modprobe fmradio_drv` works at any time.
The first `open()` of `/dev/fm` powers the FM function on through WMT. The
**last close powers it off**, so the tuner has to keep running while you
listen.

## Using it

**Radio** in the app drawer opens `titan-radio-gui`. It starts playing the
last station and stops when the window closes.
- Big frequency readout; a dial (87.5–108); seek ⏮ ⏭, step ‹ › (100 kHz)
  and power buttons; a volume slider (the AFE FM gain, −48…0 dB, default
  −24 dB; the speaker level stays with the volume keys).
- Signal meter (GETRSSI, about dBm) and a stereo/mono badge.
- **RDS:** station name, call letters and programme type under the
  frequency, radio text below them; station names are remembered and
  shown in the station list (see "RDS" below).
- **Scan** lists the stations it finds (about 8 s for the band); the star
  saves a favorite, listed first. Tap a row to tune.
- Keys: ←/→ step, ↑/↓ seek, Space power, F favorite.
- State (frequency, volume, favorites, last scan):
  `~/.config/titan-radio/state.json`.

It drives `/dev/fm` itself (the ioctls below, from Python) and sets up the
audio the same way as `titan-radio`. `spk-hold` is started with
`PR_SET_PDEATHSIG`, so the FM route closes even if the app is killed. Only
the process that powered FM up may use the ioctls (others get `EPERM`), so
`titan-fm` and the app can't share the tuner.

From a shell:

```sh
titan-radio            # 99.5 MHz on the speaker; type a frequency, u/d (±100 kHz),
                       # su/sd (seek), q (quit; routing is undone)
titan-radio 101.1 65536   # frequency, AFE gain (0x80000 = 0 dB; default 32768 = -24 dB)
titan-fm scan          # list stations, no audio
```

`titan-fm` prints the chip's raw RSSI as "signal": higher is stronger, and a
channel counts as a station at -296 or above (`FM_RX_RSSI_TH_LONG`).
`GETRSSI` returns the same register converted as raw × 6 / 16
(`mt6631_fm_lib.c`), which MediaTek's app shows as dBm: the threshold is
about −111 there. Indoors without a headset the stations here measure
−100 to −110.

## Interface (`fm/inc/fm_ioctl.h`, magic `0xf5`)

The ioctl numbers encode `sizeof(pointer)`, because the header declares them
with pointer types. Frequencies are in 10 kHz units (`9950` = 99.5 MHz).

| ioctl | Use |
|---|---|
| `POWERUP` (0) `fm_tune_parm` | power on + tune; band 1 (87.5–108), space 1 (100 kHz) |
| `TUNE` (2) | retune |
| `SOFT_MUTE_TUNE` (63) `fm_softmute_tune_t` | measure a channel without unmuting → rssi, valid; used for scan and seek |
| `PRE_SEARCH` / `RESTORE_SEARCH` (45/46) | mute around a scan |
| `GETRSSI` (7), `GETMONOSTERO` (13), `GETCHIPID` (10) | status |

## RDS

The driver decodes RDS itself (`core/fm_rds_parser.c`): `FM_IOCTL_RDS_ONOFF`
(18, a `uint16_t` 1/0) turns it on, and `read()` on `/dev/fm` returns the
whole `struct rds_t` (`inc/fm_rds.h`, **688 bytes** on arm64) when something
changed since the last read, or 0 bytes: it never blocks, so the app polls
it once a second. `event_status` (offset 554) says what changed and is
cleared by the read; the driver clears its RDS state on every tune.

| field | offset | use |
|---|---|---|
| `PI` | 24 | programme identification; for North American stations (NRSC-4) the call letters: 0x1000–0x54A7 = K + 3 letters, 0x54A8–0x994F = W + 3 letters (base 26 from AAA) |
| `PTY` | 27 | programme type, 0–31: RBDS names in the Americas (5 Rock, 9 Top 40, 10 Country …), RDS names elsewhere (the app picks by time zone) |
| `PS_Data.PS[3]` | 272 | station name, 8 characters: the copy the parser confirmed by receiving it twice the same (`PS[0..2]` are work buffers; `PS_ON` is another network's name) |
| `RT_Data.TextData[3]` | 482 | radio text, up to 64 characters, length `RT_Data.TextLength` (548), set when a text completed (`RDS_EVENT_LAST_RADIOTEXT`) |

Offsets came from compiling the header's structs for arm64
(`out/fm/rds_layout.c`). First reception (2026-10-15, headphones in): 91.5
MHz → "WUML-FM", PI = WUML, PTY Rock, stereo, −65 dBm.

## Audio path

```
MT6631 (I2S master, 32 kHz; enabled by the driver at power-up)
  -> CONNSYS I2S into the AFE ("I2S_CONNSYS", ASRC 32 -> 48 kHz)
  -> HW_GAIN1 ("Audio FM I2S Volume")
  -> I2S3 -> FS1860 speaker amp       (and I2S1 -> MT6358 DAC / earpiece)
```

This is MediaTek's hostless **device 21 `FM_I2S_Playback`**
(`sound/soc/mediatek/common_int/mtk-soc-pcm-fm-i2s.c`). Its `prepare`
connects the route and starts the ASRC and the I2S input; no samples pass
through the PCM. `titan-radio` uses the same steps as the speaker path in
[../audio.md](../audio.md):

```sh
tinymix -D 0 set "Audio FM I2S Volume" 32768
tinymix -D 0 set "Speaker Amp Volume" 160
tinymix -D 0 set Audio_i2s0_SideGen_Switch On48000
spk-hold 0 30 48000 &      # speaker amp on
spk-hold 0 21 48000 &      # FM route (prepare only)
titan-fm 99.5
```

## Not done

- Without headphones there is no long antenna: weak signals, mono. With
  headphones in, stations come in strong and in stereo, and FM plays in the
  headphones (`titan-speakerd`, [../audio.md](../audio.md#headphones)).
- The FM-over-Bluetooth path (`FM_MRGTX`, device 14); RDS clock time and
  alternative frequencies are decoded by the driver but unused.
