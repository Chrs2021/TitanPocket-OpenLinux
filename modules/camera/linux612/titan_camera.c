// SPDX-License-Identifier: GPL-2.0
/*
 * titan_camera: V4L2 driver for the Titan Pocket cameras (docs/camera.md).
 *
 * Media graph:
 *
 *   "ov16880 2-0036"  (rear sensor subdev, /dev/v4l-subdevN) --\
 *                                                               >--> "titan-camsv" (/dev/videoN)
 *   "ov8865 4-0010"   (front sensor subdev, /dev/v4l-subdevN) --/
 *
 * (entity names follow the kernel's "model bus-addr" convention, which
 * libcamera parses for the sensor model)
 *
 * Enable one sensor's link and stream from the video node. The video node is
 * a CAMSV raw DMA engine fed through the MIPI CSI-2 receiver (seninf). It
 * delivers BGGR Bayer as 8-bit samples (BA81, the top 8 of the sensor's 10
 * bits, through CAMSV's RAW8 packer) or as 16-bit words holding the 10-bit
 * samples << 4 (BG14: packer off). The sensor subdevs
 * carry the controls libcamera wants: exposure, analogue gain, vertical
 * blanking, plus read-only horizontal blanking and pixel rate; the rear one
 * also its focus motor (FOCUS_ABSOLUTE).
 *
 * Autofocus statistics: while the rear camera streams, CAM A (MediaTek's ISP
 * pass 1) runs beside CAMSV on the same seninf output in statistics-only
 * mode: TG -> DMX -> SGG1 -> AF, written by its AFO DMA (16 x 12 blocks of 40
 * bytes, the stock default AF configuration; docs/camera.md, "Hardware AF
 * statistics"). /sys/.../rear/af_stats returns the latest set; af_pgn sets
 * SGG1's gain in front of the AF filters, by default automatically: the AF
 * filter outputs scale with 1/gain and saturate when the gain is too small
 * for the scene, so the gain follows the scene brightness (the blocks' pixel
 * sums, word 3, measured before SGG1).
 *
 * Linux 6.12 version (the 4.14 one is ../v4l2/titan_camera.c). The device
 * tree describes the hardware: the camsv1 node (CAMSV1, CAM A, seninf, MIPI
 * RX, their clocks and the CAM power domain), its "rear" and "front" sensor
 * children (I2C bus, MCLK, supplies, reset and power-down GPIOs) and the dip
 * node (ISP pass 2, the ISP power domain). The sensor modes and register
 * tables are here. The IOMMU gives all multimedia masters one address
 * space, so a buffer mapped for CAMSV1 has the same address on the DIP's
 * ports (larb 5) as on CAMSV1's (larb 6). Bring-up notes and the recovered
 * register sequences: modules/camera/camcap and docs/camera.md.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/media-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-sg.h>
#include <media/videobuf2-v4l2.h>


#include "../camcap/seninf_reg.h"
#include "../camcap/ov8865_regs.h"
#include "../camcap/ov16880_regs.h"
#include "../camcap/af_regs_rear.h"
#include "dip_core.h"			/* pass-2 ISP (DIP) register programming */

static bool hwisp = true;
module_param(hwisp, bool, 0644);
MODULE_PARM_DESC(hwisp, "offer the YUYV format (hardware ISP, pass 2) as well as raw Bayer");

static bool nr = true;	/* noise reduction (NBC/ANR): split mode, docs/camera.md */
module_param(nr, bool, 0644);
MODULE_PARM_DESC(nr, "noise reduction (NBC) in the hardware ISP YUYV path");
static bool dip_split;	/* one DIP start per tile (dip_core.h dip_run) */
module_param(dip_split, bool, 0644);
MODULE_PARM_DESC(dip_split, "run the DIP one tile per start (always with nr)");
static int dip_keep, dip_pad;	/* split: tile slot layout (dip_core.h) */
static uint dip_padval = ~0u;
module_param(dip_keep, int, 0644);
module_param(dip_pad, int, 0644);
module_param(dip_padval, uint, 0644);
static uint dip_tcm2;	/* experiments: TCM2 (0x5c) override, 0 = auto */
module_param(dip_tcm2, uint, 0644);
MODULE_PARM_DESC(dip_tcm2, "DIP TCM2 override for experiments (0: auto)");
static bool dip_map;	/* experiments: map the tile loader (dip_core.h) */
module_param(dip_map, bool, 0644);
static uint dip_tcm0 = DIP_TCM0, dip_tcm3, dip_map_xor;
module_param(dip_map_xor, uint, 0644);
module_param(dip_tcm0, uint, 0644);
module_param(dip_tcm3, uint, 0644);
/* ANR_CON1 bits the stock tables set for modules this driver doesn't run:
 * SL2_LINK (24, the shading module's radius), LTM_LINK (20, LCE) and the
 * ANR LUT (TABLE_EN 28, TBL_PRC 29; the HAL fills the LUT every frame).
 * With either link set ANR waits for its partner and the DIP stalls. */
static uint nr_con1_clr = 0x31100000;
module_param(nr_con1_clr, uint, 0644);
MODULE_PARM_DESC(nr_con1_clr, "bits cleared in NBC's ANR_CON1 (0x3700)");
static uint nr_lut;	/* experiments: the ANR LUT (0x3300-0x36fc), the HAL fills it per frame */
module_param(nr_lut, uint, 0644);
static bool ee;	/* edge enhancement (SEEE's EE): docs/camera.md */
module_param(ee, bool, 0644);
MODULE_PARM_DESC(ee, "sharpening (SEEE edge enhancement) in the hardware ISP YUYV path");
static bool nr_smx;	/* experiments: allocate SMX for NBC (unused by these tile lists) */
module_param(nr_smx, bool, 0644);
/* img_sel, the DIP's clock, set at every stream start. 0: automatic, 364 MHz
 * (syspll_d3, the vendor's lower mmdvfs step) for the binned modes and 525 MHz
 * (mmpll_d6, its upper step; needs Vcore OPP 0, which 6.12 holds) for full
 * resolution, where at 364 MHz a frame with NBC (51 ms) is slower than the
 * sensor's (50 ms). The DIP is compute-bound: its time scales with the clock. */
static uint dip_rate;
module_param(dip_rate, uint, 0644);
static bool dip_poll;	/* wait for the DIP by polling (busy) instead of its interrupt */
module_param(dip_poll, bool, 0644);
/* experiments: more DIP interrupt enables. Not all: some INT_STATUS bit
 * doesn't clear on read, and every source on is an interrupt storm */
static uint dip_int_en, dip_cq_int_en;
module_param(dip_int_en, uint, 0644);
module_param(dip_cq_int_en, uint, 0644);
static bool dip_trace;	/* experiments: each frame's START, time and interrupt status */
module_param(dip_trace, bool, 0644);
static bool dip_dbg;	/* experiments: the DIP debug port after good frames too */
module_param(dip_dbg, bool, 0644);
MODULE_PARM_DESC(nr_lut, "value for every word of NBC's ANR LUT (experiments; 0 as the HAL image)");

/* auto white balance / exposure from the CAM A hardware statistics (AAO).
 * aa drives the DIP white balance from the per-block R/G/B averages (instead
 * of the grey-world subsample); ae closes the exposure/gain loop on the sensor
 * from the per-block luma. Both only run with the hardware ISP (YUYV) path. */
static bool aa = true;
module_param(aa, bool, 0644);
MODULE_PARM_DESC(aa, "run the CAM A AA statistics block (feeds auto exposure and, with aa_wb, white balance)");

static bool aa_wb;	/* white balance from AA stats; default is the CPU grey-world */
module_param(aa_wb, bool, 0644);
MODULE_PARM_DESC(aa_wb, "white balance from the AA statistics (default: CPU grey-world over the raw frame)");

static bool ae = true;
module_param(ae, bool, 0644);
MODULE_PARM_DESC(ae, "auto exposure/gain from the CAM A AA statistics");

/* 8-bit AA luma (~1/4 of the 10-bit raw) above black. 46 (mid-grey in linear terms) gave
 * photos of mean Y ~49 in an ordinary room: the linear mean is pulled up by highlights;
 * 80 gives ~75 with no clipping (measured 2026-11-09, once the OV16880's gain worked) */
static unsigned int ae_target = 80;
module_param(ae_target, uint, 0644);
MODULE_PARM_DESC(ae_target, "target mean scene luma for auto exposure (0-255)");

/* past this the loop raises gain instead: a shaky hand blurs longer exposures */
static unsigned int ae_max_us = 10000;
module_param(ae_max_us, uint, 0644);
MODULE_PARM_DESC(ae_max_us, "longest auto exposure in microseconds, then gain (0: the frame length)");

#define DRV "titan-camera"

/* output formats of the video node */
struct titan_fmt {
	u32 fourcc;
	u32 bytes;		/* output bytes per pixel */
	const char *desc;
	bool dip;		/* through the hardware ISP: packed RAW10 in, this out */
};

static const struct titan_fmt formats[] = {
	{ V4L2_PIX_FMT_YUYV, 2, "YUYV 4:2:2 (hardware ISP)", true },
	{ V4L2_PIX_FMT_SBGGR8, 1, "8-bit Bayer BGGR" },
	{ V4L2_PIX_FMT_SBGGR14, 2, "14-bit Bayer BGGR (10-bit << 4)" },
};

static const struct titan_fmt *find_format(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (formats[i].fourcc == fourcc && (hwisp || !formats[i].dip))
			return &formats[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* the sensors                                                         */

struct sensor_mode {
	const struct reg8 *regs;
	int nregs;
	u32 width, height;
	u32 hts, vts;		/* stock line length and frame length */
	u32 fps;		/* measured with the stock timing */
	bool pix2;		/* seninf and CAMSV move 2 pixels per clock */
};

struct sensor_desc {
	const char *model;	/* entity name: model, then bus-address */
	const char *node;	/* its child node of camsv1 */
	u32 orientation;	/* V4L2_CAMERA_ORIENTATION_* */
	u32 rotation;		/* V4L2_CID_CAMERA_SENSOR_ROTATION, degrees ccw */
	u16 i2c_addr;
	int csi_port;
	unsigned int step_ms;
	const struct reg8 *init;
	int ninit;
	const struct sensor_mode *modes;
	int nmodes;
	bool needs_stream_on;
	bool has_focus;
	u32 black;		/* black level, 10-bit */
	u32 array_w, array_h;	/* active pixel array; every mode covers all of it */
	/*
	 * OV16880: the analogue gain is 0x350b, 0x40 = 1x, linear to 0xff (~4x), 0x350a = 0;
	 * 0x3508/0x3509 change nothing in these modes (measured 2026-11-09: dark noise and a
	 * torch-lit scene, raw). OV8865: 0x3508/0x3509, 0x80 = 1x.
	 */
	bool gain_350b;
};

#define TBL(t) t, ARRAY_SIZE(t)
/* the rear full-resolution mode is too fast for 1 pixel per clock */
static const struct sensor_mode ov16880_modes[] = {
	{ TBL(ov16880_preview), 2336, 1752, 2512, 3824, 30 },
	{ TBL(ov16880_capture), 4672, 3504, 5024, 3824, 30, true },
};
static const struct sensor_mode ov8865_modes[] = {
	{ TBL(ov8865_preview), 1632, 1224, 1923, 1280, 30 },
	{ TBL(ov8865_capture), 3264, 2448, 1944, 2550, 30 },
};

static const struct sensor_desc sensors[] = {
	/* rotations checked on the phone held upright in portrait */
	{ "ov16880", "rear", V4L2_CAMERA_ORIENTATION_BACK, 270, 0x36, 0, 1, TBL(ov16880_init),
	  TBL(ov16880_modes), true, true, 64, 4672, 3504, true },
	{ "ov8865", "front", V4L2_CAMERA_ORIENTATION_FRONT, 90, 0x10, 1, 5, TBL(ov8865_init),
	  TBL(ov8865_modes), false, false, 16, 3264, 2448 },
};
#define NSENSORS ARRAY_SIZE(sensors)

struct titan_cam;

struct titan_sensor {
	const struct sensor_desc *d;
	const struct sensor_mode *mode;
	struct titan_cam *tc;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure, *gain, *vblank, *hblank, *pixel_rate, *focus;
	u32 dgain;		/* the gain past the sensor's, in the DIP (512 = 1x; 0: none) */
	struct device_node *np;	/* its child node of camsv1 */
	struct i2c_adapter *adap;
	struct regulator *dovdd, *avdd, *dvdd, *afvdd;
	unsigned int supplies_on;	/* bit per supply, in power-on order */
	struct gpio_desc *reset, *pwdn;
	struct clk *mclk;
	bool powered, streaming;
	bool ae_manual;		/* app set exposure/gain by hand: the AE loop backs off */
	bool ctrl_internal;	/* the driver itself is re-ranging the exposure control */
	u32 wb[3];		/* PGN gains at the end of the last YUYV stream, 0: none */
	struct device dev;	/* "rear" / "front": parent of the subdev node */
	bool dev_added;
};

/* ------------------------------------------------------------------ */
/* the device                                                          */

#define LANES		4

struct titan_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	dma_addr_t mva;		/* device address (IOMMU) */
};

/* the DIP (pass 2) device: titan-dip, bound by dip_probe */
struct titan_dip {
	void __iomem *regs;
	struct clk *clk;
	bool live;			/* clocked: the interrupt may read the registers */
	int irq;
	struct completion done;		/* CQ thread 0 done (dip_core.h dip_wait_job) */
	u32 seen_int, seen_cq;		/* experiments: status bits the interrupt read (dip_trace) */
};

struct titan_cam {
	struct platform_device *pdev;	/* the camsv1 node: our DMA device and IRQ */
	struct device *dev;		/* &pdev->dev */
	struct device *dip_dev;		/* the dip node: ISP power domain, larb 5 */
	struct clk *dip_clk;
	struct titan_dip *dipc;		/* the DIP device's state: its interrupt */
	struct clk_bulk_data *clks;	/* camsv1's: seninf, CAM, CAMSV */
	int nclks;
	struct media_pipeline pipe;
	struct media_device mdev;
	struct v4l2_device v4l2;
	struct video_device vdev;
	struct media_pad vpad;
	struct vb2_queue queue;
	struct mutex lock;
	struct titan_sensor sensor[NSENSORS];
	struct titan_sensor *active;	/* the streaming sensor */

	void __iomem *seninf, *mipi_rx, *camsv, *cama;
	int irq;
	bool clocks_on;
	struct mutex pm_lock;
	struct mutex sensor_lock;	/* sensor modes (not tc->lock: link validation
					 * runs under it, inside start_streaming) */
	int pm_count;		/* camsys power users: buffers mapped, streaming */

	/* buffers */
	spinlock_t qlock;
	struct list_head queued;
	struct titan_buf *cur, *next;	/* in the DMA now / programmed for the next frame */
	void *scratch_vaddr;		/* one page: CAMSV's frame headers */
	dma_addr_t scratch_mva;
	/* AF statistics (CAM A AFO) */
	dma_addr_t afo_mva;
	void *afo_vaddr;		/* uncached */
	struct mutex af_lock;
	bool af_running;
	u32 af_pgn;
	bool af_pgn_auto;
	unsigned long af_pgn_changed;	/* jiffies */
	struct delayed_work af_work;
	/* AE/AWB statistics (CAM A AAO): per-block R/G/B + luma averages */
	dma_addr_t aao_mva;
	void *aao_vaddr;		/* uncached */
	bool aa_running;
	u32 ae_exp, ae_gain;		/* last applied: exposure lines, gain (128 = 1x) */
	u32 ae_luma;			/* last luma the loop saw */
	bool ae_cpu;			/* ... from the CPU raw subsample, not the AAO */
	u32 sequence;

	/* format */
	const struct titan_fmt *fmt;
	u32 width, height, stride;
	u32 out_w, out_h;		/* the node's size: the frame's, or CRZ's (crz_sizes) */

	/* hardware ISP (DIP, pass 2): CAMSV captures packed RAW10 into a raw
	 * ring; a worker runs the DIP per frame into the YUYV vb2 buffer */
	bool imgsys_on;
	void __iomem *dip;
	u32 raw_stride, raw_bytes;	/* packed RAW10 line pitch / used bytes */
	int black;			/* OBC2 black level, -1 for front (OBC in pass 1) */
	struct dip_buf {		/* uncached, IOMMU-mapped */
		void *vaddr;
		dma_addr_t mva;		/* CAMSV writes / DIP reads it here */
		size_t size;
	} raw[3];
	int rc, rn;			/* raw ring: index being filled / programmed next */
	struct dip_buf tdr, desc;	/* DIP tile list + CQ descriptor */
	struct dip_buf smx;		/* NBC's stripe memory (dip_core.h) */
	bool nr, ee;			/* this stream runs NBC / SEEE: asked, and a tile list */
	struct work_struct dip_work;
	int dip_ready;			/* raw ring index ready for the DIP, -1 none */
	bool dip_busy;
	int raw_last;			/* raw ring index the DIP last took, -1 none */
	u32 wb[3];			/* current PGN gains R, G, B */
	u64 dip_frames, dip_drops;
	u64 dip_nobuf, dip_rawerr;	/* frames lost: no YUYV buffer queued, CAMSV error */
	u32 dip_us, dip_us_max;		/* last / worst DIP time per frame */
};


/* ------------------------------------------------------------------ */
/* power: the CAM and ISP power domains (runtime PM), clocks            */

/*
 * camsys power, reference counted: streaming, and the probe's register
 * pokes. Runtime PM on camsv1 powers the CAM domain and, through the
 * IOMMU's device link, larb 6 (which enables our ports in the IOMMU).
 */
static int camsys_on(struct titan_cam *tc);
static void camsys_off(struct titan_cam *tc);

static int camsys_get(struct titan_cam *tc)
{
	int ret = 0;

	mutex_lock(&tc->pm_lock);
	if (tc->pm_count++ == 0) {
		ret = camsys_on(tc);
		if (ret)
			tc->pm_count--;
	}
	mutex_unlock(&tc->pm_lock);
	return ret;
}

static void camsys_put(struct titan_cam *tc)
{
	mutex_lock(&tc->pm_lock);
	if (--tc->pm_count == 0)
		camsys_off(tc);
	mutex_unlock(&tc->pm_lock);
}

static int camsys_on(struct titan_cam *tc)
{
	int ret;

	ret = pm_runtime_resume_and_get(tc->dev);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(tc->nclks, tc->clks);
	if (ret) {
		pm_runtime_put(tc->dev);
		return ret;
	}
	tc->clocks_on = true;
	return 0;
}

static void camsys_off(struct titan_cam *tc)
{
	if (!tc->clocks_on)
		return;
	clk_bulk_disable_unprepare(tc->nclks, tc->clks);
	pm_runtime_put(tc->dev);
	tc->clocks_on = false;
}

/* imgsys power for the DIP (pass 2), on only while a hwisp stream runs:
 * the ISP domain and larb 5, through the dip node */
/* the DIP's clock for this stream's mode (dip_rate) */
static void dip_set_rate(struct titan_cam *tc)
{
	struct clk *mux = clk_get_parent(tc->dip_clk);
	u32 rate = dip_rate ?: tc->width * tc->height > 8000000 ? 525000000 : 364000000;

	if (mux && clk_set_rate(mux, rate))
		dev_warn(tc->dev, "img clock: %u Hz not set\n", rate);
}

static int imgsys_on(struct titan_cam *tc)
{
	int ret;

	if (tc->imgsys_on)
		return 0;
	ret = pm_runtime_resume_and_get(tc->dip_dev);
	if (ret)
		return ret;
	ret = clk_prepare_enable(tc->dip_clk);
	if (ret) {
		pm_runtime_put(tc->dip_dev);
		return ret;
	}
	WRITE_ONCE(tc->dipc->live, true);
	tc->imgsys_on = true;
	return 0;
}

static void imgsys_off(struct titan_cam *tc)
{
	if (!tc->imgsys_on)
		return;
	/* no interrupt may outlive the clock: enables off, status read-clear */
	writel(0, tc->dip + 0x020);
	writel(0, tc->dip + 0x024);
	readl(tc->dip + DIP_INT_STATUS);
	readl(tc->dip + DIP_CQ_STATUS);
	readl(tc->dip + 0x038);
	WRITE_ONCE(tc->dipc->live, false);
	synchronize_irq(tc->dipc->irq);
	clk_disable_unprepare(tc->dip_clk);
	pm_runtime_put(tc->dip_dev);
	tc->imgsys_on = false;
}

/* an uncached buffer at a device (IOMMU) address, for CAMSV, CAM A and the
 * DIP alike (one address space) */
static int dbuf_alloc(struct titan_cam *tc, void **vaddr, dma_addr_t *mva, size_t size)
{
	*vaddr = dma_alloc_coherent(tc->dev, size, mva, GFP_KERNEL);
	return *vaddr ? 0 : -ENOMEM;
}

static void dbuf_free(struct titan_cam *tc, void **vaddr, dma_addr_t *mva, size_t size)
{
	if (*vaddr)
		dma_free_coherent(tc->dev, size, *vaddr, *mva);
	*vaddr = NULL;
	*mva = 0;
}

/* ------------------------------------------------------------------ */
/* sensor power and registers                                          */

/* supply n of the power-on order, at uv; supplies_on tracks what to undo */
static int reg_on(struct titan_sensor *s, struct regulator *r, int n, int uv)
{
	int ret;

	ret = regulator_set_voltage(r, uv, uv);
	if (!ret)
		ret = regulator_enable(r);
	if (!ret)
		s->supplies_on |= BIT(n);
	return ret;
}

static void reg_off(struct titan_sensor *s, struct regulator *r, int n)
{
	if (s->supplies_on & BIT(n))
		regulator_disable(r);
	s->supplies_on &= ~BIT(n);
}

static int s_write(struct titan_sensor *s, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = { .addr = s->d->i2c_addr, .len = 3, .buf = buf };

	return i2c_transfer(s->adap, &msg, 1) == 1 ? 0 : -EIO;
}

static int s_read(struct titan_sensor *s, u16 reg, u8 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = s->d->i2c_addr, .len = 2, .buf = buf },
		{ .addr = s->d->i2c_addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return i2c_transfer(s->adap, msgs, 2) == 2 ? 0 : -EIO;
}

static int s_table(struct titan_sensor *s, const struct reg8 *t, int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		if (t[i].reg == REG_DELAY) {
			msleep(t[i].val);
			continue;
		}
		ret = s_write(s, t[i].reg, t[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

/* focus motor of the rear camera: AKM AK7371 at i2c2 0x0c */
#define AF_ADDR 0x0c
static int af_write(struct titan_sensor *s, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = AF_ADDR, .len = 2, .buf = buf };

	return i2c_transfer(s->adap, &msg, 1) == 1 ? 0 : -EIO;
}

static void af_set(struct titan_sensor *s, int pos)
{
	af_write(s, 0x00, pos >> 2);
	af_write(s, 0x01, (pos & 3) << 6);
}

/* the stock power sequence (docs/camera.md); reset and power-down are
 * active low: asserted (1) holds the sensor off */
static int sensor_power_on(struct titan_sensor *s)
{
	const struct sensor_desc *d = s->d;
	int ret;

	if (s->powered)
		return 0;
	ret = clk_prepare_enable(s->mclk);	/* 24 MHz (assigned-clock-parents) */
	if (ret)
		return ret;
	gpiod_set_value_cansleep(s->pwdn, 1);
	gpiod_set_value_cansleep(s->reset, 1);
	s->powered = true;	/* from here power_off undoes a partial sequence */
	mdelay(d->step_ms);
	ret = reg_on(s, s->dovdd, 0, 1800000);
	if (ret)
		return ret;
	mdelay(d->step_ms);
	ret = reg_on(s, s->avdd, 1, 2800000);
	if (ret)
		return ret;
	mdelay(d->step_ms);
	ret = reg_on(s, s->dvdd, 2, 1200000);
	if (ret)
		return ret;
	mdelay(5);
	if (s->afvdd)
		reg_on(s, s->afvdd, 3, 2800000);
	gpiod_set_value_cansleep(s->pwdn, 0);
	mdelay(d->step_ms);
	gpiod_set_value_cansleep(s->reset, 0);
	mdelay(10);
	return 0;
}

static void sensor_power_off(struct titan_sensor *s)
{
	int pos;

	if (!s->powered)
		return;
	/*
	 * The motor's supply (vldo28) also feeds the touchscreen, which keeps
	 * it on even in system sleep: dropping our reference leaves an active
	 * AK7371 holding the lens with coil current. Walk the lens back to
	 * rest (no click) and put it in standby (0x40) first.
	 */
	if (s->d->has_focus) {
		for (pos = s->focus->val; pos > 0; pos -= 64) {
			af_set(s, pos);
			usleep_range(2000, 3000);
		}
		af_set(s, 0);
		af_write(s, 0x02, 0x40);
	}
	gpiod_set_value_cansleep(s->reset, 1);
	gpiod_set_value_cansleep(s->pwdn, 1);
	reg_off(s, s->afvdd, 3);
	reg_off(s, s->dvdd, 2);
	reg_off(s, s->avdd, 1);
	reg_off(s, s->dovdd, 0);
	clk_disable_unprepare(s->mclk);
	s->powered = false;
}

/* exposure (1/16 line units), gain and frame length as one group-hold
 * update; both OmniVision sensors use the same registers */
#define OV16880_GAIN_MAX	0xff	/* ~4x: 0x350b's linear range */

/*
 * The gain control (128 = 1x, to 15.5x) to the sensor's registers, inside the caller's group
 * hold. Where the sensor's analogue gain stops (OV16880: ~4x), the rest is digital, applied by
 * the DIP with the white balance (dip_work_fn); the AE loop counts it (ae_step).
 */
static void sensor_gain_regs(struct titan_sensor *s, u32 gain)
{
	u32 a;

	if (!s->d->gain_350b) {
		s_write(s, 0x3508, gain >> 8);
		s_write(s, 0x3509, gain & 0xff);
		s->dgain = 0;
		return;
	}
	a = clamp_t(u32, gain / 2, 0x40, OV16880_GAIN_MAX);	/* 128 -> 0x40 */
	s_write(s, 0x350a, 0);
	s_write(s, 0x350b, a);
	s->dgain = gain * 256 / a;	/* gain / (2a) in 1/512 */
	if (s->dgain <= 512)
		s->dgain = 0;
}

static inline u32 sensor_dgain(const struct titan_sensor *s)
{
	return s && s->dgain ? s->dgain : 512;
}

static void sensor_apply_ctrls(struct titan_sensor *s)
{
	u32 e = s->exposure->val << 4;
	u32 vts = s->mode->height + s->vblank->val;

	s_write(s, 0x3208, 0x00);
	s_write(s, 0x380e, vts >> 8);
	s_write(s, 0x380f, vts & 0xff);
	s_write(s, 0x3500, e >> 16);
	s_write(s, 0x3501, (e >> 8) & 0xff);
	s_write(s, 0x3502, e & 0xff);
	sensor_gain_regs(s, s->gain->val);
	s_write(s, 0x3208, 0x10);
	s_write(s, 0x3208, 0xa0);
}

static inline struct titan_sensor *to_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct titan_sensor, sd);
}

static void sensor_update_limits(struct titan_sensor *s)
{
	const struct sensor_mode *m = s->mode;
	u64 rate = (u64)m->hts * m->vts * m->fps;
	u32 vblank_def = m->vts - m->height;

	__v4l2_ctrl_modify_range(s->pixel_rate, rate, rate, 1, rate);
	__v4l2_ctrl_modify_range(s->hblank, m->hts - m->width, m->hts - m->width, 1,
				 m->hts - m->width);
	__v4l2_ctrl_modify_range(s->vblank, vblank_def, 0x7fff - m->height, 1, vblank_def);
	s->ctrl_internal = true;	/* may clamp the value and call s_ctrl */
	__v4l2_ctrl_modify_range(s->exposure, 4, m->vts - 8, 1, m->vts - 64);
	s->ctrl_internal = false;
}

static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct titan_sensor *s = container_of(ctrl->handler, struct titan_sensor, ctrls);

	if (ctrl->id == V4L2_CID_VBLANK) {	/* frame length bounds exposure */
		u32 vts = s->mode->height + ctrl->val;

		s->ctrl_internal = true;
		__v4l2_ctrl_modify_range(s->exposure, 4, vts - 8, 1,
					 min_t(s32, s->exposure->default_value, vts - 8));
		s->ctrl_internal = false;
	}
	/*
	 * Exposure/gain set by the app (e.g. flash metering, or a metered still
	 * set just before its stream starts): the in-kernel AE loop backs off
	 * until this stream stops. Our own AE writes go straight to the sensor
	 * (ae_apply), not through the control framework, and the driver's own
	 * range updates are flagged, so neither lands here.
	 */
	if ((ctrl->id == V4L2_CID_EXPOSURE || ctrl->id == V4L2_CID_ANALOGUE_GAIN) &&
	    !s->ctrl_internal)
		s->ae_manual = true;
	if (!s->streaming)
		return 0;	/* applied at stream on */
	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		af_set(s, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
	case V4L2_CID_ANALOGUE_GAIN:
	case V4L2_CID_VBLANK:
		sensor_apply_ctrls(s);
		break;
	}
	return 0;
}

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.s_ctrl = sensor_s_ctrl,
};

static int sensor_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sensor_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct titan_sensor *s = to_sensor(sd);

	if (fse->index >= s->d->nmodes || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = s->d->modes[fse->index].width;
	fse->min_height = fse->max_height = s->d->modes[fse->index].height;
	return 0;
}

static void fill_fmt(const struct sensor_mode *m, struct v4l2_mbus_framefmt *f)
{
	memset(f, 0, sizeof(*f));
	f->width = m->width;
	f->height = m->height;
	f->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
}

static int sensor_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct titan_sensor *s = to_sensor(sd);

	mutex_lock(&s->tc->sensor_lock);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(state, 0);
	else
		fill_fmt(s->mode, &fmt->format);
	mutex_unlock(&s->tc->sensor_lock);
	return 0;
}

static int sensor_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct titan_sensor *s = to_sensor(sd);
	const struct sensor_mode *best = &s->d->modes[0];
	int i;

	/* the mode closest in size */
	for (i = 1; i < s->d->nmodes; i++) {
		const struct sensor_mode *m = &s->d->modes[i];

		if (abs((int)m->width - (int)fmt->format.width) <
		    abs((int)best->width - (int)fmt->format.width))
			best = m;
	}
	fill_fmt(best, &fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(state, 0) = fmt->format;
		return 0;
	}
	mutex_lock(&s->tc->sensor_lock);
	if (s->streaming) {
		mutex_unlock(&s->tc->sensor_lock);
		return -EBUSY;
	}
	s->mode = best;
	v4l2_ctrl_lock(s->exposure);
	sensor_update_limits(s);
	v4l2_ctrl_unlock(s->exposure);
	mutex_unlock(&s->tc->sensor_lock);
	return 0;
}

/* the pixel array; libcamera reads it for the sensor's properties */
static int sensor_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct titan_sensor *s = to_sensor(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:		/* binned or full: always the whole array */
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = s->d->array_w;
		sel->r.height = s->d->array_h;
		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.enum_frame_size = sensor_enum_frame_size,
	.get_fmt = sensor_get_fmt,
	.set_fmt = sensor_set_fmt,
	.get_selection = sensor_get_selection,
};

static const struct v4l2_subdev_ops sensor_ops = {
	.pad = &sensor_pad_ops,
};

/* power up, program the mode, apply the controls, start streaming */
static int sensor_start(struct titan_sensor *s)
{
	u8 hi = 0, lo = 0;
	int ret;

	ret = sensor_power_on(s);
	if (ret)
		return ret;
	s_read(s, 0x300b, &hi);
	s_read(s, 0x300c, &lo);
	dev_dbg(s->tc->dev, "%s id %02x%02x\n", s->sd.name, hi, lo);
	ret = s_table(s, s->d->init, s->d->ninit);
	if (!ret)
		ret = s_table(s, s->mode->regs, s->mode->nregs);
	if (ret)
		return ret;
	if (s->d->has_focus) {
		af_write(s, 0x02, 0x00);	/* AK7371: standby -> active */
		af_set(s, s->focus->val);
	}
	s->streaming = true;
	sensor_apply_ctrls(s);
	if (s->d->needs_stream_on)
		s_write(s, 0x0100, 1);
	return 0;
}

static void sensor_stop(struct titan_sensor *s)
{
	if (s->powered)
		s_write(s, 0x0100, 0);
	s->streaming = false;
	s->ae_manual = false;	/* the next stream starts under the AE loop */
	sensor_power_off(s);
}

static int sensor_init_one(struct titan_cam *tc, struct titan_sensor *s,
			   const struct sensor_desc *d)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	int ret;

	struct device_node *bus;

	s->d = d;
	s->tc = tc;
	s->mode = &d->modes[0];
	s->np = of_get_child_by_name(tc->dev->of_node, d->node);
	if (!s->np)
		return dev_err_probe(tc->dev, -ENODEV, "no %s node\n", d->node);
	bus = of_parse_phandle(s->np, "i2c-bus", 0);
	s->adap = bus ? of_get_i2c_adapter_by_node(bus) : NULL;
	of_node_put(bus);
	if (!s->adap)
		return dev_err_probe(tc->dev, -EPROBE_DEFER, "%s: no i2c-bus\n", d->node);
	s->mclk = devm_get_clk_from_child(tc->dev, s->np, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(tc->dev, PTR_ERR(s->mclk), "%s: no clock\n", d->node);
	s->reset = devm_fwnode_gpiod_get(tc->dev, of_fwnode_handle(s->np), "reset",
					 GPIOD_OUT_HIGH, d->node);
	if (IS_ERR(s->reset))
		return dev_err_probe(tc->dev, PTR_ERR(s->reset), "%s: reset\n", d->node);
	s->pwdn = devm_fwnode_gpiod_get(tc->dev, of_fwnode_handle(s->np), "powerdown",
					GPIOD_OUT_HIGH, d->node);
	if (IS_ERR(s->pwdn))
		return dev_err_probe(tc->dev, PTR_ERR(s->pwdn), "%s: powerdown\n", d->node);

	v4l2_subdev_init(&s->sd, &sensor_ops);
	snprintf(s->sd.name, sizeof(s->sd.name), "%s %d-%04x", d->model, s->adap->nr,
		 d->i2c_addr);
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.owner = THIS_MODULE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(h, 10);
	s->exposure = v4l2_ctrl_new_std(h, &sensor_ctrl_ops, V4L2_CID_EXPOSURE, 4,
					s->mode->vts - 8, 1, s->mode->vts - 64);
	s->gain = v4l2_ctrl_new_std(h, &sensor_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				    128, 1984, 1, 512);	/* 128 = 1x */
	s->vblank = v4l2_ctrl_new_std(h, &sensor_ctrl_ops, V4L2_CID_VBLANK, 0, 0x7fff, 1, 0);
	s->hblank = v4l2_ctrl_new_std(h, &sensor_ctrl_ops, V4L2_CID_HBLANK, 0, 0xffff, 1, 0);
	s->pixel_rate = v4l2_ctrl_new_std(h, &sensor_ctrl_ops, V4L2_CID_PIXEL_RATE,
					  1, S64_MAX, 1, 1);
	if (d->has_focus)
		s->focus = v4l2_ctrl_new_std(h, &sensor_ctrl_ops, V4L2_CID_FOCUS_ABSOLUTE,
					     0, 1023, 1, 256);
	v4l2_ctrl_new_std_menu(h, NULL, V4L2_CID_CAMERA_ORIENTATION,
			       V4L2_CAMERA_ORIENTATION_EXTERNAL, 0, d->orientation);
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_CAMERA_SENSOR_ROTATION, d->rotation, d->rotation,
			  1, d->rotation);
	if (h->error)
		return h->error;
	s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->sd.ctrl_handler = h;
	v4l2_ctrl_lock(s->exposure);
	sensor_update_limits(s);
	v4l2_ctrl_unlock(s->exposure);
	return v4l2_device_register_subdev(&tc->v4l2, &s->sd);
}

/* ------------------------------------------------------------------ */
/* seninf (ChromeOS mtk_seninf.c sequence)                             */

static const int port_seninf[3] = { 0, 2, 4 };		/* SENINF1, 3, 5 */
static const int port_rx_off[3] = { 0x0000, 0x2000, 0x4000 };

static void rmw(void __iomem *a, u32 keep, u32 set)
{
	writel((readl(a) & keep) | set, a);
}

static void seninf_start(struct titan_cam *tc, int p, bool pix2)
{
	void __iomem *top = tc->seninf;
	void __iomem *s = tc->seninf + 0x1000 * port_seninf[p];
	void __iomem *rx = tc->mipi_rx + port_rx_off[p];
	static const u32 phy_ctl[3] = {
		SENINF_TOP_PHY_SENINF_CTL_CSI0,
		SENINF_TOP_PHY_SENINF_CTL_CSI1,
		SENINF_TOP_PHY_SENINF_CTL_CSI2,
	};
	bool combo = p == 0;

	/* timestamps */
	rmw(top + SENINF1_CTRL, ~0U, 0x1);
	rmw(top + SENINF1_CTRL_EXT, 0xffffffbf, 0x40);
	writel(0x67, top + SENINF_TG1_TM_STP);

	/* receiver */
	rmw(top + phy_ctl[p], 0x7ffff8fe, 0x80000200);
	rmw(s + SENINF1_CTRL, ~0U, 0x1);
	rmw(s + SENINF1_CTRL, 0x8fffffff, 0);		/* 10-bit pad */
	rmw(s + SENINF1_CTRL, 0xffff0fff, 0);		/* source: CSI-2 */
	rmw(s + SENINF1_CTRL_EXT, 0xffffff9f, 0x40);

	/* D-PHY, 4 data lanes + clock */
	if (combo)
		rmw(rx + MIPI_RX_ANA00_CSI0A, 0xfffffffe, 0);
	rmw(rx + MIPI_RX_ANA00_CSI0A, 0xffffe49f, 0x1a40);
	if (combo)
		rmw(rx + MIPI_RX_ANA00_CSI0B, 0xfffffffe, 0);
	rmw(rx + MIPI_RX_ANA00_CSI0B, 0xffffe49f, 0x1240);
	rmw(rx + MIPI_RX_ANAA8_CSI0A, 0xfffffff8, 0x7);
	rmw(rx + MIPI_RX_ANAA8_CSI0B, 0xfffffff8, 0x7);
	if (combo) {
		rmw(rx + MIPI_RX_ANA18_CSI0A, 0xffffff0f, 0x50);
		rmw(rx + MIPI_RX_ANA1C_CSI0A, 0xff0fffff, 0x500000);
		rmw(rx + MIPI_RX_ANA20_CSI0A, 0xff0fffff, 0x500000);
		rmw(rx + MIPI_RX_ANA18_CSI0B, 0xffffff0f, 0x50);
		rmw(rx + MIPI_RX_ANA1C_CSI0B, 0xff0fffff, 0x500000);
		rmw(rx + MIPI_RX_ANA20_CSI0B, 0xff0fffff, 0x500000);
	} else {
		rmw(rx + MIPI_RX_ANA18_CSI1A - 0x2000, 0xff0fff0f, 0x500050);
		rmw(rx + MIPI_RX_ANA1C_CSI1A - 0x2000, 0xffffff0f, 0x50);
		rmw(rx + MIPI_RX_ANA18_CSI1B - 0x2000, 0xff0fff0f, 0x500050);
		rmw(rx + MIPI_RX_ANA1C_CSI1B - 0x2000, 0xffffff0f, 0x50);
	}
	writel(0x90, tc->mipi_rx + MIPI_RX_ANA40_CSI0A);
	rmw(rx + MIPI_RX_ANA24_CSI0A, 0xffffff, 0x40000000);
	rmw(rx + MIPI_RX_ANA24_CSI0B, 0xffffff, 0x40000000);
	rmw(rx + MIPI_RX_WRAPPER80_CSI0A, 0xfffcffff, 0);
	rmw(rx + MIPI_RX_WRAPPER80_CSI0B, 0xfffcffff, 0);
	rmw(rx + MIPI_RX_ANA00_CSI0A, 0xfffffff7, 0x8);
	rmw(rx + MIPI_RX_ANA00_CSI0B, 0xfffffff7, 0x8);
	usleep_range(20, 40);
	rmw(rx + MIPI_RX_ANA00_CSI0A, 0xfffffff7, 0x8);
	rmw(rx + MIPI_RX_ANA00_CSI0B, 0xfffffffb, 0x4);
	udelay(1);
	rmw(s + MIPI_RX_CON24_CSI0, 0xffffff, 0xc9000000);

	/* CSI-2 */
	writel(1 << 15, s + SENINF1_CSI2_DPCM);
	rmw(s + SENINF1_CSI2_LNRD_TIMING, 0xffff00ff, 0x15 << 8);
	writel(readl(s + SENINF1_CSI2_CTL) | (1 << 16) | 0x10 | ((1 << LANES) - 1),
	       s + SENINF1_CSI2_CTL);
	rmw(s + SENINF1_CSI2_RESYNC_MERGE_CTL, 0xfffff3f8, 0x3);
	rmw(s + SENINF1_CSI2_MODE, 0xfffff800, 0);
	writel(0x1dff00, s + SENINF1_CSI2_DPHY_SYNC);
	rmw(s + SENINF1_CSI2_SPARE0, 0xfffffffe, 0);
	rmw(s + SENINF1_CSI2_CTL, 0xf5ffff7f, 0x2000000);
	rmw(s + SENINF1_CSI2_HS_TRAIL, 0xffffff00, 0x8);
	writel(0x8000001a, s + SENINF1_CSI2_DGB_SEL);
	writel(0xffffffff, s + SENINF1_CSI2_INT_EN);
	writel(0xffffffff, s + SENINF1_CSI2_INT_STATUS);
	writel(0x1f, s + SENINF1_CSI2_INT_EN_EXT);
	rmw(s + SENINF1_CTRL, 0xffffff7f, 0x80);
	udelay(1);
	rmw(s + SENINF1_CTRL, 0xffffff7f, 0);

	/* mux 0 <- this receiver; every CAM/CAMSV input <- mux 0 */
	rmw(top + SENINF1_MUX_CTRL, 0x7fffffff, 0x80000000);
	rmw(top + SENINF1_MUX_CTRL, 0xffff0fff, 0x8000);
	rmw(top + SENINF1_MUX_CTRL_EXT, 0xfffffffc, 0x1);
	rmw(top + SENINF1_MUX_CTRL_EXT, 0xffffffef, 0);
	/* pixel mode (the HAL's setSeninfMuxCtrl): 1 pixel per clock = bit 8
	 * and EXT bit 4 clear, 2 = bit 8, 4 = EXT bit 4 */
	rmw(top + SENINF1_MUX_CTRL, 0xfffffeff, pix2 ? 0x100 : 0);
	rmw(top + SENINF1_MUX_CTRL, 0xcfffffff, 0x20000000);
	rmw(top + SENINF1_MUX_CTRL, 0xf000ffff, 0x6df0000);
	rmw(top + SENINF1_MUX_CTRL, 0xfffff9ff, 0);
	writel((readl(top + SENINF1_MUX_CTRL) | 0x3) & 0xfffffffc, top + SENINF1_MUX_CTRL);
	rmw(top + SENINF_TOP_MUX_CTRL, ~0xfU, port_seninf[p]);
	writel(0, top + SENINF_TOP_CAM_MUX_CTRL);
}

static void seninf_stop(struct titan_cam *tc, int p)
{
	void __iomem *s = tc->seninf + 0x1000 * port_seninf[p];

	rmw(s + SENINF1_CSI2_CTL, 0xffffffe0, 0);		/* lanes off */
	rmw(tc->seninf + SENINF1_MUX_CTRL, 0x7fffffff, 0);	/* mux off */
}

/* ------------------------------------------------------------------ */
/* CAMSV (sequence from the HAL's isp_function_camsv.cpp)              */

#define SV_IMGO_BASE	0x020
#define SV_IMGO_OFST	0x028
#define SV_IMGO_XSIZE	0x030
#define SV_IMGO_YSIZE	0x034
#define SV_IMGO_STRIDE	0x038
#define SV_IMGO_CON	0x03c
#define SV_IMGO_CON2	0x040
#define SV_IMGO_CON3	0x044
#define SV_IMGO_CROP	0x048
#define SV_FBC_CTL1	0x110
#define SV_TG_SEN_MODE	0x230
#define SV_TG_VF_CON	0x234
#define SV_TG_GRAB_PXL	0x238
#define SV_TG_GRAB_LIN	0x23c
#define SV_DMA_EN	0x400
#define SV_FH_BASE	0x404
#define SV_MODULE_EN	0x510
#define SV_FMT_SEL	0x514
#define SV_INT_EN	0x518
#define SV_INT_STATUS	0x51c	/* read clears */
#define SV_SW_CTL	0x520
#define SV_CLK_EN	0x530
#define SV_PAK		0x54c

#define SV_INT_VS	BIT(0)
#define SV_INT_SOF	BIT(7)
#define SV_INT_DONE	BIT(10)		/* HW pass-1 done */
#define SV_INT_IMGO_ERR	BIT(16)
#define SV_INT_OVERRUN	BIT(17)

static void sv_rmw(struct titan_cam *tc, u32 reg, u32 keep, u32 set)
{
	writel((readl(tc->camsv + reg) & keep) | set, tc->camsv + reg);
}

static void camsv_start(struct titan_cam *tc, unsigned int first, bool pix2)
{
	int i;

	writel(4, tc->camsv + SV_SW_CTL);	/* reset: 4, 0, 1, wait for 3, 0 */
	writel(0, tc->camsv + SV_SW_CTL);
	writel(1, tc->camsv + SV_SW_CTL);
	for (i = 0; i < 10000 && readl(tc->camsv + SV_SW_CTL) != 3; i++)
		udelay(1);
	writel(0, tc->camsv + SV_SW_CTL);

	/*
	 * TG on, double-buffered registers. seninf hands CAMSV the pixels
	 * MSB-aligned on a 14-bit bus. 8-bit: TG format RAW8 + the packer in
	 * RAW8 mode keeps the top 8 bits (MODULE_EN bit 2 = packer). 16-bit:
	 * TG format RAW10, packer off and MODULE_EN bit 3 (what the HAL uses for
	 * its non-Bayer formats) stores the bus as is, samples << 4.
	 */
	if (tc->fmt->dip) {
		/* packed RAW10 for the DIP (as titan_camcap p1fmt=9): TG RAW10,
		 * packer on (MODULE_EN bit 2), PAK 10-bit */
		sv_rmw(tc, SV_MODULE_EN, ~0x3f2cU, 0x40000005);
		writel(1, tc->camsv + SV_FMT_SEL);
		sv_rmw(tc, SV_PAK, ~0x37U, 0x1);
	} else if (tc->fmt->bytes == 1) {
		sv_rmw(tc, SV_MODULE_EN, ~0x3f2cU, 0x40000005);
		writel(0, tc->camsv + SV_FMT_SEL);
		sv_rmw(tc, SV_PAK, ~0x37U, 0x0);
	} else {
		sv_rmw(tc, SV_MODULE_EN, ~0x3f2cU, 0x40000009);
		writel(1, tc->camsv + SV_FMT_SEL);
		sv_rmw(tc, SV_PAK, ~0x37U, 0x1);
	}
	/*
	 * Pixel mode, from the HAL (isp_function_camsv.cpp): the packer's
	 * PAK bits 4-5 (0 = 1, 1 = 2, 2 = 4 pixels), the TG's SEN_MODE bit 1
	 * (2 pixels; bit 13 too for 4) and the IMGO bus size in STRIDE bits
	 * 16-18 (BusSizeCal: 1, 3, 7 for raw formats).
	 */
	if (pix2)
		sv_rmw(tc, SV_PAK, ~0x30U, 0x10);
	writel(SV_INT_SOF | SV_INT_DONE | SV_INT_IMGO_ERR | SV_INT_OVERRUN,
	       tc->camsv + SV_INT_EN);
	writel(0, tc->camsv + SV_DMA_EN);
	sv_rmw(tc, 0x3b0, 0x7fffffff, 0);
	writel(0xffffffff, tc->camsv + 0x3c4);
	sv_rmw(tc, 0x018, ~0U, 0x61000000);

	/* TG: the whole frame, continuous (single-frame + FBC writes nothing) */
	sv_rmw(tc, SV_TG_SEN_MODE, ~0x62003U, 0x10000 | (pix2 ? 0x2 : 0));
	sv_rmw(tc, SV_TG_VF_CON, ~0x2U, 0);
	writel(tc->width << 16, tc->camsv + SV_TG_GRAB_PXL);
	writel(tc->height << 16, tc->camsv + SV_TG_GRAB_LIN);

	/* IMGO DMA; the buffer address is switched at every start of frame */
	writel((tc->fmt->dip ? tc->raw_bytes : tc->width * tc->fmt->bytes) - 1,
	       tc->camsv + SV_IMGO_XSIZE);
	writel(tc->height - 1, tc->camsv + SV_IMGO_YSIZE);
	writel((tc->fmt->dip ? tc->raw_stride : tc->stride) | 0x1000000 |
	       (pix2 ? 3 : 1) << 16, tc->camsv + SV_IMGO_STRIDE);
	writel(0, tc->camsv + SV_IMGO_CROP);
	writel(0x80000080, tc->camsv + SV_IMGO_CON);
	writel(0x20002, tc->camsv + SV_IMGO_CON2);
	writel(0x20002, tc->camsv + SV_IMGO_CON3);
	writel(0, tc->camsv + SV_IMGO_OFST);
	writel(first, tc->camsv + SV_IMGO_BASE);
	writel(0, tc->camsv + SV_FBC_CTL1);
	writel(tc->scratch_mva, tc->camsv + SV_FH_BASE);	/* frame headers: scratch */

	readl(tc->camsv + SV_INT_STATUS);			/* clear */
	sv_rmw(tc, SV_MODULE_EN, ~0U, 0x10);			/* IMGO on */
	sv_rmw(tc, SV_DMA_EN, ~0U, 1);
	sv_rmw(tc, SV_TG_SEN_MODE, ~0U, 1);			/* CMOS_EN */
	sv_rmw(tc, SV_CLK_EN, ~0U, 0x8005);
	sv_rmw(tc, SV_TG_VF_CON, ~0U, 1);			/* VFDATA_EN */
}

static void camsv_stop(struct titan_cam *tc)
{
	sv_rmw(tc, SV_TG_VF_CON, ~1U, 0);
	sv_rmw(tc, SV_TG_SEN_MODE, ~1U, 0);
	writel(0, tc->camsv + SV_INT_EN);
	sv_rmw(tc, SV_MODULE_EN, ~0x10U, 0);
	writel(0, tc->camsv + SV_DMA_EN);
	readl(tc->camsv + SV_INT_STATUS);
}

/*
 * Buffer rotation. IMGO_BASE is double-buffered: what is written during
 * frame N is latched at the start of frame N+1. So at each start of frame
 * the buffer programmed before ("next") becomes the one being written
 * ("cur") and the following buffer is programmed; at frame done "cur" is
 * complete. With no buffer queued, "cur" is written again (that frame is
 * dropped).
 */
static irqreturn_t camsv_irq(int irq, void *data)
{
	struct titan_cam *tc = data;
	struct titan_buf *b;
	unsigned long flags;
	u32 st;

	if (!tc->clocks_on)	/* camsys off: its registers would hang the bus */
		return IRQ_NONE;
	st = readl(tc->camsv + SV_INT_STATUS);	/* read clears */
	if (!st)
		return IRQ_NONE;
	spin_lock_irqsave(&tc->qlock, flags);
	if (tc->fmt->dip) {
		/* CAMSV fills the raw ring; hand each finished frame to the DIP worker */
		if (st & SV_INT_DONE) {
			if (!(st & (SV_INT_IMGO_ERR | SV_INT_OVERRUN))) {
				if (tc->dip_busy) {
					tc->dip_drops++;	/* worker still on the last frame */
				} else {
					tc->dip_ready = tc->rc;
					tc->dip_busy = true;
					queue_work(system_highpri_wq, &tc->dip_work);
				}
			} else {
				tc->dip_rawerr++;
			}
			tc->sequence++;
		}
		if (st & SV_INT_SOF) {
			tc->rc = tc->rn;
			tc->rn = (tc->rn + 1) % ARRAY_SIZE(tc->raw);
			if (tc->rn == tc->rc)	/* keep one frame behind the reader */
				tc->rn = (tc->rn + 1) % ARRAY_SIZE(tc->raw);
			writel(tc->raw[tc->rn].mva, tc->camsv + SV_IMGO_BASE);
		}
		spin_unlock_irqrestore(&tc->qlock, flags);
		return IRQ_HANDLED;
	}
	if (st & SV_INT_DONE) {
		b = tc->cur;
		if (b && b != tc->next) {	/* not being refilled */
			b->vb.sequence = tc->sequence;
			b->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&b->vb.vb2_buf,
					(st & (SV_INT_IMGO_ERR | SV_INT_OVERRUN)) ?
					VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		}
		tc->cur = NULL;
		tc->sequence++;
	}
	if (st & SV_INT_SOF) {
		tc->cur = tc->next;
		if (tc->cur)
			tc->cur->vb.vb2_buf.timestamp = ktime_get_ns();
		b = list_first_entry_or_null(&tc->queued, struct titan_buf, list);
		if (b) {
			list_del(&b->list);
			tc->next = b;
		} else {
			tc->next = tc->cur;	/* nothing queued: refill this one */
		}
		if (tc->next)
			writel(tc->next->mva, tc->camsv + SV_IMGO_BASE);
	}
	spin_unlock_irqrestore(&tc->qlock, flags);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* CAM A (ISP pass 1): autofocus statistics only                       */

/*
 * Sequence from the HAL (CAM_TOP_CTRL, CAM_TG_CTRL, CAM_DMX_CTRL, DMAO_B,
 * ISP_MGR_AF_STAT_CONFIG, TuningMgrImp::enableEngine) as worked out with
 * titan_camcap p1=1 p1af=1. The layout is CAMSV's with the blocks moved:
 * control at 0x000, TG at 0x230, DMA at 0x1000.
 */
#define CA_CTL_EN	0x004
#define CA_CTL_DMA_EN	0x008
#define CA_CTL_FMT_SEL	0x00c
#define CA_CTL_SEL	0x010
#define CA_CTL_EN2	0x018
#define CA_INT_EN	0x020
#define CA_SW_CTL	0x040
#define CA_DB_EN	0x088
#define CA_TG_SEN_MODE	0x230
#define CA_TG_VF_CON	0x234
#define CA_TG_GRAB_PXL	0x238
#define CA_TG_GRAB_LIN	0x23c
#define CA_TG_INTER_ST	0x26c		/* frame counter in the top byte */
#define CA_DMX_CTL	0x330
#define CA_DMX_CROP	0x334
#define CA_DMX_VSIZE	0x338
#define CA_SGG1_PGN	0x810
#define CA_AFO_BASE	0x10b0
#define CA_AFO_OFST	0x10b8
#define CA_AFO_XSIZE	0x10c0
#define CA_AFO_YSIZE	0x10c4
#define CA_AFO_STRIDE	0x10c8
#define CA_AFO_CON	0x10cc
#define CA_AFO_CON2	0x10d0
#define CA_AFO_CON3	0x10d4
#define CA_EN_TG	BIT(0)
#define CA_EN_DMX	BIT(1)
#define CA_EN_AF	BIT(17)		/* tuning function 15 */
#define CA_EN_SGG1	BIT(18)		/* function 14: the AF input gain/gamma */
#define CA_EN2_SGG5	BIT(31)		/* function 22 */
#define CA_DMA_AFO	BIT(3)

#define AF_NX		16
#define AF_NY		12
#define AF_BLOCK	40
#define AFO_LINE	(AF_NX * AF_BLOCK)
#define AFO_STRIDE	656		/* ALIGN(AFO_LINE + 1, 16) */
#define AFO_SIZE	(AFO_STRIDE * AF_NY)
#define AF_HDR_WORDS	8

/* AE/AWB statistics (AA block, same CAM A): equal blocks over the frame, unity
 * pre-gain, thresholds wide open, AE histogram registers left zero. The AAO
 * data is, per block row, AA_NX x {R, G, B, 0x0f} 8-bit averages then AA_NX
 * luma bytes (AAO_BLOCK bytes per block). See camcap's p1aa and docs. */
#define CA_EN_AA	BIT(19)		/* tuning function 16 */
#define CA_DMA_AAO	BIT(5)
#define CA_AWB_WIN_ORG	0x830		/* x | y << 16 */
#define CA_AWB_WIN_SIZE	0x834		/* block w | h << 16 */
#define CA_AWB_WIN_PIT	0x838
#define CA_AWB_WIN_NUM	0x83c
#define CA_AWB_GAIN1_0	0x840		/* pre-gain R | G << 16, 512 = 1x */
#define CA_AWB_GAIN1_1	0x844		/* pre-gain B */
#define CA_AWB_LMT1_0	0x848
#define CA_AWB_LMT1_1	0x84c
#define CA_AWB_LOW_THR	0x850
#define CA_AWB_HI_THR	0x854
#define CA_AWB_PIXEL_CNT0 0x858		/* 1 << 24 / pixels of that colour per block */
#define CA_AWB_PIXEL_CNT1 0x85c
#define CA_AWB_PIXEL_CNT2 0x860
#define CA_AAO_BASE	0x1080
#define CA_AAO_OFST	0x1088
#define CA_AAO_XSIZE	0x1090
#define CA_AAO_YSIZE	0x1094
#define CA_AAO_STRIDE	0x1098
#define CA_AAO_CON	0x109c
#define CA_AAO_CON2	0x10a0
#define CA_AAO_CON3	0x10a4
#define CA_AAO_ERR	0x1368
#define AA_NX		24
#define AA_NY		18
#define AAO_BLOCK	5
#define AAO_BYTES	(AA_NX * AA_NY * AAO_BLOCK)
#define AAO_ROW		(AA_NX * AAO_BLOCK)	/* AA_NX*4 AWB + AA_NX luma */

static unsigned int af_pgn_default = 0x80;
module_param_named(af_pgn, af_pgn_default, uint, 0444);
MODULE_PARM_DESC(af_pgn, "SGG1 gain in front of the AF filters at start (stock 0x10 saturates)");

/*
 * Automatic SGG1 gain: PGN = AF_PGN_REF x brightness / AF_W3_REF, where
 * brightness is the mean pixel sum (word 3) of a block. Reference: 0x80 gave
 * a clean focus curve at 23.3 per pixel (raw ~88 at 10 bits, black 64) with
 * 146 x 146-pixel blocks. Changed only when off by more than 1/8, at most
 * every AF_PGN_HOLD_MS, so a focus sweep sees few steps (and the app scales
 * the filter energy by the gain anyway).
 */
#define AF_PGN_REF	0x80
#define AF_W3_REF	(23300ULL * 146 * 146 / 1000)	/* per block */
#define AF_PGN_MIN	0x20
#define AF_PGN_MAX	0x7ff
#define AF_PGN_HOLD_MS	500
#define AF_WORK_MS	100

static void ca_rmw(struct titan_cam *tc, u32 reg, u32 keep, u32 set)
{
	writel((readl(tc->cama + reg) & keep) | set, tc->cama + reg);
}

#define STATS_BUF	(2 * PAGE_SIZE)	/* AFO and AAO each */

static int af_alloc(struct titan_cam *tc)
{
	return dbuf_alloc(tc, &tc->afo_vaddr, &tc->afo_mva, STATS_BUF);
}

static void af_free(struct titan_cam *tc)
{
	dbuf_free(tc, &tc->afo_vaddr, &tc->afo_mva, STATS_BUF);
}

static int aa_alloc(struct titan_cam *tc)
{
	return dbuf_alloc(tc, &tc->aao_vaddr, &tc->aao_mva, STATS_BUF);
}

static void aa_free(struct titan_cam *tc)
{
	dbuf_free(tc, &tc->aao_vaddr, &tc->aao_mva, STATS_BUF);
}

/* program the AA statistics block in CAM A (m = the active sensor's mode);
 * the CAM A common setup and DMA/CTL enables are done by the caller */
static void aa_program(struct titan_cam *tc, const struct sensor_mode *m)
{
	int bw = (m->width / AA_NX) & ~1, bh = (m->height / AA_NY) & ~1, i;

	writel(0, tc->cama + CA_AWB_WIN_ORG);
	writel(bw | bh << 16, tc->cama + CA_AWB_WIN_SIZE);
	writel(bw | bh << 16, tc->cama + CA_AWB_WIN_PIT);
	writel(AA_NX | AA_NY << 16, tc->cama + CA_AWB_WIN_NUM);
	writel(0x200 | 0x200 << 16, tc->cama + CA_AWB_GAIN1_0);
	writel(0x200, tc->cama + CA_AWB_GAIN1_1);
	writel(0xfff | 0xfff << 16, tc->cama + CA_AWB_LMT1_0);
	writel(0xfff, tc->cama + CA_AWB_LMT1_1);
	writel(0, tc->cama + CA_AWB_LOW_THR);
	writel(0xffffff, tc->cama + CA_AWB_HI_THR);
	/* averages = sums * PIXEL_CNT >> 24: 1 / pixels of that colour per block */
	writel((1 << 24) / (bw * bh / 4), tc->cama + CA_AWB_PIXEL_CNT0);
	writel((1 << 24) / (bw * bh / 2), tc->cama + CA_AWB_PIXEL_CNT1);
	writel((1 << 24) / (bw * bh / 4), tc->cama + CA_AWB_PIXEL_CNT2);
	writel(0x200, tc->cama + 0x8c4);	/* the HAL's fixed values */
	writel(0x200, tc->cama + 0x8cc);
	writel(0x90200, tc->cama + 0x8d4);
	writel(0, tc->cama + 0xbd0);		/* the copy at 0xbd0 */
	writel(bw | bh << 16, tc->cama + 0xbd4);
	writel(bw | bh << 16, tc->cama + 0xbd8);
	writel(AA_NX | AA_NY << 16, tc->cama + 0xbdc);
	for (i = 0; i < 4; i++)
		writel((1 << 24) / (bw * bh / 4), tc->cama + 0xbe0 + 4 * i);

	writel(AAO_BYTES - 1, tc->cama + CA_AAO_XSIZE);
	writel(0, tc->cama + CA_AAO_YSIZE);
	writel(AAO_BYTES, tc->cama + CA_AAO_STRIDE);
	writel(0x80000080, tc->cama + CA_AAO_CON);
	writel(0x00200020, tc->cama + CA_AAO_CON2);
	writel(0x00100010, tc->cama + CA_AAO_CON3);
	writel(0, tc->cama + CA_AAO_OFST);
	writel(tc->aao_mva, tc->cama + CA_AAO_BASE);
}

/*
 * Bring up CAM A in pure-raw mode (TG -> DMX -> ...) to collect hardware
 * statistics on the active sensor while CAMSV captures the raw frames. Two
 * blocks run off the same demuxed stream: AF (rear only; its filter table is
 * for the 2336-wide mode) and AA (AE/AWB, either camera). camsys is on.
 */
static void cama_start(struct titan_cam *tc, const struct sensor_mode *m)
{
	bool want_af = tc->afo_mva && m->width == 2336;
	bool want_aa = aa && tc->aao_mva;
	u32 ctl_en = CA_EN_TG | CA_EN_DMX, dma_en = 0;
	int i;

	if (!tc->cama || (!want_af && !want_aa))
		return;
	mutex_lock(&tc->af_lock);
	writel(1, tc->cama + CA_SW_CTL);	/* reset: 1, wait for 2, 4, 0 */
	for (i = 0; i < 10000 && readl(tc->cama + CA_SW_CTL) != 2; i++)
		udelay(1);
	writel(4, tc->cama + CA_SW_CTL);
	writel(0, tc->cama + CA_SW_CTL);

	if (want_af)
		ctl_en |= CA_EN_AF | CA_EN_SGG1;
	if (want_aa)
		ctl_en |= CA_EN_AA;
	writel(1 << 12 | 1 << 2, tc->cama + CA_CTL_FMT_SEL);	/* TG RAW10 */
	writel(0x060a5010, tc->cama + CA_CTL_SEL);	/* Path_sel_RAW, 2nd variant */
	writel(ctl_en, tc->cama + CA_CTL_EN);
	writel(want_af ? CA_EN2_SGG5 : 0, tc->cama + CA_CTL_EN2);
	writel(0, tc->cama + CA_INT_EN);	/* MediaTek's camera-isp owns the IRQ */
	ca_rmw(tc, 0x13b0, 0x7fffffff, 0);
	writel(0xffffffff, tc->cama + 0x13c4);
	writel(0, tc->cama + 0x1400);

	ca_rmw(tc, CA_DMX_CTL, 0x7ff00000, 0xf0000);
	writel((m->width - 1) << 16, tc->cama + CA_DMX_CROP);
	writel(m->height, tc->cama + CA_DMX_VSIZE);

	ca_rmw(tc, CA_TG_SEN_MODE, ~0x70003U, 0x20000);
	ca_rmw(tc, CA_TG_VF_CON, ~0x2U, 0);
	writel(m->width << 16, tc->cama + CA_TG_GRAB_PXL);
	writel(m->height << 16, tc->cama + CA_TG_GRAB_LIN);
	ca_rmw(tc, 0x25c, ~0U, 0x20);
	ca_rmw(tc, 0x240, ~0U, 0x8000);

	if (want_af) {
		for (i = 0; i < ARRAY_SIZE(af_regs_2336x1752); i++)
			writel(af_regs_2336x1752[i].val, tc->cama + af_regs_2336x1752[i].off);
		writel(tc->af_pgn, tc->cama + CA_SGG1_PGN);

		writel(AFO_LINE - 1, tc->cama + CA_AFO_XSIZE);
		writel(AF_NY - 1, tc->cama + CA_AFO_YSIZE);
		writel(AFO_STRIDE, tc->cama + CA_AFO_STRIDE);
		writel(0x80000100, tc->cama + CA_AFO_CON);
		writel(0x00800080, tc->cama + CA_AFO_CON2);
		writel(0x00550055, tc->cama + CA_AFO_CON3);
		writel(0, tc->cama + CA_AFO_OFST);
		writel(tc->afo_mva, tc->cama + CA_AFO_BASE);
		dma_en |= CA_DMA_AFO;
	}
	if (want_aa) {
		memset(tc->aao_vaddr, 0, AAO_BYTES);	/* a dead AA block reads empty */
		aa_program(tc, m);
		dma_en |= CA_DMA_AAO;
	}
	writel(1, tc->cama + CA_DB_EN);

	writel(dma_en, tc->cama + CA_CTL_DMA_EN);
	ca_rmw(tc, CA_TG_SEN_MODE, ~0U, 1);	/* CMOS_EN */
	ca_rmw(tc, CA_TG_VF_CON, ~0U, 1);	/* VFDATA_EN */
	tc->af_running = want_af;
	tc->aa_running = want_aa;
	tc->af_pgn_changed = jiffies;
	mutex_unlock(&tc->af_lock);
	schedule_delayed_work(&tc->af_work, msecs_to_jiffies(AF_PGN_HOLD_MS));
}

/* the gain from the last statistics (af_lock held) */
static u32 af_auto_pgn(struct titan_cam *tc)
{
	u64 sum = 0;
	int x, y;

	for (y = 0; y < AF_NY; y++)
		for (x = 0; x < AF_NX; x++)
			sum += *(u32 *)((u8 *)tc->afo_vaddr + y * AFO_STRIDE + x * AF_BLOCK + 12);
	sum = div_u64(sum * AF_PGN_REF, AF_W3_REF * AF_NX * AF_NY);
	return clamp_t(u64, sum, AF_PGN_MIN, AF_PGN_MAX);
}

/* grey-world white balance from black-removed R/G/B means into the PGN gains */
static void wb_set(struct titan_cam *tc, const u32 rgb[3])
{
	tc->wb[0] = clamp_t(u32, 512 * rgb[1] / rgb[0], 128, 4095);
	tc->wb[1] = 512;
	tc->wb[2] = clamp_t(u32, 512 * rgb[1] / rgb[2], 128, 4095);
}

/*
 * R/G/B means above black (8-bit, the same scale as the AA averages) from a
 * subsample of a packed RAW10 frame in the raw ring: one 2x2 Bayer quad every
 * 32 px of every 16th row pair. lum[] averages every sample (scene brightness
 * for AE). wb[] averages only quads whose green is WB_MIN above black and
 * none clipped: in a mostly dark scene the near-black samples carry the
 * per-channel pedestal and noise rather than the light, and pull grey-world
 * toward 1x (the green cast). Too few such quads (a black frame): wb = lum.
 *
 * The ring is mapped write-combine, so every load goes to DRAM (~24 ms a
 * frame at 4672x3504 with byte loads): run it from the 100 ms work, not per
 * frame, and read each pixel pair with one u32 load. Returns 0, or -ENODATA.
 */
#define WB_MIN	8		/* 8-bit levels above black for a WB sample */
#define WB_CLIP	250

static int raw_stats(struct titan_cam *tc, const u8 *raw, u32 lum[3], u32 wb[3])
{
	u32 bl = tc->black >= 0 ? tc->black >> 2 : 0;	/* 10-bit -> 8-bit */
	u64 all[4] = {}, lit[4] = {};
	u32 n = 0, nl = 0, y, x, i;

	for (y = 0; y + 1 < tc->height; y += 16) {
		const u8 *p0 = raw + y * tc->raw_stride;
		const u8 *p1 = p0 + tc->raw_stride;

		for (x = 0; x + 4 < tc->raw_bytes; x += 40) {
			/*
			 * CAMSV packed RAW10 is a little-endian bitstream (4 px in
			 * 5 bytes, pixel i at bits 10i..10i+9), not CSI-2 packing:
			 * one aligned u32 holds pixels 0 and 1. Keep the top 8 bits.
			 */
			u32 bg = *(const u32 *)(p0 + x);	/* B, Gb */
			u32 gr = *(const u32 *)(p1 + x);	/* Gr, R */
			u32 q[4] = { (bg & 0x3ff) >> 2, (bg >> 12) & 0xff,
				     (gr & 0x3ff) >> 2, (gr >> 12) & 0xff };

			for (i = 0; i < 4; i++)
				all[i] += q[i];
			n++;
			if (q[1] + q[2] > 2 * (bl + WB_MIN) &&
			    max(max(q[0], q[1]), max(q[2], q[3])) < WB_CLIP) {
				for (i = 0; i < 4; i++)
					lit[i] += q[i];
				nl++;
			}
		}
	}
	if (!n)
		return -ENODATA;
#define ABOVE(v) ((v) > bl ? (v) - bl : 1)
	lum[0] = ABOVE((u32)(all[3] / n));			/* R */
	lum[1] = ABOVE((u32)((all[1] + all[2]) / (2 * n)));	/* G */
	lum[2] = ABOVE((u32)(all[0] / n));			/* B */
	if (nl < n / 256 + 16) {
		memcpy(wb, lum, 3 * sizeof(*wb));
	} else {
		wb[0] = ABOVE((u32)(lit[3] / nl));
		wb[1] = ABOVE((u32)((lit[1] + lit[2]) / (2 * nl)));
		wb[2] = ABOVE((u32)(lit[0] / nl));
	}
#undef ABOVE
	return 0;
}

/*
 * Read the AA statistics (af_lock held). With aa_wb, set the DIP white-balance
 * gains from the per-block R/G/B averages (grey world over the frame, black
 * removed); otherwise the CPU grey-world in dip_work_fn owns the gains. Return
 * the mean luma above black (0-255, AA scale ~1/4 of the 10-bit raw) for the
 * exposure loop; 0 if the statistics look empty (AAO not producing).
 *
 * Luma is derived from the same R/G/B averages (Y = (R + 2G + B) / 4) rather
 * than the block's separate AE-luma byte: the R/G/B averages are the validated
 * quantities, this tracks the white balance, and subtracting black makes it
 * independent of the sensor pedestal, so ae_target is "signal above black".
 */
static u32 aa_read(struct titan_cam *tc)
{
	const u8 *aao = tc->aao_vaddr;
	u64 sr = 0, sg = 0, sb = 0;
	u32 blk = (u32)AA_NX * AA_NY;
	int black = tc->black >= 0 ? tc->black >> 2 : 0;
	int r, g, b, x, y;

	for (y = 0; y < AA_NY; y++) {
		const u8 *row = aao + y * AAO_ROW;

		for (x = 0; x < AA_NX; x++) {
			sr += row[x * 4];
			sg += row[x * 4 + 1];
			sb += row[x * 4 + 2];
		}
	}
	if (!sg)
		return 0;
	r = max(1, (int)(sr / blk) - black);
	g = max(1, (int)(sg / blk) - black);
	b = max(1, (int)(sb / blk) - black);
	if (aa_wb) {			/* else the CPU raw subsample sets wb */
		u32 rgb[3] = { r, g, b };

		wb_set(tc, rgb);
	}
	return (r + 2 * g + b) / 4;	/* luma above black */
}

/*
 * Apply exposure (whole lines) and analogue gain (128 = 1x) to the sensor as
 * one group-hold update. The AE loop owns these registers in the YUYV path
 * (libcamera does not run its own AGC there); both OmniVision sensors match.
 */
static void ae_apply(struct titan_sensor *s, u32 exp, u32 gain)
{
	u32 e = exp << 4;	/* registers are in 1/16-line units */

	s_write(s, 0x3208, 0x00);
	s_write(s, 0x3500, e >> 16);
	s_write(s, 0x3501, (e >> 8) & 0xff);
	s_write(s, 0x3502, e & 0xff);
	sensor_gain_regs(s, gain);
	s_write(s, 0x3208, 0x10);
	s_write(s, 0x3208, 0xa0);
}

/*
 * One step of the exposure/gain loop toward ae_target from the mean luma
 * (af_lock held). Light is proportional to exposure x gain; raise exposure up
 * to the frame-length limit first (keeps gain, and so noise, low), then gain.
 * Each step is damped and there is a deadband, so the preview does not hunt.
 */
static void ae_step(struct titan_cam *tc, u32 luma)
{
	struct titan_sensor *s = tc->active;
	u32 exp_max, q, qd, exp, gain;

	if (!s || s->ae_manual)	/* app drives exposure by hand: do not fight it */
		return;
	/* the statistics are raw: they miss the digital part of the gain (sensor_gain_regs) */
	luma = (u32)div_u64((u64)luma * sensor_dgain(s), 512);
	if (!tc->ae_exp) {	/* seed from the current sensor settings */
		tc->ae_exp = s->exposure->val;
		tc->ae_gain = s->gain->val;
	}
	exp_max = s->mode->vts - 8;
	if (ae_max_us)		/* line time = 1 / (vts * fps) */
		exp_max = clamp_t(u32, div_u64((u64)ae_max_us * s->mode->vts * s->mode->fps,
					       1000000), 4, exp_max);
	if (abs((int)luma - (int)ae_target) * 16 <= (int)ae_target && tc->ae_exp <= exp_max)
		return;		/* within ~6% (and under the shutter cap): hold */
	q = tc->ae_exp * tc->ae_gain;			/* light ~ exp * gain */
	qd = (u32)div_u64((u64)q * ae_target, luma);	/* desired */
	qd = clamp(qd, q - q / 2, q + q * 3 / 4);	/* damp per step */
	if (qd / 128 <= exp_max) {
		exp = qd / 128;				/* 1x gain is enough */
		gain = 128;
	} else {
		exp = exp_max;				/* then raise gain */
		gain = qd / exp_max;
	}
	exp = clamp_t(u32, exp, 4, exp_max);
	gain = clamp_t(u32, gain, 128, 1984);
	if (exp == tc->ae_exp && gain == tc->ae_gain)
		return;
	tc->ae_exp = exp;
	tc->ae_gain = gain;
	ae_apply(s, exp, gain);			/* latched at the next frame */
	/*
	 * Keep the controls truthful, without going through s_ctrl (so not a
	 * manual set): a readback sees what the loop applied, and the next
	 * stream starts from it (sensor_apply_ctrls) - a still taken right after
	 * the preview starts at the preview's settled exposure. Both rear modes
	 * have the same line time, so the lines carry over.
	 */
	v4l2_ctrl_lock(s->exposure);
	s->exposure->val = s->exposure->cur.val = exp;
	s->gain->val = s->gain->cur.val = gain;
	v4l2_ctrl_unlock(s->exposure);
}

static void af_work_fn(struct work_struct *work)
{
	struct titan_cam *tc = container_of(to_delayed_work(work), struct titan_cam, af_work);
	u32 target;

	mutex_lock(&tc->af_lock);
	if (!tc->af_running && !tc->aa_running) {
		mutex_unlock(&tc->af_lock);
		return;
	}
	if (tc->af_running && tc->af_pgn_auto &&
	    time_after(jiffies, tc->af_pgn_changed + msecs_to_jiffies(AF_PGN_HOLD_MS))) {
		target = af_auto_pgn(tc);
		if (abs((int)target - (int)tc->af_pgn) * 8 > tc->af_pgn) {
			tc->af_pgn = target;
			tc->af_pgn_changed = jiffies;
			writel(target, tc->cama + CA_SGG1_PGN);	/* latched at the next frame */
		}
	}
	if (tc->aa_running) {
		u32 luma = aa_read(tc);		/* with aa_wb, also sets tc->wb */
		int idx = READ_ONCE(tc->raw_last);
		u32 rgb[3], wbr[3];

		/*
		 * The CPU raw subsample owns the white balance unless aa_wb, and
		 * stands in for the AE luma when the AAO is empty: in the rear's
		 * 2-pixel mode (4672x3504) CAM A's TG is set up for one pixel a
		 * clock and the AA block sees nothing. Only the YUYV path has a
		 * raw ring (raw_last stays -1 otherwise).
		 */
		tc->ae_cpu = false;
		if ((!aa_wb || !luma) && idx >= 0 &&
		    !raw_stats(tc, tc->raw[idx].vaddr, rgb, wbr)) {
			wb_set(tc, wbr);
			if (!luma) {
				luma = (rgb[0] + 2 * rgb[1] + rgb[2]) / 4;
				tc->ae_cpu = true;
			}
		}
		tc->ae_luma = luma;
		if (ae && luma)
			ae_step(tc, luma);
	}
	mutex_unlock(&tc->af_lock);
	schedule_delayed_work(&tc->af_work, msecs_to_jiffies(AF_WORK_MS));
}

static void cama_stop(struct titan_cam *tc)
{
	cancel_delayed_work_sync(&tc->af_work);
	mutex_lock(&tc->af_lock);
	if (tc->af_running || tc->aa_running) {
		ca_rmw(tc, CA_TG_VF_CON, ~1U, 0);
		ca_rmw(tc, CA_TG_SEN_MODE, ~1U, 0);
		writel(0, tc->cama + CA_CTL_DMA_EN);
		tc->af_running = false;
		tc->aa_running = false;
	}
	tc->ae_exp = tc->ae_gain = tc->ae_luma = 0;
	tc->ae_cpu = false;
	mutex_unlock(&tc->af_lock);
}

/*
 * /sys/devices/platform/1a050000.camsv1/rear/af_stats: a header of 8 u32
 * (magic "TAF1", frame counter (8 bits, wraps), running, blocks x, blocks y,
 * bytes per block, line stride, SGG1 gain) and the AFO data, AF_NY lines of
 * AFO_STRIDE bytes. The data is the last set written; with the camera off it
 * is stale and "running" is 0.
 */
static ssize_t af_stats_read(struct file *f, struct kobject *kobj, struct bin_attribute *attr,
			     char *buf, loff_t off, size_t count)
{
	struct titan_sensor *s = container_of(kobj_to_dev(kobj), struct titan_sensor, dev);
	struct titan_cam *tc = s->tc;
	u32 hdr[AF_HDR_WORDS] = { 0x31464154, 0, 0, AF_NX, AF_NY, AF_BLOCK, AFO_STRIDE, 0 };
	size_t total = sizeof(hdr) + AFO_SIZE;

	if (off >= total)
		return 0;
	count = min_t(size_t, count, total - off);
	mutex_lock(&tc->af_lock);
	hdr[2] = tc->af_running;
	hdr[7] = tc->af_pgn;
	if (tc->af_running)
		hdr[1] = readl(tc->cama + CA_TG_INTER_ST) >> 24;
	if (off < sizeof(hdr)) {
		size_t n = min_t(size_t, count, sizeof(hdr) - off);

		memcpy(buf, (u8 *)hdr + off, n);
		if (count > n)
			memcpy(buf + n, tc->afo_vaddr, count - n);
	} else {
		memcpy(buf, (u8 *)tc->afo_vaddr + off - sizeof(hdr), count);
	}
	mutex_unlock(&tc->af_lock);
	return count;
}

static struct bin_attribute af_stats_attr = {
	.attr = { .name = "af_stats", .mode = 0444 },
	.size = AF_HDR_WORDS * 4 + AFO_SIZE,
	.read = af_stats_read,
};

static ssize_t af_pgn_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct titan_sensor *s = container_of(dev, struct titan_sensor, dev);

	return sprintf(buf, "%s%#x\n", s->tc->af_pgn_auto ? "auto " : "", s->tc->af_pgn);
}

static ssize_t af_pgn_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct titan_sensor *s = container_of(dev, struct titan_sensor, dev);
	struct titan_cam *tc = s->tc;
	unsigned int v;

	if (sysfs_streq(buf, "auto")) {
		mutex_lock(&tc->af_lock);
		tc->af_pgn_auto = true;
		mutex_unlock(&tc->af_lock);
		return len;
	}
	if (kstrtouint(buf, 0, &v) || !v || v > 0x7ff)
		return -EINVAL;
	mutex_lock(&tc->af_lock);
	tc->af_pgn_auto = false;
	tc->af_pgn = v;
	if (tc->af_running)
		writel(v, tc->cama + CA_SGG1_PGN);	/* latched at the next frame */
	mutex_unlock(&tc->af_lock);
	return len;
}
static DEVICE_ATTR_RW(af_pgn);

/*
 * AE/AWB state for tuning: "running luma exp gain wbR wbB target src", src =
 * where the luma came from: aa (the AAO), cpu (the raw subsample, when the
 * AAO is empty) or manual (the app set exposure by hand; the loop is idle).
 */
static ssize_t ae_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct titan_sensor *s = container_of(dev, struct titan_sensor, dev);
	struct titan_cam *tc = s->tc;
	int n;

	mutex_lock(&tc->af_lock);
	n = sprintf(buf, "%d %u %u %u %u %u %u %s\n", tc->aa_running, tc->ae_luma,
		    tc->ae_exp, tc->ae_gain, tc->wb[0], tc->wb[2], ae_target,
		    s->ae_manual ? "manual" : tc->ae_cpu ? "cpu" : "aa");
	mutex_unlock(&tc->af_lock);
	return n;
}
static DEVICE_ATTR_RO(ae);

/* DIP worker stats for this stream: "frames busy nobuf rawerr us us_max" */
static ssize_t dip_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct titan_cam *tc = container_of(dev, struct titan_sensor, dev)->tc;

	return sprintf(buf, "%llu %llu %llu %llu %u %u\n", tc->dip_frames, tc->dip_drops,
		       tc->dip_nobuf, tc->dip_rawerr, tc->dip_us, tc->dip_us_max);
}
static DEVICE_ATTR_RO(dip);

/* ------------------------------------------------------------------ */
/* hardware ISP (DIP, pass 2): per-frame packed RAW10 -> YUYV           */

static int dipbuf_alloc(struct titan_cam *tc, struct dip_buf *b, size_t size)
{
	b->size = PAGE_ALIGN(size);
	return dbuf_alloc(tc, &b->vaddr, &b->mva, b->size);
}

static void dipbuf_free(struct titan_cam *tc, struct dip_buf *b)
{
	dbuf_free(tc, &b->vaddr, &b->mva, b->size);
}

static void dip_teardown(struct titan_cam *tc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tc->raw); i++)
		dipbuf_free(tc, &tc->raw[i]);
	dipbuf_free(tc, &tc->tdr);
	dipbuf_free(tc, &tc->desc);
	dipbuf_free(tc, &tc->smx);
}

/* raw ring + tile list + CQ descriptor */
/*
 * Smaller YUYV sizes the node offers besides the sensor modes: the DIP's
 * resizer (CRZ, out of IMG2O) scales the mode's frame down, here to about
 * the screen's size, for a preview with a quarter or less of the memory
 * traffic. CRZ's algorithm 1 covers 2x to 8x down (dip_core.h).
 */
static const struct crz_size {
	u32 w, h, ow, oh;
} crz_sizes[] = {
	{ 2336, 1752, 960, 720 },	/* rear, binned */
	{ 1632, 1224, 816, 612 },	/* front, binned */
};

static const struct crz_size *find_crz(u32 ow, u32 oh)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(crz_sizes); i++)
		if (crz_sizes[i].ow == ow && crz_sizes[i].oh == oh)
			return &crz_sizes[i];
	return NULL;
}

/* the DIP job's CRZ output: 0 when the node takes the frame's own size */
static u32 crz_w(struct titan_cam *tc) { return tc->out_w != tc->width ? tc->out_w : 0; }
static u32 crz_h(struct titan_cam *tc) { return tc->out_w != tc->width ? tc->out_h : 0; }

static int dip_setup(struct titan_cam *tc)
{
	size_t rawsz = tc->raw_stride * tc->height;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(tc->raw); i++) {
		ret = dipbuf_alloc(tc, &tc->raw[i], rawsz);
		if (ret)
			goto err;
	}
	ret = dipbuf_alloc(tc, &tc->tdr, 0x10000);
	if (!ret)
		ret = dipbuf_alloc(tc, &tc->desc, CQ_SLOTS * 8);
	/* NBC's stripe memory (SMX, ~41 MB): only for tiles split in rows, which
	 * these lists don't use; kept for experiments (nr_smx) */
	/* noise reduction where there's a tile list for it, else without */
	tc->nr = nr && dip_find_tdr_out(tc->width, tc->height, tc->raw_stride, tc->black >= 0,
					DIP_MOD_NBC | (ee ? DIP_MOD_SEEE : 0), crz_w(tc), crz_h(tc));
	tc->ee = ee && dip_find_tdr_out(tc->width, tc->height, tc->raw_stride, tc->black >= 0,
					DIP_MOD_SEEE | (tc->nr ? DIP_MOD_NBC : 0), crz_w(tc), crz_h(tc));
	if (!ret && tc->nr && nr_smx)
		ret = dipbuf_alloc(tc, &tc->smx, DIP_SMX_TOTAL);
	if (ret)
		goto err;
	if (!dip_find_tdr_out(tc->width, tc->height, tc->raw_stride, tc->black >= 0,
			      (tc->nr ? DIP_MOD_NBC : 0) | (tc->ee ? DIP_MOD_SEEE : 0),
			      crz_w(tc), crz_h(tc))) {
		dev_err(tc->dev, "no DIP tile list for %ux%u stride %u%s\n",
			tc->width, tc->height, tc->raw_stride, tc->black >= 0 ? " +OBC2" : "");
		ret = -EINVAL;
		goto err;
	}
	tc->rc = tc->rn = 0;
	tc->dip_ready = -1;
	tc->dip_busy = false;
	tc->wb[0] = tc->wb[1] = tc->wb[2] = 512;
	return 0;
err:
	dip_teardown(tc);
	return ret;
}

/* grey-world white balance from a subsample of the packed RAW10 frame */

static void dip_work_fn(struct work_struct *w)
{
	struct titan_cam *tc = container_of(w, struct titan_cam, dip_work);
	struct titan_buf *out;
	unsigned long flags;
	struct dip_job job;
	ktime_t t0;
	int idx, ret;

	for (;;) {
		spin_lock_irqsave(&tc->qlock, flags);
		idx = tc->dip_ready;
		tc->dip_ready = -1;
		if (idx < 0) {
			tc->dip_busy = false;
			spin_unlock_irqrestore(&tc->qlock, flags);
			return;
		}
		out = list_first_entry_or_null(&tc->queued, struct titan_buf, list);
		if (out)
			list_del(&out->list);
		spin_unlock_irqrestore(&tc->qlock, flags);

		if (!out) {		/* no output buffer queued: drop this frame */
			tc->dip_nobuf++;
			continue;
		}
		t0 = ktime_get();
		WRITE_ONCE(tc->raw_last, idx);	/* for the 100 ms AE/AWB work */
		if (!tc->aa_running && !(tc->dip_frames & 7)) {
			u32 rgb[3], wbr[3];	/* no AA work: WB here, every 8th frame */

			if (!raw_stats(tc, tc->raw[idx].vaddr, rgb, wbr))
				wb_set(tc, wbr);
		}
		job = (struct dip_job){
			.width = tc->width, .height = tc->height, .stride = tc->raw_stride,
			.sensor = tc->active && tc->active->d->has_focus ? 0 : 1,
			.black = tc->black,
			.nbc = tc->nr, .seee = tc->ee,
			.out_w = crz_w(tc), .out_h = crz_h(tc),
			.split = dip_split || tc->nr || tc->ee,	/* NBC's tiles: dip_split_layout */
			.keep = dip_keep, .pad = dip_pad, .padval = dip_padval,
			.tcm2 = dip_tcm2,
			.probe = dip_map, .tcm0 = dip_tcm0, .tcm3 = dip_tcm3, .probe_xor = dip_map_xor,
			.nbc_con1_clr = nr_con1_clr,
			.nbc_lut = nr_lut,
			.dbg = dip_dbg,
			.int_en = dip_int_en, .cq_int_en = dip_cq_int_en,
			.done = dip_poll ? NULL : &tc->dipc->done,
			.wb = { min_t(u32, tc->wb[0] * sensor_dgain(tc->active) / 512, 8191),
				min_t(u32, tc->wb[1] * sensor_dgain(tc->active) / 512, 8191),
				min_t(u32, tc->wb[2] * sensor_dgain(tc->active) / 512, 8191) },
			.in_mva = tc->raw[idx].mva, .out_mva = out->mva,
			.tdr_mva = tc->tdr.mva, .tdr_vaddr = tc->tdr.vaddr,
			.smx_mva = tc->smx.mva,
			.desc_mva = tc->desc.mva, .desc_vaddr = tc->desc.vaddr,
		};
		tc->dipc->seen_int = tc->dipc->seen_cq = 0;
		ret = dip_run(tc->dip, &job, 50000);
		if (dip_trace)
			pr_info("dip: trace ret %d %lld us start %x int %08x cq %08x\n", ret,
				ktime_us_delta(ktime_get(), t0), readl(tc->dip + DIP_CTL_START),
				tc->dipc->seen_int, tc->dipc->seen_cq);
		tc->dip_us = ktime_us_delta(ktime_get(), t0);
		tc->dip_us_max = max(tc->dip_us_max, tc->dip_us);
		out->vb.sequence = tc->sequence;
		out->vb.field = V4L2_FIELD_NONE;
		out->vb.vb2_buf.timestamp = ktime_get_ns();
		vb2_set_plane_payload(&out->vb.vb2_buf, 0, tc->stride * tc->out_h);
		vb2_buffer_done(&out->vb.vb2_buf,
				ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		tc->dip_frames++;
	}
}

/* ------------------------------------------------------------------ */
/* the video node                                                      */

static void set_format(struct titan_cam *tc, const struct titan_fmt *fmt, u32 w, u32 h)
{
	const struct crz_size *c = fmt->dip ? find_crz(w, h) : NULL;

	tc->fmt = fmt;
	tc->out_w = w;
	tc->out_h = h;
	tc->width = c ? c->w : w;	/* the raw frame CAMSV captures */
	tc->height = c ? c->h : h;
	tc->stride = ALIGN(w * fmt->bytes, 16);
	if (fmt->dip) {
		/* CAMSV captures packed RAW10 for the DIP; the node outputs YUYV */
		tc->raw_bytes = tc->width * 10 / 8;
		tc->raw_stride = ALIGN(tc->raw_bytes, 16);
	}
}

static int queue_setup(struct vb2_queue *q, unsigned int *nbuf, unsigned int *nplanes,
		       unsigned int sizes[], struct device *alloc_devs[])
{
	struct titan_cam *tc = vb2_get_drv_priv(q);
	unsigned int size = tc->stride * tc->out_h;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	if (*nbuf < 3)
		*nbuf = 3;
	return 0;
}

/* vb2-dma-sg maps the buffer for camsv1: one device address range, which
 * CAMSV (raw formats) or the DIP (YUYV) writes */
static int buf_init(struct vb2_buffer *vb)
{
	struct titan_cam *tc = vb2_get_drv_priv(vb->vb2_queue);
	struct titan_buf *b = container_of(to_vb2_v4l2_buffer(vb), struct titan_buf, vb);
	struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, 0);

	if (sgt->nents != 1) {
		dev_err(tc->dev, "buffer in %u device ranges\n", sgt->nents);
		return -EINVAL;
	}
	b->mva = sg_dma_address(sgt->sgl);
	return 0;
}

static int buf_prepare(struct vb2_buffer *vb)
{
	struct titan_cam *tc = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = tc->stride * tc->out_h;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void buf_queue(struct vb2_buffer *vb)
{
	struct titan_cam *tc = vb2_get_drv_priv(vb->vb2_queue);
	struct titan_buf *b = container_of(to_vb2_v4l2_buffer(vb), struct titan_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&tc->qlock, flags);
	list_add_tail(&b->list, &tc->queued);
	spin_unlock_irqrestore(&tc->qlock, flags);
}

static void return_all(struct titan_cam *tc, enum vb2_buffer_state state)
{
	struct titan_buf *b, *n;
	unsigned long flags;

	spin_lock_irqsave(&tc->qlock, flags);
	list_for_each_entry_safe(b, n, &tc->queued, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	if (!tc->fmt->dip) {	/* DIP mode: cur/next are the raw ring, not vb2 */
		if (tc->cur && tc->cur != tc->next)
			vb2_buffer_done(&tc->cur->vb.vb2_buf, state);
		if (tc->next)
			vb2_buffer_done(&tc->next->vb.vb2_buf, state);
	}
	tc->cur = tc->next = NULL;
	spin_unlock_irqrestore(&tc->qlock, flags);
}

static struct titan_sensor *linked_sensor(struct titan_cam *tc)
{
	struct media_pad *pad = media_pad_remote_pad_first(&tc->vpad);

	if (!pad)
		return NULL;
	return to_sensor(media_entity_to_v4l2_subdev(pad->entity));
}

static int start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct titan_cam *tc = vb2_get_drv_priv(q);
	struct titan_sensor *s;
	struct titan_buf *first;
	unsigned long flags;
	int ret;

	/* vb2 calls this with the queue lock (tc->lock) held */
	s = linked_sensor(tc);
	if (!s || s->mode->width != tc->width || s->mode->height != tc->height) {
		ret = -EPIPE;	/* no sensor linked, or formats don't match */
		goto err;
	}
	ret = video_device_pipeline_start(&tc->vdev, &tc->pipe);
	if (ret)
		goto err;
	ret = camsys_get(tc);
	if (ret)
		goto err_pipe;

	tc->sequence = 0;
	tc->dip_frames = tc->dip_drops = tc->dip_nobuf = tc->dip_rawerr = 0;
	tc->dip_us = tc->dip_us_max = 0;
	tc->raw_last = -1;
	if (tc->fmt->dip) {
		/* hardware ISP: CAMSV fills the raw ring, the DIP worker fills the
		 * YUYV vb2 buffers (left queued). imgsys powers the DIP */
		tc->black = s->d->black;
		ret = imgsys_on(tc);
		if (!ret) {
			dip_set_rate(tc);
			ret = dip_setup(tc);
			if (ret)	/* no tile list, no memory: let ISP power off again */
				imgsys_off(tc);
		}
		if (ret) {
			camsys_put(tc);
			goto err_pipe;
		}
		if (s->wb[1])	/* start from this camera's last gains, not unity */
			memcpy(tc->wb, s->wb, sizeof(tc->wb));
		tc->rc = tc->rn = 0;	/* camsv_start programs raw[0]; SOF(1) sets rc=0 */
		first = NULL;		/* first CAMSV base is a raw buffer */
	} else {
		spin_lock_irqsave(&tc->qlock, flags);
		first = list_first_entry(&tc->queued, struct titan_buf, list);
		list_del(&first->list);
		tc->cur = NULL;
		tc->next = first;	/* latched at the first start of frame */
		spin_unlock_irqrestore(&tc->qlock, flags);
	}

	seninf_start(tc, s->d->csi_port, s->mode->pix2);
	ret = sensor_start(s);
	if (ret) {
		dev_err(tc->dev, "%s: start failed: %d\n", s->sd.name, ret);
		goto err_sensor;
	}
	camsv_start(tc, first ? first->mva : tc->raw[0].mva, s->mode->pix2);
	tc->active = s;
	cama_start(tc, s->mode);	/* AF (rear) + AE/AWB statistics */
	ret = request_irq(tc->irq, camsv_irq, 0, DRV, tc);
	if (ret) {
		dev_err(tc->dev, "irq %d: %d\n", tc->irq, ret);
		camsv_stop(tc);
		tc->active = NULL;
		goto err_sensor;
	}
	return 0;

err_sensor:
	sensor_stop(s);
	seninf_stop(tc, s->d->csi_port);
	camsys_put(tc);
err_pipe:
	video_device_pipeline_stop(&tc->vdev);
err:
	return_all(tc, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void stop_streaming(struct vb2_queue *q)
{
	struct titan_cam *tc = vb2_get_drv_priv(q);

	/* vb2 calls this with the queue lock (tc->lock) held */
	if (tc->active) {
		cama_stop(tc);
		camsv_stop(tc);
		free_irq(tc->irq, tc);
		msleep(40);	/* let a frame in flight finish */
		if (tc->fmt->dip) {
			cancel_work_sync(&tc->dip_work);
			memcpy(tc->active->wb, tc->wb, sizeof(tc->wb));
			dip_teardown(tc);
			imgsys_off(tc);
		}
		sensor_stop(tc->active);
		seninf_stop(tc, tc->active->d->csi_port);
		camsys_put(tc);
		tc->active = NULL;
	}
	video_device_pipeline_stop(&tc->vdev);
	return_all(tc, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops titan_vb2_ops = {
	.queue_setup = queue_setup,
	.buf_init = buf_init,
	.buf_prepare = buf_prepare,
	.buf_queue = buf_queue,
	.start_streaming = start_streaming,
	.stop_streaming = stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int vidioc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV, sizeof(cap->driver));
	strscpy(cap->card, "Titan Pocket camera (CAMSV)", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DRV, sizeof(cap->bus_info));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int vidioc_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;
	f->pixelformat = formats[f->index].fourcc;
	strscpy(f->description, formats[f->index].desc, sizeof(f->description));
	return 0;
}

static void fill_pix(const struct titan_fmt *fmt, struct v4l2_pix_format *p, u32 w, u32 h)
{
	p->width = w;
	p->height = h;
	p->pixelformat = fmt->fourcc;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = ALIGN(w * fmt->bytes, 16);
	p->sizeimage = p->bytesperline * h;
	p->colorspace = V4L2_COLORSPACE_RAW;
}

static int vidioc_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct titan_cam *tc = video_drvdata(file);

	fill_pix(tc->fmt, &f->fmt.pix, tc->out_w, tc->out_h);
	return 0;
}

/* any size one of the sensors produces, or (dip) a CRZ size */
static void nearest_size(u32 *w, u32 *h, bool dip)
{
	u32 bw = 0, bh = 0, i, j;

	for (i = 0; dip && i < ARRAY_SIZE(crz_sizes); i++)
		if (!bw || abs((int)crz_sizes[i].ow - (int)*w) < abs((int)bw - (int)*w)) {
			bw = crz_sizes[i].ow;
			bh = crz_sizes[i].oh;
		}

	for (i = 0; i < NSENSORS; i++)
		for (j = 0; j < sensors[i].nmodes; j++) {
			const struct sensor_mode *m = &sensors[i].modes[j];

			if (!bw || abs((int)m->width - (int)*w) < abs((int)bw - (int)*w)) {
				bw = m->width;
				bh = m->height;
			}
		}
	*w = bw;
	*h = bh;
}

static int vidioc_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	const struct titan_fmt *fmt = find_format(f->fmt.pix.pixelformat) ?: &formats[0];
	u32 w = f->fmt.pix.width, h = f->fmt.pix.height;

	nearest_size(&w, &h, fmt->dip);
	fill_pix(fmt, &f->fmt.pix, w, h);
	return 0;
}

static int vidioc_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct titan_cam *tc = video_drvdata(file);

	if (vb2_is_busy(&tc->queue))
		return -EBUSY;
	vidioc_try_fmt(file, fh, f);
	set_format(tc, find_format(f->fmt.pix.pixelformat), f->fmt.pix.width,
		   f->fmt.pix.height);
	return 0;
}

/* the sensors' mode sizes (the node captures the linked sensor's mode) */
static int vidioc_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fs)
{
	u32 n = 0, i, j;

	if (!find_format(fs->pixel_format))
		return -EINVAL;
	for (i = 0; i < NSENSORS; i++)
		for (j = 0; j < sensors[i].nmodes; j++) {
			if (n++ != fs->index)
				continue;
			fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
			fs->discrete.width = sensors[i].modes[j].width;
			fs->discrete.height = sensors[i].modes[j].height;
			return 0;
		}
	for (i = 0; find_format(fs->pixel_format)->dip && i < ARRAY_SIZE(crz_sizes); i++)
		if (n++ == fs->index) {
			fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
			fs->discrete.width = crz_sizes[i].ow;
			fs->discrete.height = crz_sizes[i].oh;
			return 0;
		}
	return -EINVAL;
}

static int vidioc_enum_input(struct file *file, void *fh, struct v4l2_input *i)
{
	if (i->index)
		return -EINVAL;
	i->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(i->name, "camera", sizeof(i->name));
	return 0;
}

static int vidioc_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vidioc_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops titan_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
	.vidioc_enum_input = vidioc_enum_input,
	.vidioc_g_input = vidioc_g_input,
	.vidioc_s_input = vidioc_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations titan_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* only one sensor link enabled at a time */
static int video_link_setup(struct media_entity *entity, const struct media_pad *local,
			    const struct media_pad *remote, u32 flags)
{
	struct titan_cam *tc = container_of(entity, struct titan_cam, vdev.entity);
	struct media_link *link;

	if (!(flags & MEDIA_LNK_FL_ENABLED))
		return 0;
	list_for_each_entry(link, &entity->links, list)
		if (link->sink == &tc->vpad && link->source != remote &&
		    (link->flags & MEDIA_LNK_FL_ENABLED))
			return -EBUSY;
	return 0;
}

/* no .link_validate: start_streaming checks that the sensor mode matches */
static const struct media_entity_operations video_entity_ops = {
	.link_setup = video_link_setup,
};

/* ------------------------------------------------------------------ */
/* setup                                                               */

static void titan_cleanup(struct titan_cam *tc)
{
	int i;

	if (video_is_registered(&tc->vdev))
		video_unregister_device(&tc->vdev);
	for (i = 0; i < NSENSORS; i++) {
		struct titan_sensor *s = &tc->sensor[i];

		if (s->d) {
			v4l2_device_unregister_subdev(&s->sd);
			v4l2_ctrl_handler_free(&s->ctrls);
			media_entity_cleanup(&s->sd.entity);
		}
		if (!IS_ERR_OR_NULL(s->dovdd))
			regulator_put(s->dovdd);
		if (!IS_ERR_OR_NULL(s->avdd))
			regulator_put(s->avdd);
		if (!IS_ERR_OR_NULL(s->dvdd))
			regulator_put(s->dvdd);
		if (!IS_ERR_OR_NULL(s->afvdd))
			regulator_put(s->afvdd);
		if (s->dev_added)
			device_unregister(&s->dev);
		if (s->adap)
			i2c_put_adapter(s->adap);
		of_node_put(s->np);
	}
	if (media_devnode_is_registered(tc->mdev.devnode))
		media_device_unregister(&tc->mdev);
	if (tc->v4l2.dev)	/* registered (a probe can defer before) */
		v4l2_device_unregister(&tc->v4l2);
	if (tc->mdev.dev)
		media_device_cleanup(&tc->mdev);
	dbuf_free(tc, &tc->scratch_vaddr, &tc->scratch_mva, PAGE_SIZE);
	af_free(tc);
	aa_free(tc);
	if (tc->dip_dev)
		put_device(tc->dip_dev);
}

/*
 * libcamera names a camera after the device behind its sensor's subdev node,
 * so each sensor gets a child device of its own ("rear", "front") as the
 * node's parent (sd->dev). It also carries the sensor's supplies (its device
 * tree node) and the AF/AE sysfs files.
 */
static void sensor_dev_release(struct device *dev)
{
	/* embedded in struct titan_cam */
}

static struct regulator *sensor_supply(struct titan_sensor *s, const char *name)
{
	struct regulator *r = regulator_get(&s->dev, name);

	if (IS_ERR(r))
		dev_err_probe(&s->dev, PTR_ERR(r), "%s supply\n", name);
	return r;
}

static int sensor_add_device(struct titan_cam *tc, struct titan_sensor *s)
{
	int ret;

	device_initialize(&s->dev);
	s->dev.parent = tc->dev;
	s->dev.of_node = s->np;
	s->dev.release = sensor_dev_release;
	ret = dev_set_name(&s->dev, "%s", s->d->node);
	if (!ret)
		ret = device_add(&s->dev);
	if (ret) {
		put_device(&s->dev);
		return ret;
	}
	s->dev_added = true;
	s->sd.dev = &s->dev;
	s->dovdd = sensor_supply(s, "dovdd");
	if (IS_ERR(s->dovdd))
		return PTR_ERR(s->dovdd);
	s->avdd = sensor_supply(s, "avdd");
	if (IS_ERR(s->avdd))
		return PTR_ERR(s->avdd);
	s->dvdd = sensor_supply(s, "dvdd");
	if (IS_ERR(s->dvdd))
		return PTR_ERR(s->dvdd);
	if (s->d->has_focus) {		/* the AF statistics follow the focus motor */
		s->afvdd = sensor_supply(s, "afvdd");
		if (IS_ERR(s->afvdd))
			return PTR_ERR(s->afvdd);
		ret = device_create_bin_file(&s->dev, &af_stats_attr);
		if (!ret)
			ret = device_create_file(&s->dev, &dev_attr_af_pgn);
		if (ret)
			return ret;
	}
	ret = device_create_file(&s->dev, &dev_attr_ae);	/* AE/AWB on both */
	if (!ret)
		ret = device_create_file(&s->dev, &dev_attr_dip);
	return ret;
}

/* the DIP (ISP pass 2): bound so that the IOMMU enables its larb 5 ports;
 * the camera driver runs it */

/*
 * The DIP's interrupt (SPI 268), enabled only for CQ thread 0 done: the
 * frame or split tile has ended. CQ_STATUS and INT_STATUS are read-clear.
 * The registers can be read only while the camera has the DIP clocked.
 */
static irqreturn_t dip_irq(int irq, void *data)
{
	struct titan_dip *dip = data;
	u32 cq;

	if (!READ_ONCE(dip->live))
		return IRQ_NONE;
	cq = readl(dip->regs + DIP_CQ_STATUS);
	dip->seen_cq |= cq;
	dip->seen_int |= readl(dip->regs + DIP_INT_STATUS);
	if (cq & DIP_CQ_DONE)
		complete(&dip->done);
	return IRQ_HANDLED;
}

static int dip_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct titan_dip *dip;
	int ret;

	dip = devm_kzalloc(dev, sizeof(*dip), GFP_KERNEL);
	if (!dip)
		return -ENOMEM;
	dip->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dip->regs))
		return PTR_ERR(dip->regs);
	dip->clk = devm_clk_get(dev, "dip");
	if (IS_ERR(dip->clk))
		return dev_err_probe(dev, PTR_ERR(dip->clk), "no clock\n");
	init_completion(&dip->done);
	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	dip->irq = ret;
	ret = devm_request_irq(dev, dip->irq, dip_irq, 0, "titan-dip", dip);
	if (ret)
		return dev_err_probe(dev, ret, "no interrupt\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (!ret)
		ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	/*
	 * The power-domain driver switches every domain on at probe, and with
	 * pd_ignore_unused only a runtime suspend here turns ISP off again.
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;
	pm_runtime_put(dev);
	platform_set_drvdata(pdev, dip);
	return 0;
}

static const struct of_device_id dip_of_match[] = {
	{ .compatible = "mediatek,mt6771-dip" },
	{ }
};
MODULE_DEVICE_TABLE(of, dip_of_match);

static struct platform_driver dip_driver = {
	.probe = dip_probe,
	.driver = { .name = "titan-dip", .of_match_table = dip_of_match },
};

static int titan_get_dip(struct titan_cam *tc)
{
	struct device_node *np = of_parse_phandle(tc->dev->of_node, "mediatek,dip", 0);
	struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;
	struct titan_dip *dip;

	of_node_put(np);
	if (!pdev)
		return dev_err_probe(tc->dev, -ENODEV, "no mediatek,dip\n");
	tc->dip_dev = &pdev->dev;
	dip = platform_get_drvdata(pdev);
	if (!dip)
		return dev_err_probe(tc->dev, -EPROBE_DEFER, "dip not bound\n");
	if (!device_link_add(tc->dev, tc->dip_dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	/*
	 * The DIP is given addresses mapped for camsv1: both must be behind the
	 * IOMMU, in its one domain. Otherwise its DMA would reach other memory.
	 */
	if (!iommu_get_domain_for_dev(tc->dev) ||
	    iommu_get_domain_for_dev(tc->dev) != iommu_get_domain_for_dev(tc->dip_dev))
		return dev_err_probe(tc->dev, -ENODEV,
				     "camsv1 and dip are not in one IOMMU domain\n");
	tc->dip = dip->regs;
	tc->dip_clk = dip->clk;
	tc->dipc = dip;
	return 0;
}

static int titan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct titan_cam *tc;
	int i, ret;

	tc = devm_kzalloc(dev, sizeof(*tc), GFP_KERNEL);
	if (!tc)
		return -ENOMEM;
	mutex_init(&tc->lock);
	mutex_init(&tc->pm_lock);
	mutex_init(&tc->sensor_lock);
	mutex_init(&tc->af_lock);
	tc->af_pgn = af_pgn_default;
	tc->af_pgn_auto = true;
	INIT_DELAYED_WORK(&tc->af_work, af_work_fn);
	spin_lock_init(&tc->qlock);
	INIT_LIST_HEAD(&tc->queued);
	INIT_WORK(&tc->dip_work, dip_work_fn);
	tc->pdev = pdev;
	tc->dev = dev;

	tc->camsv = devm_platform_ioremap_resource_byname(pdev, "camsv");
	if (IS_ERR(tc->camsv))
		return PTR_ERR(tc->camsv);
	tc->cama = devm_platform_ioremap_resource_byname(pdev, "cam-a");
	if (IS_ERR(tc->cama))
		return PTR_ERR(tc->cama);
	tc->seninf = devm_platform_ioremap_resource_byname(pdev, "seninf");
	if (IS_ERR(tc->seninf))
		return PTR_ERR(tc->seninf);
	tc->mipi_rx = devm_platform_ioremap_resource_byname(pdev, "mipi-rx");
	if (IS_ERR(tc->mipi_rx))
		return PTR_ERR(tc->mipi_rx);
	tc->irq = platform_get_irq(pdev, 0);
	if (tc->irq < 0)
		return tc->irq;
	tc->nclks = devm_clk_bulk_get_all(dev, &tc->clks);
	if (tc->nclks < 0)
		return dev_err_probe(dev, tc->nclks, "clocks\n");
	/* the IOMMU's address space is 32 bits; one device range per buffer */
	dma_set_max_seg_size(dev, UINT_MAX);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (!ret)
		ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = titan_get_dip(tc);
	if (!ret)
		ret = dbuf_alloc(tc, &tc->scratch_vaddr, &tc->scratch_mva, PAGE_SIZE);
	if (!ret)
		ret = af_alloc(tc);
	if (!ret)
		ret = aa_alloc(tc);
	if (ret)
		goto err;
	/* as for the DIP: a runtime suspend turns the CAM domain off */
	ret = camsys_get(tc);
	if (ret)
		goto err;
	camsys_put(tc);
	dev_dbg(dev, "scratch at %pad; irq %d\n", &tc->scratch_mva, tc->irq);
	/* the IRQ is requested while streaming only (camsys powered) */

	/* media device, V4L2 device */
	tc->mdev.dev = dev;
	strscpy(tc->mdev.model, "Titan Pocket camera", sizeof(tc->mdev.model));
	strscpy(tc->mdev.driver_name, DRV, sizeof(tc->mdev.driver_name));
	strscpy(tc->mdev.bus_info, "platform:" DRV, sizeof(tc->mdev.bus_info));
	media_device_init(&tc->mdev);
	tc->v4l2.mdev = &tc->mdev;
	ret = v4l2_device_register(dev, &tc->v4l2);
	if (ret)
		goto err;

	/* video node */
	set_format(tc, &formats[0], sensors[0].modes[0].width, sensors[0].modes[0].height);
	tc->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	tc->queue.io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	tc->queue.drv_priv = tc;
	tc->queue.buf_struct_size = sizeof(struct titan_buf);
	tc->queue.ops = &titan_vb2_ops;
	tc->queue.mem_ops = &vb2_dma_sg_memops;
	tc->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	tc->queue.lock = &tc->lock;
	tc->queue.dev = dev;
	tc->queue.min_queued_buffers = 2;
	ret = vb2_queue_init(&tc->queue);
	if (ret)
		goto err;
	strscpy(tc->vdev.name, "titan-camsv", sizeof(tc->vdev.name));
	tc->vdev.v4l2_dev = &tc->v4l2;
	tc->vdev.fops = &titan_fops;
	tc->vdev.ioctl_ops = &titan_ioctl_ops;
	tc->vdev.release = video_device_release_empty;
	tc->vdev.queue = &tc->queue;
	tc->vdev.lock = &tc->lock;
	tc->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	tc->vdev.entity.ops = &video_entity_ops;
	tc->vpad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&tc->vdev.entity, 1, &tc->vpad);
	if (ret)
		goto err;
	video_set_drvdata(&tc->vdev, tc);
	ret = video_register_device(&tc->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err;

	/* sensors, linked to the video node (the rear one enabled) */
	for (i = 0; i < NSENSORS; i++) {
		struct titan_sensor *s = &tc->sensor[i];

		ret = sensor_init_one(tc, s, &sensors[i]);
		if (ret)
			goto err;
		ret = media_create_pad_link(&s->sd.entity, 0, &tc->vdev.entity, 0,
					    i == 0 ? MEDIA_LNK_FL_ENABLED : 0);
		if (ret)
			goto err;
		ret = sensor_add_device(tc, s);
		if (ret)
			goto err;
	}
	ret = v4l2_device_register_subdev_nodes(&tc->v4l2);
	if (ret)
		goto err;
	ret = media_device_register(&tc->mdev);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, tc);
	dev_info(dev, "%s: %s, irq %d\n", tc->mdev.model, video_device_node_name(&tc->vdev),
		 tc->irq);
	return 0;

err:
	titan_cleanup(tc);
	return ret;
}

static void titan_remove(struct platform_device *pdev)
{
	titan_cleanup(platform_get_drvdata(pdev));
}

/* streams don't survive a system suspend (nothing restores the sensor, seninf
 * and CAMSV): stop it first */
static int titan_suspend(struct device *dev)
{
	struct titan_cam *tc = dev_get_drvdata(dev);

	if (tc && vb2_is_streaming(&tc->queue)) {
		dev_warn(dev, "streaming: suspend refused\n");
		return -EBUSY;
	}
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(titan_pm_ops, titan_suspend, NULL);

static const struct of_device_id titan_of_match[] = {
	{ .compatible = "mediatek,mt6771-camsv" },
	{ }
};
MODULE_DEVICE_TABLE(of, titan_of_match);

static struct platform_driver titan_driver = {
	.probe = titan_probe,
	.remove = titan_remove,
	.driver = {
		.name = DRV,
		.of_match_table = titan_of_match,
		.pm = pm_sleep_ptr(&titan_pm_ops),
	},
};

static struct platform_driver * const drivers[] = { &dip_driver, &titan_driver };

static int __init titan_camera_init(void)
{
	return platform_register_drivers(drivers, ARRAY_SIZE(drivers));
}

static void __exit titan_camera_exit(void)
{
	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
}

module_init(titan_camera_init);
module_exit(titan_camera_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Titan Pocket cameras: OV16880 + OV8865 through seninf and CAMSV");
