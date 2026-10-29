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
 * Everything is driven from here, without device tree changes: sensor power
 * through the BSP imgsensor node's pin states and PMIC regulators, clocks
 * straight from their providers, buffers mapped through the M4U. Bring-up
 * notes and the recovered register sequences: modules/camera/camcap and
 * docs/camera.md.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
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

#include <dt-bindings/clock/mt6771-clk.h>

#include "../camcap/seninf_reg.h"
#include "../camcap/ov8865_regs.h"
#include "../camcap/ov16880_regs.h"
#include "../camcap/af_regs_rear.h"
#include "dip_core.h"			/* pass-2 ISP (DIP) register programming */

static bool hwisp = true;
module_param(hwisp, bool, 0644);
MODULE_PARM_DESC(hwisp, "offer the YUYV format (hardware ISP, pass 2) as well as raw Bayer");

static bool nr;	/* noise reduction (NBC): needs SMX stripe buffers, WIP */
module_param(nr, bool, 0644);
MODULE_PARM_DESC(nr, "noise reduction (NBC) in the hardware ISP YUYV path");

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

static unsigned int ae_target = 46;	/* 8-bit AA luma (~1/4 of the 10-bit raw) */
module_param(ae_target, uint, 0644);
MODULE_PARM_DESC(ae_target, "target mean scene luma for auto exposure (0-255)");

/* past this the loop raises gain instead: a shaky hand blurs longer exposures */
static unsigned int ae_max_us = 10000;
module_param(ae_max_us, uint, 0644);
MODULE_PARM_DESC(ae_max_us, "longest auto exposure in microseconds, then gain (0: the frame length)");

/* 5.8's camera location control; libcamera reads it for front/back */
#ifndef V4L2_CID_CAMERA_ORIENTATION
#define V4L2_CID_CAMERA_ORIENTATION	(V4L2_CID_CAMERA_CLASS_BASE + 34)
#define V4L2_CAMERA_ORIENTATION_FRONT	0
#define V4L2_CAMERA_ORIENTATION_BACK	1
#define V4L2_CAMERA_ORIENTATION_EXTERNAL 2
#endif
/* 5.8's mounting rotation: the correction, counter-clockwise, in degrees */
#ifndef V4L2_CID_CAMERA_SENSOR_ROTATION
#define V4L2_CID_CAMERA_SENSOR_ROTATION	(V4L2_CID_CAMERA_CLASS_BASE + 35)
#endif

/*
 * 5.10's "I/O routed by the media controller" capability: libcamera only
 * asks for the formats that go with a sensor's bus code when it is set.
 * (4.14's core clears the ENUM_FMT mbus_code field before the driver sees
 * it, so every format is listed; both suit the sensors' 10-bit Bayer bus.)
 */
#ifndef V4L2_CAP_IO_MC
#define V4L2_CAP_IO_MC			0x20000000
#endif

#ifndef V4L2_PIX_FMT_SBGGR14
#define V4L2_PIX_FMT_SBGGR14 v4l2_fourcc('B', 'G', '1', '4')	/* 4.18+ */
#endif

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
	const char *name;	/* entity name */
	u32 orientation;	/* V4L2_CAMERA_ORIENTATION_* */
	u32 rotation;		/* V4L2_CID_CAMERA_SENSOR_ROTATION, degrees ccw */
	int idx;		/* imgsensor pin-state index: cam0 / cam1 */
	int i2c_bus;
	u16 i2c_addr;
	int csi_port;
	int camtg;
	const char *avdd;	/* regulator, or NULL: the GPIO160 LDO */
	unsigned int step_ms;
	const struct reg8 *init;
	int ninit;
	const struct sensor_mode *modes;
	int nmodes;
	bool needs_stream_on;
	bool has_focus;
	u32 black;		/* black level, 10-bit */
	u32 array_w, array_h;	/* active pixel array; every mode covers all of it */
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
	{ "ov16880 2-0036", V4L2_CAMERA_ORIENTATION_BACK, 270, 0, 2, 0x36, 0, TOP_MUX_CAMTG, NULL, 1, TBL(ov16880_init),
	  TBL(ov16880_modes), true, true, 64, 4672, 3504 },
	{ "ov8865 4-0010", V4L2_CAMERA_ORIENTATION_FRONT, 90, 1, 4, 0x10, 1, TOP_MUX_CAMTG2, "vcama1", 5, TBL(ov8865_init),
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
	struct i2c_adapter *adap;
	struct regulator *dovdd, *avdd, *dvdd, *afvdd;
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

#define SENINF_BASE	0x1a040000
#define SENINF_SIZE	0x8000
#define MIPI_RX_BASE	0x11c80000
#define MIPI_RX_SIZE	0x6000
#define LANES		4

struct titan_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	unsigned int mva;
	int port;		/* M4U port it is mapped on */
};

struct titan_cam {
	struct platform_device *pdev;	/* the camsv1 node: our DMA device and IRQ */
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
	struct pinctrl *pctl;
	struct clk *mclk_parent;
	bool smi_on, clocks_on;
	struct mutex pm_lock;
	struct mutex sensor_lock;	/* sensor modes (not tc->lock: link validation
					 * runs under it, inside start_streaming) */
	int pm_count;		/* camsys power users: buffers mapped, streaming */

	/* buffers */
	void *m4u_client;
	spinlock_t qlock;
	struct list_head queued;
	struct titan_buf *cur, *next;	/* in the DMA now / programmed for the next frame */
	unsigned int scratch_mva;	/* one page: CAMSV's frame headers */
	/* AF statistics (CAM A AFO) */
	struct page *afo_pages[2];
	struct sg_table afo_sgt;
	unsigned int afo_mva;
	void *afo_vaddr;		/* uncached */
	struct mutex af_lock;
	bool af_running;
	u32 af_pgn;
	bool af_pgn_auto;
	unsigned long af_pgn_changed;	/* jiffies */
	struct delayed_work af_work;
	/* AE/AWB statistics (CAM A AAO): per-block R/G/B + luma averages */
	struct page *aao_pages[2];
	struct sg_table aao_sgt;
	unsigned int aao_mva;
	void *aao_vaddr;		/* uncached */
	bool aa_running;
	u32 ae_exp, ae_gain;		/* last applied: exposure lines, gain (128 = 1x) */
	u32 ae_luma;			/* last luma the loop saw */
	bool ae_cpu;			/* ... from the CPU raw subsample, not the AAO */
	struct sg_table scratch_sgt;
	struct page **scratch_pages;
	int scratch_npages;
	u32 sequence;

	/* format */
	const struct titan_fmt *fmt;
	u32 width, height, stride;

	/* hardware ISP (DIP, pass 2): CAMSV captures packed RAW10 into a raw
	 * ring; a worker runs the DIP per frame into the YUYV vb2 buffer */
	bool imgsys_on;
	void __iomem *dip;
	u32 raw_stride, raw_bytes;	/* packed RAW10 line pitch / used bytes */
	int black;			/* OBC2 black level, -1 for front (OBC in pass 1) */
	struct {
		struct page **pages;
		int npages;
		struct sg_table sgt;
		u32 mva_sv, mva_imgi;	/* CAMSV write / DIP read */
		void *vaddr;
	} raw[3];
	int rc, rn;			/* raw ring: index being filled / programmed next */
	struct {			/* DIP tile list + CQ descriptor */
		struct page **pages;
		int npages;
		struct sg_table sgt;
		u32 mva;
		void *vaddr;
	} tdr, desc;
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
/* clocks, power domains, M4U (BSP APIs)                               */

#define SMI_LARB6	6
extern s32 smi_bus_prepare_enable(const u32 id, const char *user);
extern s32 smi_bus_disable_unprepare(const u32 id, const char *user);

#define M4U_PORT_CAM_SOCO	72	/* larb6 port 11: CAMSV output */
#define M4U_PORT_CAM_IMGI	36	/* larb5: DIP input */
#define M4U_PORT_CAM_IMG2O	37	/* larb5: DIP tile list / CQ descriptor */
#define M4U_PORT_CAM_IMG3O	38	/* larb5: DIP YUV output */
#define M4U_PROT_RW		3

static struct {
	const char *provider;
	int id;
	struct clk *clk;
} clks[] = {
	{ "mediatek,scpsys",   SCP_SYS_DIS },
	{ "mediatek,scpsys",   SCP_SYS_CAM },
	{ "mediatek,topckgen", TOP_MUX_CAM },
	{ "mediatek,topckgen", TOP_MUX_SENINF },
	{ "mediatek,camsys",   CAMSYS_CAM_CGPDN },
	{ "mediatek,camsys",   CAMSYS_CAMTG_CGPDN },
	{ "mediatek,camsys",   CAMSYS_SENINF_CGPDN },
	{ "mediatek,camsys",   CAMSYS_LARB6_CGPDN },
	{ "mediatek,camsys",   CAMSYS_CAMSV0_CGPDN },
	{ "mediatek,camsys",   CAMSYS_CAMSV1_CGPDN },
	{ "mediatek,camsys",   CAMSYS_CAMSV2_CGPDN },
};

static struct clk *provider_clk(const char *compat, int idx)
{
	struct of_phandle_args args = { .args_count = 1, .args = { idx } };
	struct clk *clk;

	args.np = of_find_compatible_node(NULL, NULL, compat);
	if (!args.np)
		return ERR_PTR(-ENODEV);
	clk = of_clk_get_from_provider(&args);
	of_node_put(args.np);
	return clk;
}

/*
 * camsys power, reference counted. The M4U writes larb6 port registers when
 * it maps a buffer, so every mapping (and unmapping) needs camsys powered,
 * or the bus hangs; buffers keep it on while they exist.
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
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		ret = clk_prepare_enable(clks[i].clk);
		if (ret) {
			while (--i >= 0)
				clk_disable_unprepare(clks[i].clk);
			return ret;
		}
	}
	tc->smi_on = smi_bus_prepare_enable(SMI_LARB6, DRV) == 0;
	tc->clocks_on = true;
	return 0;
}

static void camsys_off(struct titan_cam *tc)
{
	int i;

	if (!tc->clocks_on)
		return;
	if (tc->smi_on)
		smi_bus_disable_unprepare(SMI_LARB6, DRV);
	tc->smi_on = false;
	for (i = ARRAY_SIZE(clks) - 1; i >= 0; i--)
		clk_disable_unprepare(clks[i].clk);
	tc->clocks_on = false;
}

/* imgsys power for the DIP (pass 2), enabled only while a hwisp stream runs */
#define SMI_LARB5	5
static struct {
	const char *provider;
	int id;
	struct clk *clk;
} imgclks[] = {
	{ "mediatek,scpsys",   SCP_SYS_ISP },
	{ "mediatek,topckgen", TOP_MUX_IMG },
	{ "mediatek,imgsys",   IMG_LARB5 },
	{ "mediatek,imgsys",   IMG_DIP },
};

static struct clk *provider_clk(const char *compat, int idx);

static int imgsys_on(struct titan_cam *tc)
{
	int i, ret;

	if (tc->imgsys_on)
		return 0;
	for (i = 0; i < ARRAY_SIZE(imgclks); i++) {
		if (!imgclks[i].clk)
			imgclks[i].clk = provider_clk(imgclks[i].provider, imgclks[i].id);
		if (IS_ERR(imgclks[i].clk))
			return PTR_ERR(imgclks[i].clk);
		ret = clk_prepare_enable(imgclks[i].clk);
		if (ret) {
			while (--i >= 0)
				clk_disable_unprepare(imgclks[i].clk);
			return ret;
		}
	}
	smi_bus_prepare_enable(SMI_LARB5, DRV);
	tc->imgsys_on = true;
	return 0;
}

static void imgsys_off(struct titan_cam *tc)
{
	int i;

	if (!tc->imgsys_on)
		return;
	smi_bus_disable_unprepare(SMI_LARB5, DRV);
	for (i = ARRAY_SIZE(imgclks) - 1; i >= 0; i--)
		clk_disable_unprepare(imgclks[i].clk);
	tc->imgsys_on = false;
}

static void *ksym(const char *name)
{
	void *fn = (void *)kallsyms_lookup_name(name);

	if (!fn)
		pr_err(DRV ": %s not found\n", name);
	return fn;
}

static int (*m4u_alloc_mva)(void *client, int port, unsigned long va, struct sg_table *sgt,
			    unsigned int size, unsigned int prot, unsigned int flags,
			    unsigned int *mva);
static int (*m4u_dealloc_mva)(void *client, int port, unsigned int mva);
static void *(*m4u_create_client)(void);
static int (*m4u_destroy_client)(void *client);

/* ------------------------------------------------------------------ */
/* sensor power and registers                                          */

static int pin(struct titan_cam *tc, const char *fmt, ...)
{
	struct pinctrl_state *st;
	char name[32];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(name, sizeof(name), fmt, ap);
	va_end(ap);
	st = pinctrl_lookup_state(tc->pctl, name);
	if (IS_ERR(st))
		return PTR_ERR(st);
	return pinctrl_select_state(tc->pctl, st);
}

static int reg_on(struct regulator **r, const char *name, int uv)
{
	int ret;

	*r = regulator_get(NULL, name);
	if (IS_ERR(*r)) {
		ret = PTR_ERR(*r);
		*r = NULL;
		return ret;
	}
	ret = regulator_set_voltage(*r, uv, uv);
	if (!ret)
		ret = regulator_enable(*r);
	if (ret) {
		regulator_put(*r);
		*r = NULL;
	}
	return ret;
}

static void reg_off(struct regulator **r)
{
	if (*r) {
		regulator_disable(*r);
		regulator_put(*r);
		*r = NULL;
	}
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

/* the stock power sequence (docs/camera.md) */
static int sensor_power_on(struct titan_sensor *s)
{
	struct titan_cam *tc = s->tc;
	const struct sensor_desc *d = s->d;
	int ret;

	if (s->powered)
		return 0;
	ret = clk_set_parent(s->mclk, tc->mclk_parent);	/* 24 MHz */
	if (!ret)
		ret = clk_prepare_enable(s->mclk);
	if (ret)
		return ret;
	pin(tc, "cam%d_mclk_4mA", d->idx);
	pin(tc, "cam%d_pnd0", d->idx);
	pin(tc, "cam%d_rst0", d->idx);
	mdelay(d->step_ms);
	ret = reg_on(&s->dovdd, "vcamio", 1800000);
	if (ret)
		goto err;
	mdelay(d->step_ms);
	ret = d->avdd ? reg_on(&s->avdd, d->avdd, 2800000) : pin(tc, "cam0_ldo_vcama_1");
	if (ret)
		goto err;
	mdelay(d->step_ms);
	ret = reg_on(&s->dvdd, "vcamd", 1200000);
	if (ret)
		goto err;
	mdelay(5);
	if (d->has_focus)
		reg_on(&s->afvdd, "vldo28", 2800000);
	pin(tc, "cam%d_pnd1", d->idx);
	mdelay(d->step_ms);
	pin(tc, "cam%d_rst1", d->idx);
	mdelay(10);
	s->powered = true;
	return 0;
err:
	s->powered = true;	/* so power_off undoes the partial sequence */
	return ret;
}

static void sensor_power_off(struct titan_sensor *s)
{
	struct titan_cam *tc = s->tc;
	const struct sensor_desc *d = s->d;

	if (!s->powered)
		return;
	pin(tc, "cam%d_rst0", d->idx);
	pin(tc, "cam%d_pnd0", d->idx);
	reg_off(&s->afvdd);
	reg_off(&s->dvdd);
	if (d->avdd)
		reg_off(&s->avdd);
	else
		pin(tc, "cam0_ldo_vcama_0");
	reg_off(&s->dovdd);
	pin(tc, "cam%d_mclk_off", d->idx);
	clk_disable_unprepare(s->mclk);
	s->powered = false;
}

/* exposure (1/16 line units), gain and frame length as one group-hold
 * update; both OmniVision sensors use the same registers */
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
	s_write(s, 0x3508, s->gain->val >> 8);
	s_write(s, 0x3509, s->gain->val & 0xff);
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

static const char * const orientation_menu[] = { "Front", "Back", "External", NULL };

static int sensor_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sensor_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
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

static int sensor_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct titan_sensor *s = to_sensor(sd);

	mutex_lock(&s->tc->sensor_lock);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY && cfg)
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, 0);
	else
		fill_fmt(s->mode, &fmt->format);
	mutex_unlock(&s->tc->sensor_lock);
	return 0;
}

static int sensor_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
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
		if (cfg)
			*v4l2_subdev_get_try_format(sd, cfg, 0) = fmt->format;
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
static int sensor_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
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
	dev_dbg(&s->tc->pdev->dev, "%s id %02x%02x\n", s->d->name, hi, lo);
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

	s->d = d;
	s->tc = tc;
	s->mode = &d->modes[0];
	s->adap = i2c_get_adapter(d->i2c_bus);
	if (!s->adap)
		return -EPROBE_DEFER;
	s->mclk = provider_clk("mediatek,topckgen", d->camtg);
	if (IS_ERR(s->mclk))
		return PTR_ERR(s->mclk);

	v4l2_subdev_init(&s->sd, &sensor_ops);
	strlcpy(s->sd.name, d->name, sizeof(s->sd.name));
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
	{
		struct v4l2_ctrl_config orientation = {
			.id = V4L2_CID_CAMERA_ORIENTATION,
			.name = "Camera Orientation",
			.type = V4L2_CTRL_TYPE_MENU,
			.max = V4L2_CAMERA_ORIENTATION_EXTERNAL,
			.def = d->orientation,
			.qmenu = orientation_menu,
			.flags = V4L2_CTRL_FLAG_READ_ONLY,
		};

		struct v4l2_ctrl_config rotation = {
			.id = V4L2_CID_CAMERA_SENSOR_ROTATION,
			.name = "Camera Sensor Rotation",
			.type = V4L2_CTRL_TYPE_INTEGER,
			.min = d->rotation,
			.max = d->rotation,
			.step = 1,
			.def = d->rotation,
			.flags = V4L2_CTRL_FLAG_READ_ONLY,
		};

		v4l2_ctrl_new_custom(h, &orientation, NULL);
		v4l2_ctrl_new_custom(h, &rotation, NULL);
	}
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
			writel(tc->raw[tc->rn].mva_sv, tc->camsv + SV_IMGO_BASE);
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
#define CAMA_BASE	0x1a004000
#define CAMA_SIZE	0x2000
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
#define M4U_PORT_CAM_AFO	64	/* larb6 port 3 */

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
#define M4U_PORT_CAM_AAO	63	/* larb6 port 2 */
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

static int af_alloc(struct titan_cam *tc)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(tc->afo_pages); i++) {
		tc->afo_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!tc->afo_pages[i])
			return -ENOMEM;
	}
	tc->afo_vaddr = vmap(tc->afo_pages, ARRAY_SIZE(tc->afo_pages), VM_MAP,
			     pgprot_writecombine(PAGE_KERNEL));
	if (!tc->afo_vaddr)
		return -ENOMEM;
	ret = sg_alloc_table_from_pages(&tc->afo_sgt, tc->afo_pages, ARRAY_SIZE(tc->afo_pages),
					0, ARRAY_SIZE(tc->afo_pages) * PAGE_SIZE, GFP_KERNEL);
	if (ret)
		return ret;
	return m4u_alloc_mva(tc->m4u_client, M4U_PORT_CAM_AFO, 0, &tc->afo_sgt,
			     ARRAY_SIZE(tc->afo_pages) * PAGE_SIZE, M4U_PROT_RW, 0, &tc->afo_mva);
}

static void af_free(struct titan_cam *tc)
{
	int i;

	if (tc->afo_mva && camsys_get(tc) == 0) {
		m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_AFO, tc->afo_mva);
		camsys_put(tc);
	}
	if (tc->afo_sgt.sgl)
		sg_free_table(&tc->afo_sgt);
	if (tc->afo_vaddr)
		vunmap(tc->afo_vaddr);
	for (i = 0; i < ARRAY_SIZE(tc->afo_pages); i++)
		if (tc->afo_pages[i])
			__free_page(tc->afo_pages[i]);
}

static int aa_alloc(struct titan_cam *tc)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(tc->aao_pages); i++) {
		tc->aao_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!tc->aao_pages[i])
			return -ENOMEM;
	}
	tc->aao_vaddr = vmap(tc->aao_pages, ARRAY_SIZE(tc->aao_pages), VM_MAP,
			     pgprot_writecombine(PAGE_KERNEL));
	if (!tc->aao_vaddr)
		return -ENOMEM;
	ret = sg_alloc_table_from_pages(&tc->aao_sgt, tc->aao_pages, ARRAY_SIZE(tc->aao_pages),
					0, ARRAY_SIZE(tc->aao_pages) * PAGE_SIZE, GFP_KERNEL);
	if (ret)
		return ret;
	return m4u_alloc_mva(tc->m4u_client, M4U_PORT_CAM_AAO, 0, &tc->aao_sgt,
			     ARRAY_SIZE(tc->aao_pages) * PAGE_SIZE, M4U_PROT_RW, 0, &tc->aao_mva);
}

static void aa_free(struct titan_cam *tc)
{
	int i;

	if (tc->aao_mva && camsys_get(tc) == 0) {
		m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_AAO, tc->aao_mva);
		camsys_put(tc);
	}
	if (tc->aao_sgt.sgl)
		sg_free_table(&tc->aao_sgt);
	if (tc->aao_vaddr)
		vunmap(tc->aao_vaddr);
	for (i = 0; i < ARRAY_SIZE(tc->aao_pages); i++)
		if (tc->aao_pages[i])
			__free_page(tc->aao_pages[i]);
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
	s_write(s, 0x3508, gain >> 8);
	s_write(s, 0x3509, gain & 0xff);
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

/* a page-backed, write-combined, M4U-mapped buffer; pages[] + sgt reused */
static int dipbuf_alloc(struct titan_cam *tc, struct page ***ppages, int *pnpages,
			struct sg_table *sgt, void **vaddr, size_t size, int port, u32 *mva)
{
	int i, npages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page **pages;

	if (!*ppages) {
		pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL | __GFP_ZERO);
		if (!pages)
			return -ENOMEM;
		for (i = 0; i < npages; i++) {
			pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!pages[i]) {
				while (--i >= 0)
					__free_page(pages[i]);
				kvfree(pages);
				return -ENOMEM;
			}
		}
		*ppages = pages;
		*pnpages = npages;
		*vaddr = vmap(pages, npages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
		if (!*vaddr)
			return -ENOMEM;
		if (sg_alloc_table_from_pages(sgt, pages, npages, 0, npages << PAGE_SHIFT,
					      GFP_KERNEL))
			return -ENOMEM;
	}
	return m4u_alloc_mva(tc->m4u_client, port, 0, sgt, npages << PAGE_SHIFT,
			     M4U_PROT_RW, 0, mva);
}

static void dipbuf_free(struct titan_cam *tc, struct page ***ppages, int *pnpages,
			struct sg_table *sgt, void **vaddr)
{
	int i;

	if (sgt->sgl)
		sg_free_table(sgt);
	sgt->sgl = NULL;
	if (*vaddr)
		vunmap(*vaddr);
	*vaddr = NULL;
	if (*ppages) {
		for (i = 0; i < *pnpages; i++)
			if ((*ppages)[i])
				__free_page((*ppages)[i]);
		kvfree(*ppages);
	}
	*ppages = NULL;
}

static void dip_teardown(struct titan_cam *tc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tc->raw); i++) {
		if (tc->raw[i].mva_sv)
			m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_SOCO, tc->raw[i].mva_sv);
		if (tc->raw[i].mva_imgi)
			m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_IMGI, tc->raw[i].mva_imgi);
		tc->raw[i].mva_sv = tc->raw[i].mva_imgi = 0;
		dipbuf_free(tc, &tc->raw[i].pages, &tc->raw[i].npages, &tc->raw[i].sgt,
			    &tc->raw[i].vaddr);
	}
	if (tc->tdr.mva)
		m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_IMG2O, tc->tdr.mva);
	tc->tdr.mva = 0;
	dipbuf_free(tc, &tc->tdr.pages, &tc->tdr.npages, &tc->tdr.sgt, &tc->tdr.vaddr);
	if (tc->desc.mva)
		m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_IMG2O, tc->desc.mva);
	tc->desc.mva = 0;
	dipbuf_free(tc, &tc->desc.pages, &tc->desc.npages, &tc->desc.sgt, &tc->desc.vaddr);
}

/* raw ring + tile list + CQ descriptor; the DIP registers are mapped once */
static int dip_setup(struct titan_cam *tc)
{
	size_t rawsz = tc->raw_stride * tc->height;
	int i, ret;

	if (!tc->dip) {
		tc->dip = ioremap(DIP_BASE, DIP_SIZE);
		if (!tc->dip)
			return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(tc->raw); i++) {
		ret = dipbuf_alloc(tc, &tc->raw[i].pages, &tc->raw[i].npages, &tc->raw[i].sgt,
				   &tc->raw[i].vaddr, rawsz, M4U_PORT_CAM_SOCO, &tc->raw[i].mva_sv);
		if (ret)
			goto err;
		/* a second mapping of the same pages, for the DIP's IMGI read */
		ret = m4u_alloc_mva(tc->m4u_client, M4U_PORT_CAM_IMGI, 0, &tc->raw[i].sgt,
				    tc->raw[i].npages << PAGE_SHIFT, M4U_PROT_RW, 0,
				    &tc->raw[i].mva_imgi);
		if (ret)
			goto err;
	}
	ret = dipbuf_alloc(tc, &tc->tdr.pages, &tc->tdr.npages, &tc->tdr.sgt, &tc->tdr.vaddr,
			   0x10000, M4U_PORT_CAM_IMG2O, &tc->tdr.mva);
	if (ret)
		goto err;
	ret = dipbuf_alloc(tc, &tc->desc.pages, &tc->desc.npages, &tc->desc.sgt, &tc->desc.vaddr,
			   CQ_SLOTS * 8, M4U_PORT_CAM_IMG2O, &tc->desc.mva);
	if (ret)
		goto err;
	if (!dip_find_tdr(tc->width, tc->height, tc->raw_stride, tc->black >= 0, nr)) {
		dev_err(&tc->pdev->dev, "no DIP tile list for %ux%u stride %u%s\n",
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
			.nbc = nr,
			.wb = { tc->wb[0], tc->wb[1], tc->wb[2] },
			.in_mva = tc->raw[idx].mva_imgi, .out_mva = out->mva,
			.tdr_mva = tc->tdr.mva, .tdr_vaddr = tc->tdr.vaddr,
			.desc_mva = tc->desc.mva, .desc_vaddr = tc->desc.vaddr,
		};
		ret = dip_program(tc->dip, &job);
		if (!ret)
			ret = dip_wait(tc->dip, 50000);
		tc->dip_us = ktime_us_delta(ktime_get(), t0);
		tc->dip_us_max = max(tc->dip_us_max, tc->dip_us);
		out->vb.sequence = tc->sequence;
		out->vb.field = V4L2_FIELD_NONE;
		out->vb.vb2_buf.timestamp = ktime_get_ns();
		vb2_set_plane_payload(&out->vb.vb2_buf, 0, tc->stride * tc->height);
		vb2_buffer_done(&out->vb.vb2_buf,
				ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		tc->dip_frames++;
	}
}

/* ------------------------------------------------------------------ */
/* the video node                                                      */

static void set_format(struct titan_cam *tc, const struct titan_fmt *fmt, u32 w, u32 h)
{
	tc->fmt = fmt;
	tc->width = w;
	tc->height = h;
	tc->stride = ALIGN(w * fmt->bytes, 16);
	if (fmt->dip) {
		/* CAMSV captures packed RAW10 for the DIP; the node outputs YUYV */
		tc->raw_bytes = w * 10 / 8;
		tc->raw_stride = ALIGN(tc->raw_bytes, 16);
	}
}

static int queue_setup(struct vb2_queue *q, unsigned int *nbuf, unsigned int *nplanes,
		       unsigned int sizes[], struct device *alloc_devs[])
{
	struct titan_cam *tc = vb2_get_drv_priv(q);
	unsigned int size = tc->stride * tc->height;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	if (*nbuf < 3)
		*nbuf = 3;
	return 0;
}

static int buf_init(struct vb2_buffer *vb)
{
	struct titan_cam *tc = vb2_get_drv_priv(vb->vb2_queue);
	struct titan_buf *b = container_of(to_vb2_v4l2_buffer(vb), struct titan_buf, vb);
	struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, 0);
	int ret;

	/* raw mode: CAMSV writes the buffer (SOCO). DIP mode: the DIP writes
	 * the YUYV output (IMG3O); CAMSV writes a separate raw ring */
	b->port = tc->fmt->dip ? M4U_PORT_CAM_IMG3O : M4U_PORT_CAM_SOCO;
	ret = camsys_get(tc);	/* held while the buffer exists */
	if (ret)
		return ret;
	ret = m4u_alloc_mva(tc->m4u_client, b->port, 0, sgt,
			    PAGE_ALIGN(vb2_plane_size(vb, 0)), M4U_PROT_RW, 0, &b->mva);
	if (ret) {
		b->mva = 0;
		camsys_put(tc);
	}
	return ret;
}

static void buf_cleanup(struct vb2_buffer *vb)
{
	struct titan_cam *tc = vb2_get_drv_priv(vb->vb2_queue);
	struct titan_buf *b = container_of(to_vb2_v4l2_buffer(vb), struct titan_buf, vb);

	if (b->mva) {
		m4u_dealloc_mva(tc->m4u_client, b->port, b->mva);
		camsys_put(tc);
	}
	b->mva = 0;
}

static int buf_prepare(struct vb2_buffer *vb)
{
	struct titan_cam *tc = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = tc->stride * tc->height;

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
	struct media_pad *pad = media_entity_remote_pad(&tc->vpad);

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
	ret = media_pipeline_start(&tc->vdev.entity, &tc->vdev.pipe);
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
		if (!ret)
			ret = dip_setup(tc);
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
		dev_err(&tc->pdev->dev, "%s: start failed: %d\n", s->d->name, ret);
		goto err_sensor;
	}
	camsv_start(tc, first ? first->mva : tc->raw[0].mva_sv, s->mode->pix2);
	tc->active = s;
	cama_start(tc, s->mode);	/* AF (rear) + AE/AWB statistics */
	ret = request_irq(tc->irq, camsv_irq, 0, DRV, tc);
	if (ret) {
		dev_err(&tc->pdev->dev, "irq %d: %d\n", tc->irq, ret);
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
	media_pipeline_stop(&tc->vdev.entity);
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
			dip_teardown(tc);	/* imgsys + camsys still on for M4U */
			imgsys_off(tc);
		}
		sensor_stop(tc->active);
		seninf_stop(tc, tc->active->d->csi_port);
		camsys_put(tc);
		tc->active = NULL;
	}
	media_pipeline_stop(&tc->vdev.entity);
	return_all(tc, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops titan_vb2_ops = {
	.queue_setup = queue_setup,
	.buf_init = buf_init,
	.buf_cleanup = buf_cleanup,
	.buf_prepare = buf_prepare,
	.buf_queue = buf_queue,
	.start_streaming = start_streaming,
	.stop_streaming = stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int vidioc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strlcpy(cap->driver, DRV, sizeof(cap->driver));
	strlcpy(cap->card, "Titan Pocket camera (CAMSV)", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:" DRV, sizeof(cap->bus_info));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int vidioc_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;
	f->pixelformat = formats[f->index].fourcc;
	strlcpy(f->description, formats[f->index].desc, sizeof(f->description));
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

	fill_pix(tc->fmt, &f->fmt.pix, tc->width, tc->height);
	return 0;
}

/* any size one of the sensors produces */
static void nearest_size(u32 *w, u32 *h)
{
	u32 bw = 0, bh = 0, i, j;

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

	nearest_size(&w, &h);
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
	return -EINVAL;
}

static int vidioc_enum_input(struct file *file, void *fh, struct v4l2_input *i)
{
	if (i->index)
		return -EINVAL;
	i->type = V4L2_INPUT_TYPE_CAMERA;
	strlcpy(i->name, "camera", sizeof(i->name));
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

static int scratch_alloc(struct titan_cam *tc)
{
	size_t size = PAGE_SIZE;	/* frame headers (FH_BASE) */
	int i, ret;

	tc->scratch_npages = size >> PAGE_SHIFT;
	tc->scratch_pages = kvmalloc_array(tc->scratch_npages, sizeof(struct page *),
					   GFP_KERNEL | __GFP_ZERO);
	if (!tc->scratch_pages)
		return -ENOMEM;
	for (i = 0; i < tc->scratch_npages; i++) {
		tc->scratch_pages[i] = alloc_page(GFP_KERNEL);
		if (!tc->scratch_pages[i])
			return -ENOMEM;
	}
	ret = sg_alloc_table_from_pages(&tc->scratch_sgt, tc->scratch_pages,
					tc->scratch_npages, 0, size, GFP_KERNEL);
	if (ret)
		return ret;
	return m4u_alloc_mva(tc->m4u_client, M4U_PORT_CAM_SOCO, 0, &tc->scratch_sgt, size,
			     M4U_PROT_RW, 0, &tc->scratch_mva);
}

static void scratch_free(struct titan_cam *tc)
{
	int i;

	if (tc->scratch_mva && camsys_get(tc) == 0) {
		m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_SOCO, tc->scratch_mva);
		camsys_put(tc);
	}
	if (tc->scratch_sgt.sgl)
		sg_free_table(&tc->scratch_sgt);
	if (tc->scratch_pages) {
		for (i = 0; i < tc->scratch_npages; i++)
			if (tc->scratch_pages[i])
				__free_page(tc->scratch_pages[i]);
		kvfree(tc->scratch_pages);
	}
}

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
		if (s->dev_added)
			device_unregister(&s->dev);
		if (s->adap)
			i2c_put_adapter(s->adap);
		if (!IS_ERR_OR_NULL(s->mclk))
			clk_put(s->mclk);
	}
	if (media_devnode_is_registered(tc->mdev.devnode))
		media_device_unregister(&tc->mdev);
	v4l2_device_unregister(&tc->v4l2);
	media_device_cleanup(&tc->mdev);
	scratch_free(tc);
	af_free(tc);
	aa_free(tc);
	if (tc->m4u_client)
		m4u_destroy_client(tc->m4u_client);
	for (i = 0; i < ARRAY_SIZE(clks); i++)
		if (!IS_ERR_OR_NULL(clks[i].clk))
			clk_put(clks[i].clk);
	if (!IS_ERR_OR_NULL(tc->mclk_parent))
		clk_put(tc->mclk_parent);
	if (!IS_ERR_OR_NULL(tc->pctl))
		pinctrl_put(tc->pctl);
	if (tc->seninf)
		iounmap(tc->seninf);
	if (tc->mipi_rx)
		iounmap(tc->mipi_rx);
	if (tc->camsv)
		iounmap(tc->camsv);
	if (tc->cama)
		iounmap(tc->cama);
	if (tc->dip)
		iounmap(tc->dip);
	kfree(tc);
}

/*
 * libcamera names a camera after the device behind its sensor's subdev node.
 * The core parents both nodes to CAMSV1, which gives the two cameras the same
 * ID; move each node under a child device of its own ("rear", "front").
 * (Registering the nodes ourselves would need v4l2_subdev_fops, which 4.14
 * doesn't export.)
 */
static void sensor_dev_release(struct device *dev)
{
	/* embedded in struct titan_cam */
}

static int sensor_add_device(struct titan_cam *tc, struct titan_sensor *s)
{
	int ret;

	device_initialize(&s->dev);
	s->dev.parent = &tc->pdev->dev;
	s->dev.release = sensor_dev_release;
	ret = dev_set_name(&s->dev, "%s",
			   s->d->orientation == V4L2_CAMERA_ORIENTATION_BACK ? "rear" : "front");
	if (!ret)
		ret = device_add(&s->dev);
	if (ret) {
		put_device(&s->dev);
		return ret;
	}
	s->dev_added = true;
	if (s->d->has_focus) {		/* the AF statistics follow the focus motor */
		ret = device_create_bin_file(&s->dev, &af_stats_attr);
		if (!ret)
			ret = device_create_file(&s->dev, &dev_attr_af_pgn);
		if (ret)
			return ret;
	}
	ret = device_create_file(&s->dev, &dev_attr_ae);	/* AE/AWB on both */
	if (!ret)
		ret = device_create_file(&s->dev, &dev_attr_dip);
	if (ret)
		return ret;
	return s->sd.devnode ? device_move(&s->sd.devnode->dev, &s->dev, DPM_ORDER_NONE) : 0;
}

static struct platform_device *find_pdev(const char *compat)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, compat);
	struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;

	of_node_put(np);
	return pdev;
}

static int titan_probe(struct platform_device *pdev)
{
	struct platform_device *imgsensor;
	struct titan_cam *tc;
	struct resource *res;
	int i, ret;

	tc = kzalloc(sizeof(*tc), GFP_KERNEL);
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

	tc->pdev = pdev;	/* CAMSV1, 0x1a050000 */
	res = platform_get_resource(tc->pdev, IORESOURCE_MEM, 0);
	tc->irq = platform_get_irq(tc->pdev, 0);
	if (!res || tc->irq <= 0) {
		ret = -ENODEV;
		tc->irq = 0;
		goto err;
	}
	tc->camsv = ioremap(res->start, resource_size(res));
	tc->cama = ioremap(CAMA_BASE, CAMA_SIZE);
	tc->seninf = ioremap(SENINF_BASE, SENINF_SIZE);
	tc->mipi_rx = ioremap(MIPI_RX_BASE, MIPI_RX_SIZE);
	if (!tc->camsv || !tc->seninf || !tc->mipi_rx) {
		ret = -ENOMEM;
		goto err;
	}
	/* the M4U gives the camera its device addresses; the DMA API only does
	 * cache maintenance here, so any page will do (no bouncing) */
	dma_set_mask_and_coherent(&tc->pdev->dev, DMA_BIT_MASK(64));

	/* sensor pins live on the BSP imgsensor node */
	imgsensor = find_pdev("mediatek,imgsensor");
	if (!imgsensor) {
		ret = -ENODEV;
		goto err;
	}
	tc->pctl = pinctrl_get(&imgsensor->dev);
	put_device(&imgsensor->dev);
	if (IS_ERR(tc->pctl)) {
		ret = PTR_ERR(tc->pctl);
		goto err;
	}
	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		clks[i].clk = provider_clk(clks[i].provider, clks[i].id);
		if (IS_ERR(clks[i].clk)) {
			ret = PTR_ERR(clks[i].clk);
			goto err;
		}
	}
	tc->mclk_parent = provider_clk("mediatek,topckgen", TOP_UNIVP_192M_D8);
	if (IS_ERR(tc->mclk_parent)) {
		ret = PTR_ERR(tc->mclk_parent);
		goto err;
	}

	tc->m4u_client = m4u_create_client();
	if (IS_ERR_OR_NULL(tc->m4u_client)) {
		tc->m4u_client = NULL;
		ret = -ENOMEM;
		goto err;
	}
	ret = camsys_get(tc);
	if (ret)
		goto err;
	ret = scratch_alloc(tc);
	if (!ret)
		ret = af_alloc(tc);
	if (!ret)
		ret = aa_alloc(tc);
	camsys_put(tc);
	if (ret)
		goto err;
	dev_dbg(&pdev->dev, "scratch at mva %#x; irq %d\n", tc->scratch_mva, tc->irq);
	/* the IRQ is requested while streaming only (camsys powered) */

	/* media device, V4L2 device */
	tc->mdev.dev = &tc->pdev->dev;
	strlcpy(tc->mdev.model, "Titan Pocket camera", sizeof(tc->mdev.model));
	strlcpy(tc->mdev.driver_name, DRV, sizeof(tc->mdev.driver_name));
	strlcpy(tc->mdev.bus_info, "platform:" DRV, sizeof(tc->mdev.bus_info));
	media_device_init(&tc->mdev);
	tc->v4l2.mdev = &tc->mdev;
	/* named here: v4l2_device_register() would build the name from
	 * dev->driver->name, and CAMSV1 has no driver (we released it) */
	strlcpy(tc->v4l2.name, DRV, sizeof(tc->v4l2.name));
	ret = v4l2_device_register(&tc->pdev->dev, &tc->v4l2);
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
	tc->queue.dev = &tc->pdev->dev;
	tc->queue.min_buffers_needed = 2;
	ret = vb2_queue_init(&tc->queue);
	if (ret)
		goto err;
	strlcpy(tc->vdev.name, "titan-camsv", sizeof(tc->vdev.name));
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
	ret = video_register_device(&tc->vdev, VFL_TYPE_GRABBER, -1);
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
	}
	ret = v4l2_device_register_subdev_nodes(&tc->v4l2);
	if (ret)
		goto err;
	for (i = 0; i < NSENSORS; i++) {
		ret = sensor_add_device(tc, &tc->sensor[i]);
		if (ret)
			goto err;
	}
	ret = media_device_register(&tc->mdev);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, tc);
	dev_info(&tc->pdev->dev, "%s: %s, irq %d\n", tc->mdev.model,
		 video_device_node_name(&tc->vdev), tc->irq);
	return 0;

err:
	pr_err(DRV ": init failed: %d\n", ret);
	titan_cleanup(tc);
	return ret;
}

static int titan_remove(struct platform_device *pdev)
{
	titan_cleanup(platform_get_drvdata(pdev));
	return 0;
}

static struct platform_driver titan_driver = {
	.probe = titan_probe,
	.remove = titan_remove,
	.driver = { .name = DRV },	/* bound through driver_override only */
};

static struct platform_device *camsv1;

/*
 * CAMSV1 belongs to the BSP's camera-isp driver (which only serves the
 * Android HAL); release it and bind ours through driver_override. Having a
 * real driver matters: the V4L2/media core reaches dev->driver (names,
 * module references).
 */
static int __init titan_camera_init(void)
{
	int ret;

	m4u_alloc_mva = ksym("m4u_alloc_mva");
	m4u_dealloc_mva = ksym("m4u_dealloc_mva");
	m4u_create_client = ksym("m4u_create_client");
	m4u_destroy_client = ksym("m4u_destroy_client");
	if (!m4u_alloc_mva || !m4u_dealloc_mva || !m4u_create_client || !m4u_destroy_client)
		return -ENOENT;

	camsv1 = find_pdev("mediatek,camsv1");
	if (!camsv1)
		return -ENODEV;
	device_release_driver(&camsv1->dev);
	device_lock(&camsv1->dev);
	kfree(camsv1->driver_override);
	camsv1->driver_override = kstrdup(DRV, GFP_KERNEL);
	device_unlock(&camsv1->dev);
	ret = platform_driver_register(&titan_driver);
	if (ret)
		goto err;
	if (camsv1->dev.driver != &titan_driver.driver) {
		if (device_attach(&camsv1->dev) <= 0 || camsv1->dev.driver != &titan_driver.driver) {
			platform_driver_unregister(&titan_driver);
			ret = -ENODEV;
			goto err;
		}
	}
	return 0;
err:
	put_device(&camsv1->dev);
	return ret;
}

static void __exit titan_camera_exit(void)
{
	platform_driver_unregister(&titan_driver);
	device_lock(&camsv1->dev);
	kfree(camsv1->driver_override);
	camsv1->driver_override = NULL;
	device_unlock(&camsv1->dev);
	put_device(&camsv1->dev);
}

module_init(titan_camera_init);
module_exit(titan_camera_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Titan Pocket cameras: OV16880 + OV8865 through seninf and CAMSV");
