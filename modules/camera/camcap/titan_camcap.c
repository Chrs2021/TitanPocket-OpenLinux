// SPDX-License-Identifier: GPL-2.0
/*
 * titan_camcap: Titan Pocket camera bring-up, phase 1 (see docs/camera.md).
 *
 * Streams one sensor through the MIPI CSI-2 receiver (seninf) into a CAMSV
 * raw DMA engine, which overwrites one frame buffer in memory with every
 * frame. A test harness for the hardware path that the V4L2 drivers will
 * use; no V4L2 here.
 *
 *   insmod titan_camcap.ko cam=0            rear OV16880, 2336x1752 (mode=1: 4672x3504)
 *   insmod titan_camcap.ko cam=1            front OV8865, 1632x1224 (mode=1: 3264x2448)
 *          [exposure=<lines>] [gain=<128 = 1x>] [focus=<0..1023>] [sv=<camsv 0..5>]
 *   cat /sys/kernel/debug/titan_camcap        receiver and CAMSV status
 *   cat /sys/kernel/debug/titan_camcap_frame > f.raw    (tools/camera/raw2png.py)
 *   rmmod titan_camcap                        stop, power off
 *
 * Sources: the seninf/D-PHY sequence follows the ChromeOS MT8183 seninf
 * driver (same block; seninf_reg.h is from there); the CAMSV sequence and the
 * sensor tables were recovered from the stock HAL and kernel (docs/camera.md).
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <dt-bindings/clock/mt6771-clk.h>

#include "seninf_reg.h"
#include "af_regs_front.h"
#include "af_regs_rear.h"
#include "ov8865_regs.h"
#include "ov16880_regs.h"

static int cam = 1;
module_param(cam, int, 0444);
MODULE_PARM_DESC(cam, "0 = rear OV16880, 1 = front OV8865");
static int mode;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "0 = preview (binned), 1 = full resolution");
static int sv;
module_param(sv, int, 0444);
MODULE_PARM_DESC(sv, "CAMSV instance 0..5");
static int exposure = -1;
module_param(exposure, int, 0444);
MODULE_PARM_DESC(exposure, "exposure in lines (default: most of a frame)");
static int gain = 512;
module_param(gain, int, 0444);
MODULE_PARM_DESC(gain, "sensor analog gain, 128 = 1x");
static int unpak;
module_param(unpak, int, 0444);
MODULE_PARM_DESC(unpak, "experiment: 0 packed RAW10, 1 packer off, 2 packer off + bit 3");
static int focus = 256;
module_param(focus, int, 0444);

/* ISP pass 1 instead of CAMSV: CAM A's IMGO, every processing block off */
static int p1;
module_param(p1, int, 0444);
static int p1fmt = 9;		/* CTL_FMT_SEL IMGO format: 9 RAW10 packed, 0 16-bit, 8 RAW8 */
module_param(p1fmt, int, 0444);
static int p1sel;		/* CTL_SEL variant (HAL Cam_path_sel::Path_sel_RAW) */
module_param(p1sel, int, 0444);
static int p1selor;		/* bits ORed into CTL_SEL (HAL Path_sel: 0x190000, |2) */
module_param(p1selor, int, 0444);
static int p1en = 0x3;		/* CTL_EN: bit 0 TG, bit 1 DMX (the demux after TG) */
module_param(p1en, int, 0444);
static int p1en2;		/* CTL_EN2 */
module_param(p1en2, int, 0444);
static int p1dmx;		/* DMX_CTL bits 15:0 (HAL: from a capability query) */
module_param(p1dmx, int, 0444);
static int p1af;		/* AF statistics through AFO */
module_param(p1af, int, 0444);
static int p1afm1 = 1;		/* AFO sizes minus one, as for every DMA. The HAL's
				 * config() leaves the -1 to a lower layer; written
				 * as is, AFO reports error 0x8280 and ignores the
				 * stride */
module_param(p1afm1, int, 0444);
static int p1proc;		/* processed path: IMGO after the raw blocks (OBC ...), not pure raw */
module_param(p1proc, int, 0444);
static int p1rrz;		/* resizer branch: RMX -> RPG -> RRZ (1:1) -> RRZO */
module_param(p1rrz, int, 0444);
static int p1rrzdma = 2;	/* CTL_DMA_EN bit of RRZO (unconfirmed) */
module_param(p1rrzdma, int, 0444);
static int p1aa;		/* AE/AWB statistics (AA) through AAO */
module_param(p1aa, int, 0444);
static int p1aanx = 32, p1aany = 24;	/* AA blocks (max 120 x 90) */
module_param(p1aanx, int, 0444);
module_param(p1aany, int, 0444);
static int p1aaen2;		/* experiments: CTL_EN2 bits for AA (HAL function 0x15: bit 23) */
module_param(p1aaen2, int, 0444);
static int p1img = -1;		/* CTL_SEL IMG_SEL (25:24), the IMGO tap: 2 pure raw from the TG,
				 * 0 before LSC, 3 after LSC (HAL CAM_RAW_PIPE::setIMG_SEL) */
module_param(p1img, int, 0444);
static uint p1regs[32];		/* experiments: offset,value pairs written to CAM A before go */
static int p1regs_n;
module_param_array(p1regs, uint, &p1regs_n, 0444);
static int p1obc;		/* OBC: subtract the sensor's black level (HAL ISP_MGR_OBC) */
module_param(p1obc, int, 0444);
static int p1wb[3];		/* RPG white-balance gains R, G, B; 512 = 1x, 0 = RPG off */
static int p1wb_n;
module_param_array(p1wb, int, &p1wb_n, 0444);
MODULE_PARM_DESC(focus, "rear lens position 0..1023 (0 = far)");

#define SENINF_BASE	0x1a040000
#define SENINF_SIZE	0x8000
#define MIPI_RX_BASE	0x11c80000
#define MIPI_RX_SIZE	0x6000
#define CAMSV_BASE	0x1a050000

#define LANES		4	/* both sensors */

/* ---- the two cameras ---- */

struct cam_mode {
	const struct reg8 *regs;
	int nregs;
	int width, height, vts;
};

struct cam_desc {
	const char *name;
	int i2c_bus;
	u16 i2c_addr;
	int camtg;		/* topckgen mux feeding its MCLK pin */
	const char *avdd;	/* regulator, or NULL: the GPIO160 LDO */
	unsigned int step_ms;
	const struct reg8 *init;
	int ninit;
	struct cam_mode modes[2];
	bool needs_stream_on;	/* mode tables don't start streaming */
	int black;		/* RAW10 black level (stock OBC tuning: 4x this, 12 bit) */
};

#define TBL(t) t, ARRAY_SIZE(t)
static const struct cam_desc cams[2] = {
	{ "OV16880", 2, 0x36, TOP_MUX_CAMTG, NULL, 1, TBL(ov16880_init),
	  { { TBL(ov16880_preview), 2336, 1752, 3824 },
	    { TBL(ov16880_capture), 4672, 3504, 3824 } }, true, 64 },
	{ "OV8865", 4, 0x10, TOP_MUX_CAMTG2, "vcama1", 5, TBL(ov8865_init),
	  { { TBL(ov8865_preview), 1632, 1224, 1280 },
	    { TBL(ov8865_capture), 3264, 2448, 2550 } }, false, 17 },
};

static const struct cam_desc *c;
static const struct cam_mode *m;
static int line_bytes, stride;

/* ---- state ---- */

static struct pinctrl *pctl;
static struct i2c_adapter *adap;
static struct regulator *dovdd, *avdd, *dvdd, *afvdd;
static struct clk *mclk, *mclk_parent;
static bool mclk_on;
static void __iomem *seninf, *mipi_rx, *camsv, *cama, *uni;
static struct dentry *dbg, *dbg_frame;
static struct debugfs_blob_wrapper frame_blob;
static bool smi_on;
static int port = -1;		/* CSI port the sensor turned out to be on */

#define SMI_LARB6	6
extern s32 smi_bus_prepare_enable(const u32 id, const char *user);
extern s32 smi_bus_disable_unprepare(const u32 id, const char *user);

/* clocks, in enable order: power domains, muxes, camsys gates */
static struct {
	const char *provider;	/* compatible of the clock provider node */
	int id;
	const char *name;
	struct clk *clk;
	bool on;
} clks[] = {
	{ "mediatek,scpsys",   SCP_SYS_DIS,         "SCP_SYS_DIS" },
	{ "mediatek,scpsys",   SCP_SYS_CAM,         "SCP_SYS_CAM" },
	{ "mediatek,topckgen", TOP_MUX_CAM,         "cam_sel" },
	{ "mediatek,topckgen", TOP_MUX_SENINF,      "seninf_sel" },
	{ "mediatek,camsys",   CAMSYS_CAM_CGPDN,    "camsys_cam" },
	{ "mediatek,camsys",   CAMSYS_CAMTG_CGPDN,  "camsys_camtg" },
	{ "mediatek,camsys",   CAMSYS_SENINF_CGPDN, "camsys_seninf" },
	{ "mediatek,camsys",   CAMSYS_LARB6_CGPDN,  "camsys_larb6" },
	{ "mediatek,camsys",   CAMSYS_CAMSV0_CGPDN, "camsys_camsv0" },
	{ "mediatek,camsys",   CAMSYS_CAMSV1_CGPDN, "camsys_camsv1" },
	{ "mediatek,camsys",   CAMSYS_CAMSV2_CGPDN, "camsys_camsv2" },
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

static int clocks_on(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		clks[i].clk = provider_clk(clks[i].provider, clks[i].id);
		if (IS_ERR(clks[i].clk)) {
			pr_err("titan_camcap: clock %s: %ld\n", clks[i].name, PTR_ERR(clks[i].clk));
			clks[i].clk = NULL;
			return -ENODEV;
		}
		ret = clk_prepare_enable(clks[i].clk);
		if (ret) {
			pr_err("titan_camcap: enable %s: %d\n", clks[i].name, ret);
			return ret;
		}
		clks[i].on = true;
	}
	return 0;
}

static void clocks_off(void)
{
	int i;

	for (i = ARRAY_SIZE(clks) - 1; i >= 0; i--) {
		if (clks[i].on)
			clk_disable_unprepare(clks[i].clk);
		if (clks[i].clk)
			clk_put(clks[i].clk);
		clks[i].on = false;
		clks[i].clk = NULL;
	}
}

/* ---- sensor power and registers ---- */

static int pin(const char *fmt, ...)
{
	struct pinctrl_state *st;
	char name[32];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(name, sizeof(name), fmt, ap);
	va_end(ap);
	st = pinctrl_lookup_state(pctl, name);
	if (IS_ERR(st)) {
		pr_err("titan_camcap: no pin state %s\n", name);
		return PTR_ERR(st);
	}
	return pinctrl_select_state(pctl, st);
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

/* the stock sensor_power_sequence (docs/camera.md) */
static int sensor_power_on(void)
{
	int ret;

	ret = clk_set_parent(mclk, mclk_parent);	/* 24 MHz */
	if (!ret)
		ret = clk_prepare_enable(mclk);
	if (ret)
		return ret;
	mclk_on = true;
	pin("cam%d_mclk_4mA", cam);
	pin("cam%d_pnd0", cam);
	pin("cam%d_rst0", cam);
	mdelay(c->step_ms);
	ret = reg_on(&dovdd, "vcamio", 1800000);
	if (ret)
		return ret;
	mdelay(c->step_ms);
	ret = c->avdd ? reg_on(&avdd, c->avdd, 2800000) : pin("cam0_ldo_vcama_1");
	if (ret)
		return ret;
	mdelay(c->step_ms);
	ret = reg_on(&dvdd, "vcamd", 1200000);
	if (ret)
		return ret;
	mdelay(5);
	if (cam == 0)	/* rear: the focus motor's supply */
		reg_on(&afvdd, "vldo28", 2800000);
	pin("cam%d_pnd1", cam);
	mdelay(c->step_ms);
	pin("cam%d_rst1", cam);
	mdelay(10);
	return 0;
}

/* rear focus motor: an AKM AK7371 at i2c2 0x0c, powered by vldo28. Register
 * based: 0x00 = position[9:2], 0x01 = position[1:0] << 6, 0x02 = mode
 * (0x40 standby at power-up, 0x00 active). Larger positions focus closer;
 * ~256 is about 30 cm. Also writable from userspace through /dev/i2c-2. */
#define AF_ADDR	0x0c
static int af_write(u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = AF_ADDR, .len = 2, .buf = buf };

	return i2c_transfer(adap, &msg, 1) == 1 ? 0 : -EIO;
}

static void af_set(int pos)
{
	af_write(0x00, pos >> 2);
	af_write(0x01, (pos & 3) << 6);
}

static void af_init(void)
{
	int ret = af_write(0x02, 0x00);		/* standby -> active */

	af_set(focus);
	pr_info("titan_camcap: focus motor %s, position %d\n",
		ret ? "not answering" : "active", focus);
}

static void sensor_power_off(void)
{
	if (!c)
		return;
	pin("cam%d_rst0", cam);
	pin("cam%d_pnd0", cam);
	reg_off(&afvdd);
	reg_off(&dvdd);
	if (c->avdd)
		reg_off(&avdd);
	else
		pin("cam0_ldo_vcama_0");
	reg_off(&dovdd);
	pin("cam%d_mclk_off", cam);
	if (mclk_on)
		clk_disable_unprepare(mclk);
	mclk_on = false;
}

static int sensor_write(u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = { .addr = c->i2c_addr, .len = 3, .buf = buf };

	return i2c_transfer(adap, &msg, 1) == 1 ? 0 : -EIO;
}

static int sensor_read(u16 reg, u8 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = c->i2c_addr, .len = 2, .buf = buf },
		{ .addr = c->i2c_addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return i2c_transfer(adap, msgs, 2) == 2 ? 0 : -EIO;
}

static int sensor_table(const struct reg8 *t, int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		if (t[i].reg == REG_DELAY) {
			msleep(t[i].val);
			continue;
		}
		ret = sensor_write(t[i].reg, t[i].val);
		if (ret) {
			pr_err("titan_camcap: write %04x=%02x failed\n", t[i].reg, t[i].val);
			return ret;
		}
	}
	return 0;
}

/* exposure (1/16 line units) and gain as one group-hold update; both
 * OmniVision sensors use the same registers */
static void sensor_exposure(int lines, int g)
{
	u32 e = lines << 4;

	sensor_write(0x3208, 0x00);
	sensor_write(0x3500, e >> 16);
	sensor_write(0x3501, (e >> 8) & 0xff);
	sensor_write(0x3502, e & 0xff);
	sensor_write(0x3508, g >> 8);
	sensor_write(0x3509, g & 0xff);
	sensor_write(0x3208, 0x10);
	sensor_write(0x3208, 0xa0);
}

/* ---- seninf: CSI-2 receivers (ChromeOS mtk_seninf.c) ---- */

enum { PORT_CSI0, PORT_CSI1, PORT_CSI2, NPORTS };
static const int port_seninf[NPORTS] = { 0, 2, 4 };	/* SENINF1, 3, 5 */
static const int port_rx_off[NPORTS] = { 0x0000, 0x2000, 0x4000 };

static void rmw(void __iomem *a, u32 keep, u32 set)
{
	writel((readl(a) & keep) | set, a);
}

static void seninf_dphy(int p)
{
	void __iomem *rx = mipi_rx + port_rx_off[p];
	void __iomem *conf = seninf + 0x1000 * port_seninf[p];
	bool combo = p == PORT_CSI0;

	if (combo)
		rmw(rx + MIPI_RX_ANA00_CSI0A, 0xfffffffe, 0);
	rmw(rx + MIPI_RX_ANA00_CSI0A, 0xffffe49f, 0x1a40);	/* 4D1C */
	if (combo)
		rmw(rx + MIPI_RX_ANA00_CSI0B, 0xfffffffe, 0);
	rmw(rx + MIPI_RX_ANA00_CSI0B, 0xffffe49f, 0x1240);
	rmw(rx + MIPI_RX_ANAA8_CSI0A, 0xfffffff8, 0x7);	/* byte clock invert */
	rmw(rx + MIPI_RX_ANAA8_CSI0B, 0xfffffff8, 0x7);
	if (combo) {	/* EQ tuning */
		rmw(rx + MIPI_RX_ANA18_CSI0A, 0xffffff0f, 0x50);
		rmw(rx + MIPI_RX_ANA1C_CSI0A, 0xff0fffff, 0x500000);
		rmw(rx + MIPI_RX_ANA20_CSI0A, 0xff0fffff, 0x500000);
		rmw(rx + MIPI_RX_ANA18_CSI0B, 0xffffff0f, 0x50);
		rmw(rx + MIPI_RX_ANA1C_CSI0B, 0xff0fffff, 0x500000);
		rmw(rx + MIPI_RX_ANA20_CSI0B, 0xff0fffff, 0x500000);
	} else {	/* CSI1/2 use the CSI1 layout, relative to their base */
		rmw(rx + MIPI_RX_ANA18_CSI1A - 0x2000, 0xff0fff0f, 0x500050);
		rmw(rx + MIPI_RX_ANA1C_CSI1A - 0x2000, 0xffffff0f, 0x50);
		rmw(rx + MIPI_RX_ANA18_CSI1B - 0x2000, 0xff0fff0f, 0x500050);
		rmw(rx + MIPI_RX_ANA1C_CSI1B - 0x2000, 0xffffff0f, 0x50);
	}
	writel(0x90, mipi_rx + MIPI_RX_ANA40_CSI0A);
	rmw(rx + MIPI_RX_ANA24_CSI0A, 0xffffff, 0x40000000);
	rmw(rx + MIPI_RX_ANA24_CSI0B, 0xffffff, 0x40000000);
	rmw(rx + MIPI_RX_WRAPPER80_CSI0A, 0xfffcffff, 0);
	rmw(rx + MIPI_RX_WRAPPER80_CSI0B, 0xfffcffff, 0);
	rmw(rx + MIPI_RX_ANA00_CSI0A, 0xfffffff7, 0x8);	/* analog power on */
	rmw(rx + MIPI_RX_ANA00_CSI0B, 0xfffffff7, 0x8);
	usleep_range(20, 40);
	rmw(rx + MIPI_RX_ANA00_CSI0A, 0xfffffff7, 0x8);
	rmw(rx + MIPI_RX_ANA00_CSI0B, 0xfffffffb, 0x4);
	udelay(1);
	rmw(conf + MIPI_RX_CON24_CSI0, 0xffffff, 0xc9000000);	/* 4D1C config base */
}

static void seninf_csi(int p)
{
	void __iomem *s = seninf + 0x1000 * port_seninf[p];
	static const u32 phy_ctl[NPORTS] = {
		SENINF_TOP_PHY_SENINF_CTL_CSI0,
		SENINF_TOP_PHY_SENINF_CTL_CSI1,
		SENINF_TOP_PHY_SENINF_CTL_CSI2,
	};

	rmw(seninf + phy_ctl[p], 0x7ffff8fe, 0x80000200);
	rmw(s + SENINF1_CTRL, 0xffffffff, 0x1);		/* enable */
	rmw(s + SENINF1_CTRL, 0x8fffffff, 0 << 28);	/* 10-bit pad */
	rmw(s + SENINF1_CTRL, 0xffff0fff, 0);		/* source: CSI-2 */
	rmw(s + SENINF1_CTRL_EXT, 0xffffff9f, 0x40);
	seninf_dphy(p);
	writel(1 << 15, s + SENINF1_CSI2_DPCM);		/* no DPCM */
	rmw(s + SENINF1_CSI2_LNRD_TIMING, 0xffff00ff, 0x15 << 8);	/* settle */
	writel(readl(s + SENINF1_CSI2_CTL) | (1 << 16) | 0x10 | ((1 << LANES) - 1),
	       s + SENINF1_CSI2_CTL);
	rmw(s + SENINF1_CSI2_RESYNC_MERGE_CTL, 0xfffff3f8, 0x3);
	rmw(s + SENINF1_CSI2_MODE, 0xfffff800, 0);
	writel(0x1dff00, s + SENINF1_CSI2_DPHY_SYNC);
	rmw(s + SENINF1_CSI2_SPARE0, 0xfffffffe, 0);
	rmw(s + SENINF1_CSI2_CTL, 0xf5ffff7f, 0x2000000);
	rmw(s + SENINF1_CSI2_HS_TRAIL, 0xffffff00, 0x8);
	writel(0x8000001a, s + SENINF1_CSI2_DGB_SEL);	/* debug port: packet count */
	writel(0xffffffff, s + SENINF1_CSI2_INT_EN);
	writel(0xffffffff, s + SENINF1_CSI2_INT_STATUS);
	writel(0x1f, s + SENINF1_CSI2_INT_EN_EXT);
	rmw(s + SENINF1_CTRL, 0xffffff7f, 0x80);		/* reset pulse */
	udelay(1);
	rmw(s + SENINF1_CTRL, 0xffffff7f, 0);
}

/* seninf mux 0 <- the port's seninf; RAW10, one pixel per clock; every
 * CAM/CAMSV input takes mux 0 */
static void seninf_mux(int p)
{
	void __iomem *mx = seninf;	/* mux 0 lives in the SENINF1 block */

	rmw(mx + SENINF1_MUX_CTRL, 0x7fffffff, 0x80000000);
	rmw(mx + SENINF1_MUX_CTRL, 0xffff0fff, 0x8000);	/* source: MIPI sensor */
	rmw(mx + SENINF1_MUX_CTRL_EXT, 0xfffffffc, 0x1);
	rmw(mx + SENINF1_MUX_CTRL_EXT, 0xffffffef, 0);
	rmw(mx + SENINF1_MUX_CTRL, 0xfffffeff, 0);
	rmw(mx + SENINF1_MUX_CTRL, 0xcfffffff, 0x20000000);
	rmw(mx + SENINF1_MUX_CTRL, 0xf000ffff, 0x6df0000);
	rmw(mx + SENINF1_MUX_CTRL, 0xfffff9ff, 0);	/* hsync/vsync polarity */
	writel((readl(mx + SENINF1_MUX_CTRL) | 0x3) & 0xfffffffc, mx + SENINF1_MUX_CTRL);
	rmw(seninf + SENINF_TOP_MUX_CTRL, ~0xfU, port_seninf[p]);
	writel(0, seninf + SENINF_TOP_CAM_MUX_CTRL);
}

static void seninf_setup(void)
{
	int p;

	rmw(seninf + SENINF1_CTRL, 0xffffffff, 0x1);
	rmw(seninf + SENINF1_CTRL_EXT, 0xffffffbf, 0x40);
	writel(0x67, seninf + SENINF_TG1_TM_STP);
	for (p = 0; p < NPORTS; p++)
		seninf_csi(p);
}

/* ---- the frame buffer: scattered pages, one M4U mapping ---- */

/* BSP m4u 2.4 API (not exported to modules: found through kallsyms) */
#define M4U_PORT_CAM_SOCO	72	/* larb6 port 11: CAMSV output */
#define M4U_PORT_CAM_IMGO	61	/* larb6 port 0: CAM IMGO */
#define M4U_PORT_CAM_RRZO	62	/* larb6 port 1: CAM RRZO */
#define M4U_PORT_CAM_AAO	63	/* larb6 port 2: CAM AAO */
#define M4U_PORT_CAM_AFO	64	/* larb6 port 3: CAM AFO */
#define buf_port()		(p1 ? M4U_PORT_CAM_IMGO : M4U_PORT_CAM_SOCO)

/* AF statistics: 16 x 12 blocks of 40 bytes (tools/camera/af_regs.py) */
#define AF_NX		16
#define AF_NY		12
#define AFO_LINE	(AF_NX * 40)
#define AFO_STRIDE	ALIGN(AFO_LINE + 1, 16)
#define M4U_PROT_READ		(1 << 0)
#define M4U_PROT_WRITE		(1 << 1)

static struct {
	struct page **pages;
	int npages;
	void *vaddr;		/* uncached CPU view */
	size_t size;
	struct sg_table sgt;
	unsigned int mva;	/* the device address */
} frame, header, afo, rrzo, aao;
static struct dentry *dbg_afo, *dbg_focus;
static struct debugfs_blob_wrapper afo_blob, rrzo_blob, aao_blob;
static struct dentry *dbg_rrzo, *dbg_aao;
/* AAO with only the AWB window set up (AE registers 0): per block row, nx x
 * {R, G, B, 0x0f} (8-bit block averages) then nx AE bytes (8-bit luma). The
 * HAL's AE configuration adds more per block (22 bytes) and histograms */
#define AAO_BLOCK	5
#define AAO_BYTES	(p1aanx * p1aany * AAO_BLOCK)

static int buf_m4u_port(void *b)
{
	return b == &afo ? M4U_PORT_CAM_AFO : b == &rrzo ? M4U_PORT_CAM_RRZO :
	       b == &aao ? M4U_PORT_CAM_AAO : buf_port();
}
static void *m4u_client;

static void *m4u_fn(const char *name)
{
	void *fn = (void *)kallsyms_lookup_name(name);

	if (!fn)
		pr_err("titan_camcap: %s not found\n", name);
	return fn;
}

static int buf_alloc(typeof(frame) *b, size_t size)
{
	int (*alloc_mva)(void *client, int port, unsigned long va, struct sg_table *sgt,
			 unsigned int size, unsigned int prot, unsigned int flags,
			 unsigned int *mva) = m4u_fn("m4u_alloc_mva");
	int i, ret;

	if (!alloc_mva)
		return -ENOENT;
	b->size = PAGE_ALIGN(size);
	b->npages = b->size >> PAGE_SHIFT;
	b->pages = kvmalloc_array(b->npages, sizeof(*b->pages), GFP_KERNEL | __GFP_ZERO);
	if (!b->pages)
		return -ENOMEM;
	for (i = 0; i < b->npages; i++) {
		b->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!b->pages[i])
			return -ENOMEM;
	}
	/* uncached, so the CPU sees what the DMA wrote without cache work */
	b->vaddr = vmap(b->pages, b->npages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!b->vaddr)
		return -ENOMEM;
	memset(b->vaddr, 0x5a, b->size);	/* so untouched memory is obvious */
	ret = sg_alloc_table_from_pages(&b->sgt, b->pages, b->npages, 0, b->size, GFP_KERNEL);
	if (ret)
		return ret;
	ret = alloc_mva(m4u_client, buf_m4u_port(b), 0, &b->sgt, b->size,
			M4U_PROT_READ | M4U_PROT_WRITE, 0, &b->mva);
	if (ret)
		pr_err("titan_camcap: m4u_alloc_mva: %d\n", ret);
	return ret;
}

static void buf_free(typeof(frame) *b)
{
	int (*dealloc_mva)(void *client, int port, unsigned int mva) =
		m4u_fn("m4u_dealloc_mva");
	int i;

	if (b->mva && dealloc_mva)
		dealloc_mva(m4u_client, buf_m4u_port(b), b->mva);
	b->mva = 0;
	if (b->sgt.sgl)
		sg_free_table(&b->sgt);
	b->sgt.sgl = NULL;
	if (b->vaddr)
		vunmap(b->vaddr);
	b->vaddr = NULL;
	if (b->pages) {
		for (i = 0; i < b->npages; i++)
			if (b->pages[i])
				__free_page(b->pages[i]);
		kvfree(b->pages);
	}
	b->pages = NULL;
}

static int buffers_alloc(void)
{
	void *(*create_client)(void) = m4u_fn("m4u_create_client");
	int ret;

	if (!create_client)
		return -ENOENT;
	m4u_client = create_client();
	if (IS_ERR_OR_NULL(m4u_client)) {
		m4u_client = NULL;
		return -ENOMEM;
	}
	ret = buf_alloc(&frame, stride * m->height);
	if (!ret)
		ret = buf_alloc(&header, PAGE_SIZE);
	if (!ret && p1af)
		ret = buf_alloc(&afo, AFO_STRIDE * AF_NY);
	if (!ret && p1rrz)
		ret = buf_alloc(&rrzo, stride * m->height);
	if (!ret && p1aa)
		ret = buf_alloc(&aao, AAO_BYTES);
	if (ret)
		return ret;
	frame_blob.data = frame.vaddr;
	frame_blob.size = stride * m->height;
	pr_info("titan_camcap: %zu KB frame buffer at mva %#x\n", frame.size >> 10, frame.mva);
	return 0;
}

static void buffers_free(void)
{
	int (*destroy_client)(void *client) = m4u_fn("m4u_destroy_client");

	buf_free(&frame);
	buf_free(&header);
	buf_free(&afo);
	buf_free(&rrzo);
	buf_free(&aao);
	if (m4u_client && destroy_client)
		destroy_client(m4u_client);
	m4u_client = NULL;
}

/* ---- CAMSV (sequence from the HAL's isp_function_camsv.cpp) ---- */

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
#define SV_FBC_CTL2	0x114
#define SV_TG_SEN_MODE	0x230
#define SV_TG_VF_CON	0x234
#define SV_TG_GRAB_PXL	0x238
#define SV_TG_GRAB_LIN	0x23c
#define SV_TG_SIZE	0x268	/* measured frame: lines << 16 | pixels */
#define SV_TG_INTER_ST	0x26c
#define SV_DMA_EN	0x400
#define SV_FH_BASE	0x404
#define SV_MODULE_EN	0x510
#define SV_FMT_SEL	0x514
#define SV_INT_EN	0x518
#define SV_INT_STATUS	0x51c
#define SV_SW_CTL	0x520
#define SV_CLK_EN	0x530
#define SV_PAK		0x54c

static void sv_rmw(u32 reg, u32 keep, u32 set)
{
	writel((readl(camsv + reg) & keep) | set, camsv + reg);
}

static void camsv_start(void)
{
	int i;

	/* reset: 4, 0, 1, wait for 3, 0 */
	writel(4, camsv + SV_SW_CTL);
	writel(0, camsv + SV_SW_CTL);
	writel(1, camsv + SV_SW_CTL);
	for (i = 0; i < 10000 && readl(camsv + SV_SW_CTL) != 3; i++)
		udelay(1);
	writel(0, camsv + SV_SW_CTL);

	/* top: TG on, double buffer, RAW10 packed, one pixel per clock */
	sv_rmw(SV_MODULE_EN, ~0x3f20U, 0x40000001);
	writel(1, camsv + SV_FMT_SEL);		/* RAW10 */
	writel(0x1304b1, camsv + SV_INT_EN);
	if (unpak == 1)
		sv_rmw(SV_MODULE_EN, ~0xcU, 0);		/* packer off */
	else if (unpak == 2)
		sv_rmw(SV_MODULE_EN, ~0xcU, 0x8);
	else
		sv_rmw(SV_MODULE_EN, ~0x8U, 0x4);	/* packer on */
	sv_rmw(SV_PAK, ~0x37U, 0x1);			/* 10 bit, 1 pixel mode */
	writel(0, camsv + SV_DMA_EN);
	sv_rmw(0x3b0, 0x7fffffff, 0);
	writel(0xffffffff, camsv + 0x3c4);
	sv_rmw(0x018, ~0U, 0x61000000);

	/* TG: whole frame, continuous. Single-frame mode with FBC reports a
	 * finished frame but writes nothing (the first frame seems to use the
	 * shadowed, not yet latched, address); continuous without FBC works. */
	sv_rmw(SV_TG_SEN_MODE, ~0x60003U, 0x10000);
	sv_rmw(SV_TG_VF_CON, ~0x2U, 0);
	writel(m->width << 16, camsv + SV_TG_GRAB_PXL);
	writel(m->height << 16, camsv + SV_TG_GRAB_LIN);

	/* IMGO DMA, overwriting the one buffer every frame */
	writel(line_bytes - 1, camsv + SV_IMGO_XSIZE);
	writel(m->height - 1, camsv + SV_IMGO_YSIZE);
	writel(stride | 0x1010000, camsv + SV_IMGO_STRIDE);	/* 16-bit bus */
	writel(0, camsv + SV_IMGO_CROP);
	writel(0x80000080, camsv + SV_IMGO_CON);
	writel(0x20002, camsv + SV_IMGO_CON2);
	writel(0x20002, camsv + SV_IMGO_CON3);
	writel(0, camsv + SV_IMGO_OFST);
	writel(frame.mva, camsv + SV_IMGO_BASE);
	writel(0, camsv + SV_FBC_CTL1);
	writel(header.mva, camsv + SV_FH_BASE);

	/* go */
	sv_rmw(SV_MODULE_EN, ~0U, 0x10);	/* IMGO on */
	sv_rmw(SV_DMA_EN, ~0U, 1);
	sv_rmw(SV_TG_SEN_MODE, ~0U, 1);	/* CMOS_EN */
	sv_rmw(SV_CLK_EN, ~0U, 0x8005);
	sv_rmw(SV_TG_VF_CON, ~0U, 1);		/* VFDATA_EN */
}

static void camsv_stop(void)
{
	if (!camsv)
		return;
	sv_rmw(SV_TG_VF_CON, ~1U, 0);
	sv_rmw(SV_TG_SEN_MODE, ~1U, 0);
	sv_rmw(SV_MODULE_EN, ~0x10U, 0);
	writel(0, camsv + SV_DMA_EN);
	msleep(100);	/* let a frame in flight finish before the memory goes */
}

/* ---- ISP pass 1, CAM A (docs/camera.md, "MediaTek ISP") ----
 *
 * From the HAL's CAM_TOP_CTRL / CAM_TG_CTRL / DMAO_B / Cam_path_sel and the
 * BSP kernel driver; register names: ../isp/cam_p1_regs.h. The layout is
 * CAMSV's with the blocks moved: control at 0x000 (CAMSV 0x500), TG at
 * 0x230 (same), DMA at 0x1000 (CAMSV 0x000).
 */
#define UNI_BASE	0x1a003000	/* CAM_UNI, shared by CAM A/B/C */
#define UNI_TOP_MOD_EN	0x010
#define UNI_EN_RLB	BIT(29)		/* RRZ line buffer (HAL UNI_TOP_CTRL::_config) */
#define P1_BASE		0x1a004000	/* CAM A */
#define P1_SIZE		0x2000
#define P1_CTL_EN	0x004
#define P1_CTL_DMA_EN	0x008
#define P1_CTL_FMT_SEL	0x00c
#define P1_CTL_SEL	0x010
#define P1_CTL_EN2	0x018
#define P1_INT_EN	0x020
#define P1_INT_STAT	0x024
#define P1_INT2_STAT	0x034
#define P1_SW_CTL	0x040
#define P1_DB_EN	0x088		/* HAL writes 1 last (double buffering?) */
#define P1_FBC_IMGO	0x110		/* assumed: CAMSV's FBC_CTL1 offset */
#define P1_DMX_CTL	0x330
#define P1_DMX_CROP	0x334
#define P1_DMX_VSIZE	0x338
#define P1_TG_SEN_MODE	0x230
#define P1_TG_VF_CON	0x234
#define P1_TG_GRAB_PXL	0x238
#define P1_TG_GRAB_LIN	0x23c
#define P1_TG_SIZE	0x268
#define P1_TG_INTER_ST	0x26c
#define P1_IMGO_BASE	0x1020
#define P1_IMGO_OFST	0x1028
#define P1_IMGO_XSIZE	0x1030
#define P1_IMGO_YSIZE	0x1034
#define P1_IMGO_STRIDE	0x1038
#define P1_IMGO_CON	0x103c
#define P1_IMGO_CON2	0x1040
#define P1_IMGO_CON3	0x1044
#define P1_IMGO_CROP	0x1048
#define P1_IMGO_ERR	0x1360		/* ChromeOS REG_IMGO_ERR_STAT */
#define P1_AFO_BASE	0x10b0
#define P1_AFO_OFST	0x10b8
#define P1_AFO_XSIZE	0x10c0
#define P1_AFO_YSIZE	0x10c4
#define P1_AFO_STRIDE	0x10c8
#define P1_AFO_CON	0x10cc
#define P1_AFO_CON2	0x10d0
#define P1_AFO_CON3	0x10d4
#define P1_AFO_ERR	0x136c		/* ChromeOS REG_AFO_ERR_STAT */
#define P1_OBC_OFFST(i)	(0x3f0 + 4 * (i))	/* 13-bit two's complement, 12-bit scale */
#define P1_OBC_GAIN(i)	(0x400 + 4 * (i))	/* 13 bit, 512 = 1x */
#define P1_RPG_SATU_1	0x4c0		/* clip B | Gb << 16, 12 bit */
#define P1_RPG_SATU_2	0x4c4		/* clip Gr | R << 16 */
#define P1_RPG_GAIN_1	0x4c8		/* B | Gb << 16, 13 bit, 512 = 1x */
#define P1_RPG_GAIN_2	0x4cc		/* Gr | R << 16 */
#define P1_RPG_OFST_1	0x4d0
#define P1_RPG_OFST_2	0x4d4
#define P1_EN_PAK	BIT(15)		/* IMGO packer: without it the processed path writes nothing */
#define P1_EN_OBC	BIT(5)
#define P1_EN_RRZ	BIT(10)
#define P1_EN_AA	BIT(19)		/* tuning function 13 */
#define P1_DMA_AAO	BIT(5)
#define P1_AWB_WIN_ORG	0x830		/* x | y << 16 */
#define P1_AWB_WIN_SIZE	0x834		/* block w | h << 16 */
#define P1_AWB_WIN_PIT	0x838
#define P1_AWB_WIN_NUM	0x83c
#define P1_AWB_GAIN1_0	0x840		/* pre-gain R | G << 16, 512 = 1x */
#define P1_AWB_GAIN1_1	0x844		/* B */
#define P1_AWB_LMT1_0	0x848		/* 12-bit limits after the pre-gain */
#define P1_AWB_LMT1_1	0x84c
#define P1_AWB_LOW_THR	0x850		/* 8-bit R | G << 8 | B << 16 */
#define P1_AWB_HI_THR	0x854
#define P1_AWB_PIXEL_CNT0 0x858		/* R, 24 bit */
#define P1_AWB_PIXEL_CNT1 0x85c		/* G */
#define P1_AWB_PIXEL_CNT2 0x860		/* B */
#define P1_AAO_BASE	0x1080
#define P1_AAO_OFST	0x1088
#define P1_AAO_XSIZE	0x1090
#define P1_AAO_YSIZE	0x1094
#define P1_AAO_STRIDE	0x1098
#define P1_AAO_CON	0x109c
#define P1_AAO_CON2	0x10a0
#define P1_AAO_CON3	0x10a4
#define P1_AAO_ERR	0x1368
#define P1_EN_RMX	BIT(11)
#define P1_RRZ_CTL	0x4e0
#define P1_RRZ_IN_IMG	0x4e4
#define P1_RRZ_OUT_IMG	0x4e8
#define P1_RRZ_HORI_STEP 0x4ec
#define P1_RRZ_VERT_STEP 0x4f0
#define P1_RRZ_MODE_TH	0x504
#define P1_RRZ_MODE_CTL	0x508
#define P1_RRZ_NNIR	0x514
#define P1_RMX_CTL	0x540
#define P1_RMX_CROP	0x544
#define P1_RMX_VSIZE	0x548
#define P1_RRZO_BASE	0x1050
#define P1_RRZO_OFST	0x1058
#define P1_RRZO_XSIZE	0x1060
#define P1_RRZO_YSIZE	0x1064
#define P1_RRZO_STRIDE	0x1068
#define P1_RRZO_CON	0x106c
#define P1_RRZO_CON2	0x1070
#define P1_RRZO_CON3	0x1074
#define P1_RRZO_CROP	0x1078
#define P1_RRZO_ERR	0x1364		/* assumed: next to IMGO's */		/* tuning function 2 */
#define P1_EN_RPG	BIT(9)		/* function 9 */
#define P1_EN_AF	BIT(17)		/* tuning function 15 (TuningMgrImp::enableEngine) */
#define P1_EN_SGG1	BIT(18)		/* function 14: the AF input gamma */
#define P1_EN2_SGG5	BIT(31)		/* function 22 */
#define P1_DMA_AFO	BIT(3)

static void p1_rmw(u32 reg, u32 keep, u32 set)
{
	writel((readl(cama + reg) & keep) | set, cama + reg);
}

static void p1_start(void)
{
	int i;

	/* reset: 1, wait for 2, 4, 0 */
	writel(1, cama + P1_SW_CTL);
	for (i = 0; i < 10000 && readl(cama + P1_SW_CTL) != 2; i++)
		udelay(1);
	writel(4, cama + P1_SW_CTL);
	writel(0, cama + P1_SW_CTL);
	pr_info("titan_camcap: cam A reset %s\n", i < 10000 ? "done" : "timed out");

	/* format: TG RAW10 (bits 14:12), IMGO format (8:4), RRZO 10 bit (3:2),
	 * one pixel per clock (21:20) */
	writel(1 << 12 | (p1fmt & 0x1f) << 4 | 1 << 2, cama + P1_CTL_FMT_SEL);
	/* CTL_SEL (HAL Cam_path_sel::Path_sel_RAW): 0x060a5010 = pure raw (IMG_SEL 2,
	 * bit 19 bypasses the raw blocks), 0x07025010 = IMGO after LSC (IMG_SEL 3) */
	writel((p1sel && !p1proc ? 0x060a5010 : 0x07025010) | p1selor, cama + P1_CTL_SEL);
	if (p1img >= 0)
		p1_rmw(P1_CTL_SEL, ~0x03000000U, (p1img & 3) << 24);
	writel(p1en | (p1proc ? P1_EN_PAK : 0), cama + P1_CTL_EN);
	writel(p1en2, cama + P1_CTL_EN2);
	/* DMX (HAL CAM_DMX_CTRL): bits 19:16 set, bit 31 clear; the whole width
	 * and height */
	/* DMX: bits 15:0 are its SRAM size (HAL capibility::GetXMXSramSize, 0x1b0).
	 * Left 0, the processed path drops the last 640 of every 4096 pixels */
	p1_rmw(P1_DMX_CTL, 0x7ff00000, 0xf0000 | ((p1dmx ?: 0x1b0) & 0xffff));
	writel((m->width - 1) << 16, cama + P1_DMX_CROP);
	writel(m->height, cama + P1_DMX_VSIZE);
	writel(0, cama + P1_INT_EN);		/* the BSP driver owns the IRQ */
	p1_rmw(0x13b0, 0x7fffffff, 0);
	writel(0xffffffff, cama + 0x13c4);
	writel(0, cama + 0x1400);

	/* TG: whole frame, continuous */
	p1_rmw(P1_TG_SEN_MODE, ~0x70003U, 0x20000);
	p1_rmw(P1_TG_VF_CON, ~0x2U, 0);
	writel(m->width << 16, cama + P1_TG_GRAB_PXL);
	writel(m->height << 16, cama + P1_TG_GRAB_LIN);
	p1_rmw(0x25c, ~0U, 0x20);
	p1_rmw(0x240, ~0U, 0x8000);

	/* IMGO: FIFO settings from the BSP's ISP_ConfigDMAControl() */
	writel(line_bytes - 1, cama + P1_IMGO_XSIZE);
	writel(m->height - 1, cama + P1_IMGO_YSIZE);
	writel(stride | 0x1010000, cama + P1_IMGO_STRIDE);
	writel(0, cama + P1_IMGO_CROP);
	writel(0x80000180, cama + P1_IMGO_CON);
	writel(0x00400040, cama + P1_IMGO_CON2);
	writel(0x00200020, cama + P1_IMGO_CON3);
	writel(0, cama + P1_IMGO_OFST);
	writel(frame.mva, cama + P1_IMGO_BASE);
	writel(0, cama + P1_FBC_IMGO);
	writel(1, cama + P1_DB_EN);

	if (p1obc) {
		/* black level: offset -4 * black on the 12-bit scale, then a gain
		 * that stretches the rest back to full range (as the stock
		 * tuning tables do: OV16880 0x1f00 / 0x222, OV8865 0x1fbc / 0x209) */
		int b = 4 * c->black;

		for (i = 0; i < 4; i++) {
			writel(-b & 0x1fff, cama + P1_OBC_OFFST(i));
			writel(512 * 4095 / (4095 - b), cama + P1_OBC_GAIN(i));
		}
		p1_rmw(P1_CTL_EN, ~0U, P1_EN_OBC);
	}

	if (p1wb_n == 3) {
		/* white balance (HAL ISP_MGR_RPG::setIspPregain) */
		writel(0x0fff0fff, cama + P1_RPG_SATU_1);
		writel(0x0fff0fff, cama + P1_RPG_SATU_2);
		writel((p1wb[2] & 0x1fff) | (p1wb[1] & 0x1fff) << 16, cama + P1_RPG_GAIN_1);
		writel((p1wb[1] & 0x1fff) | (p1wb[0] & 0x1fff) << 16, cama + P1_RPG_GAIN_2);
		writel(0, cama + P1_RPG_OFST_1);
		writel(0, cama + P1_RPG_OFST_2);
		p1_rmw(P1_CTL_EN, ~0U, P1_EN_RPG);
	}

	if (p1af) {
		/* AF statistics (HAL ISP_MGR_AF_STAT_CONFIG with the stock default
		 * AF config) and their DMA, AFO */
		int i, n = cam ? ARRAY_SIZE(af_regs_1632x1224) : ARRAY_SIZE(af_regs_2336x1752);

		for (i = 0; i < n; i++) {
			if (cam)
				writel(af_regs_1632x1224[i].val, cama + af_regs_1632x1224[i].off);
			else
				writel(af_regs_2336x1752[i].val, cama + af_regs_2336x1752[i].off);
		}
		p1_rmw(P1_CTL_EN, ~0U, P1_EN_AF | P1_EN_SGG1);
		p1_rmw(P1_CTL_EN2, ~0U, P1_EN2_SGG5);
		writel(AFO_LINE - !!p1afm1, cama + P1_AFO_XSIZE);
		writel(AF_NY - !!p1afm1, cama + P1_AFO_YSIZE);
		writel(AFO_STRIDE, cama + P1_AFO_STRIDE);
		writel(0x80000100, cama + P1_AFO_CON);
		writel(0x00800080, cama + P1_AFO_CON2);
		writel(0x00550055, cama + P1_AFO_CON3);
		writel(0, cama + P1_AFO_OFST);
		writel(afo.mva, cama + P1_AFO_BASE);
		p1_rmw(P1_CTL_DMA_EN, ~0U, P1_DMA_AFO);
	}

	if (p1aa) {
		/* AE/AWB statistics (HAL ISP_MGR_AWB_STAT_CONFIG::config): equal
		 * blocks over the frame, unity pre-gain, thresholds wide open; the
		 * AE registers (0x900...) stay 0: no histograms */
		int bw = (m->width / p1aanx) & ~1, bh = (m->height / p1aany) & ~1;

		writel(0, cama + P1_AWB_WIN_ORG);
		writel(bw | bh << 16, cama + P1_AWB_WIN_SIZE);
		writel(bw | bh << 16, cama + P1_AWB_WIN_PIT);
		writel(p1aanx | p1aany << 16, cama + P1_AWB_WIN_NUM);
		writel(0x200 | 0x200 << 16, cama + P1_AWB_GAIN1_0);
		writel(0x200, cama + P1_AWB_GAIN1_1);
		writel(0xfff | 0xfff << 16, cama + P1_AWB_LMT1_0);
		writel(0xfff, cama + P1_AWB_LMT1_1);
		writel(0, cama + P1_AWB_LOW_THR);
		writel(0xffffff, cama + P1_AWB_HI_THR);
		/* averages = sums * PIXEL_CNT >> 24: 1 / pixels of that colour per block */
		writel((1 << 24) / (bw * bh / 4), cama + P1_AWB_PIXEL_CNT0);
		writel((1 << 24) / (bw * bh / 2), cama + P1_AWB_PIXEL_CNT1);
		writel((1 << 24) / (bw * bh / 4), cama + P1_AWB_PIXEL_CNT2);
		/* the HAL's fixed values for 0x8c4..0x8d4 */
		writel(0x200, cama + 0x8c4);
		writel(0x200, cama + 0x8cc);
		writel(0x90200, cama + 0x8d4);
		/* the copy at 0xbd0 */
		writel(0, cama + 0xbd0);
		writel(bw | bh << 16, cama + 0xbd4);
		writel(bw | bh << 16, cama + 0xbd8);
		writel(p1aanx | p1aany << 16, cama + 0xbdc);
		for (i = 0; i < 4; i++)
			writel((1 << 24) / (bw * bh / 4), cama + 0xbe0 + 4 * i);
		p1_rmw(P1_CTL_EN, ~0U, P1_EN_AA);
		p1_rmw(P1_CTL_EN2, ~0U, p1aaen2);
		writel(AAO_BYTES - 1, cama + P1_AAO_XSIZE);
		writel(0, cama + P1_AAO_YSIZE);
		writel(AAO_BYTES, cama + P1_AAO_STRIDE);
		writel(0x80000080, cama + P1_AAO_CON);
		writel(0x00200020, cama + P1_AAO_CON2);
		writel(0x00100010, cama + P1_AAO_CON3);
		writel(0, cama + P1_AAO_OFST);
		writel(aao.mva, cama + P1_AAO_BASE);
		p1_rmw(P1_CTL_DMA_EN, ~0U, P1_DMA_AAO);
	}

	if (p1rrz) {
		/* resizer at 1:1 (HAL CAM_RRZ_CTRL::setDefault/_config): coefficient
		 * table 0x1f for both directions, step (in - 1) * 32768 / (out - 1),
		 * and its mux RMX (SRAM size as DMX's) cropped to the output */
		/* the resizer's line buffer lives in CAM_UNI: without it RRZ stalls
		 * the whole pipe (PIPE_CHECK::RRZ_CHECK "can't enable rrz without RLB") */
		writel(readl(uni + UNI_TOP_MOD_EN) | UNI_EN_RLB, uni + UNI_TOP_MOD_EN);
		writel(readl(uni + 0x20) | 1, uni + 0x20);	/* HAL UNI_TOP_CTRL: 0x20 |= 1 */
		writel(0x1f << 24 | 0x1f << 16 | 0x80 | 3, cama + P1_RRZ_CTL);
		writel(m->width | m->height << 16, cama + P1_RRZ_IN_IMG);
		writel(m->width | m->height << 16, cama + P1_RRZ_OUT_IMG);
		writel(0x8000, cama + P1_RRZ_HORI_STEP);
		writel(0x8000, cama + P1_RRZ_VERT_STEP);
		writel(0x00502850, cama + P1_RRZ_MODE_TH);
		p1_rmw(P1_RRZ_MODE_CTL, ~0x1ffffU, 0x10000 | 0x10 << 11);
		writel(0x1f1f, cama + P1_RRZ_NNIR);
		writel(0x1f0000 | 0x1b0, cama + P1_RMX_CTL);
		writel((m->width - 1) << 16, cama + P1_RMX_CROP);
		writel(m->height, cama + P1_RMX_VSIZE);
		p1_rmw(P1_CTL_EN, ~0U, P1_EN_RRZ | P1_EN_RMX);
		writel(line_bytes - 1, cama + P1_RRZO_XSIZE);
		writel(m->height - 1, cama + P1_RRZO_YSIZE);
		writel(stride, cama + P1_RRZO_STRIDE);
		writel(0, cama + P1_RRZO_CROP);
		writel(0x80000180, cama + P1_RRZO_CON);
		writel(0x00800080, cama + P1_RRZO_CON2);
		writel(0x00400040, cama + P1_RRZO_CON3);
		writel(0, cama + P1_RRZO_OFST);
		writel(rrzo.mva, cama + P1_RRZO_BASE);
		p1_rmw(P1_CTL_DMA_EN, ~0U, BIT(p1rrzdma));
	}

	for (i = 0; i + 1 < p1regs_n; i += 2)
		writel(p1regs[i + 1], cama + (p1regs[i] & 0x1ffc));

	/* go */
	p1_rmw(P1_CTL_DMA_EN, ~0U, 1);		/* IMGO */
	p1_rmw(P1_TG_SEN_MODE, ~0U, 1);		/* CMOS_EN */
	p1_rmw(P1_TG_VF_CON, ~0U, 1);		/* VFDATA_EN */
}

static void p1_stop(void)
{
	if (!cama)
		return;
	p1_rmw(P1_TG_VF_CON, ~1U, 0);
	p1_rmw(P1_TG_SEN_MODE, ~1U, 0);
	writel(0, cama + P1_CTL_DMA_EN);
	msleep(100);
}

/* ---- debugfs ---- */

static int status_show(struct seq_file *s, void *v)
{
	int p;

	seq_printf(s, "%s %dx%d on CSI%d, camsv%d\n", c->name, m->width, m->height, port, sv);
	for (p = 0; p < NPORTS; p++) {
		void __iomem *b = seninf + 0x1000 * port_seninf[p];

		seq_printf(s, "CSI%d (seninf%d): packets %08x  int %08x\n", p, port_seninf[p] + 1,
			   readl(b + SENINF1_CSI2_DBG_PORT), readl(b + SENINF1_CSI2_INT_STATUS));
	}
	seq_printf(s, "mux0: ctrl %08x  intsta %08x  frame %u x %u\n",
		   readl(seninf + SENINF1_MUX_CTRL), readl(seninf + SENINF1_MUX_INTSTA),
		   readl(seninf + SENINF1_MUX_DEBUG_2) >> 16,
		   readl(seninf + SENINF1_MUX_DEBUG_2) & 0xffff);
	if (cama) {
		u32 sz = readl(cama + P1_TG_SIZE);

		seq_printf(s, "cam A: int %08x int2 %08x  frames %u  measured %u x %u\n"
			   "  ctl_en %08x dma_en %08x fmt %08x sel %08x  imgo err %08x\n",
			   readl(cama + P1_INT_STAT), readl(cama + P1_INT2_STAT),
			   readl(cama + P1_TG_INTER_ST) >> 24, sz >> 16, sz & 0xffff,
			   readl(cama + P1_CTL_EN), readl(cama + P1_CTL_DMA_EN),
			   readl(cama + P1_CTL_FMT_SEL), readl(cama + P1_CTL_SEL),
			   readl(cama + P1_IMGO_ERR));
		if (afo.vaddr) {
			const u32 *w = afo.vaddr;

			seq_printf(s, "  afo err %08x  block(0,0): %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				   readl(cama + P1_AFO_ERR), w[0], w[1], w[2], w[3], w[4], w[5],
				   w[6], w[7], w[8], w[9]);
		}
		if (aao.vaddr) {
			const u8 *b = aao.vaddr;

			seq_printf(s, "  aao err %08x  block 0: %*ph  ae %02x\n",
				   readl(cama + P1_AAO_ERR), 4, b, b[4 * p1aanx]);
		}
		if (rrzo.vaddr)
			seq_printf(s, "  rrzo err? %08x  rrz ctl %08x\n", readl(cama + P1_RRZO_ERR),
				   readl(cama + P1_RRZ_CTL));
		seq_printf(s, "  dmx %08x %08x %08x  en2 %08x\n", readl(cama + P1_DMX_CTL),
			   readl(cama + P1_DMX_CROP), readl(cama + P1_DMX_VSIZE),
			   readl(cama + P1_CTL_EN2));
	}
	if (camsv) {
		u32 sz = readl(camsv + SV_TG_SIZE);

		seq_printf(s, "camsv%d: int %08x  frames %u  measured %u x %u  module %08x\n", sv,
			   readl(camsv + SV_INT_STATUS), readl(camsv + SV_TG_INTER_ST) >> 24,
			   sz >> 16, sz & 0xffff, readl(camsv + SV_MODULE_EN));
	}
	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, NULL);
}

static const struct file_operations status_fops = {
	.open = status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* lens position at run time: echo 384 > /sys/kernel/debug/titan_camcap_focus */
static int focus_get(void *data, u64 *val)
{
	*val = focus;
	return 0;
}

static int focus_set(void *data, u64 val)
{
	if (val > 1023)
		return -EINVAL;
	focus = val;
	af_set(focus);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(focus_fops, focus_get, focus_set, "%llu\n");

/* ---- module ---- */

static void teardown(void)
{
	debugfs_remove(dbg);
	debugfs_remove(dbg_frame);
	debugfs_remove(dbg_afo);
	debugfs_remove(dbg_rrzo);
	debugfs_remove(dbg_aao);
	dbg_rrzo = dbg_aao = NULL;
	debugfs_remove(dbg_focus);
	dbg = dbg_frame = dbg_afo = dbg_focus = NULL;
	camsv_stop();
	p1_stop();
	if (camsv)
		iounmap(camsv);
	if (cama)
		iounmap(cama);
	if (uni)
		iounmap(uni);
	camsv = cama = uni = NULL;
	if (adap) {
		sensor_write(0x0100, 0);	/* stream off */
		i2c_put_adapter(adap);
		adap = NULL;
	}
	buffers_free();
	if (smi_on)
		smi_bus_disable_unprepare(SMI_LARB6, "titan_camcap");
	smi_on = false;
	sensor_power_off();
	if (seninf)
		iounmap(seninf);
	if (mipi_rx)
		iounmap(mipi_rx);
	seninf = mipi_rx = NULL;
	clocks_off();
	if (!IS_ERR_OR_NULL(mclk))
		clk_put(mclk);
	if (!IS_ERR_OR_NULL(mclk_parent))
		clk_put(mclk_parent);
	if (!IS_ERR_OR_NULL(pctl))
		pinctrl_put(pctl);
}

static int __init camcap_init(void)
{
	struct platform_device *pdev;
	struct device_node *np;
	u8 id_hi = 0, id_lo = 0;
	int ret, p;

	if (cam < 0 || cam > 1 || mode < 0 || mode > 1 || sv < 0 || sv > 5)
		return -EINVAL;
	c = &cams[cam];
	m = &c->modes[mode];
	if (p1)
		/* IMGO formats: 8/9/10/11 packed RAW8/10/12/14, 0 16-bit */
		line_bytes = p1fmt >= 8 && p1fmt <= 11 ? m->width * (8 + 2 * (p1fmt - 8)) / 8
						      : m->width * 2;
	else
		line_bytes = unpak ? m->width * 2 : m->width * 10 / 8;	/* 16-bit or packed RAW10 */
	stride = ALIGN(line_bytes, 16);
	if (exposure < 0)
		exposure = m->vts - 64;

	np = of_find_compatible_node(NULL, NULL, "mediatek,imgsensor");
	pdev = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	pctl = pinctrl_get(&pdev->dev);
	put_device(&pdev->dev);
	if (IS_ERR(pctl))
		return PTR_ERR(pctl);
	mclk = provider_clk("mediatek,topckgen", c->camtg);
	mclk_parent = provider_clk("mediatek,topckgen", TOP_UNIVP_192M_D8);
	if (IS_ERR(mclk) || IS_ERR(mclk_parent)) {
		ret = -ENODEV;
		goto err;
	}

	/* camera subsystem and receivers */
	ret = clocks_on();
	if (ret)
		goto err;
	if (smi_bus_prepare_enable(SMI_LARB6, "titan_camcap") == 0)
		smi_on = true;
	seninf = ioremap(SENINF_BASE, SENINF_SIZE);
	mipi_rx = ioremap(MIPI_RX_BASE, MIPI_RX_SIZE);
	if (p1) {
		cama = ioremap(P1_BASE, P1_SIZE);
		uni = ioremap(UNI_BASE, 0x1000);
	}
	else
		camsv = ioremap(CAMSV_BASE + sv * 0x1000, 0x1000);
	if (!seninf || !mipi_rx || !(camsv || cama)) {
		ret = -ENOMEM;
		goto err;
	}
	seninf_setup();
	ret = buffers_alloc();
	if (ret)
		goto err;

	/* sensor */
	ret = sensor_power_on();
	if (ret)
		goto err;
	adap = i2c_get_adapter(c->i2c_bus);
	if (!adap) {
		ret = -ENODEV;
		goto err;
	}
	sensor_read(0x300b, &id_hi);
	sensor_read(0x300c, &id_lo);
	pr_info("titan_camcap: %s (id %02x%02x), mode %dx%d\n", c->name, id_hi, id_lo,
		m->width, m->height);
	ret = sensor_table(c->init, c->ninit);
	if (!ret)
		ret = sensor_table(m->regs, m->nregs);
	if (ret)
		goto err;
	sensor_exposure(exposure, gain);
	if (cam == 0)
		af_init();
	if (c->needs_stream_on)
		sensor_write(0x0100, 1);
	msleep(300);

	/* the receiver that sees packets is the sensor's port */
	for (p = 0; p < NPORTS; p++)
		if (readl(seninf + 0x1000 * port_seninf[p] + SENINF1_CSI2_DBG_PORT))
			break;
	if (p == NPORTS) {
		pr_err("titan_camcap: no MIPI packets on any port\n");
		ret = -EIO;
		goto err;
	}
	port = p;
	seninf_mux(port);
	msleep(100);
	pr_info("titan_camcap: CSI%d, mux0 sees %u x %u\n", port,
		readl(seninf + SENINF1_MUX_DEBUG_2) >> 16,
		readl(seninf + SENINF1_MUX_DEBUG_2) & 0xffff);

	if (p1) {
		p1_start();
		msleep(400);
		pr_info("titan_camcap: cam A int %08x, %u frames, measured %08x, imgo err %08x\n",
			readl(cama + P1_INT_STAT), readl(cama + P1_TG_INTER_ST) >> 24,
			readl(cama + P1_TG_SIZE), readl(cama + P1_IMGO_ERR));
	} else {
		camsv_start();
		msleep(400);
		pr_info("titan_camcap: camsv%d int %08x, %u frames\n", sv,
			readl(camsv + SV_INT_STATUS), readl(camsv + SV_TG_INTER_ST) >> 24);
	}

	dbg = debugfs_create_file("titan_camcap", 0444, NULL, NULL, &status_fops);
	dbg_frame = debugfs_create_blob("titan_camcap_frame", 0444, NULL, &frame_blob);
	if (cam == 0)
		dbg_focus = debugfs_create_file("titan_camcap_focus", 0644, NULL, NULL, &focus_fops);
	if (aao.vaddr) {
		aao_blob.data = aao.vaddr;
		aao_blob.size = AAO_BYTES;
		dbg_aao = debugfs_create_blob("titan_camcap_aao", 0444, NULL, &aao_blob);
	}
	if (rrzo.vaddr) {
		rrzo_blob.data = rrzo.vaddr;
		rrzo_blob.size = stride * m->height;
		dbg_rrzo = debugfs_create_blob("titan_camcap_rrzo", 0444, NULL, &rrzo_blob);
	}
	if (afo.vaddr) {
		afo_blob.data = afo.vaddr;
		afo_blob.size = AFO_STRIDE * AF_NY;
		dbg_afo = debugfs_create_blob("titan_camcap_afo", 0444, NULL, &afo_blob);
	}
	return 0;

err:
	teardown();
	return ret;
}

static void __exit camcap_exit(void)
{
	teardown();
	pr_info("titan_camcap: stopped, powered off\n");
}

module_init(camcap_init);
module_exit(camcap_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Titan Pocket camera bring-up (sensor, MIPI CSI-2 receiver, CAMSV DMA)");
