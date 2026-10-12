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

## Known limitations / TODO

- **MMAP access plays silence** on the DRAM path (the vendor buffer is not
  mapped correctly through the generic ALSA mmap). PipeWire/alsa-lib prefer
  mmap; either fix the driver's mmap or force read/write access.
- Volume scale of `Speaker Amp Volume` (0.5 dB/step) is inferred, not verified.
- Only 44.1/48 kHz preset values are replayed; 16/32 kHz need preset index 1.
- No ALSA UCM profile yet; the mixer steps above need to become one.
- Microphones and the earpiece path are untested.
- Speaker protection coefficients are this unit's (see the FS1860 doc).
