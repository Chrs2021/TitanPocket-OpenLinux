# Hardware video encoder (VENC)

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
from your own phone's `libvcodecdrv.so` (the 32-bit one, in the vendor
partition's `lib/`) and install it, never commit it. `tools/re/emu_venc.py`
runs the library in `tools/re/emu32.py` (Python with the `unicorn`
package) and writes the bytes the driver loads:

```sh
venv/bin/python3 tools/re/emu_venc.py VENDOR/lib/libvcodecdrv.so --fmt 6 \
    --frames 1 --rc-out venc_rc.bin
scp venc_rc.bin root@PHONE:/lib/firmware/titan/venc_rc.bin
```

Register 0x90's 0x400-byte buffer starts zeroed (the processor's state).

## The V4L2 encoder (2026-11-19; works on the phone)

`titan_venc` is now a V4L2 stateful memory-to-memory encoder, in the 6.12
tree (`drivers/media/platform/mediatek/mt6771-venc`,
`CONFIG_VIDEO_MT6771_VENC`; `modules/venc/build612.sh` builds it out of tree
against the running kernel's build). The register sequence below is
unchanged; what is new is around it:

- **OUTPUT**: NV12 (one plane) or NV12M (two), the luma pitch the width
  rounded up to 16 and the chroma after a 16-line-aligned luma, as the
  encoder reads them. MMAP or DMABUF; addresses must be 16-byte aligned.
- **CAPTURE**: H.264 Constrained Baseline, Annex B, the SPS and PPS in
  the IDR frame's buffer (`V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME`),
  key frames flagged. The SPS now sets `constraint_set0` and `1` (the
  stream is Constrained Baseline); the hardware's own level field keeps
  the library's 3.1 or 4.0.
- **Controls**: bit rate (CBR, the encoder's rate control, taken at the
  next frame), GOP size, force key frame, profile, level, header mode, no
  B frames; the frame rate through `S_PARM` on OUTPUT (1-60 fps).
- **Draining**: `V4L2_ENC_CMD_STOP` through the mem2mem helpers. The last
  frame comes in a buffer of its own, then an empty buffer flagged
  `V4L2_BUF_FLAG_LAST` and an EOS event (GStreamer 1.22 drops the data of
  a LAST buffer). A frame's buffers stay on the ready queues until it is
  done, so a STOP during the last frame still drains it.
- **Sizes**: 64x64 up to 1920x1088's macroblocks either way up, so a
  portrait 1080x1440 recording fits (checked on the phone, below).
- The encoder powers up (runtime PM, autosuspend after 200 ms) when a
  stream's first frame comes and down when either queue stops; the
  rate-control code (`titan/venc_rc.bin`) is loaded then. debugfs
  `titan_venc/stats` has the frame and error counts and the last frame's
  line (type, bytes, status, time).
- A CAPTURE buffer larger than the driver's (W×H + 256 bytes) is kept when
  the client asks for one, up to 8 MiB, also across a later OUTPUT
  `S_FMT` (GStreamer asks for 2 MiB and refuses smaller buffers).
- A media device, so the node reports itself as an encoder
  (`MEDIA_ENT_F_PROC_VIDEO_ENCODER`), and the `MIN_BUFFERS_FOR_OUTPUT`
  control (1).

GStreamer's `v4l2h264enc` takes it as it is; the camera app already uses
`v4l2h264enc` when it exists (with a GOP of two seconds), so recording
moves from x264 on the CPU to the encoder once the module loads.
`tools/venc/venc-test.sh [frames] [w h kbps fps gop]` runs `v4l2-compliance`
when installed, encodes GStreamer's test pattern (`PATTERN=`, default
`ball`) through `v4l2h264enc` into `/tmp/venc-test.mp4`, and decodes it
again with libavcodec, or openh264 when gst-libav is missing, counting
frames. The module needs `v4l2-mem2mem` and `videobuf2-dma-contig`
(modules of the 6.12 build); installed in `/lib/modules/*/extra` with
`depmod`, it loads at boot from the DT node.

**On the phone (2026-11-19, kernel #151).**

- `v4l2-compliance` (Debian 12's v4l-utils): 41 of 46 pass. The five
  failures are the older tool against a 6.12 kernel: it expects NV12's old
  description (`Y/CbCr 4:2:0`, now `Y/UV 4:2:0`) and then misses NV12 in
  the format tests that follow, and it wants `CREATE_BUFS`'s reserved
  field zero where 6.12 put `max_num_buffers`.
- GStreamer `videotestsrc ! v4l2h264enc`, decoded again with openh264:
  every frame comes back (3, 40 and 41-frame streams at 320×240, 1280×720
  and 1920×1080; 300 frames at 720p; 150 at 1080p); 0 errors in over a
  thousand frames.
- Rate control holds the target on a busy picture (SMPTE bars): 4053
  kbit/s for 4000 at 720p, 8205 for 8000 at 1080p. A nearly flat picture
  (`ball`) stays far below it (110 kbit/s), as CBR without padding does.
- Hardware time per frame: about 8 ms at 720p, 18 ms at 1080p.
- Picture, as luma PSNR of the decoded frames against the source at
  6 Mbit/s, the same against x264 (`ultrafast`, Constrained Baseline): 720p
  45.5 dB (x264 44.0), 1080p 30.3 (31.1), 1080×1440 portrait 32.4 (34.4);
  the bottom quarter of the SMPTE pattern is noise and costs both the
  same. No stride, crop or offset error at any size, the 1080-wide
  portrait (padded pitch) and 1080-high (cropped) frames included.
  Chroma is coarser than x264's (about 40 dB against 58-62), the same at
  every size: a quantization choice, not a misplaced plane.
- CPU, 300 frames (10 s) of 720p, less the 1.37 s the test source itself
  costs: 1.3 s with the encoder (copying frames into its buffers), 8.2 s
  with x264 `ultrafast`, 27.7 s with `veryfast`.

Two bugs found on the phone, both fixed: GStreamer's pool failed until
the larger CAPTURE size above was kept, and every `rmmod` leaked one
enable of the encoder clock. The driver core resumes the device for
`remove()`, and with runtime PM switched off in `remove()` its put never
suspends it again. The clock stays counted as on while the power domain,
going off, resets the vencsys gate, which the clock framework then never
reopens: every register reads 0 and no frame finishes (status 0, 200 ms
timeout), with the old debugfs driver as much as the new one, until a
reboot. `remove()` now suspends the device itself (`titan_vdec` had the
same bug and has the same fix).

## The first test driver: debugfs (2026-11-08; worked on the phone, 2026-11-09)

The first version, now replaced (its source is in the public repository's
history as `modules/venc/titan_venc.c`): one NV12 frame at a time
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

It was tested with an earlier `tools/venc/venc-test.sh`, which wrote a
synthetic moving pattern through debugfs.

**On the phone (2026-11-09, kernel #115).** The first run worked:
- 320×240, 5 frames: an I frame and four P frames, each ending with
  status 0x4 (frame done), 1.3-1.5 ms of hardware time a frame.
  Decoded with GStreamer's `openh264dec` on the phone, the frames are the
  test pattern: the gradient, the box 12 pixels further right each frame,
  the chroma following the script.
- 1280×720, 60 frames at 4000 kbit/s, GOP 30: every frame status 0x4, I
  frames at 0 and 30, 4063 kbit/s at 30 fps (the hardware rate control
  holds the target), about 10-12 ms a frame, and the whole 1 MB stream
  decodes without an error.

So the register sequence, the buffers, the RC microcode and the SPS/PPS
writer are right as they stand; the V4L2 encoder above keeps them.

## Plan

1. Field-level spec of all 45 registers from the decompiled code, for the
   case we need: NV12 input, baseline/main profile, I and P frames, rate
   control on, no ROI, no B frames.
2. DT: the `vencsys` clock node, the VENC power domain under scpsys, larb4,
   the VENC node with its IOMMU ports. The same domain also enables the
   hardware JPEG encoder (mainline `mediatek,mt8183-jpgenc`).
3. ✅ A debugfs test module (like `titan_dip`): one NV12 frame in, H.264
   out (2026-11-09, above).
4. ✅ A V4L2 memory-to-memory (stateful) encoder, so GStreamer's
   `v4l2h264enc` and the camera app can record (2026-11-19, above). Still
   to do: a camera recording, battery current against x264, and a clean
   boot with the module loading from `/lib/modules` (it was run in a boot
   whose clock count an earlier unload had leaked, with the gate opened by
   hand).

Hygiene: the vendor libraries and the decompiled output stay local; the
docs describe register semantics in our own words.
