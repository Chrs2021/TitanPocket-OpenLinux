# Cameras (OV16880 rear, OV8865 front)

Status (2026-10-15): **phase 2 done, phase 3 under way**. A V4L2 /
media-controller driver (`titan_camera`, loaded at boot) streams both
cameras at 30 fps, and **libcamera** (v0.7.2, built from source with a
small patch) turns them into RGB with its software ISP, debayering on the
GPU: `cam`, GStreamer's `libcamerasrc` and a live preview on the screen
work.
- Rear OV16880: 2336×1752. Front OV8865: 1632×1224 and 3264×2448.
- Output: 8-bit Bayer (`BA81`) or 16-bit words holding the 10-bit samples
  << 4 (`BG14`).
- Sensor controls: exposure, analogue gain, vertical blanking, focus (rear,
  AK7371 motor).
- Kernels #31/#32 add what libcamera needs from Linux 4.20/5.0
  (`/dev/udmabuf`, V4L2 buffer orphaning).
- **Camera** app (`titan-camera-gui`, app drawer): live preview, photos
  at full resolution, front/rear switch, **autofocus** (contrast detection,
  tap to focus) and manual focus (rear).
- The driver reports each sensor's location and mounting rotation, so
  libcamera knows them too.
- Not yet: the rear 4672×3504 mode (needs CAMSV 2-pixel mode), the flash
  LED, video recording, MediaTek's hardware ISP.

## Plan: an open V4L2 stack

The BSP's camera stack is Android-only. MediaTek's closed camera HAL
programs the MIPI receiver (seninf) and the ISP through register-poking
char devices (`/dev/seninf`, `/dev/camera-isp`, `/dev/kd_camera_hw`). The
two sensor drivers were never published. The target instead is the usual
mainline-style stack, with MediaTek's hardware ISP unused:

| Phase | Goal | Work |
|---|---|---|
| 0 ✅ | sensors answer | power rails, MCLK, reset/power-down; chip IDs over I2C |
| 1 ✅ | first raw frame | MIPI CSI-2 receiver (seninf) + a CAMSV raw DMA engine; stream one sensor mode; dump a Bayer frame |
| 2 ✅ | proper drivers | V4L2 + media controller: both sensors (from the stock init tables), seninf + CAMSV as the capture device, the focus VCM. Flash LED not done |
| 3 ✅ | camera app | libcamera (simple pipeline + software ISP: GPU debayer, AE, AWB), GStreamer, the Camera app |

## Hardware

| | rear | front |
|---|---|---|
| sensor | OmniVision **OV16880** (16 MP) | OmniVision **OV8865** (8 MP) |
| I2C | i2c2, **0x36** | i2c4, **0x10** |
| chip ID (0x300a-0x300c) | `01 68 80` | `00 88 65` |
| MCLK | CMMCLK0 = GPIO99 function 1, from `camtg_sel` | CMMCLK1 = GPIO100 function 1, from `camtg2_sel` |
| DOVDD 1.8 V | PMIC `vcamio` | PMIC `vcamio` |
| AVDD 2.8 V | external LDO, enable GPIO160 | PMIC `vcama1` |
| DVDD 1.2 V | PMIC `vcamd` | PMIC `vcamd` |
| AF 2.8 V | PMIC `vldo28`; VCM **AKM AK7371** at i2c2 0x0c | none (fixed focus) |
| PDN / RST | GPIO97 / GPIO101 | GPIO98 / GPIO102 |
| EEPROM | i2c2 0x50 (calibration) | |

Also on the boards: an LM3643 flash LED driver (i2c3 0x63). The DT has
nodes for more cameras (`camera_main_two`, `camera_sub_two`); this phone
has only two.

**Power-on sequence** (stock `sensor_power_sequence`, both sensors):
1. MCLK on, PDN low, RST low.
2. DOVDD, then AVDD, then DVDD. The OV16880 waits 1 ms between steps
   (5 ms after DVDD); the OV8865 waits 5 ms.
3. AF supply, PDN high, RST high.

**MCLK** is the topckgen mux `camtg_sel` (`TOP_MUX_CAMTG` = 5;
`camtg2_sel` = 30 for the front camera) with parent `univ_192m_d8`
(`TOP_UNIVP_192M_D8` = 101) = 24 MHz, routed out by the pin function.
- The BSP's seninf clock code expects these as named clocks on a node.
  Our DT has none, so it would fail.
- The clock framework reports 0 Hz because the PLL's rate isn't modeled.
  The sensors answer, so the clock runs.

**Where this came from:** the stock kernel (`vmlinux.elf`, see
[reverse-engineering.md](reverse-engineering.md)).
- `sensor_power_sequence` (`0xffffff80099a13d8`, 256-byte entries) gives
  the per-sensor order and delays.
- `imgsensor_custom_config` (`0xffffff80099a1168`, 104-byte entries) says
  which rail is a regulator and which a GPIO.
- The dtbo `__fixups__` map the supplies to PMIC LDOs.
- The kd_camera_hw1 pinctrl states give the GPIOs.

The stock sensor drivers' function tables are at `0xffffff80099a0ef8`
(OV16880) and `0xffffff80099a0f98` (OV8865); the modes come from there.
The OV16880 init registers were recovered earlier into
`ov16880_sensor_init.c`.

## titan_camprobe (phase 0 tool)

`modules/camera/camprobe/`: an out-of-tree module (build with `build.sh`)
that powers one sensor using the BSP imgsensor node's pinctrl states, PMIC
regulators by name, and the camtg clocks taken straight from topckgen. It
then reads the chip ID at 0x36 and 0x10.

```sh
insmod titan_camprobe.ko cam=0   # rear;  cam=1 front. Stays loaded (powered) if the ID matches
rmmod titan_camprobe             # power off
```

## Kernel pieces in the BSP

- `drivers/misc/mediatek/imgsensor/src/mt6771/`: power, GPIO and MCLK
  tables (`camera_hw/`) and `seninf/seninf_clk.c` (clocks only; the
  register programming is in the HAL, `libcam.halsensor.so`).
- `drivers/misc/mediatek/cameraisp/src/isp_50/`: the ISP 5.0 driver
  (MT6771/MT6775). It's a char device for the HAL; `inc/cam_regs.h` has
  the CAM/CAMSV register map.
- **Register blocks:**
  - camsys 0x1a000000;
  - ISP pass 1: CAM_UNI 0x1a003000, CAM A/B/C 0x1a004000, 0x1a006000,
    0x1a008000 (see "MediaTek ISP" below);
  - CAMSV1-6 at 0x1a050000 + n·0x1000;
  - seninf 0x1a040000 to 0x1a047000.

  This layout matches MT8183, whose open ChromeOS seninf driver is the
  reference for the MIPI receiver.

## Phase 1: raw frames (titan_camcap)

`modules/camera/camcap/` (build with `build.sh`). One out-of-tree module
does the whole path:

1. Power the sensor (as camprobe).
2. Power camsys from the clock providers by index (`scpsys`
   SCP_SYS_DIS/CAM; `topckgen` cam_sel/seninf_sel; `camsys` CAM, CAMTG,
   SENINF, LARB6, CAMSV0-2 gates) plus the SMI larb6
   (`smi_bus_prepare_enable`).
3. Set up the CSI-2 receivers, load the stock sensor init and mode
   tables, set exposure and gain, and route the receiver through seninf
   mux 0 into a CAMSV.

```sh
insmod titan_camcap.ko cam=1 lines=1224    # front, full frame; exposure= gain= sv=
cat /sys/kernel/debug/titan_camcap          # receiver / CAMSV status
cat /sys/kernel/debug/titan_camcap_frame > f.raw
tools/camera/raw2png.py f.raw f.png        # --gray, --order BGGR|RGGB...
rmmod titan_camcap
```

**Sensor tables:** `tools/re/emu_sensor.py` runs the stock driver's
functions (`sensor_init`, `preview`, `capture`) in Unicorn and logs every
`iWriteRegI2C`. This recovered the OV8865 tables in `ov8865_regs.h`.
- Preview: 1632×1224, 4 lanes, 0x3018 = 0x72.
- Capture: 3264×2448.
- The stock mode leaves exposure at 32 lines; the HAL's AE set the real
  value. The module sets 1216 lines and 4× gain.

**MIPI receiver (seninf):** the MT8183 ChromeOS sequence works as is
(`seninf_reg.h` is from there).
- The front OV8865 is on **CSI1** (seninf3).
- Seninf mux 0 measures the frame in `SENINF1_MUX_DEBUG_2`
  (`0x066004c8` = 1632×1224); `MUX_SIZE` stays 0.
- After start-up the CSI-2 status settles at `0x9044`.

**CAMSV** (0x1a050000 + n·0x1000): the register map is MT6768's with its
blocks moved (DMA at 0x000, TG at 0x230, TOP at 0x500). The sequence was
read from the HAL's `isp_function_camsv.cpp` (Ghidra; `libimageio_plat_drv.so`):

| block | registers |
|---|---|
| TOP | reset `0x520` = 4, 0, 1, wait 3, 0; `MODULE_EN 0x510` \|= 0x40000001 (TG, double buffer), \|= 0x4 packer, \|= 0x10 IMGO; `FMT_SEL 0x514` = 1 (RAW10); `INT_EN 0x518` = 0x1304b1; `PAK 0x54c` = 1; `CLK_EN 0x530` \|= 0x8005 |
| TG | `SEN_MODE 0x230` bit 16, bit 0 = CMOS_EN; `VF_CON 0x234` bit 0 = start, bit 1 = single frame; `GRAB_PXL 0x238` = x \| (x+w)<<16; `GRAB_LIN 0x23c` = y \| (y+h)<<16 |
| IMGO DMA | `BASE 0x020`, `XSIZE 0x030` = bytes−1, `YSIZE 0x034` = lines−1, `STRIDE 0x038` = stride \| 0x1010000 (16-bit bus), `CON 0x03c..0x044` = 0x80000080, 0x20002, 0x20002; `0x400` \|= 1 |
| status | `INT_STATUS 0x51c`: bit 0 VS, 7 SOF, 10 HW pass-1 done, 20 SW pass-1 done, 16 IMGO error, 17 overrun; `TG_INTER_ST 0x26c` (frame counter in the top byte); measured size at `0x268` |

- **Run continuous, without FBC.** Single-frame mode with FBC reports a
  finished frame but writes nothing; the first frame seems to use the
  not-yet-latched shadow address. Continuous mode with `FBC_CTL1` = 0
  overwrites one buffer every frame.
- **Buffer:** `dma_alloc_coherent` on the imgsensor device, mapped through
  the M4U with `m4u_alloc_mva` (found via kallsyms) on port CAM_SOCO
  (larb6 port 11).
  - The CAMSV outputs are larb6 ports 11-13 (CAM_SOCO/SOC1/SOC2, M4U IDs
    72-74).
- **Output format:** MediaTek packed RAW10, a little-endian bitstream (4
  pixels per 5 bytes). The Bayer order is probably BGGR, to be confirmed
  with a colorful scene.

### Rear camera (OV16880)

- **Modes:** the stock tables are emulated in `ov16880_regs.h`.
  - Init: 1241 writes.
  - Preview: 2336×1752.
  - Capture: 4672×3504.
  - Video: 4272×2404.
  - These tables don't start streaming; write `0x0100 = 1` afterwards.
- **Link:** CSI0, 4 lanes.
- **Black level:** 64, against 16 on the OV8865 (`raw2png.py --black 64`).
- **Buffers:** a preview frame is 5 MB, more than the kernel can give
  contiguously without CMA. The module builds every buffer from single
  pages, maps them to one M4U address (`m4u_alloc_mva` with a scatter
  list), and `vmap`s them write-combined for the CPU.
- **Focus motor (AK7371, i2c2 0x0c):**
  - Powered by `vldo28` 2.8 V.
  - It comes up in standby (`0x02 = 0x40`); write `0x02 = 0x00` to make it
    active.
  - Position is 10 bits: `0x00` = pos[9:2], `0x01` = pos[1:0] << 6.
  - 0 = far end; ~256 focused at about 30 cm; 1023 = closest.
  - Not the FP5510 the stock kernel also contains. DW9714-style writes are
    acknowledged but land in the AK7371's registers.
- **Module:** `insmod titan_camcap.ko cam=0 focus=256` (mode=1 for full
  resolution).

## Phase 2: the V4L2 driver (titan_camera)

`modules/camera/v4l2/` (build with `build.sh`; the rootfs installs it in
`/lib/modules/.../extra` and loads it at boot from
`/etc/modules-load.d/titan-camera.conf`). One module, no device tree
changes:

```
"ov16880 2-0036"  rear,  /dev/v4l-subdev0 ──[enabled]──┐
                                                         ├──> "titan-camsv"  /dev/video0
"ov8865 4-0010"   front, /dev/v4l-subdev1 ──[off]──────┘
```

The entity names follow the kernel's `model bus-addr` convention
(libcamera parses the sensor model from it).

- **Choosing a camera:** enable one sensor link (the video node refuses a
  second one with `EBUSY`), then set a size the sensor has:

  ```sh
  media-ctl -d /dev/media0 -l '"ov16880 2-0036":0 -> "titan-camsv":0 [0], "ov8865 4-0010":0 -> "titan-camsv":0 [1]'
  v4l2-ctl -d /dev/video0 --set-fmt-video=width=1632,height=1224,pixelformat=BA81 \
      --stream-mmap=4 --stream-count=30 --stream-to=front.raw
  ```

  Each sensor starts in its first mode (the sizes above). Other modes are
  set on the sensor subdev, e.g.
  `media-ctl -V '"ov8865 4-0010":0 [fmt:SBGGR10_1X10/3264x2448]'`.
  Stream-on fails with `EPIPE` if the video node's size doesn't match the
  linked sensor's mode.
- **Formats:** seninf hands CAMSV the pixels MSB-aligned on a 14-bit bus.
  - `BA81` (default): TG format RAW8 and the packer in RAW8 mode keep the
    top 8 bits. This is what libcamera's software ISP takes.
  - `BG14`: TG format RAW10, packer off, `MODULE_EN` bit 3 (the HAL's
    setting for non-Bayer formats): the bus is stored as is, 10-bit
    samples << 4 in 16-bit words. Full precision, for raw tools
    (`tools/camera/raw2png.py`). 4.14 knew no `BG14`; kernel #31 adds its
    name to `v4l2-ioctl.c`.
  - CAMSV's packed RAW10 is a little-endian bitstream, not V4L2's CSI-2
    packing, so it isn't offered.
- **Controls** on the sensor subdevs: `exposure` (lines), `analogue_gain`
  (128 = 1×, up to 1984), `vertical_blanking`, read-only
  `horizontal_blanking` and `pixel_rate`; the rear one also
  `focus_absolute` (0 = far, 1023 = closest). They're written in one
  sensor group hold. Read-only `camera_orientation` (back/front) and
  `camera_sensor_rotation` (rear 270, front 90: the correction,
  counter-clockwise, for the phone held upright in portrait) use 5.8's
  control IDs, registered as custom controls. The pixel array (crop bounds, native size) is
  reported through the subdev selection API.
- **Buffers:** videobuf2 scatter-gather (`VIDEOBUF2_DMA_SG`, made
  selectable in kernel #30). Each buffer is mapped to one M4U address on
  port CAM_SOCO. IMGO's base address is double-buffered: at each start of
  frame the driver programs the next queued buffer, and completes a buffer
  at frame done. With nothing queued, the current buffer is overwritten
  (that frame is dropped).
- **Power:** camsys (clocks, SMI larb6) is reference-counted and held
  while buffers are mapped or the stream runs: M4U operations write larb6
  registers, which hang the bus when camsys is off. The sensor is powered
  only while streaming, and the IRQ is requested only then.
- **Binding:** the driver takes the CAMSV1 platform device from the BSP's
  camera-isp driver (`driver_override`). The V4L2 and media cores need a
  real bound driver (they read `dev->driver` for names and module
  references).

Bugs met on the way, each of which hung or crashed the phone:
- M4U mapping with camsys off: bus hang.
- 32-bit DMA mask: `vb2-dma-sg` bounced through swiotlb and failed with
  `ENOMEM`. The M4U does the addressing, so the mask is 64-bit.
- Taking the queue lock in `start_streaming`/`stop_streaming` (vb2 already
  holds it) and in a `link_validate` that ran inside
  `media_pipeline_start`: deadlocks.

### udmabuf (kernel #31)

libcamera's software ISP allocates its output buffers from a DMA heap or
`/dev/udmabuf`; 4.14 has neither. `drivers/dma-buf/udmabuf.c` is Linux
4.20's driver backported (`CONFIG_UDMABUF=y`): 4.14's `dma_buf_export()`
also wants the `map`/`map_atomic` callbacks, memfd seals come from
`shmem_get_seals()`, and fault handlers return `int`.

### V4L2 buffer orphaning (kernel #32)

libcamera allocates buffers on the capture node, exports them as
dma-bufs and frees the queue (`REQBUFS(0)`) while keeping the exported
buffers. 4.14 refused that with `EBUSY` as long as a buffer was mapped or
exported; Linux 5.0 (commit d65842f7126a) allows it, because the
allocators reference-count the memory anyway. `videobuf2-core.c` carries
that change.

## Phase 3: libcamera

libcamera v0.7.2, built on the phone by `userspace/libcamera/build.sh` into
`/usr/local` (Debian 12's libcamera 0.0.3 predates the software ISP). Its
simple pipeline handler drives the media graph; the software ISP debayers
on the GPU (OpenGL ES 3.1 on the Mali-G72 through Panfrost; from the
`BA81` stream) and runs auto exposure and white balance. Output: RGB at the
sensor mode's size (rear 2332×1752, front 1632×1224) at 30 fps.

```sh
cam -l
#  1: Internal back camera (platform/1a050000.camsv1/rear ov16880)
#  2: Internal front camera (platform/1a050000.camsv1/front ov8865)
cam -c 2 --capture=10 --stream role=viewfinder,pixelformat=BGR888 --file="front-#.ppm"

# live preview on the screen
export GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0
gst-launch-1.0 libcamerasrc camera-name="platform/1a050000.camsv1/rear ov16880" ! \
    video/x-raw,format=BGRx,width=1168,height=876 ! queue ! waylandsink fullscreen=true
```

**The libcamera patch** (`userspace/libcamera/0001-titan-pocket.patch`):
- the simple pipeline handler accepts the `titan-camera` media device,
  with the software ISP;
- the V4L2 version check accepts 4.14 (v5.0 was required for buffer
  orphaning, which kernel #32 has);
- an OV16880 sensor helper (gain code 128 = 1×, black level 64 at 10
  bits) and its properties (1.0 µm pixels). The OV8865 is already known.
- a `libcamerasrc` fix: closing the element didn't disconnect it from the
  camera's `requestCompleted` signal. The Camera object is shared through
  the CameraManager, so the next pipeline on that camera (or the same
  element reopened) also ran the stale handler on an empty request queue:
  an assertion, then a crash freeing the wrong request. The app hit it at
  every photo (preview stopped, still pipeline started).

**What the driver needed for libcamera:**
- `V4L2_CAP_IO_MC` on the video node (else libcamera won't ask for the
  formats matching the sensor's bus code), and `VIDIOC_ENUM_FRAMESIZES`;
- the sensor selection rectangles (pixel array);
- `V4L2_CID_CAMERA_ORIENTATION` and `V4L2_CID_CAMERA_SENSOR_ROTATION`
  (5.8's IDs, registered as custom controls): libcamera's `Location` and
  `Rotation` properties;
- **a device per sensor:** libcamera names a camera after the device
  behind the sensor's subdev node, and both nodes hung off CAMSV1 (two
  cameras with one ID: libcamera aborts). The driver moves each node under
  a child device, `.../1a050000.camsv1/rear` and `.../front`
  (`device_move()`; registering the nodes itself would need the unexported
  `v4l2_subdev_fops`).

libcamera still warns that there are no tuning files (`uncalibrated.yaml`
is used) and no sensor delay values. It can't rotate by 90°, so
applications rotate the picture themselves (the Camera app does).

## The Camera app (titan-camera-gui)

`userspace/debian/overlay/usr/local/bin/titan-camera-gui` (Python, GTK 3,
GStreamer), **Camera** in the app drawer.

- **Preview:** `libcamerasrc` at 960×720, rotated (mirrored for the front
  camera) by `glvideoflip` and shown by `gtkglsink`, so after the ISP
  everything stays on the GPU.
- **Photo:** one stream at a time, so the shutter stops the preview, runs
  the camera at its largest RGB size (rear 2332×1752, front 3260×2448: the
  OV8865's full mode), drops the first 15 frames while auto exposure
  settles (it converges in about 8), rotates, encodes one frame as JPEG
  (quality 92) into `~/Pictures/Camera/IMG_<date>_<time>.jpg` and restarts
  the preview: about 2 s. The thumbnail shows the last photo; tap it to
  view it.
- **Rotation:** from the driver's `camera_sensor_rotation` (clockwise in
  the app: rear 90°, front 270°, both checked upright on the phone; the
  stock HAL's angles weren't found, MTK compiles them into its custom
  code). The rotate button overrides it per camera
  (`~/.config/titan-camera/state.json`), for photos too.
- **Autofocus** (rear; the front camera is fixed-focus), contrast
  detection in the app:
  - a second preview branch hands the frames (the same BGRx buffers) to an
    appsink; sharpness is the gradient energy of the focus region's green
    channel (30 % of the frame, binned 2×2, normalised by brightness);
  - the lens (`FOCUS_ABSOLUTE` on the sensor subdev; libcamera leaves it
    alone) sweeps 0–1023 in steps of 64, two frames per position, stopping
    once sharpness is well past the peak, then ±48 in steps of 16, four
    frames each: 2–3 s;
  - after a move, only frames whose capture began after the lens settled
    count (sensor timestamps against the pipeline clock), since the ISP
    adds latency;
  - no clear peak (best < 1.25× worst): "Low contrast", the lens stays;
  - focused, it measures every third frame and refocuses when the median
    sharpness stays below 60 % of the focused level for ~3 s;
  - tap the preview to focus there; dragging the Far–Near slider switches
    to manual focus (MF) until the next tap. A photo keeps the focus;
  - CPU: the measurement thread takes ~5 %; the app ~55 % with preview.
- Keys: Space/Enter photo, F switch camera, R rotate, A autofocus.

## MediaTek ISP (hardware), work in progress

The camera path so far bypasses MediaTek's ISP: CAMSV only stores raw
frames, and libcamera's software ISP does the rest. The ISP has two parts:

- **Pass 1 (CAM A/B/C):** takes the sensor stream from seninf. Black level
  (OBC), bad-pixel and noise correction (BNR), lens shading (LSC, table read
  by the LSCI DMA), white-balance gains (RPG), a resizer (RRZ, output RRZO),
  full-size output (IMGO), and hardware statistics for exposure and white
  balance (AA), flicker and **autofocus** (AF, output AFO).
- **Pass 2 (DIP):** demosaic, colour matrix, gamma, noise reduction,
  sharpening, YUV at any size.

No open code programs either. MediaTek's kernel driver (`isp_50`) is a
register-access char device for its closed HAL; on MT8183 (ChromeOS) the
configuration runs in co-processor firmware, and its kernel driver only
touches 22 pass-1 registers (interrupts, frame-buffer control, DMA error
status, the command queue base). So the sequences have to come from the
HAL, as for CAMSV.

**Register map (pass 1):** the HAL's `libcamdrv_twin.so` (which splits wide
frames across two CAM blocks) dumps registers with their names and
physical addresses; its Ghidra decompile (`out/camera/re/twin.c`) gives 927
named registers, 309 per CAM block, consistent with ChromeOS where they
overlap (`CTL_EN` 0x4, `CTL_DMA_EN` 0x8, `CTL_FMT_SEL` 0xc).
`modules/camera/isp/cam_p1_regs.h` has them, plus ChromeOS's.

| block | base |
|---|---|
| CAM_UNI (shared) | 0x1a003000 |
| CAM_A | 0x1a004000 |
| CAM_B | 0x1a006000 |
| CAM_C | 0x1a008000 |

Each CAM block (0x2000) has 40 function blocks, in pipeline order: CTL
0x004, TG 0x230, DMX 0x330, RMG 0x350, RMM 0x390, **OBC 0x3f0**, **BNR
0x420**, SCM 0x4b0, **RPG 0x4c0**, **RRZ 0x4e0**, RMX 0x540, BMX 0x580,
**LSC 0x5d0**, **AF 0x610** (37 registers), RCP 0x800, SGG1 0x810, VBN, AMX,
BIN, DBN, DBS 0xac0, SL2F 0xb00, ADBS 0xd50, CPN/DCPN, SGM 0xfa0; then the
DMA ports from 0x1000: IMGO 0x1020, RRZO 0x1050, AFO 0x10b0, UFEO, BPCI
0x1180, LSCI 0x11d8, UFGO 0x1320.

Not in that library yet: the AE/AWB statistics block (AA, AAO), flicker
(FLK), the TG details, interrupts, the command queue (CQ) and frame-buffer
control (FBC). The HAL normally writes pass-1 registers through **CQ**
buffers in memory that the hardware loads each frame (ChromeOS's
`CQ_THR0_BASEADDR` 0x198), which is why few writes show up as plain MMIO.

**Plan:**
1. Route seninf's mux to CAM A's TG instead of CAMSV and get a raw frame
   out of IMGO with every processing block off (the CAMSV result, through
   the ISP).
2. Turn on AF statistics (AFO) and use them for autofocus instead of the
   app's software measurement.
3. OBC, LSC (tables from `libCamera_ov16880mipiraw_*.so`), RPG, and the AA
   statistics for exposure and white balance; the rear 4672×3504 mode.
4. Pass 2 (DIP) with the stock tuning.

