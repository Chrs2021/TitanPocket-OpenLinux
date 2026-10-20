# Audio

How sound gets from a Linux process to the Titan Pocket's loudspeaker on this
MediaTek 4.14 BSP kernel, and the bugs that had to be fixed on the way.

## Signal chain

```
userspace PCM (ALSA device 7, "I2S0DL1_PLayback")
   -> AFE memory interface DL1 (DMA from a 32 KB DRAM buffer)
   -> AFE interconnect: DL1 L/R (I05/I06) -> O00/O01
   -> I2S3 output (GPIO170 BCK, 171 LRCK, 172 DO)        48 kHz, 32-bit slots
   -> FS1860 smart amp (i2c3/0x34) -> bottom loudspeaker
```

DL1 is simultaneously routed to the internal MT6358 codec DAC (I2S1), which
drives the **earpiece**. Two transducers, one stream: during bring-up always
note which end the sound comes from.

## Playing audio (what `speaker-test` does)

```sh
tinymix -D 0 set "Speaker Amp Volume" 160          # amp gain, 0-255 (160 = comfortable)
tinymix -D 0 set Audio_i2s0_SideGen_Switch On48000 # I2S clocks + amp pin mux
spk-hold 0 30 48000 &                              # hold the codec link open -> amp on
tinyplay file.wav -D 0 -d 7 -p 2048 -n 4           # audio data on DL1 -> I2S3
kill %1                                            # amp off
tinymix -D 0 set Audio_i2s0_SideGen_Switch Off
```

This mirrors MediaTek's Android HAL:

1. **`Audio_i2s0_SideGen_Switch`** enables I2S0/I2S3 clock generation at the
   chosen rate and switches the smart-amp pins to their I2S function.
2. **Device 30 `Speaker_PLayback`** is a *hostless* link (dummy CPU DAI) whose
   only job is to drive the FS1860 codec: `hw_params` sets the amp's rate,
   `prepare`/unmute powers it on, close powers it off. It carries no data.
   `userspace/src/spk-hold.c` opens and **prepares** it, then waits; it must
   not *start* it (an empty playback stream underruns immediately).
3. **Device 7 `I2S0DL1_PLayback`** carries the samples.

Start the clock (step 1) **before** the amp (step 2): the amp's DSP only starts
on an amp-enable edge while the I2S clock is running (see
[drivers/fs1860.md](drivers/fs1860.md)).

## Buffer size: use DRAM

`mtk-soc-pcm-dl1-i2s0Dl1.c` places the buffer in the AFE's 8 KB on-chip SRAM
whenever it fits, else in DRAM. Use a buffer **larger than 8 KB**
(`-p 2048 -n 4` = 32 KB at 16-bit stereo) to get the DRAM path; the SRAM path
garbled audio during bring-up.

## Bugs fixed

### 1. MediaTek DL copy dropped audio while reporting success

`mtk_mem_dlblk_copy()` (`sound/soc/mediatek/common_int/mtk-soc-afe-control.c`)
ignored the `pos` ALSA passes to `.copy_user` and wrote at a private index,
limited by a private free-space count; anything that didn't fit was dropped
and the call still returned 0. Whenever ALSA's and the driver's views
diverged, samples landed in the wrong place or vanished. It now writes at
ALSA's position (with wrap-around) and keeps its bookkeeping consistent for
the DL IRQ handler's sanity check.

### 2. tinyalsa rewound the stream in read/write mode

On this kernel tinyalsa cannot mmap the PCM control page, so it uses the
`SYNC_PTR` ioctl. Called with flags 0 (e.g. from `pcm_get_htimestamp`), it
copies tinyalsa's local `appl_ptr` into the kernel — which in read/write mode
is never advanced — rewinding ALSA's write position. Symptom: 3 s of audio
"played" in 0.13 s, then cut off. Fixed in our vendored copy:
`userspace/third_party/patches/tinyalsa-0001-rw-sync-ptr-keep-kernel-appl.patch`.

### 3. FS1860 never started its DSP

The amp powered up and locked its PLL but stayed silent until amp-enable was
toggled with the clock present. Fixed in the codec driver.

### Diagnosis aids that paid off

- `pcm-probe` (`userspace/src/pcm-probe.c`): logs every write's result,
  timing and avail; `VERBOSE=1`, `MMAP=1`, `PCM_NORESTART` behaviour.
- Comparing amp registers against an Android capture: `0xBD` = `0x5803`
  means the amp DSP is processing audio.
- `/sys/kernel/debug/mtksocaudio`: full AFE register dump (needs
  `CONFIG_DEBUG_FS`; stock Android has it off).
- `/proc/asound/card0/pcm7p/sub0/{status,hw_params}`.
- AFE IRQ rate in `/proc/interrupts` (`Afe_ISR_Handle`): should be
  rate / period_size per second.

## Desktop audio: PipeWire

Apps play through **PipeWire** (0.3.65, with WirePlumber and the PulseAudio
layer `pipewire-pulse`), running in root's user session, which the desktop
session starts (desktop.md):

```
app (Chromium via PulseAudio API, pw-play, ...)
  -> "Loudspeaker"            filter-chain sink, the default: lookahead limiter, peaks <= -1 dBFS
  -> "Loudspeaker (direct)"   ALSA hw:0,7, read/write access (mmap disabled), 48 kHz, 4 x 1024 frames
  -> DL1 -> I2S3 -> FS1860    powered on demand by titan-speakerd
```

| Piece | Where | Why |
|---|---|---|
| Hardware sink | `/etc/pipewire/pipewire.conf.d/50-titan-speaker.conf` | the AFE card has ~40 PCMs and no UCM profile, so WirePlumber's ALSA monitor is off (`/etc/wireplumber/main.lua.d/51-titan.lua`) and the sink is defined by hand. `api.alsa.disable-mmap` avoids the silent mmap path; 16 KB buffer keeps it in DRAM |
| Limiter | `.../60-titan-limiter.conf` | `swh-plugins` `fast_lookahead_limiter` (LADSPA): input gain 0 dB, ceiling -1 dB, release 0.2 s. Measured: -18 dBFS in -> -18.06 out; 0 and +6 dBFS in -> -1.00 out. Its output link is passive, so an idle system still closes the device |
| Root user units | `/etc/systemd/user/{pipewire,pipewire-pulse,wireplumber}.*.d/titan-root.conf` | Debian's units skip root (`ConditionUser=!root`); the session runs as root |
| Amp power | `titan-speakerd` (`titan-speaker.service`) | watches devices 7 (samples) and 21 (FM): while either is open, I2S clock on, **50 ms** to settle, then the amp link (device 30) opened. Without the settle time the FS1860 DSP sometimes stays idle (`0xBD` = `0x0803`, speaker silent); the daemon also checks `0xBD` over I2C and re-enables the amp if needed. Off 2 s after the devices close (PipeWire closes after 5 s of silence) |
| Chromium | `titan-chromium` | runs as user `browser`; gets ACL access to `/run/user/0/pulse/native` and `PULSE_SERVER` |

### Volume

One master level: the FS1860 amp gain (`Speaker Amp Volume`, 0-224, 224
shown as 100 %), stored in `/var/lib/titan-audio/volume`. Level 0 is muted.
Everything that touches loudness goes through it:

| Who | Reads | Writes |
|---|---|---|
| Side rocker and headset buttons (`titan-powerkey`, from every input device with volume keys: `mtk-kpd`, `ACCDET`) | before each step | file + amp |
| Top-bar indicator (desktop shell) | inotify on the file | - |
| **Sound** app, "All apps" (`titan-mixer`) | file monitor | file + amp |
| `titan-speakerd` | every time it powers the amp on; with headphones, on every change | amp, or the headphone gain; on a jack change it swaps the file with `volume.speaker` / `volume.headphones` |
| `titan-powerkey` at start and after resume | file | amp |
| FM (`titan-radio`) | follows the amp (FM bypasses PipeWire) | - |

Two traps this closes:

- **The codec's chip init writes full gain.** `fs1860.c` initializes the
  chip once, on the first stream after boot (`hw_params → ensure_init`), and
  both of its register tables set `0x06 = 0xff00`: volume 255, above the cap.
  Before the fix the speaker played at full gain after every boot while the
  file, indicator and mixer all showed the saved level. `titan-speakerd` now
  re-applies the level right after it opens the amp link. (A kernel-side fix
  would be to cache the volume in the driver and restore it after init.)
- **A stale volume-key grab.** `titan-remote` takes the volume keys (for a
  TV's volume) while it's focused, through `/run/titan-volume-keys.grab`. If
  it was killed (desktop restart, crash), the file stayed and the keys were
  dead until reboot. The file now holds the Remote's PID, and
  `titan-powerkey` drops a grab whose process is gone.

PipeWire's own sink volumes ("Loudspeaker" and "Loudspeaker (direct)") stay
at 100 %: WirePlumber had restored them to 53 % and 81 % at times, silently
scaling everything down. The mixer re-pins them when it runs and every volume
key press does too (at most every 10 s). Per-app volume and mute are PipeWire
stream volumes; WirePlumber remembers them.

`titan-mixer` uses `pactl` (`pulseaudio-utils`): `pactl subscribe` for live
updates, JSON listings, `set-sink-input-*`.

## Headphones

The 3.5 mm jack is watched by the MT6358's accessory detection: the BSP
`accdet` driver (`drivers/misc/mediatek/accdet/mt6358/`) works as is and
reports standard input switches on the `ACCDET` input device:
`SW_HEADPHONE_INSERT` (3-pole headphones), `SW_MICROPHONE_INSERT` (4-pole
headset), `SW_LINEOUT_INSERT`, plus the headset buttons as keys. Checked
with `evtest /dev/input/event0` (`evtest --query /dev/input/event0 EV_SW
SW_HEADPHONE_INSERT` exits 10 while plugged).

**Sound:** DL1's samples reach the MT6358 codec's DAC as well as I2S3, so
the headphones need no separate stream, only the codec's headphone amps.
The stock HAL's `headphone_output` path (`/vendor/etc/audio_device.xml`)
is just `Audio_Amp_R_Switch` / `Audio_Amp_L_Switch` On; the gain is
`Headset_PGAL_GAIN` / `Headset_PGAR_GAIN` (+8 dB to −10 dB in 1 dB steps,
or −40 dB). The amps must be switched on **after** the stream started:
switched on before, they stayed silent in the first test.

`titan-speakerd` does the switching: with headphones (or a line-out cable)
in the jack, playback on devices 7 and 21 powers the headphone amps (300 ms
after the device opened) instead of the speaker; plugging in or out while
something plays moves the sound over. FM follows too; the headphone cable
is also the FM antenna.

**Volume per output:** the shared level file keeps working for the volume
keys, the Sound app and the panel indicator, but it follows the output in
use. On a jack change `titan-speakerd` saves the level to
`volume.speaker` or `volume.headphones` and loads the other one;
`volume.output` says whose level the file holds. For the headphones the
daemon applies the level itself, whenever the file changes:

- Full scale (level 224, 100 %) is **−8 dB**: the codec's +8 dB top is far
  too loud, so the headphones get only the bottom tenth of its range.
  Below that the level spreads 42 dB: −8 − 42 × (224 − level) / 224 dB,
  1.5 dB per volume-key step (8). Level 0 mutes.
- The codec's gain only has 1 dB steps, down to −10 dB. It is set to the
  step at or above the target (−10, −9 or −8 dB), and **PipeWire's sink
  volume** takes the rest (`pactl set-sink-volume @DEFAULT_SINK@`, linear;
  the speaker is off meanwhile). Back on the speaker the sink returns to
  100 %; the Sound app, which otherwise keeps the sink at 100 %, leaves it
  alone while `volume.output` is `headphones`.
- FM bypasses PipeWire, so on headphones its fine level is the Radio app's
  own (AFE) gain; the volume keys move only the codec step for it.
- Headphones start at level 107 (about −30 dB).
- The top bar shows a headphone icon while they're in use, and the volume
  indicator shows headphones instead of a speaker
  (desktop.md).

## Known limitations / TODO

- Chromium sometimes pauses ("corks") its stream when a YouTube ad is
  skipped and plays the video silently; PipeWire and the speaker are fine.
  Open; `chrome://media-internals` should say why.

- **MMAP access plays silence** on the DRAM path (the vendor buffer is not
  mapped correctly through the generic ALSA mmap). PipeWire is set to
  read/write access; fixing the driver's mmap is still open.
- Volume scale of `Speaker Amp Volume` (0.5 dB/step) is inferred, not verified.
- Only 44.1/48 kHz preset values are replayed; 16/32 kHz need preset index 1.
- No ALSA UCM profile yet (titan-speakerd does the mixer steps instead).
- Microphones and the earpiece path are untested.
- Speaker protection coefficients are this unit's (see the FS1860 doc).
