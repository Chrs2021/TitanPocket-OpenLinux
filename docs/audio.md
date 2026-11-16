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

On Linux 6.12 the same chain plays through the `titanpocket` card's DL1
(`hw:CARD=titanpocket,DEV=0`) instead of device 7 (2026-11-01). The sink
config lists both, each with `flags = [ nofail ]`, so each kernel creates
the one its card has (without `nofail`, the missing device stopped
PipeWire from starting at all). `/etc/udev/rules.d/91-titan-audio.rules`
switches the `I2S3_CH1/2 DL1_CH1/2` routes on when the card's control
device appears, after `90-alsa-restore.rules` (whose restore of the saved
state switched them off again when the rule ran first). ASoC's DAPM powers
the amp, so `titan-speaker.service` doesn't run there
(`ConditionPathExists=!/proc/asound/titanpocket`). Verified: a tone through
`pw-play` ran DL1 at 48 kHz S16_LE and was heard from the speaker.
Headphones on 6.12: `titan-headphones`, see "Headphones on Linux 6.12".

the udev rule also routes the CONNSYS I2S to UL2 (`UL2_CH1/2
CONNSYS_I2S_CH1/2`), and while the radio is on the app pipes `arecord` from
`hw:CARD=titanpocket,DEV=3` (WAV, 48 kHz S16 stereo) into `pw-cat`, a
playback stream on the default sink (limiter, speaker). Both ends run on the
AFE clock, so they don't drift. The volume slider (-48..0 dB) sets that
stream's node `volume` with `pw-cli set-param` and holds its
`channelVolumes` at 1.0: the two multiply, and `pactl`'s dB setting changes
only the channel volumes (asked for -24 dB, it read back -18 dB with the
node volume at -4 dB). The stream is found by `application.name` "Radio":
`pw-cat` names the stream after its input file, `-`. Measured: 99.5 MHz
captured at -13.3 dBFS RMS (peak 25974); heard, slider working. 4.14 keeps
the hostless route (device 21, `spk-hold`, AFE FM gain). The command-line
`titan-radio` is still 4.14-only.

| Piece | Where | Why |
|---|---|---|
| Hardware sink | `/etc/pipewire/pipewire.conf.d/50-titan-speaker.conf` | the AFE card has ~40 PCMs and no UCM profile, so WirePlumber's ALSA monitor is off (`/etc/wireplumber/main.lua.d/51-titan.lua`) and the sink is defined by hand. `api.alsa.disable-mmap` avoids the silent mmap path; 16 KB buffer keeps it in DRAM |
| Limiter | `.../60-titan-limiter.conf` | `swh-plugins` `fast_lookahead_limiter` (LADSPA): input gain 0 dB, ceiling -1 dB, release 0.2 s. Measured: -18 dBFS in -> -18.06 out; 0 and +6 dBFS in -> -1.00 out. Its output link is passive, so an idle system still closes the device |
| Root user units | `/etc/systemd/user/{pipewire,pipewire-pulse,wireplumber}.*.d/titan-root.conf` | Debian's units skip root (`ConditionUser=!root`); the session runs as root |
| Amp power | `titan-speakerd` (`titan-speaker.service`) | watches devices 7 (samples) and 21 (FM): while either is open, I2S clock on, **50 ms** to settle, then the amp link (device 30) opened. Without the settle time the FS1860 DSP sometimes stays idle (`0xBD` = `0x0803`, speaker silent); the daemon also checks `0xBD` over I2C and re-enables the amp if needed. Off 2 s after the devices close (PipeWire closes after 5 s of silence) |
| Chromium | `titan-chromium` | runs as user `browser`; gets ACL access to `/run/user/0/pulse/native` and `PULSE_SERVER` |

### Volume

One master level, 0-224 (224 shown as 100 %), stored in
`/var/lib/titan-audio/volume`. Level 0 is muted. Everything that touches
loudness goes through it.

**The level is on a dB curve (2026-10-28).** The FS1860's volume
(`Speaker Amp Volume`, 0-255) is 0.5 dB a step (`fs1860.c`'s TLV), and the
level used to be written to it one to one: 100 % was -15.5 dB, 40 % was
-82.5 dB (near silent, as reported), so the useful range was squeezed into
the top third. Now the level spreads 42 dB below 100 %, like the
headphones: amp = 224 - 84 x (1 - level / 224), 1.5 dB per volume-key step;
40 % is 25 dB below full. `amp_value()` in `titan-powerkey` and
The mixer, the same in `titan-speakerd` (`SPK_RANGE_DB`). The level
file keeps its meaning; only what reaches the amp changed, so a saved level
now plays louder below the top.

| Who | Reads | Writes |
|---|---|---|
| Side rocker and headset buttons (`titan-powerkey`, from every input device with volume keys: `mtk-kpd`, `ACCDET`) | before each step | file + amp |
| Top-bar indicator (desktop shell) | inotify on the file | - |
| The mixer | file monitor | file + amp |
| `titan-speakerd` | every time it powers the amp on; with headphones, on every change | amp, or the headphone gain; on a jack change it swaps the file with `volume.speaker` / `volume.headphones` |
| `titan-headphones` (6.12) | with headphones, on every change | the headphone gain (codec step + sink); on a switch the amp from the speaker's level, and the file swap as above |
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
- **A stale volume-key grab.** the remote-control app takes the volume keys (for a
  TV's volume) while it's focused, through `/run/titan-volume-keys.grab`. If
  it was killed (desktop restart, crash), the file stayed and the keys were
  dead until reboot. The file now holds the Remote's PID, and
  `titan-powerkey` drops a grab whose process is gone.

PipeWire's own sink volumes ("Loudspeaker" and "Loudspeaker (direct)") stay
at 100 %: WirePlumber had restored them to 53 % and 81 % at times, silently
scaling everything down. The mixer re-pins them when it runs and every volume
key press does too (at most every 10 s). Per-app volume and mute are PipeWire
stream volumes; WirePlumber remembers them.

The mixer uses `pactl` (`pulseaudio-utils`): `pactl subscribe` for live
updates, JSON listings, `set-sink-input-*`.

The earpiece has its own level, apart from the master: the codec's handset
gain (`Handset Volume`, -10 to +8 dB in 1 dB steps), used only by calls.
`titan-ims` keeps it in `/var/lib/titan-ims/earpiece-db` and sets it when a
call starts on the earpiece; the volume keys in a call and the mixer's
"Earpiece" slider change it (docs/modem.md, "Call volume"). Measured 2026-11-15 with no call (a -20 dBFS 1 kHz tone through
PipeWire on the earpiece route, the main mic at 24 dB preamp, 1.5 s
Goertzel at 1 kHz): -10 dB gave -78.6 dBFS at the mic, -7 dB -75.4, -4 dB
-72.7, against -101.6 and -106.5 dBFS of silence, so the control's 3 dB
steps come out as about 3 dB. (Start the tone before the capture: capture
overruns by about 0.5 s when DL1 starts.)

While the default output is a Bluetooth device's (a `bluez_output.*` sink),
the volume keys step that sink instead of the speaker level (2026-11-15,
`titan-powerkey`): a sixteenth (6.25 %) a press, 0-100 %, unmuting on
volume up, without the key tones (they would play in the ears). PipeWire
passes the sink volume to the device as AVRCP absolute volume while audio
flows (a Bluetooth headset: 30 % gave 38 of 127). The panel's volume indicator still
shows the speaker level; the mixer's Bluetooth row follows the keys.

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
keys, the mixer and the panel indicator, but it follows the output in
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
  100 %; the mixer, which otherwise keeps the sink at 100 %, leaves it
  alone while `volume.output` is `headphones`.
- FM bypasses PipeWire, so on headphones its fine level is the Radio app's
  own (AFE) gain; the volume keys move only the codec step for it.
- Headphones start at level 107 (about −30 dB).
- The top bar shows a headphone icon while they're in use, and the volume
  indicator shows headphones instead of a speaker
  (desktop.md).

### Headphones on Linux 6.12

The jack is detected by the kernel (our `mt6358-accdet`, below) and the
headphone route works, but on 6.12 nothing switched to it until
2026-11-16: `titan-speakerd` is 4.14 only, so sound stayed on the
loudspeaker with headphones in. `titan-headphones`
(`userspace/titan-phone/src/bin/titan-headphones.rs`, Rust,
`titan-headphones.service`, `ConditionPathExists=/proc/asound/titanpocket`)
handles them, with a sink of their own so each app can play on either:

- **Routes.** The loudspeaker keeps DL1 (`hw:0,0`, PipeWire's
  `titan-speaker` behind the limiter) through I2S3. While something is in
  the jack, DL2 (`hw:0,1`, otherwise used only by the modem loop test) goes
  to the codec (`ADDA_DL_CH1/2 DL2_CH1/2`) and the headphone muxes (`HPL/HPR
  Mux` Audio Playback); DL1 stays off the codec. Pulled out, DL2's routes and
  the muxes go off. The jack is the "titanpocket Headset Jack" input device
  (`SW_HEADPHONE_INSERT`, `SW_MICROPHONE_INSERT`, `SW_LINEOUT_INSERT`), read
  300 ms after an event, past the plug's ring crossing the mic contact.
- **Sink.** `titan-headphones` ("Headphones"), an ALSA adapter on DL2 with
  the speaker's settings (no mmap, 4 x 1024 frames), created by a `pw-cli`
  the daemon keeps running (`create-node adapter {...}` on its stdin): the
  sink lives as long as that process, so it goes when the headphones are
  pulled out, and the daemon makes it again if PipeWire restarts.
  pipewire-pulse 0.3.65 has no `module-alsa-sink`, and a one-shot `pw-cli
  create-node` exits before the node exists. The values must be quoted
  (`api.alsa.path="hw:CARD=titanpocket,DEV=1"`): SPA JSON reads `:` as a
  separator, and unquoted the path came out as `hw`, i.e. DL1 (logged as
  `'hw': playback open failed: Device or resource busy` while DL1 played).
- **Plugging in** makes the sink PipeWire's default and moves the apps on
  the speaker (`titan-speaker-limited`'s streams, not the limiter's own) to
  it, after DL2's routes are up and before DL1 leaves the codec. Afterwards
  the mixer moves the default or single apps between the speaker, the
  headphones and Bluetooth like any outputs; WirePlumber remembers an app's
  choice. Pulled out, the sink goes, its apps fall back, and the default
  returns to the speaker.
- **Levels.** Each output keeps its own, as on 4.14 (`volume.speaker`,
  `volume.headphones`, `volume.output`), and the shared level file belongs
  to the default: the headphones' while their sink is the default (followed
  through `pactl subscribe`), else the speaker's. The headphone curve is
  4.14's: full scale −8 dB, 1.5 dB a key step. The 6.12 control is
  `Headphone Volume` (0..18 = −10..+8 dB), so it takes 0, 1 or 2 and the
  headphones' sink the rest, as a linear factor: `pactl` reads a signed dB
  value (`-28dB`) as a step from the current volume, which once sent a sink
  to −88 dB. While the level is the headphones', `titan-powerkey` and the
  mixer leave the speaker's amp alone; the speaker's sinks stay at
  100 %.
- **Calls** switch these routes themselves (the UCM profile's devices) and
  put the speaker back when they end (`titan-ims` re-enables `Speaker`). The
  daemon subscribes to the card's control events; when one of its routes
  changes and no call is on (no `titan-call-audio run`, no `pw-cat` with the
  Phone role), it puts them back 1.5 s later. During a call it switches
  nothing; a plug or unplug waits for the call's end.

Measured on the phone, first version (one sink for both, 2026-11-16): the
switch, the level swap (speaker 115 saved, headphones 48 loaded: `Headphone
Volume` 0, sink −31 dB, −41 dB in all), levels 64 and 224 (−28 dB; `Headphone
Volume` 2 and 0 dB), the volume keys, and routes changed by hand as a call
would, put back within 2.5 s. The two-sink version (headphones in,
Chromium playing on the speaker's sink): on start the `titan-headphones` sink
came up on DL2 (`pcm1p` RUNNING, `pcm0p` closed), became the default, and
Chromium's stream moved to it; the speaker's sinks idle at 100 %, DL1 off
the codec; level 40 gave `Headphone Volume` 0 and the sink −32.5 dB
(−42.5 dB in all). Not yet tried: an app moved to the speaker by touch,
both playing at once, a call with headphones in, plugging and unplugging
under the daemon.

## Microphones

Two analog MEMS microphones (Knowles SPH1642HT5H, −41 dB sensitivity, per
the stock `MicInfo_AudioParam.xml`): the main one at the bottom on the
MT6358's preamp input `IN_ADC1`, the back one on `IN_ADC3` (`IN_ADC2` is
the headset mic). The device tree's mic mode is **DCC** (`Audio_MIC_Mode`
reads 2); DMIC mode records exact zeros, ACC records but is not what the
board uses.

The stock path (`builtin_Mic_Mic1` in `/vendor/etc/audio_device.xml`):
`Audio_MicSource1_Setting` ADC1, `Audio_ADC_1_Switch` and `_2_` On,
`Audio_Preamp1_Switch` and `_2_` `IN_ADC1` (`IN_ADC3` for the back mic,
one of each for dual). **Gotcha:** the codec powers the uplink (mic bias
1.9 V on `AUDENC_ANA_CON9`, PGA, ADC) when `Audio_ADC_1_Switch` goes Off →
On, in the `Audio_MIC1_Mode_Select` mode set at that moment; changing the
mode while it is on does nothing. `titan-mic on [main|back|dual] [GAIN]` /
`off` does the sequence. Capture is **ALSA device 1** (`MultiMedia1_Capture`),
48 kHz stereo S16: left = preamp 1, right = preamp 2.

Measured (PGA 18 dB, 2026-10-19): a quiet room −64 to −68 dBFS on either
mic; the vibration motor running lifts the main mic by 20 dB. A 1 kHz tone
from the speaker did not show up in a capture taken at the same time (not
investigated: the speaker may not run while the uplink starts).

**Recorder** (`titan-recorder`, desktop.md)
records the main mic at PGA 30 dB.

**On Linux 6.12** the uplink is upstream's MT6358 codec under DAPM:
`titan-mic` sets `Mic Type Mux` (DCC), `PGA L/R Mux` (AIN0 for the main mic,
AIN2 for the back one), `ADC L/R Mux` (the preamplifiers), `PGA Volume`
(0-24 dB in 6 dB steps) and the AFE's `UL1_CH1/CH2 ADDA_UL_CH1/CH2`
routes; capture is UL1 (`hw:0,2`), and the codec powers the uplink while
that device is open. Both inputs were recorded this way on 2026-11-01;
the script's 6.12 branch is from 2026-11-02, not yet run.

## Modem voice (PCM 2)

On Linux 6.12 the card has the modem's voice links (2026-11-03): the
`Hostless_Speech` front end (`hw:0,5`) and the `PCM 2` back end, the
AFE's internal modem PCM interface. Upstream's routes connect the modem's
downlink to the codec DAC and I2S3 (`ADDA_DL_CH1/2 PCM_2_CAP_CH1/2`,
`I2S3_CH1/2 ...`) and the codec's ADC to its uplink (`PCM_2_PB_CH1/2
ADDA_UL_CH1/2`); `PCM_2_PB_CH1 DL2_CH1` lets an AP stream into the
uplink. Two changes to upstream's MT8183 AFE driver:

- **UL2 can record the modem's downlink** (`UL2_CH1/2 PCM_2_CAP_CH1/2`,
  interconnect bits I14/I21 in `AFE_CONN5/6`), for testing and for
  recording a call.
- **PCM interface rates.** `mt8183_rate_transform()` gave the PCM DAIs the
  general rate code (32 kHz = 8), which shifted into `PCM2_INTF_CON` set
  the FIFO bit and left the mode at 0: 8 kHz whatever was asked. It now
  uses the PCM codes (8, 16, 32, 48 kHz = 0-3), as MT8192's driver does.
  An upstream bug, worth sending.

Measured (`tools/modem/sphloop.sh`, silent: DL2 -> modem -> UL2): with the
modem looping at its PCM interface a -20 dBFS tone comes back unchanged
at 8 and 32 kHz. What the modem needs for real voice is in
[modem.md](modem.md#speech-voice-audio).

## All paths on Linux 6.12 (2026-11-03)

| Path | Route | State |
|---|---|---|
| Loudspeaker | DL1 -> I2S3 -> FS1860 | works (PipeWire sink) |
| Earpiece | DL1 -> ADDA -> `RCV Mux` Voice Playback, `Handset Volume` | works: a -20 dBFS tone at -6 dB reached the main mic at -77 dBFS (-98 without), the back mic at -70 (-94); the driver left the gain at -40 dB after each power-down until kernel #111 (docs/modem.md, "Call volume") |
| Main / back mic | AIN0 / AIN2 -> PGA -> ADC -> UL1 (`titan-mic`, hw:0,2) | work; floor about -72 dBFS at 18 dB PGA after a 100-200 ms power-on thump at the start of each capture |
| Headset mic | AIN1 | not yet tested (needs a headset) |
| Headphones | DL1 -> ADDA -> `HPL/HPR Mux` Audio Playback, `Headphone Volume` | works: a 3-pole plug detected as headphones; 440 Hz left and 660 Hz right tones (-22.5 dBFS at -6 dB) heard each in its own ear |
| FM | CONNSYS I2S -> UL2 | works |
| BT SCO | BTCVSD | works (earlier) |
| Modem voice | PCM 2 <-> ADDA / I2S3 | links and routes in; see "Modem voice" |

**UCM.** `alsa-ucm-conf` is installed and the card has a profile of its own
(`/usr/share/alsa/ucm2/conf.d/titanpocket/`, `Unihertz/titan-pocket/`, from
the Debian overlay): verb `HiFi` with Speaker, Earpiece, Headphones, Mic
(main), Mic2 (back) and Headset devices, and verb `Voice Call` with the
PCM 2 routes (Earpiece, Speaker, Headphones, Mic: main + back for the
modem's noise reduction, Headset). Checked with `alsaucm`: enabling and
disabling Earpiece and Mic sets and clears their controls, and a capture
works with Mic on. PipeWire does not use it yet (its ALSA monitor is off
and the speaker sink is hand-made, see above); the udev rule still turns
the speaker routes on.

**Jack detection** (kernel #99): a driver of our own,
`sound/soc/codecs/mt6358-accdet.c`, after the vendor's MT6358 ACCDET
sequence (4.14 `accdet/mt6358`) with upstream's MT6359 binding: the PMIC's
EINT0 (level, low active; polarity flipped after each edge) sees the plug,
then ACCDET's comparator says headphones (A/B 00), headset (01) or the
hook button (00 with a headset in). The machine driver makes a `Headset
Jack` (`snd_soc_jack`: ALSA controls `Headphones Jack` and `Headset Mic
Jack`, input device "titanpocket Headset Jack", KEY_PLAYPAUSE). Settings
from the stock dtbo (mic bias 6, mode 1, PWM and debounces). The 4-key
detection (AUXADC) is not ported. Measured with headphones (3-pole), unplugged and
replugged twice: `SW_HEADPHONE_INSERT` 0 and 1 on the input device for
each, the jack controls following. On the second removal the comparator
read 01 (a headset) for 0.2 s before EINT0 saw the plug go: the plug's
ring passing the mic contact on the way out. The end state was right each
time; a short settle delay before reporting a headset would hide it. Not yet tried: a headset (mic, hook).

Getting it running turned up an upstream bug: the MT6358's top-level
interrupt status has XPP at bit 6, AUD at 7 and MISC at 8, but
`mt6358_irq_top_status_shift` had no XPP, so AUD and MISC interrupts (the
jack, the RTC alarm) were never dispatched and, once enabled, the PMIC's
level interrupt stormed (340398 times in a few minutes). Fixed by adding
`MT6358_XPP_TOP`; worth sending upstream, as is the AFE's PCM rate fix.

### A tone after deep sleep (fixed 2026-11-05, kernel #110)

With deep sleep the boot default, waking the phone and popping the lock
screen's bubble sometimes left a steady high tone on the loudspeaker until a
reboot. The kernel logged `mtk_afe_pcm_pointer hw_ptr err` without end, the
first one 0.5 s after the resume; `pcm0p` was `RUNNING` with `hw_ptr` 0 and
the buffer full, so the DAC repeated one buffer.

PipeWire keeps the speaker's PCM open (`SETUP`) while idle, which holds the
AFE runtime-active, and the MT8183 AFE driver saves and restores its
registers only in its runtime callbacks: nothing ran across a system sleep.
s2idle kept the audio block powered; the SPM's deep sleep does not, so the
first playback after it started the memif from reset values. Fixed in the
kernel: `mt8183-afe-pcm.c` gets system-sleep callbacks
(`pm_runtime_force_suspend`/`_resume`, so the regcache saves and restores
the registers), and `mtk_afe_fe_prepare()` writes the buffer's addresses
again on every prepare (the end address is a volatile register, outside the
cache). Checked: 6 deep suspends, each followed by the pop, no tone and no
`hw_ptr err`. While it's stuck, `pactl suspend-sink titan-speaker 1` (as root,
`XDG_RUNTIME_DIR=/run/user/0`) stops the tone without a reboot.

### A quiet earpiece (fixed 2026-11-06, kernel #111)

The MT6358 driver's receiver power-down (`mt_rcv_event`, `PRE_PMD`) ramps
`ZCD_CON3` to -40 dB, and its power-up never wrote the gain back: the
earpiece came up at -40 dB, whatever `Handset Volume` had been set to while
it was off, and the control then read -13 (18 minus the register's 0x1f).
Upstream has the same code. The headphone path restores its gain from
`ana_gain[]`; the receiver now does the same at the end of `PRE_PMU`, and
`mt6358_codec_init_reg()` starts that gain at -10 dB (0 in the register
would be +8 dB). Setting the control while the earpiece is powered took
effect at once (checked: -6 dB read back during a silent stream).

## Known limitations / TODO

- Chromium sometimes pauses ("corks") its stream when a YouTube ad is
  skipped and plays the video silently; PipeWire and the speaker are fine.
  Open; `chrome://media-internals` should say why.

- **MMAP access plays silence** on the DRAM path (the vendor buffer is not
  mapped correctly through the generic ALSA mmap). PipeWire is set to
  read/write access; fixing the driver's mmap is still open.
- Volume scale of `Speaker Amp Volume` (0.5 dB/step) is inferred, not verified.
- Only 44.1/48 kHz preset values are replayed; 16/32 kHz need preset index 1.
- UCM profile in place but not used by PipeWire yet.
- Speaker protection coefficients are this unit's (see the FS1860 doc).
