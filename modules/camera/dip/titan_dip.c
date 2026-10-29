// SPDX-License-Identifier: GPL-2.0
/*
 * titan_dip: MediaTek pass-2 ISP (DIP) test harness for the Titan Pocket
 * (see docs/camera.md, "Pass 2").
 *
 * Takes one Bayer frame written to debugfs, runs it through the DIP in frame
 * mode (no tiles) and leaves the YUV result in another debugfs file:
 *
 *   insmod titan_dip.ko [wb=R,G,B] [width=1632 height=1224 stride=2048 fmt=9 pixid=0]
 *   cat f.raw > /sys/kernel/debug/titan_dip_in    processed RAW10 (titan_camcap p1proc=1 p1obc=1)
 *   [cat tdr.bin > /sys/kernel/debug/titan_dip_tdr]   tile list (built in for 1632x1224)
 *   echo 1 > /sys/kernel/debug/titan_dip_run      configure, start, poll for the end
 *   cat /sys/kernel/debug/titan_dip               status registers
 *   cat /sys/kernel/debug/titan_dip_out > f.yuyv  YUYV 4:2:2
 *   echo 0x1234 0x5678 > /sys/kernel/debug/titan_dip_poke    write a DIP register
 *
 * Register layout and the command queue (CQ) descriptor format come from the
 * stock HAL: libdip_drv.so (VirDipDrvImp::cqAddModule, loadInitSetting),
 * libdip_postproc.so (DIP_DMA, DIP_ISP_PIPE) and the libcam.hal3a.v3.so pass-2
 * tuning managers; the per-sensor tables from libCamera_<sensor>_*.so.
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <dt-bindings/clock/mt6771-clk.h>

#include "dip_tdr.h"
#include "dip_tuning.h"

static int width = 1632, height = 1224;
module_param(width, int, 0444);
module_param(height, int, 0444);
static int stride = 2048;		/* input line pitch in bytes */
module_param(stride, int, 0444);
static int fmt = 9;			/* FMT_SEL cam_in_fmt: 9 packed RAW10 (as pass 1) */
module_param(fmt, int, 0444);
static int pixid;			/* Bayer order: 0 B first, 1 Gb, 2 Gr, 3 R */
module_param(pixid, int, 0444);
static int sensor = 1;			/* tuning: 0 rear OV16880, 1 front OV8865 */
module_param(sensor, int, 0644);
static int black = -1;			/* RAW10 black level for OBC2 (CAMSV input), -1: none
					 * (pass 1 already subtracted it) */
module_param(black, int, 0644);
static int imgi_bus = 0x40010000;	/* bits ORed into IMGI_STRIDE: bus size 1, bit 30 (HAL) */
module_param(imgi_bus, int, 0644);
static int img3o_bus = 0x01910000;	/* IMG3O_STRIDE: format_en, YUY2, bus size 1 (HAL) */
module_param(img3o_bus, int, 0644);
static int rgb_en = 0xd5;		/* UNP 0, PGN 2, UDM 4, G2G (CCM) 6, GGM 7 (GGM is not
					 * optional: off, the RGB path outputs zeros) */
module_param(rgb_en, int, 0644);
static int yuv_en = 0x18;		/* G2C 3, C42 4 */
module_param(yuv_en, int, 0644);
static int imgi_xbytes;		/* IMGI bytes per line, 0: width * 10 / 8 */
module_param(imgi_xbytes, int, 0644);
static uint imgi_base;		/* experiments: IMGI address instead of the input buffer */
module_param(imgi_base, uint, 0644);
static int tdr_ctl = 0xb;		/* TDR_CTL with a TDR loaded (HAL: 0xb tiles, 0 frame mode) */
module_param(tdr_ctl, int, 0644);
static int tdr_tile;			/* 0x54 (HAL: 0) */
module_param(tdr_tile, int, 0644);
static uint tcm[3] = { 0x08182002, 0x100, 0 };	/* 0x58, 0x5c, 0xe0 (HAL PostProcPipe::configPipe) */
static int tcm_n;
module_param_array(tcm, uint, &tcm_n, 0644);
static uint fmt_keep;			/* FMT_SEL reset bits kept (31 makes IMGI fetch a second plane) */
module_param(fmt_keep, uint, 0644);
static int misc = 0x249010;		/* MISC_SEL (HAL); -1 keeps the reset value */
module_param(misc, int, 0644);
static int wb[3] = { 512, 512, 512 };	/* PGN white-balance gains R, G, B; 512 = 1x */
static int wb_n;
module_param_array(wb, int, &wb_n, 0644);
static int path_sel = 0x16040;		/* srz1_sel 1, crz_sel 2, nr3d_sel 1, fe_sel 1 (HAL) */
module_param(path_sel, int, 0644);
static int cq;			/* 0: START alone, 1: CQ thread 0 with all-NOP
					 * descriptor, 2: CQ loads the configured modules
					 * from a register image, as the HAL does */
module_param(cq, int, 0644);

#define DIP_BASE	0x15022000
#define DIP_SIZE	0x6000

/* control */
#define DIP_CTL_START	0x000		/* bit n: CQ thread n */
#define DIP_YUV_EN	0x004
#define DIP_YUV2_EN	0x008
#define DIP_RGB_EN	0x00c
#define DIP_DMA_EN	0x010
#define DIP_FMT_SEL	0x014		/* cam_in_fmt 3:0, img3o_fmt 21:20, pixel_id 29:28 */
#define DIP_PATH_SEL	0x018
#define DIP_MISC_SEL	0x01c
#define DIP_INT_EN	0x020
#define DIP_INT_STATUS	0x030
#define DIP_CQ_STATUS	0x034
#define DIP_CQ_LDSTATUS	0x038
#define DIP_TDR_CTL	0x050		/* 0: frame mode */
#define DIP_RGB2_EN	0x0e8
#define DIP_TDR_TILE	0x054
#define DIP_TDR_TCM	0x058
#define DIP_TDR_TCM2	0x05c
#define DIP_TDR_TCM3	0x0e0
#define DIP_TDRI_BASE	0x304
#define DIP_TDRI_OFST	0x308
#define DIP_TDRI_XSIZE	0x30c
#define DIP_CQ_THR_CTL(n)	(0x204 + 0xc * (n))
#define DIP_CQ_THR_BASE(n)	(0x208 + 0xc * (n))
#define DIP_CQ_THR_SIZE(n)	(0x20c + 0xc * (n))
#define DIP_DMA_ERR	0x744		/* one word per port: img2o, img2bo, img3o, ... imgi 0x760 */

#define DMA_EN_IMGI	BIT(0)
#define DMA_EN_IMG3O	BIT(13)

/* DMA ports: base +0, offset +8, xsize +0x10, ysize +0x14, stride +0x18,
 * FIFO +0x1c/+0x20/+0x24, crop +0x28 (outputs) */
#define DIP_IMGI	0x500
#define DIP_IMG3O	0x390

/* processing blocks */
#define DIP_PGN		0x1440		/* SATU 2, GAIN 2, OFST 2: B | G << 16, G | R << 16 */
#define DIP_UDM		0x1540		/* 20 registers, ov8865mipiraw_UDM_* order */
#define DIP_G2G		0x2080		/* CCM: c00|c01<<16, c02, c10|c11<<16, c12, c20|c21<<16, c22, ctl, ctl */
#define DIP_GGM		0x2180		/* 192 LUT words, control at 0x2480 */
#define DIP_GGM_CTRL	0x2480
#define DIP_G2C		0x3240		/* RGB -> YUV: c00|c01<<16, c02|ofst<<16, ... */

#define CQ_SLOTS	97		/* VirDipDrvImp module table; DESC_SIZE 0x308 */
#define CQ_NOP		0x04004184	/* VirDipDrvImp::cqDelModule */

/* CQ module slots used (VirDipDrvImp module table: slot, offset, count) */
static const u16 cq_mods[][3] = {
	{ 0x00, 0x0004, 7 },	/* YUV_EN .. MISC_SEL */
	{ 0x01, 0x00e8, 1 },	/* RGB2_EN */
	{ 0x02, 0x0050, 4 },	/* TDR */
	{ 0x0c, 0x0394, 6 },	/* IMG3O (all but the base) */
	{ 0x0d, 0x03b8, 1 },	/* IMG3O crop */
	{ 0x15, 0x0504, 6 },	/* IMGI */
	{ 0x20, 0x1440, 6 },	/* PGN */
	{ 0x22, 0x1540, 0x17 },	/* UDM */
	{ 0x23, 0x2080, 8 },	/* G2G */
	{ 0x27, 0x3240, 0xd },	/* G2C */
};

/* the HAL's PhyDipDrvImp::loadInitSetting: interrupts off, CQ threads, DMA FIFOs */
static const u32 init_regs[][2] = {
	{ 0x020, 0 }, { 0x024, 0 }, { 0x028, 0 }, { 0x02c, 0 },
	{ 0x34c, 0x80000040 }, { 0x350, 0x00400040 }, { 0x354, 0x00150015 },	/* IMG2O */
	{ 0x37c, 0x80000040 }, { 0x380, 0x00400040 }, { 0x384, 0x00150015 },	/* IMG2BO */
	{ 0x3ac, 0x80000040 }, { 0x3b0, 0x00400040 }, { 0x3b4, 0x00150015 },	/* IMG3O */
	{ 0x3dc, 0x80000020 }, { 0x3e0, 0x00200020 }, { 0x3e4, 0x000a000a },	/* IMG3BO */
	{ 0x40c, 0x80000020 }, { 0x410, 0x00200020 }, { 0x414, 0x000a000a },	/* IMG3CO */
	{ 0x51c, 0x80000080 }, { 0x520, 0x00800080 }, { 0x524, 0x002a002a },	/* IMGI */
};

static const struct {
	const u32 *udm, *ccm, *ggm;
} tuning[2] = {
	{ ov16880_udm, ov16880_ccm, ov16880_ggm },
	{ ov8865_udm, ov8865_ccm, ov8865_ggm },
};

/* built-in tile lists (dip_tdr.h): width, height, input stride, OBC2 */
static const struct {
	int w, h, stride, obc2;
	const u32 *words;
	size_t size;
} tdrs[] = {
#define T(w, h, s, o, t) { w, h, s, o, t, sizeof(t) }
	T(1632, 1224, 2048, 0, tdr_1632x1224),
	T(1632, 1224, 2048, 1, tdr_1632x1224_obc2),
	T(2336, 1752, 2928, 0, tdr_2336x1752),
	T(2336, 1752, 2928, 1, tdr_2336x1752_obc2),
	T(4672, 3504, 5840, 1, tdr_4672x3504_obc2),
#undef T
};

#define DIP_OBC2	0x11c0		/* OFFST0-3, GAIN0-3: as pass 1's OBC */
#define RGB_EN_OBC2	BIT(11)

/* UDM registers in the order of the stock tables */
static const u16 udm_addr[20] = {
	0x1540, 0x1544, 0x1548, 0x154c, 0x1550, 0x1554, 0x1558, 0x155c, 0x1560, 0x1564,
	0x1568, 0x156c, 0x1570, 0x1574, 0x157c, 0x1580, 0x1584, 0x1588, 0x1594, 0x1598,
};
/* BT.601 full range, 512 = 1 */
#define C11(x)	((x) & 0x7ff)
static const u32 g2c_regs[6] = {
	C11(153) | C11(301) << 16, C11(58),
	C11(-87) | C11(-169) << 16, C11(256),
	C11(256) | C11(-214) << 16, C11(-42),	/* the output already centres U/V at 128 */
};

/* ---- power and clocks ---- */

#define SMI_LARB5	5
extern s32 smi_bus_prepare_enable(const u32 id, const char *user);
extern s32 smi_bus_disable_unprepare(const u32 id, const char *user);

static struct {
	const char *provider;
	int id;
	const char *name;
	struct clk *clk;
	bool on;
} clks[] = {
	{ "mediatek,scpsys",   SCP_SYS_DIS, "SCP_SYS_DIS" },
	{ "mediatek,scpsys",   SCP_SYS_ISP, "SCP_SYS_ISP" },
	{ "mediatek,topckgen", TOP_MUX_IMG, "img_sel" },
	{ "mediatek,imgsys",   IMG_LARB5,   "img_larb5" },
	{ "mediatek,imgsys",   IMG_DIP,     "img_dip" },
};
static bool smi_on;

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

static int power_on(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		clks[i].clk = provider_clk(clks[i].provider, clks[i].id);
		if (IS_ERR(clks[i].clk)) {
			pr_err("titan_dip: clock %s: %ld\n", clks[i].name, PTR_ERR(clks[i].clk));
			clks[i].clk = NULL;
			return -ENODEV;
		}
		ret = clk_prepare_enable(clks[i].clk);
		if (ret) {
			pr_err("titan_dip: enable %s: %d\n", clks[i].name, ret);
			return ret;
		}
		clks[i].on = true;
	}
	if (smi_bus_prepare_enable(SMI_LARB5, "titan_dip") == 0)
		smi_on = true;
	return 0;
}

static void power_off(void)
{
	int i;

	if (smi_on)
		smi_bus_disable_unprepare(SMI_LARB5, "titan_dip");
	smi_on = false;
	for (i = ARRAY_SIZE(clks) - 1; i >= 0; i--) {
		if (clks[i].on)
			clk_disable_unprepare(clks[i].clk);
		if (clks[i].clk)
			clk_put(clks[i].clk);
		clks[i].on = false;
		clks[i].clk = NULL;
	}
}

/* ---- buffers: scattered pages, one M4U mapping (as titan_camcap) ---- */

#define M4U_PORT_CAM_IMGI	36	/* larb5: DIP input */
#define M4U_PORT_CAM_IMG3O	38	/* larb5: DIP YUV output */
#define M4U_PORT_CAM_IMG2O	37	/* used for the CQ descriptor (any larb5 read port works?) */
#define M4U_PROT_READ		BIT(0)
#define M4U_PROT_WRITE		BIT(1)

struct buf {
	struct page **pages;
	int npages;
	size_t size;
	void *vaddr;
	struct sg_table sgt;
	unsigned int mva;
	int port;
};

static struct buf in, out, desc, vreg, tdr;
static size_t tdr_len;		/* bytes written to titan_dip_tdr */
static void *m4u_client;

static void *m4u_fn(const char *name)
{
	void *fn = (void *)kallsyms_lookup_name(name);

	if (!fn)
		pr_err("titan_dip: %s not found\n", name);
	return fn;
}

static int buf_alloc(struct buf *b, size_t size, int port, u8 fill)
{
	int (*alloc_mva)(void *client, int port, unsigned long va, struct sg_table *sgt,
			 unsigned int size, unsigned int prot, unsigned int flags,
			 unsigned int *mva) = m4u_fn("m4u_alloc_mva");
	int i, ret;

	if (!alloc_mva)
		return -ENOENT;
	b->port = port;
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
	memset(b->vaddr, fill, b->size);
	ret = sg_alloc_table_from_pages(&b->sgt, b->pages, b->npages, 0, b->size, GFP_KERNEL);
	if (ret)
		return ret;
	ret = alloc_mva(m4u_client, port, 0, &b->sgt, b->size,
			M4U_PROT_READ | M4U_PROT_WRITE, 0, &b->mva);
	if (ret)
		pr_err("titan_dip: m4u_alloc_mva port %d: %d\n", port, ret);
	return ret;
}

static void buf_free(struct buf *b)
{
	int (*dealloc_mva)(void *client, int port, unsigned int mva) =
		m4u_fn("m4u_dealloc_mva");
	int i;

	if (b->mva && dealloc_mva)
		dealloc_mva(m4u_client, b->port, b->mva);
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

/* ---- the DIP ---- */

static void __iomem *dip;
static u32 last_status[4];
static u32 fmt_reset, misc_reset;	/* FMT_SEL, MISC_SEL after power-up */
static int last_polls;

static void dip_w(u32 off, u32 val)
{
	writel(val, dip + off);
	if (vreg.vaddr)
		((u32 *)vreg.vaddr)[off / 4] = val;
}

static u32 dip_r(u32 off)
{
	return readl(dip + off);
}

static void dip_dma(u32 port, u32 mva, u32 xbytes, u32 lines, u32 pitch)
{
	dip_w(port + 0x00, mva);
	dip_w(port + 0x08, 0);
	dip_w(port + 0x10, xbytes - 1);
	dip_w(port + 0x14, lines - 1);
	dip_w(port + 0x18, pitch);
}

static int dip_run(void)
{
	u32 *d = desc.vaddr;
	int i;

	for (i = 0; i < ARRAY_SIZE(init_regs); i++)
		dip_w(init_regs[i][0], init_regs[i][1]);

	if (!tdr_len) {
		for (i = 0; i < ARRAY_SIZE(tdrs); i++) {
			if (tdrs[i].w == width && tdrs[i].h == height && tdrs[i].stride == stride &&
			    tdrs[i].obc2 == (black >= 0)) {
				memcpy(tdr.vaddr, tdrs[i].words, tdrs[i].size);
				tdr_len = tdrs[i].size;
			}
		}
		if (!tdr_len)
			pr_warn("titan_dip: no built-in tile list for %dx%d stride %d%s\n", width,
				height, stride, black >= 0 ? " with OBC2" : "");
	}
	if (tdr_len) {
		/* tiles: the TDR list from the stock tile engine (tools/re/emu_tile.py) */
		dip_w(DIP_TDR_CTL, tdr_ctl);
		dip_w(DIP_TDR_TILE, tdr_tile);
		dip_w(DIP_TDR_TCM, tcm[0]);
		dip_w(DIP_TDR_TCM2, tcm[1]);
		dip_w(DIP_TDR_TCM3, tcm[2]);
		dip_w(DIP_TDRI_BASE, tdr.mva);
		dip_w(DIP_TDRI_OFST, 0);
		dip_w(DIP_TDRI_XSIZE, tdr_len - 1);
	} else {
		dip_w(DIP_TDR_CTL, 0);		/* frame mode: only fits narrow frames */
	}
	dip_w(DIP_YUV_EN, yuv_en);
	dip_w(DIP_YUV2_EN, 0);
	dip_w(DIP_RGB_EN, rgb_en | (black >= 0 ? RGB_EN_OBC2 : 0));
	if (black >= 0) {
		/* offset -4 * black on the 12-bit scale, gain back to full range */
		for (i = 0; i < 4; i++) {
			dip_w(DIP_OBC2 + 4 * i, -4 * black & 0x1fff);
			dip_w(DIP_OBC2 + 0x10 + 4 * i, 512 * 4095 / (4095 - 4 * black));
		}
	}
	dip_w(DIP_RGB2_EN, 0);
	/* img3o_fmt 0: YUV422 1 plane; keep the reset value's other bits (31) */
	dip_w(DIP_FMT_SEL, (fmt_reset & fmt_keep & ~0x3030000fU) | (fmt & 0xf) | (pixid & 3) << 28);
	dip_w(DIP_PATH_SEL, path_sel);
	dip_w(DIP_MISC_SEL, misc < 0 ? misc_reset : misc);

	/* PGN (pass 2's white balance, laid out like pass 1's RPG): 12-bit clip,
	 * GAIN_1 = B | G << 16, GAIN_2 = G | R << 16 */
	dip_w(DIP_PGN + 0x00, 0x0fff0fff);
	dip_w(DIP_PGN + 0x04, 0x0fff0fff);
	dip_w(DIP_PGN + 0x08, (wb[2] & 0x1fff) | (wb[1] & 0x1fff) << 16);
	dip_w(DIP_PGN + 0x0c, (wb[1] & 0x1fff) | (wb[0] & 0x1fff) << 16);
	dip_w(DIP_PGN + 0x10, 0);
	dip_w(DIP_PGN + 0x14, 0);
	for (i = 0; i < ARRAY_SIZE(udm_addr); i++)
		dip_w(udm_addr[i], tuning[sensor & 1].udm[i]);
	for (i = 0; i < 8; i++)
		dip_w(DIP_G2G + 4 * i, tuning[sensor & 1].ccm[i]);
	dip_w(0x3040, 0xf0);			/* HAL: always (module slot 0x28) */
	for (i = 0; i < 192; i++)
		dip_w(DIP_GGM + 4 * i, tuning[sensor & 1].ggm[i]);
	dip_w(DIP_GGM_CTRL, 0x100307fe);	/* HAL ISP_MGR_GGM::apply */
	for (i = 0; i < ARRAY_SIZE(g2c_regs); i++)
		dip_w(DIP_G2C + 4 * i, g2c_regs[i]);

	dip_dma(DIP_IMGI, imgi_base ?: in.mva, imgi_xbytes ?: width * 10 / 8, height, stride | imgi_bus);
	dip_dma(DIP_IMG3O, out.mva, width * 2, height, (width * 2) | img3o_bus);
	dip_w(DIP_IMG3O + 0x28, 0);		/* crop */
	dip_w(DIP_DMA_EN, DMA_EN_IMGI | DMA_EN_IMG3O);

	/* start: CQ thread 0 with every module slot a NOP (the registers are
	 * already written), or the START bit alone */
	for (i = 0; i < CQ_SLOTS; i++) {
		d[2 * i] = CQ_NOP;
		d[2 * i + 1] = desc.mva;
	}
	if (cq == 2) {
		/* word 0: register (0x15000000-relative) bits 15:0, bits 18:16 at
		 * 31:29, count - 1 at 25:16; word 1: where the values are */
		for (i = 0; i < ARRAY_SIZE(cq_mods); i++) {
			u32 reg = 0x22000 + cq_mods[i][1];

			d[2 * cq_mods[i][0]] = (reg & 0xffff) | (reg >> 16) << 29 |
					       ((cq_mods[i][2] - 1) & 0x3ff) << 16;
			d[2 * cq_mods[i][0] + 1] = vreg.mva + cq_mods[i][1];
		}
	}
	wmb();
	dip_r(DIP_INT_STATUS);			/* read-clear */
	dip_r(DIP_CQ_STATUS);
	if (cq) {
		dip_w(DIP_CQ_THR_BASE(0), desc.mva);
		dip_w(DIP_CQ_THR_SIZE(0), CQ_SLOTS * 8);
		dip_w(DIP_CQ_THR_CTL(0), 0x11);
	}
	dip_w(DIP_CTL_START, 1);

	for (i = 0; i < 500; i++) {
		last_status[0] |= dip_r(DIP_INT_STATUS);
		last_status[1] |= dip_r(DIP_CQ_STATUS);
		last_status[2] = dip_r(DIP_CQ_LDSTATUS);
		last_status[3] = dip_r(DIP_CTL_START);
		if (!last_status[3] && i > 2)
			break;
		usleep_range(1000, 2000);
	}
	last_polls = i;
	pr_info("titan_dip: run: start %08x int %08x cq %08x ld %08x after %d polls\n",
		last_status[3], last_status[0], last_status[1], last_status[2], i);
	return 0;
}

/* ---- debugfs ---- */

static struct dentry *dbg, *dbg_in, *dbg_out, *dbg_run, *dbg_poke, *dbg_tdr;
static struct debugfs_blob_wrapper out_blob;

static int status_show(struct seq_file *s, void *v)
{
	int i;

	seq_printf(s, "%dx%d stride %d fmt %d pixid %d  in mva %#x  out mva %#x  desc %#x\n",
		   width, height, stride, fmt, pixid, in.mva, out.mva, desc.mva);
	seq_printf(s, "last run: int %08x cq %08x ld %08x start %08x, %d polls\n",
		   last_status[0], last_status[1], last_status[2], last_status[3], last_polls);
	seq_puts(s, "ctl 000:");
	for (i = 0; i < 0x60; i += 4)
		seq_printf(s, "%s%08x", i % 32 ? " " : "\n  ", dip_r(i));
	seq_printf(s, "\ntdr %zu bytes: ctl %08x tile %08x tcm %08x %08x %08x  tdri %08x %08x %08x",
		   tdr_len, dip_r(DIP_TDR_CTL), dip_r(DIP_TDR_TILE), dip_r(DIP_TDR_TCM),
		   dip_r(DIP_TDR_TCM2), dip_r(DIP_TDR_TCM3), dip_r(DIP_TDRI_BASE),
		   dip_r(DIP_TDRI_OFST), dip_r(DIP_TDRI_XSIZE));
	seq_printf(s, "\nrgb2 %08x  cq thr0 %08x %08x %08x\n", dip_r(DIP_RGB2_EN),
		   dip_r(DIP_CQ_THR_CTL(0)), dip_r(DIP_CQ_THR_BASE(0)), dip_r(DIP_CQ_THR_SIZE(0)));
	seq_puts(s, "dma err (img2o img2bo img3o img3bo img3co feo mfbo imgi imgbi imgci vipi vip2i vip3i dmgi depi lcei ufdi):\n ");
	for (i = 0; i < 17; i++)
		seq_printf(s, " %08x", dip_r(DIP_DMA_ERR + 4 * i));
	seq_printf(s, "\nimgi %08x %08x %08x %08x  img3o %08x %08x %08x %08x\n",
		   dip_r(DIP_IMGI), dip_r(DIP_IMGI + 0x10), dip_r(DIP_IMGI + 0x14),
		   dip_r(DIP_IMGI + 0x18), dip_r(DIP_IMG3O), dip_r(DIP_IMG3O + 0x10),
		   dip_r(DIP_IMG3O + 0x14), dip_r(DIP_IMG3O + 0x18));
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

static ssize_t in_write(struct file *f, const char __user *ubuf, size_t n, loff_t *pos)
{
	return simple_write_to_buffer(in.vaddr, in.size, pos, ubuf, n);
}

static ssize_t in_read(struct file *f, char __user *ubuf, size_t n, loff_t *pos)
{
	return simple_read_from_buffer(ubuf, n, pos, in.vaddr, in.size);
}

static const struct file_operations in_fops = {
	.write = in_write,
	.read = in_read,
	.llseek = default_llseek,
};

static ssize_t run_write(struct file *f, const char __user *ubuf, size_t n, loff_t *pos)
{
	memset(last_status, 0, sizeof(last_status));
	dip_run();
	return n;
}

static ssize_t tdr_write(struct file *f, const char __user *ubuf, size_t n, loff_t *pos)
{
	ssize_t r = simple_write_to_buffer(tdr.vaddr, tdr.size, pos, ubuf, n);

	if (r > 0)
		tdr_len = max_t(size_t, tdr_len, *pos);
	return r;
}

static const struct file_operations tdr_fops = {
	.write = tdr_write,
	.llseek = default_llseek,
};

static const struct file_operations run_fops = {
	.write = run_write,
};

static ssize_t poke_write(struct file *f, const char __user *ubuf, size_t n, loff_t *pos)
{
	char buf[64];
	u32 off, val;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	if (sscanf(buf, "%x %x", &off, &val) != 2 || off >= DIP_SIZE || off & 3)
		return -EINVAL;
	dip_w(off, val);
	return n;
}

static const struct file_operations poke_fops = {
	.write = poke_write,
};

/* ---- module ---- */

static void cleanup(void)
{
	int (*destroy_client)(void *client) = m4u_fn("m4u_destroy_client");

	debugfs_remove(dbg);
	debugfs_remove(dbg_in);
	debugfs_remove(dbg_out);
	debugfs_remove(dbg_run);
	debugfs_remove(dbg_poke);
	debugfs_remove(dbg_tdr);
	buf_free(&in);
	buf_free(&out);
	buf_free(&desc);
	buf_free(&vreg);
	buf_free(&tdr);
	if (m4u_client && destroy_client)
		destroy_client(m4u_client);
	m4u_client = NULL;
	if (dip)
		iounmap(dip);
	dip = NULL;
	power_off();
}

static int __init dip_init(void)
{
	void *(*create_client)(void) = m4u_fn("m4u_create_client");
	int ret;

	ret = power_on();
	if (ret)
		goto err;
	dip = ioremap(DIP_BASE, DIP_SIZE);
	if (!dip) {
		ret = -ENOMEM;
		goto err;
	}
	fmt_reset = dip_r(DIP_FMT_SEL);
	misc_reset = dip_r(DIP_MISC_SEL);
	pr_info("titan_dip: reset values: fmt %08x misc %08x\n", fmt_reset, misc_reset);

	if (!create_client) {
		ret = -ENOENT;
		goto err;
	}
	m4u_client = create_client();
	if (IS_ERR_OR_NULL(m4u_client)) {
		m4u_client = NULL;
		ret = -ENOMEM;
		goto err;
	}
	ret = buf_alloc(&in, stride * height, M4U_PORT_CAM_IMGI, 0);
	if (!ret)
		ret = buf_alloc(&out, width * 2 * height, M4U_PORT_CAM_IMG3O, 0x5a);
	if (!ret)
		ret = buf_alloc(&desc, CQ_SLOTS * 8, M4U_PORT_CAM_IMG2O, 0);
	if (!ret)
		ret = buf_alloc(&vreg, DIP_SIZE, M4U_PORT_CAM_IMG2O, 0);
	if (!ret)
		ret = buf_alloc(&tdr, 0x10000, M4U_PORT_CAM_IMGI, 0);
	if (ret)
		goto err;

	out_blob.data = out.vaddr;
	out_blob.size = width * 2 * height;
	dbg = debugfs_create_file("titan_dip", 0444, NULL, NULL, &status_fops);
	dbg_in = debugfs_create_file("titan_dip_in", 0644, NULL, NULL, &in_fops);
	dbg_out = debugfs_create_blob("titan_dip_out", 0444, NULL, &out_blob);
	dbg_run = debugfs_create_file("titan_dip_run", 0200, NULL, NULL, &run_fops);
	dbg_poke = debugfs_create_file("titan_dip_poke", 0200, NULL, NULL, &poke_fops);
	dbg_tdr = debugfs_create_file("titan_dip_tdr", 0200, NULL, NULL, &tdr_fops);
	pr_info("titan_dip: ready, %dx%d, in %#x out %#x desc %#x\n", width, height,
		in.mva, out.mva, desc.mva);
	return 0;
err:
	cleanup();
	return ret;
}

static void __exit dip_exit(void)
{
	cleanup();
	pr_info("titan_dip: powered off\n");
}

module_init(dip_init);
module_exit(dip_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Titan Pocket DIP (ISP pass 2) test harness");
