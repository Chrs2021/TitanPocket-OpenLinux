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

The driver needs the WMT core (`titan-wmt.service`) to be running. Its
module depends on `wmt_drv`, so `modprobe fmradio_drv` works at any time.
The first `open()` of `/dev/fm` powers the FM function on through WMT. The
**last close powers it off**, so the tuner has to keep running while you
listen.

## Using it

```sh
titan-radio            # 99.5 MHz on the speaker; type a frequency, u/d (±100 kHz),
                       # su/sd (seek), q (quit; routing is undone)
titan-radio 101.1 65536   # frequency, AFE gain (0x80000 = 0 dB; default 32768 = -24 dB)
titan-fm scan          # list stations, no audio
```

`titan-fm` prints the chip's raw RSSI as "signal": higher is stronger, and a
channel counts as a station at -296 or above (`FM_RX_RSSI_TH_LONG`, about
-4 dBµV). The driver's unit conversion isn't documented, so no dBm is shown.

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

- Stereo is reported as mono in tests so far. Without a headset there is no
  long antenna, so this is probably signal-related; not investigated.
- Headset output, RDS, the FM-over-Bluetooth path (`FM_MRGTX`, device 14).
