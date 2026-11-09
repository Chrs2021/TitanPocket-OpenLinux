# Cameras (OV16880 rear, OV8865 front)

Status (2026-10-15; the hardware ISP sections below are later, and the
Linux 6.12 port is at the end): **phase 2 done, phase 3 under way**. A V4L2 /
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

### When the flash won't light: power throttling

On 2026-10-18 the flash and the torch stopped. The kernel logged
`flashlight: fl_enable: Failed to enable since pt(0,1,0), pt strict(1)`
(`/sys/class/flashlight_core/flashlight/flashlight_pt` showed `0 1 0 1`).

- **The cause.** MediaTek's flashlight core blocks the LEDs in strict mode
  whenever it thinks the battery is low. One of its sources is the PMIC
  driver's battery-percentage notifier (`pmic_throttling_dlpt.c`). Every
  10 s, if not charging, it reads `battery_get_uisoc()` and raises
  "level 1" at 15 % or below.
- **Why it misfired.** That percentage is MediaTek's own fuel gauge, which
  this board doesn't use. The real gauge is the OZ8806 (`oz8806-battery`,
  92 % at the time); MediaTek's `battery` supply read -1. So the flash was
  blocked at 92 %. It had worked on the 16th, when the stale value
  happened to be above 15 %.
- **The fix.** `titan-flash-pt.service` (oneshot, at boot):
  - writes `1` to the PMIC's `battery_percent_protect_stop`, which stops
    the notifier calling anyone;
  - writes `0 0 0 1` to `flashlight_pt`, which clears the flags and keeps
    strict mode.
- **What still protects the battery.** The low-voltage protection, the
  PMIC's battery voltage comparator, isn't touched: a truly sagging
  battery still blocks the flash.

## The Camera app (titan-camera-gui)

`userspace/debian/overlay/usr/local/bin/titan-camera-gui` (Python, GTK 4 and
`titan_ui4` since 2026-10-28, GStreamer), **Camera** in the app drawer.

- **Preview:** `libcamerasrc`, converted, rotated (mirrored for the front
  camera) by `glvideoflip` and scaled to about the screen by `glcolorscale`
  on the GPU. GTK 3 showed it with `gtkglsink`; GTK 4 has no GStreamer GL
  sink in Debian 12, so the small frame is downloaded by an appsink and
  drawn as a texture (~30 fps, see desktop.md).
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

### The Rust camera app (titan-camera, 2026-11-09, not yet run on the phone)

`userspace/titan-camera` (`build.sh` -> `out/titan-camera`, installed as
`/usr/local/bin/titan-camera` beside the Python app until it has been tried):
the same hardware-ISP preview, stills, autofocus and flash, rewritten in Rust
(gtk4-rs 0.9, gstreamer-rs 0.23, built against the phone's Debian 12
libraries like titan-messages), plus:

- **Video**: Photo/Video mode. A recording replaces the preview stream with
  one that also encodes: the GPU-rotated frames are scaled to the video size
  (4:3: 1440×1080 at 12 Mbit/s, 960×720 at 6, 640×480 at 3) as NV12 and go to
  the best H.264 encoder installed (`v4l2h264enc` once the hardware encoder
  has a V4L2 driver, docs/video-encoder.md; else `x264enc`, ultrafast,
  zero latency; else `openh264enc`), with Opus sound from `pulsesrc`, into
  MP4 (Matroska when this `mp4mux` takes no Opus) in `~/Videos/Camera`.
  Stopping sends EOS so the muxer closes the file. The rootfs now installs
  `gstreamer1.0-plugins-ugly` and `gstreamer1.0-pulseaudio` for this.
- **Shutter speed and ISO** (settings panel): Auto, or 1/2000 s to 1/30 s on
  the preview and in video, and down to 1 s for photos. With both on Auto
  the driver's own exposure runs. Fixing either one takes over: the sensor
  gets the fixed value (exposure in lines from the line time, (width +
  hblank) / pixel rate; gain with 128 = 1×, ISO 100 = 1×), and the app moves
  the other one from the preview's centre-weighted luma toward Y 105 (shutter
  priority or ISO priority). A photo starts from the preview's exposure ×
  gain; a shutter longer than the frame raises VBLANK for that stream (the
  driver limits exposure to frame length − 8 lines), and photos longer than
  1/15 s take one frame instead of a burst.
- **Photo size**: the sensor's full mode (rear 16 MP, front 8 MP) or its
  binned mode (4 MP, 2 MP; faster).

QR codes (2026-11-09): as the Python app's (above), with two changes. The
decoder (`qr.rs`) loads libzbar with `dlopen`, so the build needs no zbar
and the app runs without it, and it reads the luma straight from the
preview's YUYV, every n-th pixel for frames over 1000 wide, so the branch
has no `videoscale` or `videoconvert`. 3 scans a second: at 5 a second the
decoding thread took about 26% of a core on the 960x720 preview, at 3
about 12%. Not ported yet: the flash pre-metering. Flash On and Auto light
the torch maximum through the capture instead of a metered strobe.

The preview runs at the ISP's scaled-down size (`Camera::preview`: rear
960x720, front 816x612; see "The resizer" below), or the binned mode when
a recording is larger. Measured with the app on the phone's screen: the
whole app took 89% of a core with the 2336x1752 preview, 61% with the
960x720 one (QR decoding apart).

The settings live in `~/.config/titan-camera/settings` (key=value). Unit
tests cover the settings file, the sharpness measure and the focus search
(a simulated lens), the exposure loop, and the V4L2 control layouts.

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
3. ✅ The processed path and ✅ OBC, ✅ AA statistics (see "Pass 1
   processing" below). Open: LSC (tables from
   `libCamera_ov16880mipiraw_*.so`), BNR, RPG (needs RRZO, parked), the
   rear 4672×3504 mode.
4. Pass 2 (DIP) with the stock tuning: frame mode (no tiles) first.

### Pass 1 processing (2026-10-26)

`titan_camcap` options, all on top of `p1=1`:

| option | what |
|---|---|
| `p1proc=1` | IMGO after the raw blocks instead of pure raw |
| `p1obc=1` | black level (OBC) |
| `p1aa=1` (`p1aanx`, `p1aany`) | AE/AWB block statistics through AAO, `/sys/kernel/debug/titan_camcap_aao` |
| `p1wb=R,G,B` | RPG white-balance gains (512 = 1×) |
| `p1rrz=1` | resizer branch RRZ → RRZO (not producing output yet) |
| `p1img=N` | IMGO tap (CTL_SEL IMG_SEL) |
| `p1regs=off,val,...` | raw CAM A writes before start, for experiments |

The enables come from the HAL: each `ISP_MGR_*::apply` in
`libcam.hal3a.v3.so` (Ghidra: `out/camera/re/ispmgr.c`) passes a tuning
function to `TuningMgrImp::enableEngine` and sets the same bit in
`ISP_MGR_CTL_EN_P1`, which mirrors `CTL_EN`
(`tools/re/ispmgr_engines.sh`):

| CTL_EN bit | block | | CTL_EN bit | block |
|---|---|---|---|---|
| 0 | TG | | 11 | RMX (resizer mux) |
| 1 | DMX | | 13 | BMX |
| 3 | RMG | | 15 | PAK (IMGO packer) |
| 4 | RMM | | 17 | AF |
| 5 | OBC | | 18 | SGG1 |
| 6 | BNR (+ BPCI) | | 19 | AA (+ AAO) |
| 7 | LSC (+ LSCI) | | 21 | LCS |
| 9 | RPG | | 23 | RCP |
| 10 | RRZ | | 26, 28 | SL2F, DBS |

DMA enables (`CTL_DMA_EN`, HAL `DMAO_B::_enable`): IMGO 0, UFEO 1, RRZO 2,
AFO 3, AAO 5. The DMA ports sit 0x30 apart: IMGO 0x1020, RRZO 0x1050,
AAO 0x1080, AFO 0x10b0; error status from 0x1360, one word each.

- **Processed path:** `CTL_SEL` = 0x07025010 (HAL `Path_sel_RAW`; bits
  25:24 are IMG_SEL: 2 pure raw from the TG, 0 before LSC, 3 after LSC;
  bit 19 forces pure raw) and the packer (`CTL_EN` bit 15). Without the
  packer IMGO reports error 0x8280 and writes nothing. BMX and RCP aren't
  needed for IMGO.
- **DMX SRAM size:** `DMX_CTL` bits 15:0 must hold the DMX SRAM size
  (0x1b0, `capibility::GetXMXSramSize` in `libimageio_plat_drv.so`). Left
  at 0, the processed path drops the last 640 of every 4096 pixels (a
  fixed pattern lands there) — in packed RAW10 that looked like diagonal
  stripes. Packed RAW10/12 then decode as MediaTek's little-endian bit
  stream.
- **OBC:** `OFFST0..3` (0x3f0) are 13-bit two's complement on a 12-bit
  scale, `GAIN0..3` (0x400) 13 bit with 512 = 1×. The stock tables
  (`libCamera_<sensor>_Scene_Preview.so`, symbols `..._OBC_0020..0039`
  per ISO) give OV16880 0x1f00/0x222 (black 64 in RAW10) and OV8865
  0x1fbc/0x209 (black 17): gain = 512·4095/(4095 − 4·black). The HAL also
  folds the ISP digital gain into GAIN. Measured: the black floor goes to
  0 on both cameras.
- **RPG** (0x4c0: `SATU` 0x0fff0fff, `GAIN_1` = B | Gb << 16, `GAIN_2` =
  Gr | R << 16, `OFST` 0) has no effect on IMGO at any tap: it feeds the
  resizer branch only.
- **RRZ/RRZO:** enabling RRZ (`CTL_EN` bit 10) stalls the whole pipe
  unless `CAM_UNI_TOP_MOD_EN` (0x1a003010) bit 29, the resizer's line
  buffer, is set (HAL `PIPE_CHECK::RRZ_CHECK`: "can't enable rrz without
  RLB"). CAM_UNI reads 0 and ignores writes while CAM is powered off.
  With RLB on, frames run but RRZO (port 62, DMA bit 2) still writes
  nothing.
- **AA statistics:** AWB window registers 0x830 (origin, block size,
  pitch, count), pre-gain 0x840/0x844 (512 = 1×), limits, low/high
  thresholds, and **`PIXEL_CNT` 0x858–0x860 = 2²⁴ / pixels of that colour
  per block** (left 0, every average is 0); the HAL's fixed 0x8c4/0x8cc =
  0x200, 0x8d4 = 0x90200; a copy at 0xbd0. AAO: port 63, FIFO
  0x80000080/0x00200020/0x00100010. With the AE registers (0x900–0x970)
  left 0, each block row is nx × {R, G, B, 0x0f} 8-bit averages followed
  by nx 8-bit luma values. Against block means of the same IMGO frame:
  correlation 0.96 for R, G, B and Y. (The HAL's AE setup makes it 22
  bytes per block, plus histograms.)

**Pass 2 (DIP)** is at 0x15022000 (0x6000, IRQ 268, imgsys power domain,
larb5). `libdip_postproc.so`'s `DIP_ISP_PIPE::configTpipeData` writes the
tile registers 0x50–0x5c/0xe0 only when the tile engine
(`tpipe_main_platform` in `libdpframework.so`) is used; otherwise it
writes `TDR_CTL` 0x50 = 0, frame mode. The pass-2 tuning managers
(`ISP_MGR_CCM`, `GGM`, `UDM`, `G2C`, `PGN` ...) write offset/value lists
into the DIP register file, which gives its layout.

DIP map so far (all offsets from 0x15022000; the named control bits are
in `out/camera/re/dip_ctl_bits.txt`, derived from
`DIP_ISP_PIPE::configTpipeData`, which copies register bits into the tile
config struct, and `dumpIspTPipeInfo`, which prints that struct's field
names):

| reg | contents |
|---|---|
| 0x000 `CTL_START` | bit *n* starts command-queue thread *n* (base at 0x208 + n·stride) |
| 0x004 `YUV_EN` | C24 2, G2C (RGB→YUV) 3, C42 4, NBC 5, NBC2 6, PCA 7, SEEE 8, NR3D 10, CRZ 17 |
| 0x00c `RGB_EN` | UNP 0, PGN (WB gain) 2, SL2 3, UDM (demosaic) 4, LCE 5, G2G (CCM) 6, GGM (gamma) 7, OBC2 11, BNR2 13, LSC2 15, RNR 18 |
| 0x010 `DMA_EN` | IMGI 0, IMGBI 1, IMGCI 2, UFDI 3, VIPI 4, LCEI 7, DMGI 9, IMG2O 11, IMG3O 13, FEO 16 |
| 0x014 `FMT_SEL` | input format 3:0, VIPI 12:8, IMG3O 21:20, IMG2O 24, Bayer order 29:28 |
| 0x018 `PATH_SEL` | G2G 1:0, PGN 2, G2C 4, CRZ 13:12, MDP 15, GGM 21, IMGI 22 |
| 0x050–0x05c, 0x0e0 | tiles (TDR); 0x050 = 0 is frame mode |

DMA ports (base, +8 offset, +0x10 xsize, +0x14 ysize, +0x18 stride):
inputs IMGI 0x500, IMGBI 0x530, IMGCI 0x560, VIPI 0x590, VIP2I 0x5c0,
VIP3I 0x5f0, DMGI 0x620, DEPI 0x650, LCEI 0x680, UFDI 0x6b0; outputs
IMG2O 0x330, IMG2BO 0x360, IMG3O 0x390, IMG3BO 0x3c0, IMG3CO 0x3f0,
FEO 0x420, PAK2O 0x450 (names from the `DIP_DMA` id table). Processing
blocks (from the managers' register lists): OBC2 0x11c0, BNR2 0x1240,
PGN ~0x1440, UDM 0x1544, CCM 0x2080, LCE 0x2100, GGM 0x2184–0x2480, G2C
0x3244, SEEE 0x46c4, NBC2 0x4800.

**`titan_dip` (`modules/camera/dip/`, 2026-10-26)** powers the imgsys
domain (`SCP_SYS_ISP`, `img_sel`, `IMG_LARB5`, `IMG_DIP`, larb5), maps
IMGI/IMG3O through the IOMMU (ports 36/38) and runs one frame from
debugfs (`titan_dip_in` → `titan_dip_run` → `titan_dip_out`, plus
`titan_dip_tdr` for a tile list and `titan_dip_poke`). Status:

- Writing `CTL_START` bit 0 with the command-queue thread disabled runs a
  frame from the directly written registers; IMGI fetches (an unmapped
  base faults on port CAM_IMGI), IMG3O writes, G2C runs. A YUV passthrough
  (`PATH_SEL` bit 10) carries real input bytes to IMG3O.
- The DIP line buffer is about 900 bytes of RAW10, so a 1632-wide frame
  needs tiles. `tools/re/emu_tile.py` runs the stock tile engine
  (`tile_main_platform` in `libdpframework.so`) in Unicorn from a config
  written by field name (`out/camera/re/tile_cfg_fields.txt`) and prints
  its tile list (TDR) with the engine's own decoder: 3 tiles of 16 words
  for 1632×1224 (edges, per-tile module enables, IMGI/IMG3O offsets and
  sizes, `UNP_OFST`, UDM/C42 tile edges, G2C shading).

### First light through the DIP (2026-10-26)

`titan_dip` turns a pass-1 frame (`titan_camcap p1proc=1 p1obc=1`) into
YUYV on the hardware: **UNP → PGN (white balance) → UDM (demosaic) → G2G
(colour matrix) → GGM (gamma) → G2C (RGB→YUV) → C42 → IMG3O**, the full
1632×1224 frame in 3 tiles, a few milliseconds per frame:

```sh
insmod titan_dip.ko wb=610,512,1347              # PGN gains R, G, B (512 = 1x)
cat f.raw > /sys/kernel/debug/titan_dip_in        # processed RAW10, stride 2048
echo 1 > /sys/kernel/debug/titan_dip_run
cat /sys/kernel/debug/titan_dip_out > f.yuyv
```

The register values came from running the **stock HAL in an emulator**:
`tools/re/emu_hal.py` loads several Android libraries into one Unicorn
address space (APS2-packed and RELR relocations, cross-library linking,
static constructors, shims for libc/logging/properties/ION and a fake
`/dev/camera-dip`), and `tools/re/emu_dip_config.py` drives
`libdip_postproc.so`'s `PostProcPipe` through `createInstance`, `init`,
`queryCQ` (takes a DIP working buffer), `configPipe` and `start` with a
fake `FrameParams` (input/output `android::Vector`s of 0x20-byte port
entries), fake `IImageBuffer` objects (vtable: 0x28 format, 0x30 size,
0x48 plane count, 0xe0 PA, 0xe8 VA, 0xf0 size, 0xf8 stride) and a p2
tuning register image at `PipePackageInfo` + 0x28 (the HAL takes every
module enable and parameter from it). The register image it leaves in the
working buffer is what the hardware needs:

| register | value | |
|---|---|---|
| `PATH_SEL` 0x18 | 0x16040 | srz1_sel 1, crz_sel 2, nr3d_sel 1, fe_sel 1 |
| `MISC_SEL` 0x1c | 0x249010 | |
| `TDR_CTL` 0x50, 0x54 | 0xb, 0 | tiles |
| TCM 0x58, 0x5c, 0xe0 | 0x08182002, 0x100, 0 | which registers the tile words go to; wrong values make the tile loader overwrite the control registers |
| `IMGI_STRIDE` 0x518 | stride \| 0x40010000 | |
| `IMG3O_STRIDE` 0x3a8 | stride \| 0x01910000 | format_en, YUY2, bus size |
| 0x3040 | 0xf0 | always |
| `FMT_SEL` 0x14 | 9 | packed RAW10; the reset value's bit 31 makes IMGI fetch a second plane from 0 |

Two findings decided it:

- **GGM is not optional.** With gamma disabled the RGB path outputs zeros
  (that was every earlier black frame). Its table: 192 words at 0x2180,
  each B | G << 10 | R << 20 (10 bit), control 0x2480 = 0x100307fe; the
  stock curve is `ov8865mipiraw_GMA_0001`.
- The G2C offsets are 0: the output already centres U/V at 128 (offsets
  of 128 come out as +32, a magenta cast).

`CTL_START` bit 0 with command-queue thread 0 disabled runs the frame
from the directly written registers (the HAL loads them through the
command queue; not needed here). Defaults in `titan_dip` are the HAL's;
the 1632×1224 tile list is built in, other sizes take one through
`titan_dip_tdr`.

### Rear camera through the DIP (2026-10-26)

Both rear modes run, with the OV16880's own tuning:

| mode | input | tiles | DIP time |
|---|---|---|---|
| 2336×1752 | pass 1 processed + OBC (`titan_camcap cam=0 p1=1 p1fmt=9 p1proc=1 p1obc=1`), stride 2928 | 4 | ~5 ms |
| 4672×3504 | CAMSV raw in 2-pixel mode (`titan_camcap cam=0 mode=1`), stride 5840; black level in the DIP | 7 | ~16-30 ms |

```sh
insmod titan_dip.ko width=2336 height=1752 stride=2928 sensor=0 wb=602,512,1102
insmod titan_dip.ko width=4672 height=3504 stride=5840 sensor=0 black=64 wb=678,512,879
```

- **Per-sensor tuning** (`sensor=0` rear, `1` front) is in
  `modules/camera/dip/dip_tuning.h`, generated from the stock tables:
  UDM (`ov16880mipiraw_UDM_0040`, `ov8865mipiraw_UDM_0080`), the first
  CCM of `..._CCM_0000`/`..._CCM_0001` (capture library), and
  `..._GMA_0001`; noise reduction (NBC) from the stock `ISP_MGR_NBC::apply`
  run in emulation. Generated values stay local like the other
  vendor-derived tables: the repository's `dip_tuning.h` is a placeholder
  with neutral values (identity colour matrix, straight gamma, no noise
  reduction), so the drivers build and run untuned. **Generate the real
  one from your own phone's libraries** (2026-10-29):

  ```sh
  modules/camera/tools/gen-dip-tuning.py --vendor VENDOR/lib64 --system SYSTEM/lib64
  ```

  `VENDOR/lib64` and `SYSTEM/lib64` are the vendor and system partitions'
  library folders from your phone's firmware. The demosaic, colour and
  gamma tables are read straight from the libraries' symbols; noise
  reduction needs Unicorn (`pip install unicorn`), or `--no-nbc` leaves it
  out (the driver keeps it off by default). Checked: it reproduces every
  table the driver was tuned with.
- **OBC2** (`black=`): pass 2's black level, laid out like pass 1's OBC
  (offsets 0x11c0, gains 0x11d0, `RGB_EN` bit 11). It lets raw CAMSV
  frames in, which matters for 4672×3504: CAM A's pass 1 can't take it yet
  (it needs 2-pixel mode), CAMSV already does.
- **Tile lists** for each mode are built in (`dip_tdr.h`, generated by
  `tools/camera/dip_tdr.py WIDTH HEIGHT STRIDE [--obc2]`, which runs the
  stock tile engine). Running the HAL emulation with OBC2 and PGN enabled
  showed the TCM words don't change for them.
- No visible seams: at each tile boundary the column-to-column luma step
  equals its neighbourhood's.

### Streaming hardware ISP in the driver (2026-10-26)

`titan_camera` now offers a **`YUYV`** format beside the raw Bayer ones:
the whole pass-2 pipeline runs live, per frame, in the kernel, so the
video node delivers finished YUV and the GPU software ISP is not needed.
Both cameras stream at **30 fps**:

```sh
v4l2-ctl -d /dev/video0 --set-fmt-video=width=2336,height=1752,pixelformat=YUYV
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=60     # 29.98 fps
# libcamera picks it up directly (GPU debayer bypassed):
cam -c 1 --capture=30 --stream pixelformat=YUYV,width=2336,height=1752
```

How it fits together (`module param `hwisp`, on by default; `modules/
camera/dip/dip_core.h` is the shared register programming):

- **CAMSV captures packed RAW10** (as `titan_camcap p1fmt=9`) into an
  internal 3-buffer **raw ring**, not the vb2 buffer — the output buffers
  are YUYV. The raw buffers are M4U-mapped twice: on the CAMSV port
  (larb6, write) and the DIP IMGI port (larb5, read) of the same pages.
- **A workqueue runs the DIP per frame.** On each CAMSV frame-done the
  IRQ hands the finished raw buffer to a worker, which programs the DIP
  (`dip_program()`), waits (~5 ms for preview), and completes the YUYV
  vb2 buffer. At 30 fps the ~5 ms leaves plenty of headroom; a frame is
  dropped only if the worker is still busy (it isn't, in practice).
- **imgsys is powered** (`SCP_SYS_ISP`, `IMG_DIP`/`IMG_LARB5`, smi larb5)
  only while a `YUYV` stream runs, alongside camsys for CAMSV.
- **White balance** is grey-world from a subsample of the raw (into the PGN
  gains); since 2026-10-27 it runs every 100 ms off the per-frame path and
  decodes the raw correctly (see "Full-resolution live DIP streaming"). The
  rear sensor's black level is removed by the DIP's own OBC2 (the pass-1
  CAM A path isn't used here).
- **libcamera** needs no change: its simple pipeline sees `YUYV`, finds
  the GPU debayer "can't" process it (it's already processed), and passes
  it straight through. The old raw→GPU path still works for `BGR888`.

So the camera path is complete end to end on the hardware ISP: sensor →
seninf → CAMSV (raw) → DIP (pass 2) → app, at 30 fps, with the GPU free.

**Still open (quality, not plumbing):** noise reduction (NBC/NBC2) and
sharpening (SEEE), and the 4272×2404 video mode. Full-resolution live DIP
streaming is wired in the driver as of 2026-10-27 (see below). Auto white
balance and exposure from the AA statistics are below.

### Camera app preview on the hardware ISP (2026-10-27)

`titan-camera-gui`'s preview now requests the driver's `YUYV` node, so the
live preview runs through MediaTek's pass-2 hardware ISP and the GPU only
converts `YUYV`→RGB for display (no software debayer). Per camera it asks
for the sensor-mode size the DIP delivers — rear 2336×1752, front
1632×1224 — and the GL sink scales to the window. `TITAN_CAMERA_HWISP=0`
falls back to the old 960×720 `BGRx` GPU software-ISP path.

The app change is just the source caps: a named `capsfilter` after
`libcamerasrc` is set to `video/x-raw,format=YUY2,width=…,height=…` at
configure time (GStreamer spells the format `YUY2`, libcamera `YUYV`). The
autofocus `appsink` reads the same buffers, so `sharpness()` was made
format-aware (2 bytes/px YUYV, luma at byte `2x`; 4 bytes/px BGRx, green at
`4x+1`) and `on_af_sample` now takes the frame size from the sample caps
instead of the fixed preview size. Stills still stop the preview and run
the GPU software ISP at full sensor resolution (the DIP full-res path isn't
wired into the live node). Verified on device: both cameras negotiate
exactly `YUY2` at their hardware-ISP sizes at 30 fps and the pipeline
reaches `PLAYING`.

### Auto white balance and exposure from the AA statistics (2026-10-27)

The hardware ISP path now drives white balance and exposure from the CAM A
**AA statistics** (AAO) instead of the grey-world raw subsample and the
fixed sensor default. CAM A already ran in pure-raw mode for the rear AF
statistics; that bring-up (`af_start`) is generalised into `cama_start`,
which runs AF (rear 2336 mode only, its filter table) and the AA block
(either camera) off the same demuxed stream. The AA block is configured as
in `camcap`'s `p1aa` — equal `AA_NX`×`AA_NY` blocks over the frame, unity
pre-gain, thresholds wide open, the AE histogram registers left zero — and
its AAO DMA (M4U port 63, larb6) writes, per block row, `AA_NX`×{R,G,B,flag}
8-bit averages then `AA_NX` luma bytes. CAM A for the *front* is new here
(the seninf top mux already feeds every CAM/CAMSV input from the active
sensor); it was not needed before because the front is fixed-focus.

A 100 ms work item (the old `af_work`, now shared) reads the AAO each tick:

- **White balance:** grey world over the per-block R/G/B averages, black
  removed, into the DIP PGN gains (`tc->wb[]`) — only when `aa_wb` is on
  (default **off**). By default the CPU raw subsample owns WB (see "Full-
  resolution live DIP streaming" below for how it works and why). The AA path
  averages every block, near-black ones included, so it has the same
  dark-scene bias the CPU path now avoids with its thresholds; the AWB
  low/high thresholds (0x850/0x854, left wide open) are the hardware's way to
  do the same, untested.
- **Exposure/gain:** the mean luma, derived from the same R/G/B averages
  (`Y = (R + 2G + B) / 4`, above black — more reliable than the block's
  separate AE-luma byte, and consistent with the white balance) drives a
  damped closed loop on the sensor. Light ∝ exposure × gain; it raises
  exposure up to the frame-length limit first (keeps gain and noise low)
  then gain, with a ±6 % deadband so the preview does not hunt. The
  exposure/gain registers are written directly as one group-hold update
  (`ae_apply`), as `af_pgn` is — libcamera does not run its own AGC in the
  YUYV passthrough path, so nothing fights it.

Module params: `aa` (run the AA stats block, default on), `aa_wb` (WB from AA,
default **off**), `ae` (exposure loop, default on), `ae_target` (target mean
luma above black, 0–255). Per-sensor debug: `/sys/.../{rear,front}/ae` prints
`running luma exp gain wbR wbB target src` (`src`: `aa`, `cpu` or `manual`)
and `/sys/.../{rear,front}/dip` prints `frames busy nobuf rawerr us us_max`
for the current stream (frames the DIP did; frames lost because the worker
was busy, because no YUYV buffer was queued, or to a CAMSV error; last and
worst per-frame worker time in µs).

Verified on device: AA runs on both cameras (the new front CAM A path
included), the AAO produces live stats, the white-balance gains track the
scene, and the exposure loop drives the sensor — register writes confirmed
over I²C, and a forced high/low target swings the captured frame from
`meanY` ≈ 20 down to ≈ 1. Midtone regulation and the final `ae_target` still
need a normally-lit scene to tune: both test scenes were near-black (rear
face-down, front dim), so the signal sat close to the sensor pedestal and
the loop correctly railed at its limits without a mid-range to settle in.


### Full-resolution live DIP streaming (2026-10-27)

The driver can now run the pass-2 DIP at the sensors' full-resolution modes,
so a still can come from the hardware ISP (~tens of ms) instead of stopping
the preview and debayering on the GPU (~4 s). The DIP streaming path was
already resolution-independent (`set_format` sizes the raw ring and strides
from the mode, `start_streaming` carries `pix2` for the rear's 2-pixel mode,
`cama_start` runs AA at any size and skips AF off the 2336 mode); the only
missing piece was a **built-in tile list** for each full-res mode.

- Rear 4672×3504 (stride 5840, OBC2, 7 tiles): `tdr_4672x3504_obc2`, already
  present.
- Front 3264×2448 (stride 4080, OBC2, 5 tiles): `tdr_3264x2448_obc2`, added
  now — `tools/camera/dip_tdr.py 3264 2448 4080 --obc2`. The generator is
  deterministic (it reproduces the committed rear list bit-for-bit).

Both are registered in `dip_tdrs[]` (`dip_core.h`), so `dip_find_tdr` matches
the full-res YUYV stream on either camera.

**App still path.** `titan-camera-gui`'s `capture()` requests `YUY2` at the
exact sensor-mode size (`hwstill`: 4672×3504 / 3264×2448) when the hardware
ISP is on, so stills come finished from the DIP; `TITAN_CAMERA_HWISP=0` keeps
the old `BGRx` GPU software-ISP path at its cropped size (4668 / 3260). The
same caps through `gst-launch` give a 4672×3504 JPEG.

Measured on device (`dip` sysfs, `cam` with no file writing):

| Mode | fps | DIP worker per frame | drops |
|---|---|---|---|
| rear 4672×3504 (2-pixel) | 20.6 (the sensor's rate) | 32 ms | 0 |
| front 3264×2448 | 29.5 | 16 ms | 0 |

Getting there took four fixes, all in the CPU raw statistics:

- **Off the per-frame path.** The raw ring is mapped write-combine, so the
  CPU subsample went to DRAM for every load: 24 ms a full-res frame, which on
  top of the DIP's 32 ms dropped every other frame (10 fps). It now runs in
  the 100 ms AE/AWB work on the frame the DIP last took (`raw_last`), and only
  per frame (every 8th) when `aa` is off and there is no such work.
- **Decoding.** CAMSV's packed RAW10 is a little-endian bitstream (pixel *i*
  at bits 10*i*…10*i*+9), not CSI-2 packing — the sampler had been reading
  byte 0 as pixel 0's top bits since the streaming path was written, which
  made every white-balance result close to meaningless. One aligned `u32`
  holds pixels 0 and 1. Checked against an unpacked 8-bit `BA81` frame at the
  same exposure: driver wbR/wbB/luma 1003/627/29, reference 995/618/30.
- **Black level** on the right scale: the 10-bit pedestal >> 2 against the
  8-bit samples (it had subtracted the full 10-bit value, ~4× too much; in a
  dim scene that clamped the red gain at 8×).
- **Lit samples only for WB.** In a mostly dark scene the near-black samples
  carry the per-channel pedestal and noise rather than the light, and pull
  grey-world toward 1×; since raw green is the strongest channel, gains stuck
  near 1× look green. WB averages only quads with green ≥ 8 levels above black
  and nothing clipped (falling back to every sample if too few); on a test
  frame the gains then settle (R 2.05×, B 1.15–1.19× from +8 up) where the
  full average gave 1.81×/1.21×. The luma for AE still averages everything.

The green tint in photos taken before this change did *not* come from the AA
white balance: stills then went through libcamera's GPU software ISP, whose
white balance has its own green cast (noted above). Not checked: the AA
white balance with these lessons applied.

**AE at the rear's full resolution.** The 4672×3504 mode is 2-pixel; CAMSV
handles it, but CAM A's TG is set up for one pixel a clock, so the AA block
sees nothing and the AAO stays empty. The AE then takes its luma from the CPU
raw subsample (`src` = `cpu`). The AAO buffer is cleared at each stream start,
so a dead AA block reads empty instead of returning the previous stream's
statistics (which briefly steered the AE in testing).

**Exposure and white balance carry over between streams.** The AE loop writes
what it applies back into the exposure/gain controls' cached values (not
through `s_ctrl`, so it is not a manual set): the readback is truthful and the
next stream starts from it (`sensor_apply_ctrls`), so a still taken right
after the preview starts at the preview's settled exposure — both rear modes
have the same line time (`LINE_US` in the app), so lines carry over. The PGN
gains are saved per sensor at stream stop and restored at the next start, so
the first frames are not unity-balanced (green).

**AE vs. manual exposure.** The in-kernel AE loop would fight the app's flash
metering and metered still. And libcamera's manual exposure doesn't reach the
sensor in the YUYV path at all: the earlier libcamera patch implements it in
the software ISP's AGC, which passthrough doesn't run (`gst-launch` with
`exposure-time-mode=manual` left the sensor untouched). So:

- With the hardware ISP the app writes the metered exposure/gain straight to
  the sensor subdev — on the preview during metering, and after the preview
  stops but before the still starts for the photo.
- The driver treats any userspace `EXPOSURE`/`ANALOGUE_GAIN` write — while
  streaming or not — as manual: `sensor_s_ctrl` sets a per-sensor `ae_manual`
  and `ae_step` backs off until that stream stops (`sensor_stop` clears it).
  The driver's own range updates (`sensor_update_limits`, the VBLANK handler)
  may clamp the exposure and call `s_ctrl`; they set `ctrl_internal` so they
  don't count. The AE's own writes never go through `s_ctrl`.

Tested on device: auto preview → `aa`, AE running; auto full res → `cpu`, AE
running; exposure pre-set to 500 lines / 2× then a full-res stream → `manual`,
sensor held at 500/256 throughout; the next stream → auto again. libcamera
does not write exposure/gain at stream start, so auto streams don't latch.

Flash photos through the real app were then tested end to end (Space
injected through `/dev/uinput`): metering on the preview with the AE paused
(`manual`), the full-res still at the metered exposure with the flash lit,
the preview back on auto afterwards. Colour came out neutral.

### Shake tolerance, image autofocus, and a preview stall (2026-10-27)

The flash photos were soft. Taking that apart:

- **Exposure is capped for handheld use.** The driver's AE stops at
  `ae_max_us` (default 10 000 = 1/100 s; 0 = the frame length) and raises gain
  after that; the cap applies before the deadband, so an exposure already
  above it comes down. The app's flash meter stops at `PHOTO_MAX_US` (also
  1/100 s; it was 1/30 s), with `FLASH_MAX_GAIN` raised to the sensor's 15.5×.
  The flash is the torch LED lit continuously, so it freezes nothing.
- **Stills are the sharpest of a burst.** The still pipeline now ends in a
  raw `appsink`: after the settle frames, `STILL_BURST` (6) frames are scored
  with `sharpness()` on the focus region, the best one is kept, the camera
  and flash are released, and a worker thread rotates and JPEG-encodes that
  frame alone (`appsrc ! videoflip ! videoconvert ! jpegenc`). Handheld, the
  scores in a burst spread 2× (shake); resting, 5%. With the hardware ISP the
  still skips `HW_SETTLE_FRAMES` (8), not 15: exposure and white balance carry
  over from the preview. The meter logs its decision to stderr
  (`meter: pre-flash … N lines x G`).
- **Focus was the main cause, not shake.** A metered flash still was 0.9 ms
  and still soft. With the phone resting, image sharpness over the lens range
  peaked clearly (2.6× at 512 against either end), and the AK7371 position
  registers followed every command — but the hardware AF statistics the
  rear autofocus used barely moved: `w1`/`w2`/`w4` peak at the same place by
  only ~15% over a noise floor, at high gain in a dim scene. So with the
  hardware ISP the app now focuses on the preview frames themselves
  (`hw_af` = statistics exist *and* not `HW_ISP`; `hw_stats` keeps flash
  metering on the statistics). On the resting scene it sweeps 0 → 704,
  refines 464–560 and settles at 528; the photo is sharp at 100%.
- **Two bugs that path exposed:**
  - *Idle starvation.* Streaming threads handed results to the main thread
    with `GLib.idle_add`; a playing video widget keeps a GTK redraw (priority
    120) pending, so idle sources (200) never ran and the AF sweep waited
    forever ("Focusing…"). The hand-offs (AF samples, QR codes, burst done,
    photo encoded) now use `priority=GLib.PRIORITY_DEFAULT`.
  - *Preview stall at start-up.* libcamera's simple pipeline gave the stream
    4 buffers; the preview tees to the display, the AF appsink and the QR
    branch through leaky queues (which keep the *newest* buffers) and a
    videorate, and at start-up they could hold all 4 — libcamera then had
    nothing to queue and the preview froze after ~5 frames, about one launch
    in four. `kNumBuffersDefault` is now 8 in the libcamera patch (64 MB at
    preview size); 6 of 6 launches then streamed. It predates this work; the
    hardware-statistics AF just didn't need frames, so it went unnoticed.

**Noise reduction: the fault is IMGI, not SMX (2026-11-05, later).** On 6.12,
with `nr=1` the rear camera's stream stops at once and the IOMMU reports
`larb=5 port=0` (IMGI) reading iova `0xc00`, over and over: the DIP's raw input
runs from base 0 plus a tile offset, so its base register is overwritten once
NBC's tile list is in use. Clearing NBC's links to the local contrast engines
(ANR_CON1 bits 28-29, new `nr_con1_clr` parameter, default `0x30000000`) didn't
change it, and neither did leaving them set. The NBC tile list has 17 words a
tile against 16 (the extra one last; word 2 becomes 1, word 10 `0x100000`
instead of `0x40000`); the HAL's own NBC run shows 0x11 in its tile settings,
so 17 is probably right and the question is which register each word reaches.
Decoded with the tile engine's own printer (`emu_tile.py --dump-tdr`), tile 0
of the two lists differs only by `CTRL_NBC_EN` and the extra `NBC_TILE_EDGE`
word (load size 16 -> 17 words). Read live while stalled, the DIP's registers
hold tile words in the wrong places (IMGI's offset register held tile 0's
control word `0xc32a2001`; `RGB_EN` and `DMA_EN` garbage, switching on SMX and
other DMAs: the `0xc00` reads), where a working frame holds what the driver
wrote (`RGB_EN 0x8d5`, `DMA_EN 0x2001`, IMGI offset `0xa50`). So the tile
loader takes 16 words a tile from a 17-word list and slips a word each tile.
Not the cause, each tried with NBC on: SMX (the HAL's 41 MB area allocated and
all eight ports' bases set: `dip_core.h`, allocated only with `nr`), ANR_CON1's
link bits, `TDR_TILE` (0x54) at 0x10 or 0x11, and any single extra bit in
`TCM` or `TCM2` (all 62 tried, none gave a frame). The HAL's `0x11` sits in
its tile settings with or without NBC, so its lists differ from ours to begin
with; its `TCM2` 0x2100 probably goes with a list that has no NBC word. Its
real lists are built by the MDP framework's stream code, which the emulation
doesn't reach. Next: find how the real stack maps tile words to registers (the
DIP driver's or the tile loader's code), or build the list the way the HAL's
stream does. SEEE adds a `SEEE_TILE_EDGE` word the same way and will likely
meet the same problem. `configTpipeData` (`libdip_postproc.so` 0x132338) is
decompiled for this; it copies the TCM words, it doesn't compute them.

**How the HAL really builds a tile list** (found later the same day). Not
from hand-picked tile fields, as `dip_tdr.py` does, but from the DIP's
register image, in two steps: `DIP_ISP_PIPE::configTpipeData`
(`libdip_postproc.so` 0x132338) fills an `ISP_TPIPE_CONFIG` (at its
`lVar17 + 0x560`) from the module registers (NBC's mode from ANR_CON1 bits
0, 1 and 20, `NBC_ANR_LTM_LINK`; SEEE, NBC2 and the others likewise), and
`tile_copy_config_by_platform` (`libdpframework.so` 0xab030) copies that into
the tile engine's config, including the SMX enables and sizes (SMX2's enable
from `ISP_TPIPE_CONFIG` + 0x114, its input size from + 0x580). Our lists skip
both, which is why only the basic module set works. More tried with NBC on,
none giving a frame: `TCM2` = 0x100 plus each single bit (bit 13, the HAL's
NBC bit, is what makes the stray DMA reads), clearing ANR_CON1 bit 20.
Next: run that pipeline in emulation (capture the HAL's `ISP_TPIPE_CONFIG`
from the `configPipe` run with our register image, then
`tile_copy_config_by_platform` and `tile_main_platform`), which gives the list
the HAL would build, for NBC, SEEE, LSC2 and CRZ alike. The experiment
parameters (`tcm2`, `nr_con1_clr`) are still in the driver.

**The HAL's own tile lists, in emulation (2026-11-06).** `emu_dip_config.py
--size W H STRIDE --rgb RGB_EN [--nbc TABLE] --trace-tile` runs `configPipe`
with our register settings and dumps the `ISP_TPIPE_CONFIG` that
`configTpipeData` filled (at its `lVar17 + 0x6a8`; `pixel_id`, `cam_in_fmt`,
`ctl_extension_en`, `mix1_sel` confirm the layout is `tile_cfg_fields.txt`'s),
and `emu_tile.py --cfg FILE sw.tpipe_width=768` runs the tile engine on it.
Checked: for 2336x1752 with OBC2 the list is word for word ours (64 words).
With the rear NBC table the HAL's config adds only `nbc_en` and ANR's ENY, ENC
and LTM_LINK, no SMX; its list has our layout (17 words a tile) and differs
only in tile geometry (LTM_LINK widens the overlap). That list, put in
`dip_tdr.h` (it replaces ours for 2336x1752+NBC), stalls the same way: the
enable registers and IMGI's offset fill with tile words. So the list is not
what's wrong; how the DIP is started differs from the HAL's. Writing the rest
of the tile control block as the HAL's image has it (0x60, 0x64, 0x68, 0xe4
= 0, now in `dip_program`) changed nothing. The remaining difference: the HAL
loads the DIP's registers through the command queue (a descriptor of module
slots), where `dip_program` writes them directly and runs thread 0 with an
all-NOP descriptor. The emulation at this size builds no descriptor to compare;
that is the next thing to get.

**Parked (2026-11-06).** `--write2cq` calls `DIP_ISP_PIPE::write2CQ` in the
emulation and reads the descriptor back (`VirDipDrvImp::getCQDescBufVirAddr`,
`getCQModuleInfo`): slot words `0x4000_0000 | (count - 1) << 16 | address`,
the second word an offset into the HAL's register buffer, `0x1c000000` the
end. A normal frame loads 17 blocks (TDR control 0x50 x 4, 0xe0, 0x310,
0x31c, IMG3O 0x394 x 6, 0x3b8, IMGI 0x504 x 6, PGN, UDM 0x1540 x 23, G2G,
G2C, 0x3040, 0x5380 x 39, 0x1000, GGM 0x2180 x 193, OBC2, 0x5000 x 155); NBC
adds one, its whole block 0x3300-0x37e4 (314 registers; ours sets the 51 from
0x3700; below that are ANR's lookup and tone-mapping tables,
`ISP_NVRAM_ANR_LUT_T`, `Calculate_NBC_LTM`). Clearing 0x3300-0x36fc as the
HAL's image has it (now in `dip_program`) didn't help: NBC still stalls.
Untested: moving the whole pipeline onto the command queue, and the values
the HAL fills in at enqueue (0x310, 0x31c, the 0x5000 and 0x5380 blocks are
still zero at this point of the emulation). The tools stay: `emu_dip_config.py
--size --rgb --nbc --trace-tile --write2cq`, `emu_tile.py --cfg`. The text
below is the earlier (4.14) reading.

**Which modules switch which tile-loader groups (2026-11-09).** The HAL
builds the TCM enables bit by bit in `PostProcPipe::configPipe`
(`libdip_postproc.so`, no constant anywhere): the DMA ports and input
formats give the base bits (TCM 0x08182002, TCM2 0x100 for our path:
IMGI, IMG3O, UNP, UDM, C42), and the tuning image's module enables the rest:

| module (enable bit) | TCM bit |
|---|---|
| G2C (YUV_EN 3) | TCM 27 |
| SL2B-SL2E (YUV_EN 11-14) | TCM 23-26 |
| LSC2 (RGB_EN 15) | TCM 21 |
| SL2 (RGB_EN 3) | TCM 22 |
| LCE (RGB_EN 5) | TCM 31 |
| YUV2_EN 6 / 7 | TCM 0 / 28 |
| NBC, NBC2 (YUV_EN 5, 6) | TCM2 13, 14 |
| PCA, SEEE (YUV_EN 7, 8) | TCM2 11, 12 |
| NR3D / YUV_EN 9 | TCM2 2 |
| DBS2 (RGB_EN 10) | TCM2 15 |
| RMG2, BNR2, RMM2 (RGB_EN 12-14) | TCM2 16-18 |
| RGB_EN 18-20 (RNR ...) | TCM2 23-25 |
| RGB_EN 22 / 23 (FLC) / 24 | TCM2 29 / 31 / 1 |
| DMA_EN 17 | TCM2 30 |

The base list's words fit groups taken in ascending bit order (5 control
words, then IMGI 2, IMG3O 3, UNP 1, UDM 1, G2C 3 on TCM 27, C42 1), so NBC
(TCM2 13) comes last, as the tile engine emits it. Still open: with TCM2
bit 13 the loader takes 20 words a tile (TDRI_XSIZE reads back 0x4f), the
engine emits 17 (NBC_TILE_EDGE only); its register map also has
`NBC_LTM_{TOP,BOTTOM,LEFT,RIGHT}_LOSS`, which may be the missing three.
`tile_tdr_isp_mask_to_dump` (`libdpframework.so` 0x209258) lays out all 269
tile words in fixed slots, each gated by a field of `TILE_REG_MAP_STRUCT`:
running it in emulation one field at a time gives every group's size.
Probing this live hangs the phone (a stalled DIP read from userspace):
don't. `tools/re/emu_dip_config.py --watch-tcm` logs the HAL's TCM writes.
With NBC on, the watch shows the HAL writing TCM 0x08182002, TCM2 0x2100,
TCM3 0, TDR_CTL 0xb and TDR_TILE 0 (`configTpipeData` copies them to
0x50-0x5c/0xe0 from `DIP_ISP_PIPE`, whose fields sit 4 bytes below
`PostProcPipe`'s; the 0x11 in front is a software setting, not a register).
That is what split mode programmed, and the HAL's list is the 17-word
layout the base tile engine emits (emulated `tile_tdr_isp_mask_to_dump`:
NBC enable field 0x208 gates one slot, SEEE 0x238 one, NBC2 0x20c one; the
`_internal` variants are another, smaller layout). So the loader settings
and the list match the HAL's; what differs is elsewhere (the command-queue
load of the NBC block, or state in the HAL's register image).

**Noise reduction works (2026-11-09).** NBC's ANR runs on both cameras in
every mode and is on by default (`nr=1`): on a dark indoor scene (mean luma
about 4/255 before display) the luma noise, measured as the mean absolute
Laplacian, drops from 15 to 5 and the chroma noise to nearly nothing, with
edges kept and no tile seams; 300 rear frames ran without a stall. A frame
costs 14.1 ms instead of 12.1 (rear 2336x1752), 7.1 instead of 6.1 (front
1632x1224), 49.6 instead of 46.6 (rear full res). Three things were wrong:

- *The tile loader.* With `TCM2` bit 13 the loader fetches each tile in
  4-word bursts: NBC's 17-word tile is read as 20 words and the last 3 are
  dropped (markers placed there appear nowhere in 0x0-0x6000; `TDRI_XSIZE`
  reads back 0x4f). Split mode (one DIP start per tile) now pads every
  slot to a multiple of 4 words with zeros (`dip_split_layout`) and runs
  whenever NBC is on. NBC's one tile word, `NBC_TILE_EDGE`, lands in
  ANR_CON1 bits 16-19 (0xd first tile, 0xc middle, 0xe last).
- *ANR's links.* The stock tables set ANR_CON1 bit 24, `NBC_ANR_SL2_LINK`
  (ANR takes the shading module SL2's radius), and bit 20, the tile
  engine's `NBC_ANR_LTM_LINK` (ANR takes LCE's tone map). Neither partner
  runs here, and with either link set ANR takes input and never produces
  output: the DIP's debug port (`DMA_DEBUG_SEL` 0x7bc, read in 0x84, as the
  vendor driver's stall dump does; `dip_debug_port`) showed IMGI part way
  through the tile and IMG3O with nothing written. The field names and bit
  positions come from `SmoothANR` in `libcam.hal3a.v3.so`, which extracts
  each field from the register words to log it: bit 0 `ENC`, bit 1 `ENY`,
  bits 4-11 the luma filter indices, bit 24 `SL2_LINK`, bit 28 `TABLE_EN`,
  bit 29 `TBL_PRC`. Bits 28-29 enable the ANR LUT (0x3300-0x36fc), which
  the HAL builds every frame (`Paramctrl::prepareHw_PerFrame_ANR_TBL`) and
  we don't have; the driver clears 20, 24, 28 and 29 (`nr_con1_clr`
  0x31100000). Earlier notes here called 28-29 the LCE/LTM links; they
  aren't.
- *The front tile list* was generated with ANR's ENY and ENC clear, so its
  tiles carried no overlap for the filter and ANR waited for pixels that
  never came. `dip_tdr.py --nbc` now sets them (`--anr`, default both); the
  rear list (from the HAL) already had them. Lists for the full-resolution
  modes were added. Modes with no NBC list run without it.

SMX isn't needed: its crops work at tile rows (`TRU`/`TRD`, `TILE_YSIZE`)
and these tiles are full height; the 41 MB area is no longer allocated
(`nr_smx` for experiments). Left for later: the LUT (needs the HAL's
per-frame table generation), the LTM and SL2 links, and per-ISO tables
(the driver uses one mid-ISO table). A stalled DIP recovers on the next
frame's programming and the kernel's stall report is safe; only userspace
reads of a stalled DIP hung the phone. Experiment parameters: `dip_dbg`
(debug port after good frames too), `nr_lut`, `dip_keep`/`dip_pad`/`dip_padval`,
`dip_tcm2`.

**The DIP's interrupt and clock (2026-11-09).** The driver used to wait
for each frame (and each split tile) by polling `CTL_START` with
`udelay(500)`, a CPU core spinning for as long as the DIP ran: 8% of the
eight cores busy during a 30 fps rear stream. It now sleeps on the DIP's
interrupt (SPI 268, already in the DT): `CTL_CQ_INT_EN` bit 0, command
queue thread 0 done, the only enable set. Found by setting every enable
and reading the latched status after a frame: `CTL_INT_STATUS` 0x00031004
and `CTL_CQ_INT_STATUS` 0x1 (the HAL leaves the enables at 0 and waits on
GCE events instead). Both status registers are read-clear; the handler
touches them only while the camera has the DIP clocked. Same stream: 3%
busy, 360 interrupts for 90 frames of 4 tiles. `dip_poll=1` restores the
old wait.

The DIP is compute-bound: its frame time scales with its clock, img_sel.
The DT gives it 364 MHz (syspll_d3), the lower step of MediaTek's mmdvfs
table for it; the upper is 525 MHz (mmpll_d6), which needs Vcore OPP 0
(0.8 V), where 6.12 holds Vcore. Frame times (three runs each, ms):

| mode | 364 MHz | 364, NBC | 525 MHz | 525, NBC |
|---|---|---|---|---|
| rear 2336x1752 | 12.1 | 13.7 | 8.4 | 10.0 |
| rear 4672x3504 | 46.7 | 51.4 | 32.4 | 36.6 |
| front 1632x1224 | 6.1 | 7.2 | 4.2 | 5.7 |

At full resolution with NBC a 364 MHz frame takes longer than the
sensor's (about 50 ms), so the burst for stills lost every other frame
(10 drops in 10). The driver now picks the rate per stream (`dip_rate`, 0
automatic): 525 MHz above 8 MP, 364 MHz for the binned modes, where 30 fps
has room to spare and a faster clock left on for the whole session only
costs power. Full resolution at 525 MHz: no drops.

**Sharpening (SEEE) works (2026-11-09), off by default (`ee=1`).** The
same recipe as NBC: `YUV_EN` bit 8, `TCM2` bit 12, tile lists generated
with `top.seee_en` and `seee.se_edge` 2 (`dip_tdr.py --seee`;
`configTpipeData` sets `se_edge` to SEEE_CTRL & 3, and the HAL's
SEEE_CTRL is 2, EE on and SE off), split mode, and the 30 registers
0x46c4-0x473c from the stock `ISP_MGR_SEEE::apply` run on a mid-ISO EE
table (`gen-dip-tuning.py`, `*_seee` in `dip_tuning.h`; the EE table's
byte b goes to 0x46c8 + b). From `SmoothEE`'s field extraction the
table's link and extra features are all off: `GLUT_LINK_EN` (0x46e0 bit
21), `CBOOST_EN` (0x4714 bit 24), `PBC_EN` (0x471c bit 31). The tile
lists are now keyed by module set (`DIP_MOD_NBC`, `DIP_MOD_SEEE`; lists
for NBC, SEEE and both at all four sizes); NBC+SEEE tiles have 18 words.
Every mode ran clean with both on (rear 2336x1752 14.1 ms a frame, front
1632x1224 7.1, front full 24.6, rear full 50.1). On the same dark scene
the mean absolute Laplacian rose from 4.7 to 5.1 with EE (NBC on in
both): EE's gain depends on luma and cores out small detail, so it needs
judging on a lit scene before it goes on by default.

**Noise reduction needs the SMX stripe buffers (2026-11-05).** NBC's tables,
its tile lists and the `nr` switch are in the drivers, off by default: with
NBC on the DIP faults on its SMX port. Tiled noise reduction carries its
state from one tile to the next through the SMX DMAs (SMX1-4, inputs and
outputs at 0x7d0-0x920; enables SMX1 `RGB_EN` bit 25, SMX2 and SMX3
`YUV2_EN` bits 8 and 9, SMX4 `RGB2_EN` bit 0, their DMAs `DMA_EN` bits
18-24). The HAL allocates one SMX area per command queue,
`0x2710040` bytes (about 41 MB), the first port's share `0x9c4008`
(`DipRingBufferImp`, `getSMXIOBuffer`), and the tile engine gives each
tile's SMX offsets once the SMX ports are enabled in its config. Our NBC
tile lists were generated without them (`dip_tdr.py --nbc` sets only
`nbc_en`), and the HAL emulation with NBC alone leaves the SMX enables
clear, so which SMX NBC and SEEE (sharpening) use is to be found on the
phone. Next: allocate and map the SMX area, regenerate the tile lists
with SMX, then NBC and SEEE.

**The resizer: a small preview (2026-11-09).** The DIP's CRZ scales the
frame down for IMG2O (0x330) in place of the full-size IMG3O. The node
offers 960x720 (from the rear's 2336x1752) and 816x612 (from the front's
1632x1224) besides the sensor modes (`crz_sizes` in `titan_camera.c`); for
those the raw capture stays at the mode's size and the DIP job gets an
output size (`dip_job.out_w/h`). What the HAL's `configPipe` does for an
IMG2O port of 960x720 with a CRZ crop entry (`emu_dip_config.py --port 0x1e
960 720 --only-port`; the frame's crop and resize list is FrameParams
+0xa8/+0xb0, 0x34-byte entries, group 1 CRZ, crop size +0x1c/+0x20,
target +0x2c/+0x30), and what the driver writes:

- `YUV_EN` bit 17 (CRZ), `DMA_EN` IMGI + IMG2O (bit 11), `TCM` 0x08180802
  (IMG3O's bit 13 off, IMG2O's bit 11 on), `TCM2` bit 0 (CRZ).
- CRZ at 0x5300: CONTROL 0xa3, IN (h << 16 | w), OUT, the horizontal and
  vertical steps in 1/2^20 at 0x530c/0x5310; the offsets come in the tile
  words. The step and algorithm are `CrzDrvImp::CalAlgoAndCStep`'s
  (`libdip_drv`): 2x to 8x down is algorithm 1, step ((out - 1) << 20 +
  in - 2) / (in - 1) (0x69242 and 0x691e9 for 2336x1752 to 960x720, as
  the HAL's); less than 2x is algorithm 0 (6-tap, coefficient tables),
  more than 8x algorithm 2. Only algorithm 1 is supported.
- IMG2O: x size 2w - 1, y size h - 1, stride 2w | 0x01910000.
- Tile lists: `dip_tdr.py --crz W H` (the `cdrz.*` fields of the tile
  config, at 0x4c0-0x4ec as in `ISP_TPIPE_CONFIG`); 24 words a tile (an
  8-word CRZ group), the HAL's list for 960x720 word for word. Lists for
  each size with and without NBC and SEEE, keyed by output size
  (`dip_find_tdr_out`).

The DIP takes as long as for the full-size output (the work is the
input's): 11.9 ms a rear frame, 14.5 with NBC, 6.2 / 6.8 for the front;
300 rear frames clean. What it saves is the output (1.4 MB a frame instead
of 8.2, about 41 MB/s at 30 fps instead of 245) and the GPU's upload and
conversion. libcamera's simple pipeline offers only the sensor's sizes for
a capture node, so `0001-titan-pocket.patch` adds a configuration for each
of the driver's pairs (sensor size, node size) when the video node is
`titan-camera`; `cam -s width=960,height=720` then streams 1382400-byte
frames at 30 fps.

Two traps on the way, both found the hard way. With every DIP interrupt
source enabled (`dip_int_en` all ones) some `INT_STATUS` bit doesn't clear
on read: an interrupt storm (3.8 million in seconds) that hung the phone
once the clock went off under it; stopping a stream now turns the enables
off and clears the status before it lets go of the interrupt. And a stream
start that failed after the DIP was powered (no tile list) kept the ISP's
clock and power reference: the DIP then wedged and only a reboot cleared
it; that path now powers it off again.

**Planned: a small preview and zero shutter lag.** The preview now has the
DIP write the full 2336×1752 YUYV (8.2 MB a frame, about 245 MB/s at 30
fps) for the GPU to scale to the screen. The plan: the sensor in its full
mode (4672×3504, about 20 fps) with CAMSV writing a ring of the last few raw
frames; the DIP's resizer (CRZ, `YUV_EN` bit 17, `PATH_SEL` 13:12) giving
the preview at screen size (960×720, 1.4 MB a frame); at the shutter, the
sharpest frame of the ring from before the press run once at full size
with NBC and SEEE. The full-mode stream costs power while the camera is
open (about 400 MB/s of raw writes).

**Still open:** low light — at 1/100 s and full gain a dim indoor scene gives
a photo of mean 7/255 and heavy chroma noise without the flash (noise
reduction, NBC, now runs and removes most of the noise: see above); the AA luma read 21 whether the
exposure was 1147 or 3816 lines in a very dark scene, so its black level may
be off, which would make the AE under-expose — check on a lit scene with
`ae_target`; the app should keep the screen awake while it's open (Weston's
`idle-time` is 60 s); if a pre-set exposure's stream never starts,
`ae_manual` stays set until the next stream stops; `ae_step`'s `exp_max` is
the mode's nominal VTS, so it can't use the longer frames libcamera's default
frame duration gives the rear full-res mode (20.6 fps).

## On Linux 6.12 (2026-11-02: works)

`modules/camera/linux612/titan_camera.c` is the driver for the 6.12 kernel
(`modules/camera/build612.sh`); the 4.14 one in `v4l2/` is unchanged. The
sensor tables, seninf/CAMSV/CAM A sequences, statistics, AE/AWB and the DIP
path are the same code. What 4.14 found through vendor interfaces now comes
from the device tree and upstream drivers:

- **Hardware description.** `mt6771.dtsi` has `camsv1` (CAMSV1 with the
  CAM A, seninf and MIPI RX register blocks, its clocks, interrupt and the
  CAM power domain) and `dip` (pass 2, the ISP power domain), the camsys and
  imgsys clock controllers, the CAM and ISP power domains under DISP, and
  larbs 5 and 6. All of it matches the MT8183's data: the clock gates, the
  power domains' control registers, SRAM bits and bus protection (vendor
  `clk-mt6771-pg.c`), the larb addresses and their port order (vendor
  `m4u_port.h`). The board file adds the sensors as `rear` and `front`
  children of `camsv1`: I2C bus, MCLK (CAMTG/CAMTG2 at 24 MHz, set by
  `assigned-clock-parents`), the supplies and the reset and power-down
  GPIOs of the stock dtbo's `kd_camera_hw` states (rear: reset GPIO 101,
  power-down 97, AVDD the LDO on GPIO 160; front: 102, 98, VCAMA1; both
  VCAMIO and VCAMD; the rear's AK7371 on VLDO28). The rear sensor's bus,
  i2c2 (0x11009000), is a shared controller like i2c4: the AP's channel
  is at +0x100 and ATF's multi-channel register is restored after deep
  sleep (`mediatek,secure-channel-id = <2>`).
- **Buffers.** Upstream's MT8183 IOMMU puts every multimedia master in one
  group with one address space, so a buffer mapped for `camsv1` has the
  same device address on larb 5 (DIP) as on larb 6 (CAMSV, CAM A). 4.14
  mapped the raw ring twice (SOCO and IMGI); 6.12 maps it once.
  Internal buffers are `dma_alloc_coherent()` (uncached, as the 4.14
  write-combined pages were); the vb2 buffers are vb2-dma-sg's mapping for
  `camsv1`, which must be one device range (checked in `buf_init`). The
  IOMMU allows one larb per device, hence two devices; the module carries a
  small driver for `dip`, which has to be bound for the IOMMU to switch
  its ports on.
- **Power.** Runtime PM on `camsv1` and `dip` powers the CAM and ISP
  domains and, through the IOMMU's device links, larbs 6 and 5 (which
  program the ports). 4.14's clock-provider lookups by vendor ID and
  `smi_bus_prepare_enable()` are gone.
- **V4L2.** Each sensor's child device ("rear", "front") is its subdev
  node's parent through `sd->dev`; 4.14 moved the registered nodes there.
  Entity names carry the real I2C bus number. Orientation and rotation are
  the standard controls.
- **Kernel.** The media core is built as modules (`build.sh modules`; the
  camera driver needs `videodev`, `mc` and `videobuf2-*`), and
  `CONFIG_UDMABUF=y` for libcamera's software ISP.
- **The focus motor in standby (2026-11-05).** The AK7371's supply
  (VLDO28) also feeds the touchscreen, whose driver keeps it on in system
  sleep, so dropping the camera's reference at stream stop left the motor
  active, holding the lens with coil current until the next reboot. The
  driver now walks the lens back to rest in steps (no click) and writes
  standby (0x02 = 0x40) before powering the sensor off; the next start
  wakes it as before. Checked on the phone: streaming, mode 0x00 at
  position 0x40; stopped, 0x40 at 0. The rear sensor's bus is I2C 3 on
  6.12 (2 on 4.14).
- **Install.** The 6.12 system has no in-tree modules installed;
  `build612.sh` packs the ones the driver needs as
  `out612/media-modules.tar` (paths under `/lib/modules/<version>/`). Unpack
  it there, put `titan_camera.ko` in `extra/` and run `depmod`; the driver
  then loads on its device tree match.
- **Userspace.** `titan-camera-gui` finds the driver's sysfs directory and
  libcamera's camera IDs from where `1a050000.camsv*` is (on 6.12 under
  `platform/soc/`).

- **Clock rates.** On 4.14 MediaTek's mmdvfs set `cam_sel` and `img_sel`
  at late init: it forces its top step, then releases to the lowest one
  without a camera request, 364 MHz from syspll_d3 for both
  (`mmdvfs_pmqos` `cam_step1`/`img_step1`). Nothing sets them on 6.12, so
  `camsv1` and `dip` assign that parent. The vendor seninf driver only
  enables `seninf_sel` (no rate), so it is the same on both kernels. The MIPI
  RX ports' 26 MHz gates (`AP_PLL_CON8` bits 6 and 11) are on as LK leaves
  them; the vendor clock init's write that would clear them is `#if 0`.
  6.12's `clk_ignore_unused` keeps them, and `camsv1` now holds them, for
  when that option goes.
- **Checked offline.** The vendor `camera-isp` probe (isp_50) only gets its
  clocks; seninf and CAMSV are programmed by the camera HAL, which never ran
  here, so 4.14's working state came from LK and our sequences alone. The
  vendor CAM power-on defines an efuse S2P wait but never uses it. The
  compiled tree's IOMMU IDs (larb 5 ports 0-2 = 160-162; larb 6 ports 2, 3,
  11 = 194, 195, 203), power domains (CAM 8, ISP 9), larb IDs and GPIOs were
  read back from the dtb. libcamera's patch only lowers its kernel version
  check, which 6.12 passes.
- **Guards.** Probe fails unless `camsv1` and `dip` are in one IOMMU domain
  (the DIP gets camsv1's addresses; otherwise its DMA would reach other
  memory), and system suspend is refused while streaming.

**Measured on the phone (2026-11-02, `boot-6.12-deep.img`, build #86).**
The IOMMU bound with larbs 0, 5 and 6 and the display came up as before;
`titan-dip` and `titan-camera` joined IOMMU group 0 (the domain check
passed) and stay runtime-suspended with the CAM and ISP domains off
between streams. Entity names: `ov16880 3-0036`, `ov8865 2-0010`.
- Rear raw BA81 2336x1752: 30 frames, all data (the black level, 16, in
  that dark scene).
- Rear YUYV 2336x1752 through the DIP: 29.92 fps, 60 frames, no drops,
  DIP 12 ms a frame (13.7 max).
- Front YUYV 1632x1224: 30.25 fps, 300 frames, no drops, DIP 6 ms (6.8
  max); a real picture.
- AE and AWB run on both; in that dim room both sat at the 1/100 s cap
  and the sensors' full 15.5x gain (front registers read back over I2C:
  exposure 0x1800 = 384 lines, gain 0x07c0), luma 23-33 against a target
  of 46. A brighter picture there needs digital gain in the DIP or a
  longer exposure (not done).
- libcamera lists both cameras; on 6.12 it names them by device tree node
  (`/base/soc/camsv@1a050000/rear`), which `titan-camera-gui` now derives
  (`camera_id()`); the app previews and takes photos.

Not ported: the MT6370 flash (its LED driver is not enabled on 6.12; the
app hides its flash control without `/dev/flashlight`), the
`titan_camcap` and `titan_dip` bring-up modules.
