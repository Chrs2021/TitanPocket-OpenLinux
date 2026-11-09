# Hardware video encoder (VENC), work in progress

Goal: H.264 recording on the MT6771's hardware encoder from our own driver,
with no Android library at run time. Started 2026-11-08.

## What the hardware is

- **VENC** at `0x17020000` (vendor DT: 0x1000 bytes, IRQ SPI 247, level
  low), behind IOMMU **larb 4**. Its clock and power block, `venc_gcon` at
  `0x17000000`, gates larb4, VENC and the JPEG encoder. Mainline Linux has
  the same layout for the MT8183: the `venc` power domain
  (`MT8183_POWER_DOMAIN_VENC`), `mediatek,mt8183-vencsys` clocks
  (`clk-mt8183-venc.c`) and the larb4 ports in `mt8183-larb-port.h`
  (`M4U_PORT_VENC_RCPU`, `_REC`, `_BSDMA`, `_SV_COMV`, `_RD_COMV`,
  `_CUR_LUMA`, `_CUR_CHROMA`, `_REF_LUMA`, `_REF_CHROMA`). Our 6.12 DT has
  none of these nodes yet.
- The vendor kernel driver (`drivers/misc/mediatek/videocodec/mt6771/`) is a
  thin pass-through: power, a hardware lock, `mmap` of the registers and an
  interrupt wait. The interrupt status bits it names: SPS 0x1, PPS 0x2,
  frame 0x4, DRAM 0x8, pause 0x10, switch 0x20, VPS 0x80.
- **H.264 only.** The vendor's HEVC encoder (`libhevce_sb`) is software: it
  imports no register, lock or interrupt calls. MPEG-4 and VP8 are
  software too.

## Where the register programming is

In the 32-bit vendor library `libvcodecdrv.so` (Ghidra output in
`out/venc/re/vcodecdrv.c`, local only). All register access goes through
`H264_WriteHW(handle, offset, value)` / `H264_ReadHW` (a plain store to
the mapped registers plus a barrier). 45 registers, offsets 0x000–0x1f0.

SPS and PPS are written **in software** (`H264_EncodeSPS`/`PPS` with
`h264_enc_putbits` and exp-Golomb), so the hardware encodes slice data
only.

One frame (`H264_EncodeFrameEnc`):

| step | registers | function |
|---|---|---|
| reset | 0xa8 ← 0, 0, 1 (with read-backs) | `H264_ResetVENC` |
| sequence | 0x04, 0x08, 0x0c (encode info), 0x24 (size in MBs), 0x28 (pitch, crop), 0x2c, 0x30, 0xf4, 0x12c, 0x1c0, 0x1e0–0x1f0 | `H264_SeqLvlMMRSetting` |
| frame | 0x10, 0x14–0x20, 0x124, 0x128 | `H264_FrmLvlMMRSetting` |
| work buffers | 0x8c, 0x90 (when field +0x94 is set) | |
| bitstream | 0x64 address >> 4, 0x68 size >> 7 | `H264_EncodeFrameEnc_SetBufs` |
| input | 0x6c Y >> 4, 0x70 chroma >> 4, third plane at a format-dependent register (default 0x130) | `_SetBufs` |
| recon / reference | 0x74–0x88 (luma, chroma and one more pair; current and reference swap each frame) | `H264_EncodeFrameEnc`, `H264_SearchReference` |
| go | 0xa4 ← 0x3f (interrupt enable), 0x00 ← mode (2 = frame) | `H264_SetHWModeSel` |
| wait | `VCODEC_WAITISR`, timeout 50–100 ms | `H264_WaitVENCDone` |

Buffer addresses are written shifted right by 4, so buffers are 16-byte
aligned (the bitstream size is in 128-byte units).

## The library running in emulation (2026-11-08)

`tools/re/emu32.py` loads 32-bit ARM Android libraries into Unicorn (REL,
APS2 and RELR relocations, ARM/Thumb calls, Python shims for imports);
`tools/re/emu_venc.py` drives the H.264 driver in `libvcodecdrv.so`
through its own interface (`h264_enc_init`, `h264_enc_setParameter` type 6
with the encoder settings, `h264_enc_encode` type 2 SPS, 3 PPS, 4 frame).
Its utility layer is shimmed: buffers get their emulated addresses as
device addresses, a fake page stands in for the registers, and the
interrupt wait reports "frame done". Ghidra loads the library at 0x10000,
so its addresses are the emulator's plus 0x10000.

```sh
venv/bin/python3 tools/re/emu_venc.py VENDOR/lib/libvcodecdrv.so --fmt 6 \
    --size 1280 720 --profile 1 --level 0xb --kbps 4000 --fps 30 --gop 30 \
    --frames 4 --read 0x98=0x6000
```

Settings struct (`setParameter` 6): word 0 input format (6 = NV12, scan
type 0; 3/5/10 are three-plane, scan type 3), 1 profile (1 Baseline,
4 Main, 0x10 High), 2 level (enum: 0xb → 3.1, 0xd → 4.0), 3/4 width and
height, 5/6 buffer pitch and height, word 12 a pointer to the rate
settings (GOP length, bit/s, 16-bit frame rate at byte 10). The driver
context passed to `h264_enc_init` carries the driver type at +0xb0
(`VAL_DRIVER_TYPE_H264_ENC` = 5). The library reads chip ID 0x20 for this
build, which selects register base 0x17020000.

SPS and PPS touch no register. Measured sequence for one frame, 1280×720
NV12, Baseline 3.1, 4 Mbit/s, 30 fps, GOP 30 (values from the trace,
meanings from the decompiled code):

| reg | value | meaning |
|---|---|---|
| 0x0a8 | 0, 0, 1 (read back) | reset |
| 0x004 | 0x5807801e | sequence encode info (GOP 30 in the low bits) |
| 0x008 | 0x626e5fae | search ranges and mode enables (table defaults) |
| 0x00c | 0x104e0000 (Main/High 0x924e0000) | more mode enables |
| 0x024 | (mbw·65536 + 256)·mbh + mbw | picture size in macroblocks |
| 0x028 | mbw << 21 \| pitch_mb << 11 \| crop_bottom | 1080p: 0x0f03c008 |
| 0x030 | CABAC 31, profile 30:29 (0 B, 1 M, 2 H), level 27:22, 21 (Main/High) | 0x07c00000 |
| 0x048 | 0x40000 \| kbit/s | hardware rate control on |
| 0x04c | fps << 16 \| 5 << 8 \| 3 | rate control: frame rate, QP steps |
| 0x050 | 0x3308 | QP limits |
| 0x12c | 0x1f4 | |
| 0x164, 0x168 | 0x11; per-row sizes from the width | 1280: 0xf400a2, 1920: 0x16d00f3 |
| 0x010 | I 0x10b406, P 0x108400 / 0x108500 | frame type: base 0x108400; I adds 0x3000\|4\|2 and toggles bit 3; P adds 0x100 except right after I |
| 0x014, 0x018 | frame number | |
| 0x01c | number of the last I frame | |
| 0x124, 0x128 | 0; 0x128 = 0x10000 for Main/High | |
| 0x08c, 0x090 | work buffers >> 4 | 0x4040 and 0x400 bytes |
| 0x068, 0x064 | bitstream size >> 7, address >> 4 | |
| 0x06c, 0x070 | input Y >> 4, UV >> 4 | |
| 0x074–0x088 | recon luma, chroma; reference luma, chroma; MV write, MV read | the two sets swap each frame |
| 0x0a4 | 0x3f | interrupt enable |
| 0x000 | I 0x0f60bff0, P 0x0fa0bff0 | mode: 0x0f20bff0 + bit 22 (first frame) or 23 |
| 0x0ec | 1 | |
| 0x058 | 4 | start |
| (wait) | interrupt status 4 frame done, 8 bitstream full | |
| 0x098 | read | bytes of slice data written |
| 0x0ec | 0 | |

Buffers the driver allocates at 1280×720: recon/reference luma 0x157400
and chroma 0x73000 bytes (two sets), motion vectors 0x7080 bytes
(8 bytes a macroblock, two), work 0x4040 and 0x400.

## Rate-control code

Register 0x8c points at a 0x4040-byte buffer the library fills from a blob
it carries (`H264_GetRCCode`; three variants, the default used here): code
for the encoder's rate-control processor (larb 4's `VENC_RCPU` port). The
same bytes at every size and bit rate. It is vendor firmware: extract it
from your own phone's `libvcodecdrv.so` and install it, never commit it:

```sh
venv/bin/python3 tools/re/emu_venc.py VENDOR/lib/libvcodecdrv.so --fmt 6 \
    --frames 1 --rc-out venc_rc.bin
scp venc_rc.bin root@PHONE:/lib/firmware/titan/venc_rc.bin
```

Register 0x90's 0x400-byte buffer starts zeroed (the processor's state).

## Test driver: titan_venc (2026-11-08, not yet run on the phone)

`modules/venc/titan_venc.c` (`build612.sh`): one NV12 frame at a time
through debugfs (`config`, `in`, `run`, `out`, `regs`), Baseline profile,
hardware rate control. It powers the VENC domain through runtime PM with
larb 4 linked, allocates the buffers with the library's sizes (reference
compression on, as the library runs it), loads `titan/venc_rc.bin`, and
writes SPS and PPS itself with the library's field choices.

Checked off the phone:

- Its SPS/PPS writer, compiled on the host, gives the library's bytes for
  1280×720 level 3.1 and 1920×1080 level 4.0 (with cropping).
- Its frame function, compiled on the host with logging stubs, writes the
  library's register sequence (same registers, order and values, buffer
  addresses aside) for 4 frames at 720p, 3 at 1080p and 33 frames across a
  GOP of 30 (the second I frame toggles 0x010 bit 3; 0x01c becomes 30).

DT (`mt6771.dtsi`): the VENC power domain under DISP, `vencsys` clocks
(`mediatek,mt8183-vencsys`, built in), larb 4 (added to the IOMMU's
larbs), the encoder node with its nine larb-4 ports, and the JPEG encoder
(disabled until tried). Interrupt status 0x05c, acknowledged a bit at a
time through 0x060 (the vendor's ISR); stop = pause (0xac), wait for status
0x10, reset.

Test on the phone: `tools/venc/venc-test.sh [frames] [w h kbps fps gop]`
encodes a synthetic moving pattern to `/tmp/venc-test.h264`.

## Plan

1. Field-level spec of all 45 registers from the decompiled code, for the
   case we need: NV12 input, baseline/main profile, I and P frames, rate
   control on, no ROI, no B frames.
2. DT: the `vencsys` clock node, the VENC power domain under scpsys, larb4,
   the VENC node with its IOMMU ports. The same domain also enables the
   hardware JPEG encoder (mainline `mediatek,mt8183-jpgenc`).
3. A debugfs test module (like `titan_dip`): one NV12 frame in, H.264 slice
   data out; check with ffmpeg after prepending our own SPS/PPS.
4. A V4L2 memory-to-memory (stateful) encoder, so GStreamer's
   `v4l2h264enc` and the camera app can record.

Hygiene: the vendor libraries and the decompiled output stay local; the
docs describe register semantics in our own words.
