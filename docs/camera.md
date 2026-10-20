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
- **Flash:** the dual-tone LED pair works (Off / Auto / On / Torch in the
  app).
- The rear camera's full 4672×3504 mode runs in 2-pixel mode (photos
  4668×3504, 16 MP).
- Not yet: video
  recording, the rest of MediaTek's hardware ISP.

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

### Full resolution: 2-pixel mode (rear 4672×3504)

The OV16880's capture mode (4672×3504, line length 5024, about 576 Mpixel/s
at 30 fps) is too fast for one pixel per clock, so seninf and CAMSV move
two pixels per clock in that mode (`sensor_mode.pix2`). From the HAL
(`setSeninfMuxCtrl`; `isp_function_camsv.cpp` TG, TOP and `BusSizeCal`):

| Block | 1 pixel | 2 pixels | 4 pixels (unused) |
|---|---|---|---|
| seninf `SENINF1_MUX_CTRL` bit 8 / `MUX_CTRL_EXT` bit 4 | 0 / 0 | 1 / 0 | 0 / 1 |
| CAMSV `TG_SEN_MODE` bits 1, 13 | 0, 0 | 1, 0 | 1, 1 |
| CAMSV `PAK` bits 4-5 | 0 | 1 | 2 |
| IMGO bus size, `STRIDE` bits 16-18 (raw formats) | 1 | 3 | 7 |

- Raw stream: 4672×3504 `BA81` at 20.6 fps with libcamera's default
  vertical blanking (frame length 5576), the same field of view as the
  2336×1752 mode, no tearing or line slips.
- libcamera offers RGB up to 4668×3504 and picks the smallest sensor mode
  that covers the request, so the 960×720 preview keeps the 2336×1752
  mode. The software ISP does ~9.5 fps at 16 MP.
- The CAM A AF statistics only run in the 2336×1752 mode (their table is
  for it; CAM A would need 2-pixel mode too).

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
- **manual exposure in the software ISP's AE** (`src/ipa/simple/algorithms/agc.cpp`):
  - The upstream AE ignores `AeEnable`, `ExposureTimeMode`,
    `AnalogueGainMode`, `ExposureTime` and `AnalogueGain` and doesn't
    advertise them.
  - The patch registers them and stores them per request. When manual,
    it uses the requested time (converted to lines, clamped) and gain
    instead of its own step.
  - Back in auto, AE continues from the last manual values.
- **another `libcamerasrc` fix:** controls set while streaming were lost.
  - `queueRequest()` merged the pending controls into a new request and
    cleared them before acquiring buffers.
  - When no buffer was free (usual while streaming) the request was
    dropped, and its controls with it.
  - The controls are now applied only once the request is certain to be
    queued, and take effect about 250 ms after being set.

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

## Flash (MT6370, dual tone)

The rear flash is a **dual-tone pair** on the MT6370 PMIC's two flash
channels: channel 1 is the white (cool) LED, channel 2 the yellowish (warm)
one. The device tree also has an LM3643 node (i2c3 0x63), a leftover of
MediaTek's reference design: no driver binds it, and the stock kernel only
builds the MT6370 flashlight driver.

- **Interfaces:** Richtek's flashlight class
  (`/sys/class/flashlight/mt-flash-led1`, `-led2`: brightness only; `mode`
  is read-only and stays "Off" even while lit) and MediaTek's
  `flashlight-core` above it: `/dev/flashlight` ioctls (`flashlight.h`:
  `struct {type_id, ct_id, arg}`, 1-based: type 1 = rear, ct 1/2 = the
  channels) and `/sys/class/flashlight_core/flashlight/flashlight_strobe`
  ("TYPE CT PART LEVEL DURATION_ms", 0-based, lights for the duration
  inside the write).
- **Levels** (`flashlights-mt6370.c`): 0–15 torch, 25–400 mA in 25 mA
  steps; 16–31 strobe, 450–1200 mA (strobe has a hardware timeout,
  2.4 s at most).
- **The trap: coupled mode.** The default scenario is "coupled", and
  `mt6370_operate()` only switches anything on once **both** channels have
  a state; each `flashlight_strobe` write and each driver enable resets
  both to "none". So one channel alone does nothing (the enable register,
  `FLEDEN` 0x7E on i2c-5 0x34, stays 0x08). Either set the decoupled
  scenario (`FLASH_IOC_SET_SCENARIO` = camera | decouple = 3: one channel
  at a time) or give both channels a level and an on/off (coupled: both
  LEDs together, `FLEDEN` 0x0B). Channel 1 alone reads 0x0A, channel 2
  0x09.
- **Sequence** (as the stock HAL): `FLASHLIGHTIOC_X_SET_DRIVER` 1,
  `FLASH_IOC_SET_SCENARIO`, per channel `SET_TIME_OUT_TIME_MS` and
  `SET_DUTY`, then `SET_ONOFF` 1 for both; off: `SET_ONOFF` 0 both,
  scenario 0, `SET_DRIVER` 0.

## The Camera app (titan-camera-gui)

`userspace/debian/overlay/usr/local/bin/titan-camera-gui` (Python, GTK 3,
GStreamer), **Camera** in the app drawer.

- **Preview:** `libcamerasrc` at 960×720, rotated (mirrored for the front
  camera) by `glvideoflip` and shown by `gtkglsink`, so after the ISP
  everything stays on the GPU.
- **Photo:** one stream at a time, so the shutter stops the preview, runs
  the camera at its largest RGB size (rear 4668×3504 from the sensor's
  full-resolution mode, front 3260×2448: the OV8865's full mode), drops
  the first 15 frames while auto exposure settles (it converges in about
  8), rotates, encodes one frame as JPEG (quality 92) into
  `~/Pictures/Camera/IMG_<date>_<time>.jpg` and restarts the preview.
  Front about 2 s; rear about 4 s (the software ISP debayers 16 MP at
  ~9.5 fps). The thumbnail shows the last photo; tap it to
  view it. All photos are browsable in the **Photos** app
  (`titan-gallery`, desktop.md).
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
- **Flash** (rear camera, button beside rotate; T): Off → Auto → On →
  Torch, both LEDs together (coupled). On: 400 mA each (the torch
  maximum) for the whole photo capture, since the still pipeline needs
  ~4 s at full resolution, longer than a strobe; a 6 s hardware
  timeout backs it up (measured: lit 3.7 s of a 3.8 s capture). Auto: fires when auto exposure has raised the
  sensor's analogue gain to 4× or more. Torch: 175 mA each while the
  app runs; off on another mode, the front camera, or quitting.
- **Flash exposure (pre-flash metering)**, for flash photos (Auto when
  it fires, On):
  - **The problem:** libcamera's software AE starts a still capture from
    the dark-scene settings (full exposure, ~15× gain) and moves at most
    10–15 % a frame (a gain step about every 4 frames). With the flash on,
    every frame up to the saved one was 100 % clipped; reaching a
    flash-lit exposure would take hundreds of frames.
  - **Metering:** with the preview still running, the app switches it to
    manual exposure (400 µs, gain 1×), lights the flash at full power and
    reads the hardware AF statistics. Their block pixel sums (word 3,
    before any processing) are linear: raw / 8 per pixel, black about 8,
    clipping at about 119.
  - **Timing:** a control change reaches the sensor a few requests later,
    so the app waits until the sensor's exposure register shows the new
    value, then 3 frames.
  - **Stepping:** while the 97th-percentile block clips, the exposure
    drops 8× (down to 40 µs); while the scene is too dark, it rises 8×
    (up to 25.6 ms). Usually 1–2 steps, under 0.5 s.
  - **Dimming the pre-flash:** if it still clips at 40 µs (a very close
    subject), the pre-flash itself steps down about 4× (level 15 → 3 → 0).
    The model scales its share back to full power.
  - **Choosing the exposure:** the ambient levels just before metering
    (at the preview AE's exposure × gain) separate the flash's share from
    the ambient light. The app then picks the exposure × gain that puts
    the centre-weighted mean at 32 with the 97th-percentile block at most
    66. Measured against the output, a block level s comes out as about
    303 × (s / 111)^(1/2.2), so that's a photo mean of about 80 with
    highlights at about 240.
  - **Dimming:** if that would be under the sensor's shortest exposure (4
    lines, 35 µs: a close subject), the flash itself is dimmed (level
    0–15, about 25 mA × (level + 1)).
  - **Splitting:** exposure up to 1/30 s (3800 lines), the rest as gain
    up to 8×.
  - **Capture:** the still pipeline starts with that manual exposure and
    the flash at that level, so it skips only 6 frames instead of 15.
  - **Results:** before, flash photos of a close bottle were 100 %
    clipped. Now two scenes metered to 31 and 108 lines both came out with
    mean 60 and highlights at 222–244 of 255 (0.4–1.4 % clipped:
    glints), label text readable, in 4.2 s per photo.
  - **Close subjects** (bottle label, held by hand):
    - at ~5 cm: flash level 11 at 4 lines, mean 73, 0.8 % clipped;
    - at ~2 cm: pre-flash stepped down to level 3, photo at level 2
      (~75 mA per LED), mean 79, 0.6 % clipped. Before the pre-flash could
      dim, its reading clipped and the photo had 20 % clipped.
    - Both show a green cast (the software ISP's white balance under
      flash) and the 2 cm one is closer than the lens focuses.
- **AF assist** (flash Auto or On, when it's dark: the same 4× gain):
  - both LEDs light at torch level (175 mA) for the autofocus run;
  - the sweep waits 500 ms first, so auto exposure settles on the lit
    scene (it converges in ~8 frames);
  - the light goes off as soon as the lens is placed, about 3 s in all;
  - the focus is then held rather than tracked, since a tracking refocus
    in the dark would keep switching the light on. Tap to refocus;
  - auto exposure needs a moment to recover after the light goes off, so
    Auto treats the scene as dark for 2 s afterwards. A photo taken during
    or right after an assisted focus still gets the flash;
  - tested with the threshold forced to 0: FLEDEN read 0x0b from the start
    of the wait to the focused position, then 0x08.
- **QR codes**, found passively while the preview runs (either camera):
  - **Inline:** a third branch of the preview's `tee`, straight after
    `libcamerasrc` and before the GPU upload: `videorate max-rate=5` → a
    leaky one-buffer queue → 640×480 `GRAY8` → appsink. The decoding
    thread is that branch's own, so a slow scan only drops QR frames, never
    preview frames.
  - **Decoder:** libzbar through ctypes, configured for QR only and every
    second line and column (`ZBAR_CFG_X/Y_DENSITY` 2). GStreamer's `zbar`
    element tries every symbology on every line and can't be configured:
    136 ms per 640×480 frame of sensor noise, against 14 ms (37 ms QR-only
    at density 1). Live, with a code in view: 5 scans/s at ~30 ms, ~15 %
    of a core.
  - **Alert:** each code becomes a chip over the bottom of the preview
    ("QR · LINK/WI-FI/TEXT/…" and what it holds), up to 3, newest on top. A
    chip stays (sticky) until its ✕ is tapped, across camera switches and
    photos, and is outlined in yellow while its code is in view (1.2 s
    after the last sighting). Copy puts the text on the clipboard; links
    also get Open (Chromium, `titan-chromium`). Nothing opens on its own;
    Wi-Fi codes show the network name, and Copy gives the whole `WIFI:`
    string.
  - **Buzz:** a new chip comes with a short double buzz of the vibration
    motor (40 ms, 90 ms pause, 40 ms; [hardware.md](hardware.md#vibration-motor)).
    It isn't repeated while the code stays in view, only for a new code or
    one whose chip was dismissed.
- Keys: Space/Enter photo, F switch camera, R rotate, A autofocus, T flash.

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

### First frame through pass 1 (2026-10-15)

`titan_camcap p1=1 p1sel=1` captures through CAM A instead of CAMSV
(front camera, 1632×1224): **TG → DMX → IMGO**, every processing block
off. Correct colours, same BGGR raw as CAMSV.

```sh
insmod titan_camcap.ko cam=1 p1=1 p1sel=1 p1fmt=0   # 16-bit; p1fmt=9 packed RAW10
cat /sys/kernel/debug/titan_camcap                 # TG frames, CTL_*, IMGO error status
cat /sys/kernel/debug/titan_camcap_frame > f.raw
```

- **Layout:** CAMSV's with the blocks moved: control at 0x000 (CAMSV
  0x500), TG at 0x230 (same), DMA at 0x1000 (CAMSV 0x000; IMGO base 0x1020,
  `XSIZE` 0x1030 ...). The DMA error setup at 0x13b0/0x13c4 is CAMSV's
  0x3b0/0x3c4.
- **Routing:** `SENINF_TOP_CAM_MUX_CTRL` = 0 already feeds seninf mux 0 to
  every camera input, so CAM A's TG sees the sensor as soon as it runs.
- **Sequence** (from the HAL's `CAM_TOP_CTRL`, `CAM_TG_CTRL`,
  `CAM_DMX_CTRL`, `DMAO_B`, `Cam_path_sel`, decoded with
  `tools/re/hal_regseq.py`, and the BSP driver):
  1. reset `SW_CTL` 0x40: 1, wait for 2, 4, 0;
  2. `CTL_FMT_SEL` 0x0c: TG input format in bits 14:12 (RAW10 = 1; the HAL
     rejects others as "unsupported TG format"), IMGO format in 8:4
     (8 = 8-bit, 9 = packed 10-bit, 0 = 16-bit, samples << 4 as on CAMSV),
     RRZO format in 3:2, pixel mode in 21:20;
  3. `CTL_SEL` 0x10 = 0x060a5010: `Path_sel_RAW`'s second variant. The
     first (0x07025010) leaves IMGO without a source: nothing or scattered
     zero bursts get written, IMGO error status 0x8280;
  4. `CTL_EN` 0x04 = TG (bit 0) | DMX (bit 1). With the TG alone no data
     reaches IMGO;
  5. DMX: `DMX_CTL` 0x330 bits 19:16 set, bit 31 clear; `DMX_CROP` 0x334
     = (width − 1) << 16; `DMX_VSIZE` 0x338 = height;
  6. TG as on CAMSV (`SEN_MODE` bit 17 instead of 16 per the HAL, grab
     window, continuous);
  7. IMGO: the BSP's FIFO settings (`CON` 0x80000180, `CON2` 0x00400040,
     `CON3` 0x00200020), continuous without FBC, M4U port 61 (larb6 port
     0; all larb6 ports already have the MMU on);
  8. `CTL_DMA_EN` bit 0, `CMOS_EN`, `VFDATA_EN` (the BSP kernel driver
     starts the TG the same way, in its `ISP_VF_LOG` ioctl).
- Interrupts stay off: MediaTek's `camera-isp` driver still owns CAM A's
  IRQ. The tool watches the TG frame counter and IMGO's error status.

### Hardware AF statistics (2026-10-15)

`titan_camcap p1=1 p1sel=1 p1af=1` also runs the AF block and its DMA
(AFO), and they measure focus: in a lens sweep on a book, the filter
outputs peak at the same lens position as the image's own sharpness.

- **Configuration:** the HAL's `ISP_MGR_AF_STAT_CONFIG::config()`
  (`libcam.hal3a.v3.so`) turns an `AF_CONFIG_T` into register values. The
  stock default `AF_CONFIG_T` is built at run time by `_getAFConfig` in
  `libcameracustom.so`: `tools/re/emu_so.py` runs it in Unicorn and dumps
  it (`out/camera/re/af_config_default.bin`), and `tools/camera/af_regs.py`
  applies the HAL's formulas: AF_CON 0x610, thresholds, 12 filter
  registers (H0/H1 FIR taps 15, 36, 43, 36, 22, 10, 2; IIR −10, −22, −36,
  −43 as 11-bit fields), lookup tables, plus the gain/gamma stages SGG1
  (0x810) and SGG5 (0xcb0). Geometry (from the sensor mode): `AF_SIZE`
  0x640 = width, `AF_VLD` 0x644 = window start, `AF_BLK_0` 0x648 = block
  size (even width ≤ 254), `AF_BLK_1` 0x64c = block count; 16 × 12 here.
- **Enables:** the HAL asks `TuningMgrImp::enableEngine`
  (`libcamdrv_tuning_mgr.so`) for tuning functions 15 (AF), 14 (SGG1), 22
  (SGG5): `CTL_EN` bit 17, `CTL_EN` bit 18, `CTL_EN2` bit 31; AFO is
  `CTL_DMA_EN` bit 3 (DMA port 0x19; ChromeOS's `AFO_DONE_ST` is bit 5 of
  the second interrupt status). Turning SGG1 off while streaming stalls AFO
  until the next reset.
- **AFO:** base 0x10b0, sizes 0x10c0/0x10c4, stride 0x10c8, FIFO
  0x80000100/0x00800080/0x00550055 (BSP), M4U port 64. 40 bytes per block,
  one line per block row. The HAL's `config()` writes the sizes without
  the usual −1 (a lower layer subtracts it): written as is, AFO reports
  error 0x8280 and packs the rows without the stride.
- **Block format** (ten 32-bit words): w0, w1, w2, w4 are filter outputs
  (zero with the filters zeroed; w1 = w4 because the stock H0 and H1
  filters are identical); w3, w6, w7 are pixel sums (they follow the
  block's brightness); w5's top half is constant (0x5344).
- **SGG1 gain:** with the stock `SGG1_PGN` (0x10) the outputs are large
  and flat; at 0x80 (sweep below, a book under a lamp, 15× analogue gain)
  they peak where the image is sharpest. MediaTek's 3A presumably sets it
  from the exposure; ours will have to as well.

  | lens | 0 | 192 | 320 | 384 | 448 | 576 | 1023 |
  |---|---|---|---|---|---|---|---|
  | w0 | 230 | 391 | 630 | **781** | 743 | 493 | 224 |
  | w1 (M) | 25.8 | 28.3 | 33.2 | **34.4** | 33.2 | 28.5 | 25.6 |
  | image gradient | 19 | – | 51 | **65** | 52 | – | 14 |

- **Live tuning:** the AF registers latch every frame, so
  `/dev/mem` writes to 0x1a004000 + offset while camcap streams (and
  `echo N > /sys/kernel/debug/titan_camcap_focus` for the lens) make
  experiments quick; module reloads in quick succession sometimes leave
  the sensor without MIPI packets.

### AF statistics in the driver (2026-10-16)

While the rear camera streams, `titan_camera` also runs CAM A in
statistics-only mode, on the same seninf output as CAMSV: TG → DMX → SGG1 →
AF, written by AFO (16 × 12 blocks, the stock default AF configuration,
SGG1 gain `af_pgn`, automatic by default, see below). No IMGO, no interrupts; libcamera still
gets 30 fps from CAMSV.

```sh
R=/sys/devices/platform/1a050000.camsv1/rear
cat $R/af_pgn            # "auto 0x66": SGG1 gain; echo 0x100 > $R/af_pgn fixes it,
                         # echo auto > $R/af_pgn goes back (applied next frame)
od -A d -t x4 -N 32 $R/af_stats
```

`af_stats` is a 32-byte header (8 × u32: magic `TAF1`, the ISP's 8-bit
frame counter (`TG_INTER_ST` top byte), running, 16, 12, 40 bytes per
block, 656-byte line stride, SGG1 gain) and then the AFO data: 12 lines of
656 bytes, the last set the DMA wrote (it overwrites one buffer every
frame). The buffer is two pages mapped on M4U port 64 at probe.

The Camera app reads it once a frame for the rear camera: sharpness is the
H0 filter energy (w1) of the focus region's blocks divided by their pixel
sum (w3), from sets at least 3 frames after a lens move; no frame analysis
on the CPU then. Without `af_stats` (older driver) it falls back to
measuring the preview frames.

**SGG1 gain, automatic:** measured with `af_pgn` stepped live: words 3, 6,
7 and 8 don't change with the gain (they are taken before SGG1; word 3 is
the block's pixel sum), while the filter outputs scale with 1/gain (w1
halves per doubling: 0x100 → 67 M, 0x200 → 32.5 M, 0x400 → 15.3 M) and
saturate below ~0x40. So the gain works as a divisor and has to grow with
the scene's brightness. The driver keeps the AF input level constant:
every 100 ms while the statistics run, PGN = 0x80 × (mean word 3 per
block) / (23.3 × 146 × 146), 0x80 being the gain that gave a clean focus
curve at 23.3 per pixel (raw ~88 with the black level of 64), clamped to
0x20–0x7ff, changed only when more than 1/8 off and at most every 0.5 s.
In darkness it settles at ~0x63 (black level only). The app multiplies the
filter energy by the gain from the header, so a gain step in the middle of
a sweep doesn't look like a focus change.

**Status:** the mechanics work (the app finds the statistics, frame-count
timing, a sweep in ~3 s). In the first test the rear camera faced
darkness, so the focus curve under libcamera's auto exposure (and whether
0x80 still suits the brighter, AE-regulated input) is still to check with
a lit scene.

**Plan:**
1. ✅ A raw frame out of CAM A's IMGO.
2. ✅ AF statistics (AFO) that measure focus, ✅ in the V4L2 driver beside
   CAMSV, ✅ used by the Camera app (see "AF statistics in the driver").
   ✅ SGG1's gain follows the scene brightness (below). Open: checking
   the focus curve on a lit scene under libcamera's auto exposure.
3. OBC, LSC (tables from `libCamera_ov16880mipiraw_*.so`), RPG, and the AA
   statistics for exposure and white balance; the rear 4672×3504 mode.
4. Pass 2 (DIP) with the stock tuning.

