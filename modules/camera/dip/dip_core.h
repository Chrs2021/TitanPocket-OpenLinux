/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dip_core.h: MediaTek pass-2 ISP (DIP) register programming, shared by the
 * titan_dip test tool and the titan_camera streaming driver. One frame of
 * packed RAW10 Bayer -> YUYV through UNP, OBC2 (optional), PGN (white
 * balance), UDM (demosaic), G2G (colour matrix), GGM (gamma), G2C (RGB->YUV)
 * and C42, in tiles. See docs/camera.md "Pass 2" for how the values were
 * found (the stock HAL, emulated with tools/re/emu_hal.py).
 *
 * The caller owns power (imgsys SCP_SYS_ISP, IMG_DIP/IMG_LARB5, smi larb5),
 * the DIP register mapping (0x15022000) and the M4U buffers.
 */
#ifndef TITAN_DIP_CORE_H
#define TITAN_DIP_CORE_H

#include <linux/io.h>
#include <linux/delay.h>

#include "dip_tdr.h"
#include "dip_tuning.h"

#define DIP_BASE	0x15022000
#define DIP_SIZE	0x6000

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
#define DIP_TDR_CTL	0x050		/* 0: frame mode, 0xb: tiles */
#define DIP_TDR_TILE	0x054
#define DIP_TDR_TCM	0x058
#define DIP_TDR_TCM2	0x05c
#define DIP_RGB2_EN	0x0e8
#define DIP_TDR_TCM3	0x0e0
#define DIP_TDRI_BASE	0x304
#define DIP_TDRI_OFST	0x308
#define DIP_TDRI_XSIZE	0x30c
#define DIP_DMA_ERR	0x744

#define DMA_EN_IMGI	BIT(0)
#define DMA_EN_IMG3O	BIT(13)

/* DMA ports: base +0, offset +8, xsize +0x10, ysize +0x14, stride +0x18 */
#define DIP_IMGI	0x500
#define DIP_IMG3O	0x390

#define DIP_OBC2	0x11c0		/* OFFST0-3, GAIN0-3: as pass 1's OBC */
#define RGB_EN_OBC2	BIT(11)
#define DIP_PGN		0x1440		/* SATU 2, GAIN 2, OFST 2 */
#define DIP_G2G		0x2080		/* CCM */
#define DIP_GGM		0x2180		/* 192 LUT words */
#define DIP_GGM_CTRL	0x2480
#define DIP_G2C		0x3240		/* RGB -> YUV */

#define CQ_SLOTS	97		/* VirDipDrvImp module table; DESC_SIZE 0x308 */
#define CQ_NOP		0x04004184	/* VirDipDrvImp::cqDelModule */

/* the HAL's defaults (PostProcPipe::configPipe / PhyDipDrvImp::loadInitSetting) */
#define DIP_IMGI_BUS	0x40010000	/* ORed into IMGI_STRIDE: bus size 1, bit 30 */
#define DIP_IMG3O_BUS	0x01910000	/* IMG3O_STRIDE: format_en, YUY2, bus size 1 */
#define DIP_RGB_YUYV	0xd5		/* UNP 0, PGN 2, UDM 4, G2G 6, GGM 7 */
#define DIP_YUV_YUYV	0x18		/* G2C 3, C42 4 */
#define DIP_PATH_YUYV	0x16040		/* srz1_sel 1, crz_sel 2, nr3d_sel 1, fe_sel 1 */
#define DIP_MISC_YUYV	0x249010
#define DIP_TCM0	0x08182002
#define DIP_TCM1	0x100
#define DIP_FMT_RAW10	9		/* cam_in_fmt packed RAW10 */

/* UDM registers in the order of the stock tables */
static const u16 udm_addr[20] __maybe_unused = {
	0x1540, 0x1544, 0x1548, 0x154c, 0x1550, 0x1554, 0x1558, 0x155c, 0x1560, 0x1564,
	0x1568, 0x156c, 0x1570, 0x1574, 0x157c, 0x1580, 0x1584, 0x1588, 0x1594, 0x1598,
};
/* BT.601 full range, 512 = 1; the output already centres U/V at 128 */
#define DIP_C11(x)	((x) & 0x7ff)
static const u32 g2c_regs[6] __maybe_unused = {
	DIP_C11(153) | DIP_C11(301) << 16, DIP_C11(58),
	DIP_C11(-87) | DIP_C11(-169) << 16, DIP_C11(256),
	DIP_C11(256) | DIP_C11(-214) << 16, DIP_C11(-42),
};

static const struct dip_tuning {
	const u32 *udm, *ccm, *ggm;
	const u32 (*nbc)[2];
	int nnbc;
} dip_tuning[2] __maybe_unused = {
	{ ov16880_udm, ov16880_ccm, ov16880_ggm, ov16880_nbc, ARRAY_SIZE(ov16880_nbc) },
	{ ov8865_udm, ov8865_ccm, ov8865_ggm, ov8865_nbc, ARRAY_SIZE(ov8865_nbc) },
};

/* built-in tile lists (dip_tdr.h): width, height, input stride, OBC2, NBC */
static const struct dip_tdr_entry {
	int w, h, stride, obc2, nbc;
	const u32 *words;
	int size;
} dip_tdrs[] __maybe_unused = {
#define DIP_T(w, h, s, o, n, t) { w, h, s, o, n, t, sizeof(t) }
	DIP_T(1632, 1224, 2048, 0, 0, tdr_1632x1224),
	DIP_T(1632, 1224, 2048, 1, 0, tdr_1632x1224_obc2),
	DIP_T(1632, 1224, 2048, 1, 1, tdr_1632x1224_obc2_nbc),
	DIP_T(2336, 1752, 2928, 0, 0, tdr_2336x1752),
	DIP_T(2336, 1752, 2928, 1, 0, tdr_2336x1752_obc2),
	DIP_T(2336, 1752, 2928, 1, 1, tdr_2336x1752_obc2_nbc),
	DIP_T(3264, 2448, 4080, 1, 0, tdr_3264x2448_obc2),	/* front full res */
	DIP_T(4672, 3504, 5840, 1, 0, tdr_4672x3504_obc2),	/* rear full res */
#undef DIP_T
};

/* interrupts off, CQ threads idle, DMA FIFOs (PhyDipDrvImp::loadInitSetting) */
static const u32 dip_init_regs[][2] __maybe_unused = {
	{ 0x020, 0 }, { 0x024, 0 }, { 0x028, 0 }, { 0x02c, 0 },
	{ 0x34c, 0x80000040 }, { 0x350, 0x00400040 }, { 0x354, 0x00150015 },
	{ 0x37c, 0x80000040 }, { 0x380, 0x00400040 }, { 0x384, 0x00150015 },
	{ 0x3ac, 0x80000040 }, { 0x3b0, 0x00400040 }, { 0x3b4, 0x00150015 },
	{ 0x3dc, 0x80000020 }, { 0x3e0, 0x00200020 }, { 0x3e4, 0x000a000a },
	{ 0x40c, 0x80000020 }, { 0x410, 0x00200020 }, { 0x414, 0x000a000a },
	{ 0x51c, 0x80000080 }, { 0x520, 0x00800080 }, { 0x524, 0x002a002a },
};

struct dip_job {
	int width, height, stride;
	int sensor;			/* 0 rear OV16880, 1 front OV8865 */
	int black;			/* RAW10 black level for OBC2, < 0: none */
	u32 wb[3];			/* PGN white-balance gains R, G, B; 512 = 1x */
	bool nbc;			/* noise reduction (NBC/ANR) */
	u32 in_mva, out_mva;		/* IMGI (RAW10), IMG3O (YUYV) */
	u32 tdr_mva;			/* tile list in M4U memory */
	void *tdr_vaddr;		/* where to copy the tile list */
	u32 desc_mva;			/* CQ descriptor (CQ_SLOTS * 8 bytes) */
	void *desc_vaddr;
};

static inline void dip_w(void __iomem *dip, u32 off, u32 val)
{
	writel(val, dip + off);
}

static inline const struct dip_tdr_entry *dip_find_tdr(int w, int h, int stride,
						       bool obc2, bool nbc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dip_tdrs); i++)
		if (dip_tdrs[i].w == w && dip_tdrs[i].h == h &&
		    dip_tdrs[i].stride == stride && dip_tdrs[i].obc2 == obc2 &&
		    dip_tdrs[i].nbc == nbc)
			return &dip_tdrs[i];
	return NULL;
}

/*
 * Program the DIP for one frame and start it (CQ thread 0, an all-NOP
 * descriptor; every register is written directly). Returns 0, or -EINVAL
 * when no built-in tile list matches. dip_wait() then polls for the end.
 */
static inline int dip_program(void __iomem *dip, const struct dip_job *j)
{
	const struct dip_tuning *t = &dip_tuning[j->sensor & 1];
	const struct dip_tdr_entry *tdr;
	u32 *desc = j->desc_vaddr;
	int i;

	tdr = dip_find_tdr(j->width, j->height, j->stride, j->black >= 0, j->nbc);
	if (!tdr)
		return -EINVAL;
	memcpy(j->tdr_vaddr, tdr->words, tdr->size);

	for (i = 0; i < ARRAY_SIZE(dip_init_regs); i++)
		dip_w(dip, dip_init_regs[i][0], dip_init_regs[i][1]);

	dip_w(dip, DIP_TDR_CTL, 0xb);
	dip_w(dip, DIP_TDR_TILE, 0);
	dip_w(dip, DIP_TDR_TCM, DIP_TCM0);
	dip_w(dip, DIP_TDR_TCM2, DIP_TCM1 | (j->nbc ? 0x2000 : 0));	/* NBC module bit */
	dip_w(dip, DIP_TDR_TCM3, 0);
	dip_w(dip, DIP_TDRI_BASE, j->tdr_mva);
	dip_w(dip, DIP_TDRI_OFST, 0);
	dip_w(dip, DIP_TDRI_XSIZE, tdr->size - 1);

	dip_w(dip, DIP_YUV_EN, DIP_YUV_YUYV | (j->nbc ? BIT(5) : 0));
	dip_w(dip, DIP_YUV2_EN, 0);
	dip_w(dip, DIP_RGB_EN, DIP_RGB_YUYV | (j->black >= 0 ? RGB_EN_OBC2 : 0));
	if (j->black >= 0) {
		for (i = 0; i < 4; i++) {
			dip_w(dip, DIP_OBC2 + 4 * i, -4 * j->black & 0x1fff);
			dip_w(dip, DIP_OBC2 + 0x10 + 4 * i,
			      512 * 4095 / (4095 - 4 * j->black));
		}
	}
	dip_w(dip, DIP_RGB2_EN, 0);
	dip_w(dip, DIP_FMT_SEL, DIP_FMT_RAW10);		/* pixel_id 0 (BGGR); reset bits dropped */
	dip_w(dip, DIP_PATH_SEL, DIP_PATH_YUYV);
	dip_w(dip, DIP_MISC_SEL, DIP_MISC_YUYV);

	/* PGN white balance: 12-bit clip, GAIN_1 = B | G << 16, GAIN_2 = G | R << 16 */
	dip_w(dip, DIP_PGN + 0x00, 0x0fff0fff);
	dip_w(dip, DIP_PGN + 0x04, 0x0fff0fff);
	dip_w(dip, DIP_PGN + 0x08, (j->wb[2] & 0x1fff) | (j->wb[1] & 0x1fff) << 16);
	dip_w(dip, DIP_PGN + 0x0c, (j->wb[1] & 0x1fff) | (j->wb[0] & 0x1fff) << 16);
	dip_w(dip, DIP_PGN + 0x10, 0);
	dip_w(dip, DIP_PGN + 0x14, 0);
	for (i = 0; i < 20; i++)
		dip_w(dip, udm_addr[i], t->udm[i]);
	for (i = 0; i < 8; i++)
		dip_w(dip, DIP_G2G + 4 * i, t->ccm[i]);
	dip_w(dip, 0x3040, 0xf0);			/* HAL: always */
	for (i = 0; i < 192; i++)
		dip_w(dip, DIP_GGM + 4 * i, t->ggm[i]);
	dip_w(dip, DIP_GGM_CTRL, 0x100307fe);
	for (i = 0; i < 6; i++)
		dip_w(dip, DIP_G2C + 4 * i, g2c_regs[i]);
	if (j->nbc)			/* noise reduction (NBC/ANR), per-sensor tuning */
		for (i = 0; i < t->nnbc; i++)
			dip_w(dip, t->nbc[i][0], t->nbc[i][1]);

	dip_w(dip, DIP_IMGI + 0x00, j->in_mva);
	dip_w(dip, DIP_IMGI + 0x08, 0);
	dip_w(dip, DIP_IMGI + 0x10, j->width * 10 / 8 - 1);
	dip_w(dip, DIP_IMGI + 0x14, j->height - 1);
	dip_w(dip, DIP_IMGI + 0x18, j->stride | DIP_IMGI_BUS);
	dip_w(dip, DIP_IMG3O + 0x00, j->out_mva);
	dip_w(dip, DIP_IMG3O + 0x08, 0);
	dip_w(dip, DIP_IMG3O + 0x10, j->width * 2 - 1);
	dip_w(dip, DIP_IMG3O + 0x14, j->height - 1);
	dip_w(dip, DIP_IMG3O + 0x18, (j->width * 2) | DIP_IMG3O_BUS);
	dip_w(dip, DIP_IMG3O + 0x28, 0);		/* crop */
	dip_w(dip, DIP_DMA_EN, DMA_EN_IMGI | DMA_EN_IMG3O);

	for (i = 0; i < CQ_SLOTS; i++) {
		desc[2 * i] = CQ_NOP;
		desc[2 * i + 1] = j->desc_mva;
	}
	wmb();
	readl(dip + DIP_INT_STATUS);			/* read-clear */
	readl(dip + DIP_CQ_STATUS);
	dip_w(dip, DIP_CTL_START, 1);
	return 0;
}

/* poll for the frame to finish; return 0 on completion, -ETIMEDOUT otherwise */
static inline int dip_wait(void __iomem *dip, int max_us)
{
	int i;

	for (i = 0; i < max_us / 500; i++) {
		if (!readl(dip + DIP_CTL_START) && i > 2)
			return 0;
		udelay(500);
	}
	return -ETIMEDOUT;
}

#endif /* TITAN_DIP_CORE_H */
