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
	u32 bytes;		/* per pixel */
	const char *desc;
};

static const struct titan_fmt formats[] = {
	{ V4L2_PIX_FMT_SBGGR8, 1, "8-bit Bayer BGGR" },
	{ V4L2_PIX_FMT_SBGGR14, 2, "14-bit Bayer BGGR (10-bit << 4)" },
};

static const struct titan_fmt *find_format(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (formats[i].fourcc == fourcc)
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
/* 1-pixel mode only so far; the rear 4672x3504 mode needs 2-pixel mode */
static const struct sensor_mode ov16880_modes[] = {
	{ TBL(ov16880_preview), 2336, 1752, 2512, 3824, 30 },
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

	void __iomem *seninf, *mipi_rx, *camsv;
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
	struct sg_table scratch_sgt;
	struct page **scratch_pages;
	int scratch_npages;
	u32 sequence;

	/* format */
	const struct titan_fmt *fmt;
	u32 width, height, stride;
};


/* ------------------------------------------------------------------ */
/* clocks, power domains, M4U (BSP APIs)                               */

#define SMI_LARB6	6
extern s32 smi_bus_prepare_enable(const u32 id, const char *user);
extern s32 smi_bus_disable_unprepare(const u32 id, const char *user);

#define M4U_PORT_CAM_SOCO	72	/* larb6 port 11: CAMSV output */
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
	__v4l2_ctrl_modify_range(s->exposure, 4, m->vts - 8, 1, m->vts - 64);
}

static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct titan_sensor *s = container_of(ctrl->handler, struct titan_sensor, ctrls);

	if (ctrl->id == V4L2_CID_VBLANK) {	/* frame length bounds exposure */
		u32 vts = s->mode->height + ctrl->val;

		__v4l2_ctrl_modify_range(s->exposure, 4, vts - 8, 1,
					 min_t(s32, s->exposure->default_value, vts - 8));
	}
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

static void seninf_start(struct titan_cam *tc, int p)
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
	rmw(top + SENINF1_MUX_CTRL, 0xfffffeff, 0);
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

static void camsv_start(struct titan_cam *tc, unsigned int first)
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
	if (tc->fmt->bytes == 1) {
		sv_rmw(tc, SV_MODULE_EN, ~0x3f2cU, 0x40000005);
		writel(0, tc->camsv + SV_FMT_SEL);
		sv_rmw(tc, SV_PAK, ~0x37U, 0x0);
	} else {
		sv_rmw(tc, SV_MODULE_EN, ~0x3f2cU, 0x40000009);
		writel(1, tc->camsv + SV_FMT_SEL);
		sv_rmw(tc, SV_PAK, ~0x37U, 0x1);
	}
	writel(SV_INT_SOF | SV_INT_DONE | SV_INT_IMGO_ERR | SV_INT_OVERRUN,
	       tc->camsv + SV_INT_EN);
	writel(0, tc->camsv + SV_DMA_EN);
	sv_rmw(tc, 0x3b0, 0x7fffffff, 0);
	writel(0xffffffff, tc->camsv + 0x3c4);
	sv_rmw(tc, 0x018, ~0U, 0x61000000);

	/* TG: the whole frame, continuous (single-frame + FBC writes nothing) */
	sv_rmw(tc, SV_TG_SEN_MODE, ~0x60003U, 0x10000);
	sv_rmw(tc, SV_TG_VF_CON, ~0x2U, 0);
	writel(tc->width << 16, tc->camsv + SV_TG_GRAB_PXL);
	writel(tc->height << 16, tc->camsv + SV_TG_GRAB_LIN);

	/* IMGO DMA; the buffer address is switched at every start of frame */
	writel(tc->width * tc->fmt->bytes - 1, tc->camsv + SV_IMGO_XSIZE);
	writel(tc->height - 1, tc->camsv + SV_IMGO_YSIZE);
	writel(tc->stride | 0x1010000, tc->camsv + SV_IMGO_STRIDE);
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
/* the video node                                                      */

static void set_format(struct titan_cam *tc, const struct titan_fmt *fmt, u32 w, u32 h)
{
	tc->fmt = fmt;
	tc->width = w;
	tc->height = h;
	tc->stride = ALIGN(w * fmt->bytes, 16);
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

	ret = camsys_get(tc);	/* held while the buffer exists */
	if (ret)
		return ret;
	ret = m4u_alloc_mva(tc->m4u_client, M4U_PORT_CAM_SOCO, 0, sgt,
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
		m4u_dealloc_mva(tc->m4u_client, M4U_PORT_CAM_SOCO, b->mva);
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
	if (tc->cur && tc->cur != tc->next)
		vb2_buffer_done(&tc->cur->vb.vb2_buf, state);
	if (tc->next)
		vb2_buffer_done(&tc->next->vb.vb2_buf, state);
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
	spin_lock_irqsave(&tc->qlock, flags);
	first = list_first_entry(&tc->queued, struct titan_buf, list);	/* min_buffers_needed */
	list_del(&first->list);
	tc->cur = NULL;
	tc->next = first;	/* latched at the first start of frame */
	spin_unlock_irqrestore(&tc->qlock, flags);

	seninf_start(tc, s->d->csi_port);
	ret = sensor_start(s);
	if (ret) {
		dev_err(&tc->pdev->dev, "%s: start failed: %d\n", s->d->name, ret);
		goto err_sensor;
	}
	camsv_start(tc, first->mva);
	tc->active = s;
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
		camsv_stop(tc);
		free_irq(tc->irq, tc);
		msleep(40);	/* let a frame in flight finish */
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
	spin_lock_init(&tc->qlock);
	INIT_LIST_HEAD(&tc->queued);

	tc->pdev = pdev;	/* CAMSV1, 0x1a050000 */
	res = platform_get_resource(tc->pdev, IORESOURCE_MEM, 0);
	tc->irq = platform_get_irq(tc->pdev, 0);
	if (!res || tc->irq <= 0) {
		ret = -ENODEV;
		tc->irq = 0;
		goto err;
	}
	tc->camsv = ioremap(res->start, resource_size(res));
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
