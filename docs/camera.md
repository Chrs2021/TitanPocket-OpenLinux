# Cameras (OV16880 rear, OV8865 front)

Status (2026-10-15): **phase 1 done for both cameras**.
- Each sensor streams over MIPI into the seninf receiver and a CAMSV DMA
  engine, which writes full RAW10 frames to memory:
  - front OV8865: 1632×1224;
  - rear OV16880: 2336×1752.
- The rear focus motor (AK7371) works.
- `tools/camera/raw2png.py` turns frames into pictures.
- Next: proper V4L2 drivers (phase 2).

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
| 2 | proper drivers | V4L2: OV8865 (backport from mainline), OV16880 (new, from the stock init tables), seninf + CAMSV as a media-controller capture device, focus VCM, flash LED |
| 3 | camera app | libcamera (simple pipeline + software ISP: debayer, AE, AWB), GStreamer, a camera app |

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
  - CAM A/B/C 0x1a003000, 0x1a004000, 0x1a006000;
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
