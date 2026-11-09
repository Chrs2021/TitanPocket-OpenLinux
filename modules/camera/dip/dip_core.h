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

#include <linux/completion.h>

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
#define DIP_CQ_STATUS	0x034		/* read-clear; bit 0: CQ thread 0 done */
#define DIP_CQ_DONE	BIT(0)
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
#define DMA_EN_IMG2O	BIT(11)

/* DMA ports: base +0, offset +8, xsize +0x10, ysize +0x14, stride +0x18 */
/*
 * SMX: the stripe memory NBC carries its state in from one tile to the next
 * (out through SMXnO, back through SMXnI). The HAL always allocates one area
 * per command queue and sets the bases through the queue each frame
 * (DipRingBufferImp::getSMXIOBuffer), so its register images show none of
 * them; the tile engine adds the per-tile offsets. Ports from the vendor's
 * DIP driver (camera_dip.c, isp_50): SMX1O 0x7d0 .. SMX4O 0x860, SMX1I 0x890
 * .. SMX4I 0x920, 0x30 apart. Sizes as the HAL's.
 */
#define DIP_SMXO(n)	(0x7d0 + 0x30 * (n))
#define DIP_SMXI(n)	(0x890 + 0x30 * (n))
static const u32 dip_smx_size[4] = { 0x9c4008, 0x3e8008, 0x3e8008, 0x1f4008 };
#define DIP_SMX_TOTAL	0x2710040	/* the four, as the HAL's one area */
#define DIP_IMGI	0x500
#define DIP_IMG3O	0x390
#define DIP_IMG2O	0x330		/* after CRZ (the resizer) */
#define DIP_CRZ		0x5300		/* CONTROL, IN, OUT, H/V step, luma/chroma offsets */

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
	const u32 (*nbc)[2], (*seee)[2];
	int nnbc, nseee;
} dip_tuning[2] __maybe_unused = {
	{ ov16880_udm, ov16880_ccm, ov16880_ggm, ov16880_nbc, ov16880_seee,
	  ARRAY_SIZE(ov16880_nbc), ARRAY_SIZE(ov16880_seee) },
	{ ov8865_udm, ov8865_ccm, ov8865_ggm, ov8865_nbc, ov8865_seee,
	  ARRAY_SIZE(ov8865_nbc), ARRAY_SIZE(ov8865_seee) },
};

/* modules with their own tile words, so their own tile lists */
#define DIP_MOD_NBC	BIT(0)		/* noise reduction (ANR) */
#define DIP_MOD_SEEE	BIT(1)		/* edge enhancement (EE, sharpening) */

/* built-in tile lists (dip_tdr.h): width, height, input stride, OBC2, DIP_MOD_*,
 * and the CRZ output size (0: none, the frame's own size out of IMG3O) */
static const struct dip_tdr_entry {
	int w, h, stride, obc2, mods, ow, oh;
	const u32 *words;
	int size;
} dip_tdrs[] __maybe_unused = {
#define DIP_T(w, h, s, o, n, t) { w, h, s, o, n, 0, 0, t, sizeof(t) }
#define DIP_TC(w, h, s, o, n, ow, oh, t) { w, h, s, o, n, ow, oh, t, sizeof(t) }
	DIP_T(1632, 1224, 2048, 0, 0, tdr_1632x1224),
	DIP_T(1632, 1224, 2048, 1, 0, tdr_1632x1224_obc2),
	DIP_T(1632, 1224, 2048, 1, 1, tdr_1632x1224_obc2_nbc),
	DIP_T(2336, 1752, 2928, 0, 0, tdr_2336x1752),
	DIP_T(2336, 1752, 2928, 1, 0, tdr_2336x1752_obc2),
	DIP_T(2336, 1752, 2928, 1, 1, tdr_2336x1752_obc2_nbc),
	DIP_T(3264, 2448, 4080, 1, 0, tdr_3264x2448_obc2),	/* front full res */
	DIP_T(3264, 2448, 4080, 1, 1, tdr_3264x2448_obc2_nbc),
	DIP_T(4672, 3504, 5840, 1, 0, tdr_4672x3504_obc2),	/* rear full res */
	DIP_T(4672, 3504, 5840, 1, 1, tdr_4672x3504_obc2_nbc),
	DIP_T(1632, 1224, 2048, 1, 3, tdr_1632x1224_obc2_nbc_seee),
	DIP_T(1632, 1224, 2048, 1, 2, tdr_1632x1224_obc2_seee),
	DIP_T(2336, 1752, 2928, 1, 3, tdr_2336x1752_obc2_nbc_seee),
	DIP_T(2336, 1752, 2928, 1, 2, tdr_2336x1752_obc2_seee),
	DIP_T(3264, 2448, 4080, 1, 3, tdr_3264x2448_obc2_nbc_seee),
	DIP_T(3264, 2448, 4080, 1, 2, tdr_3264x2448_obc2_seee),
	DIP_T(4672, 3504, 5840, 1, 3, tdr_4672x3504_obc2_nbc_seee),
	DIP_T(4672, 3504, 5840, 1, 2, tdr_4672x3504_obc2_seee),
	DIP_TC(1632, 1224, 2048, 1, 0, 816, 612, tdr_1632x1224_obc2_crz816x612),
	DIP_TC(1632, 1224, 2048, 1, 1, 816, 612, tdr_1632x1224_obc2_nbc_crz816x612),
	DIP_TC(1632, 1224, 2048, 1, 2, 816, 612, tdr_1632x1224_obc2_seee_crz816x612),
	DIP_TC(1632, 1224, 2048, 1, 3, 816, 612, tdr_1632x1224_obc2_nbc_seee_crz816x612),
	DIP_TC(2336, 1752, 2928, 1, 0, 960, 720, tdr_2336x1752_obc2_crz960x720),
	DIP_TC(2336, 1752, 2928, 1, 1, 960, 720, tdr_2336x1752_obc2_nbc_crz960x720),
	DIP_TC(2336, 1752, 2928, 1, 2, 960, 720, tdr_2336x1752_obc2_seee_crz960x720),
	DIP_TC(2336, 1752, 2928, 1, 3, 960, 720, tdr_2336x1752_obc2_nbc_seee_crz960x720),
#undef DIP_TC
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
	bool seee;			/* edge enhancement (SEEE's EE, sharpening) */
	int out_w, out_h;		/* CRZ: resize to this out of IMG2O (0: IMG3O, full size) */
	bool split;			/* one DIP start per tile (dip_run) */
	/* split: each tile's slot = its first `keep` words (0: all), then `pad`
	 * words of padval (~0: the tile's last word again, else padval | index) */
	int keep, pad;
	u32 padval;
	u32 tcm2;			/* TCM2 override for experiments, 0: auto */
	/* loader map probe: TCM0/TCM3 as given and a tile list of markers
	 * (DIP_PROBE_MARK | word index after a real first word) */
	bool probe;
	u32 tcm0, tcm3, probe_xor;	/* markers XOR probe_xor: diff two runs */
	u32 nbc_con1_clr;		/* bits cleared in ANR_CON1 (0x3700): the LCE/LTM links */
	bool dbg;			/* print the debug port after a good tile 0 */
	u32 int_en, cq_int_en;		/* CTL_INT_EN (0x20), CTL_CQ_INT_EN (0x24) */
	/* done by the DIP interrupt (CQ thread 0 done, CQ_STATUS bit 0), or
	 * NULL to poll; armed before every start */
	struct completion *done;
	u32 nbc_lut;			/* every word of the ANR LUT (0x3300-0x36fc) */
	u32 in_mva, out_mva;		/* IMGI (RAW10), IMG3O (YUYV) */
	u32 tdr_mva;			/* tile list in M4U memory */
	u32 smx_mva;			/* the SMX area (DIP_SMX_TOTAL), 0: none */
	void *tdr_vaddr;		/* where to copy the tile list */
	u32 desc_mva;			/* CQ descriptor (CQ_SLOTS * 8 bytes) */
	void *desc_vaddr;
};

/* CRZ algorithm 1's step for in -> out pixels (CrzDrvImp::CalAlgoAndCStep) */
static inline u32 dip_crz_step(u32 in, u32 out)
{
	return (((u64)(out - 1) << 20) + in - 2) / (in - 1);
}

static inline int dip_mods(const struct dip_job *j)
{
	return (j->nbc ? DIP_MOD_NBC : 0) | (j->seee ? DIP_MOD_SEEE : 0);
}

static inline void dip_w(void __iomem *dip, u32 off, u32 val)
{
	writel(val, dip + off);
}

static inline const struct dip_tdr_entry *dip_find_tdr_out(int w, int h, int stride,
							    bool obc2, int mods, int ow, int oh)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dip_tdrs); i++)
		if (dip_tdrs[i].w == w && dip_tdrs[i].h == h &&
		    dip_tdrs[i].stride == stride && dip_tdrs[i].obc2 == obc2 &&
		    dip_tdrs[i].mods == mods && dip_tdrs[i].ow == ow && dip_tdrs[i].oh == oh)
			return &dip_tdrs[i];
	return NULL;
}

static inline const struct dip_tdr_entry *dip_find_tdr(int w, int h, int stride,
						       bool obc2, int mods)
{
	return dip_find_tdr_out(w, h, stride, obc2, mods, 0, 0);
}

/*
 * Split mode. The tile loader takes as many words a tile as the TCM bits
 * enable registers, not the count in each tile's first word: after a frame
 * TDRI_XSIZE reads one tile's size (0x3f for 16 words) whatever was written.
 * NBC's tile list has 17 words a tile; with the base TCM bits the loader
 * takes 16, so tile 0 loads right and every later tile starts a word further
 * off (enables and IMGI's offset filled with tile words, docs/camera.md).
 * Split, each tile is its own one-tile list (its first word marked last) and
 * its own DIP start, so every tile loads as tile 0 does. Returns the number of
 * words in tile k's slot, from its first word's load size (bits 23:16, n - 1).
 */
static inline int dip_tile_words(const u32 *w)
{
	return ((w[0] >> 16) & 0xff) + 1;
}

#define DIP_TILE_LAST	BIT(5)		/* first word: LAST_IRQ, the frame ends */
/* words a tile the loader takes with the base TCM bits (DIP_TCM0/DIP_TCM1);
 * it writes TDRI_XSIZE back as that count's byte size - 1. A tile DMA size
 * that doesn't match stalls the loader. It fetches in 4-word bursts: with
 * TCM2 bit 13 (NBC) a 17-word tile is fetched as 20 and the last 3 dropped,
 * which dip_split_layout's slots allow for (NBC's word goes to ANR_CON1 19:16). */
#define DIP_TCM_WORDS	16
#define DIP_PROBE_MARK	0xa5a50000
#define DIP_SLOT	32		/* split: words per tile slot in the TDR buffer */

/* split: lay the tiles out one per slot; returns the tile count, *words the
 * words each slot loads */
static inline int dip_split_layout(const struct dip_job *j, const u32 *src, int nsrc,
				   int *words)
{
	u32 *w = j->tdr_vaddr;
	int i, k, n, keep, t = 0;

	for (i = 0; i < nsrc; i += n, t++) {
		u32 *slot = w + t * DIP_SLOT;

		n = dip_tile_words(src + i);
		keep = j->keep ? min(j->keep, n) : n;
		memcpy(slot, src + i, 4 * keep);
		for (k = 0; k < j->pad && keep + k < DIP_SLOT; k++)
			slot[keep + k] = j->padval == ~0u ? src[i + n - 1] : j->padval | k;
		/* the loader fetches 4-word bursts and drops what's past the tile */
		for (k = keep + j->pad; k < DIP_SLOT && k % 4; k++)
			slot[k] = 0;
		slot[0] |= DIP_TILE_LAST;
		*words = min(round_up(keep + j->pad, 4), DIP_SLOT);
	}
	return t;
}

/*
 * Program the DIP for one frame and start it (CQ thread 0, an all-NOP
 * descriptor; every register is written directly). Returns 0, or -EINVAL
 * when no built-in tile list matches. dip_wait() then polls for the end;
 * dip_run() does both and runs the remaining tiles of a split job.
 */
static inline int dip_program(void __iomem *dip, const struct dip_job *j)
{
	const struct dip_tuning *t = &dip_tuning[j->sensor & 1];
	const struct dip_tdr_entry *tdr;
	u32 *desc = j->desc_vaddr;
	u32 *w = j->tdr_vaddr;
	int i, words = 0;

	tdr = dip_find_tdr_out(j->width, j->height, j->stride, j->black >= 0, dip_mods(j),
			       j->out_w, j->out_h);
	if (!tdr)
		return -EINVAL;
	memcpy(j->tdr_vaddr, tdr->words, tdr->size);
	if (j->probe)
		for (i = 1; i < tdr->size / 4; i++)
			w[i] = (DIP_PROBE_MARK | i) ^ j->probe_xor;
	if (j->split)
		dip_split_layout(j, tdr->words, tdr->size / 4, &words);

	for (i = 0; i < ARRAY_SIZE(dip_init_regs); i++)
		dip_w(dip, dip_init_regs[i][0], dip_init_regs[i][1]);
	dip_w(dip, 0x020, j->int_en);		/* CTL_INT_EN */
	dip_w(dip, 0x024, j->cq_int_en | (j->done ? DIP_CQ_DONE : 0));	/* CTL_CQ_INT_EN */

	dip_w(dip, DIP_TDR_CTL, 0xb);
	dip_w(dip, DIP_TDR_TILE, 0);
	/* CRZ out of IMG2O: IMG3O's TCM bit (13) off, IMG2O's (11) on, as the HAL's */
	dip_w(dip, DIP_TDR_TCM, j->probe ? j->tcm0 :
	      j->out_w ? (DIP_TCM0 & ~BIT(13)) | BIT(11) : DIP_TCM0);
	/* NBC: TCM2 bit 13, its tile word (NBC_TILE_EDGE) to ANR_CON1 19:16;
	 * SEEE: bit 12 (configPipe's module -> TCM map, docs/camera.md) */
	dip_w(dip, DIP_TDR_TCM2, j->tcm2 ? j->tcm2 :
	      DIP_TCM1 | (j->nbc ? 0x2000 : 0) | (j->seee ? 0x1000 : 0) | (j->out_w ? BIT(0) : 0));
	dip_w(dip, DIP_TDR_TCM3, j->probe ? j->tcm3 : 0);
	/* the rest of the tile control block, as the HAL's image has it (the
	 * vendor driver's "tdr ctrl" dump: 0x50-0x68, 0xe0, 0xe4) */
	dip_w(dip, 0x060, 0);
	dip_w(dip, 0x064, 0);
	dip_w(dip, 0x068, 0);
	dip_w(dip, 0x0e4, 0);
	dip_w(dip, DIP_TDRI_BASE, j->tdr_mva);
	dip_w(dip, DIP_TDRI_OFST, 0);
	dip_w(dip, DIP_TDRI_XSIZE, (j->split ? 4 * words : tdr->size) - 1);

	dip_w(dip, DIP_YUV_EN, DIP_YUV_YUYV | (j->nbc ? BIT(5) : 0) | (j->seee ? BIT(8) : 0) |
	      (j->out_w ? BIT(17) : 0));
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
	if (j->nbc)			/* the whole NBC block, as the HAL's queue loads it (0x3300-0x37e4) */
		for (i = 0x3300; i < 0x37e8; i += 4)
			dip_w(dip, i, i < 0x3700 ? j->nbc_lut : 0);
	if (j->nbc)			/* noise reduction (NBC/ANR), per-sensor tuning */
		for (i = 0; i < t->nnbc; i++)
			dip_w(dip, t->nbc[i][0], t->nbc[i][0] == 0x3700 ?
			      t->nbc[i][1] & ~j->nbc_con1_clr : t->nbc[i][1]);
	if (j->seee)			/* edge enhancement: SEEE_CTRL 0x46c4 (EE 2) to 0x473c */
		for (i = 0; i < t->nseee; i++)
			dip_w(dip, t->seee[i][0], t->seee[i][1]);

	dip_w(dip, DIP_IMGI + 0x00, j->in_mva);
	dip_w(dip, DIP_IMGI + 0x08, 0);
	dip_w(dip, DIP_IMGI + 0x10, j->width * 10 / 8 - 1);
	dip_w(dip, DIP_IMGI + 0x14, j->height - 1);
	dip_w(dip, DIP_IMGI + 0x18, j->stride | DIP_IMGI_BUS);
	if (j->out_w) {
		/*
		 * CRZ resizes to out_w x out_h for IMG2O (CrzDrvImp, the HAL's
		 * image for 2336x1752 -> 960x720): CONTROL 0xa3 (both directions
		 * on, algorithm 1, 2x-8x down), IN and OUT sizes, the steps in
		 * 1/2^20 (CalAlgoAndCStep); the offsets come in the tile words.
		 */
		dip_w(dip, DIP_CRZ + 0x00, 0xa3);
		dip_w(dip, DIP_CRZ + 0x04, j->height << 16 | j->width);
		dip_w(dip, DIP_CRZ + 0x08, j->out_h << 16 | j->out_w);
		dip_w(dip, DIP_CRZ + 0x0c, dip_crz_step(j->width, j->out_w));
		dip_w(dip, DIP_CRZ + 0x10, dip_crz_step(j->height, j->out_h));
		for (i = 0x14; i <= 0x30; i += 4)
			dip_w(dip, DIP_CRZ + i, 0);
		dip_w(dip, DIP_IMG2O + 0x00, j->out_mva);
		dip_w(dip, DIP_IMG2O + 0x08, 0);
		dip_w(dip, DIP_IMG2O + 0x10, j->out_w * 2 - 1);
		dip_w(dip, DIP_IMG2O + 0x14, j->out_h - 1);
		dip_w(dip, DIP_IMG2O + 0x18, (j->out_w * 2) | DIP_IMG3O_BUS);
		dip_w(dip, DIP_IMG2O + 0x28, 0);	/* crop */
		dip_w(dip, DIP_DMA_EN, DMA_EN_IMGI | DMA_EN_IMG2O);
	} else {
		dip_w(dip, DIP_IMG3O + 0x00, j->out_mva);
		dip_w(dip, DIP_IMG3O + 0x08, 0);
		dip_w(dip, DIP_IMG3O + 0x10, j->width * 2 - 1);
		dip_w(dip, DIP_IMG3O + 0x14, j->height - 1);
		dip_w(dip, DIP_IMG3O + 0x18, (j->width * 2) | DIP_IMG3O_BUS);
		dip_w(dip, DIP_IMG3O + 0x28, 0);		/* crop */
		dip_w(dip, DIP_DMA_EN, DMA_EN_IMGI | DMA_EN_IMG3O);
	}
	if (j->smx_mva) {
		u32 a = j->smx_mva;

		for (i = 0; i < 4; i++) {
			dip_w(dip, DIP_SMXO(i), a);
			dip_w(dip, DIP_SMXO(i) + 0x08, 0);
			dip_w(dip, DIP_SMXI(i), a);
			dip_w(dip, DIP_SMXI(i) + 0x08, 0);
			a += (dip_smx_size[i] + 3) & ~3;
		}
	}

	for (i = 0; i < CQ_SLOTS; i++) {
		desc[2 * i] = CQ_NOP;
		desc[2 * i + 1] = j->desc_mva;
	}
	wmb();
	readl(dip + DIP_INT_STATUS);			/* read-clear */
	readl(dip + DIP_CQ_STATUS);
	if (j->done)
		reinit_completion(j->done);
	dip_w(dip, DIP_CTL_START, 1);
	return 0;
}

/* poll for the frame to finish; return 0 on completion, -ETIMEDOUT otherwise */
static inline int dip_wait(void __iomem *dip, int max_us);

/* the frame's (or tile's) end: the interrupt if the job has one, else polled */
static inline int dip_wait_job(void __iomem *dip, const struct dip_job *j, int max_us)
{
	if (!j->done)
		return dip_wait(dip, max_us);
	if (wait_for_completion_timeout(j->done, usecs_to_jiffies(max_us)))
		return 0;
	return readl(dip + DIP_CTL_START) ? -ETIMEDOUT : 0;
}

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

/*
 * The DIP's debug port, as the vendor driver's stall dump reads it
 * (camera_dip.c, isp_50): a select in DMA_DEBUG_SEL (0x7bc, DMA state:
 * IMGI 0x0013-0x0313, IMG3O 0x1813-0x1b13) or CTL_DBG_SET (0x80, module
 * state), the value in CTL_DBG_PORT (0x84). Selects as the vendor's.
 */
static inline void dip_debug_port(void __iomem *dip, const char *tag)
{
	static const u32 dma_sel[] = { 0x0013, 0x0113, 0x0213, 0x0313,
				       0x1813, 0x1913, 0x1a13, 0x1b13, 0x0301, 0x0101,
				       0x1013, 0x1113, 0x1213, 0x1313 };	/* + IMG2O */
	static const u32 ctl_sel[] = { 0x3016, 0x3017, 0x3018, 0x3019, 0x5100, 0x30514 };
	u32 v[ARRAY_SIZE(dma_sel) + ARRAY_SIZE(ctl_sel)];
	int i, n = 0;

	dip_w(dip, 0x080, 0);
	for (i = 0; i < ARRAY_SIZE(dma_sel); i++) {
		dip_w(dip, 0x7bc, dma_sel[i]);
		v[n++] = readl(dip + 0x084);
	}
	for (i = 0; i < ARRAY_SIZE(ctl_sel); i++) {
		dip_w(dip, 0x080, ctl_sel[i]);
		v[n++] = readl(dip + 0x084);
	}
	dip_w(dip, 0x080, 0);
	pr_info("dip: dbg %s: imgi %08x %08x %08x %08x img3o %08x %08x %08x %08x 301 %08x 101 %08x img2o %08x %08x %08x %08x ctl %08x %08x %08x %08x %08x %08x\n",
		tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
		v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19]);
}

/* what a stalled frame left: tile control, the enables, IMGI, DMA errors */
static inline void dip_stall_report(void __iomem *dip, int tile)
{
	pr_warn_ratelimited("dip: tile %d stalled: tdr %08x/%08x tdri %08x+%x/%x en %08x %08x %08x imgi %x err %08x %08x %08x nbc %08x\n",
			    tile, readl(dip + DIP_TDR_CTL), readl(dip + DIP_TDR_TILE),
			    readl(dip + DIP_TDRI_BASE), readl(dip + DIP_TDRI_OFST),
			    readl(dip + DIP_TDRI_XSIZE), readl(dip + DIP_YUV_EN),
			    readl(dip + DIP_RGB_EN), readl(dip + DIP_DMA_EN),
			    readl(dip + DIP_IMGI + 0x08), readl(dip + DIP_DMA_ERR),
			    readl(dip + DIP_DMA_ERR + 4), readl(dip + DIP_DMA_ERR + 8),
			    readl(dip + 0x3700));
	dip_debug_port(dip, "stall");
}

/*
 * Program, start and wait for one frame; a split job runs its tiles one DIP
 * start each, moving TDRI_BASE to the next tile (every other register
 * stays as programmed). 0, -EINVAL (no tile list) or -ETIMEDOUT.
 */
static inline int dip_run(void __iomem *dip, const struct dip_job *j, int max_us)
{
	const struct dip_tdr_entry *tdr;
	int ret, i, n, words;

	ret = dip_program(dip, j);
	if (ret)
		return ret;
	ret = dip_wait_job(dip, j, max_us);
	if (ret)
		dip_stall_report(dip, 0);
	else if (j->dbg) {
		pr_info("dip: done: int %08x cq %08x cq2 %08x\n", readl(dip + DIP_INT_STATUS),
			readl(dip + DIP_CQ_STATUS), readl(dip + 0x038));
		dip_debug_port(dip, "done");
	}
	if (ret || !j->split)
		return ret;
	tdr = dip_find_tdr_out(j->width, j->height, j->stride, j->black >= 0, dip_mods(j),
			       j->out_w, j->out_h);
	n = dip_split_layout(j, tdr->words, tdr->size / 4, &words);
	for (i = 1; i < n; i++) {
		dip_w(dip, DIP_TDRI_BASE, j->tdr_mva + 4 * DIP_SLOT * i);
		dip_w(dip, DIP_TDRI_OFST, 0);
		dip_w(dip, DIP_TDRI_XSIZE, 4 * words - 1);
		readl(dip + DIP_INT_STATUS);		/* read-clear */
		readl(dip + DIP_CQ_STATUS);
		if (j->done)
			reinit_completion(j->done);
		dip_w(dip, DIP_CTL_START, 1);
		ret = dip_wait_job(dip, j, max_us);
		if (ret) {
			dip_stall_report(dip, i);
			return ret;
		}
	}
	return 0;
}

#endif /* TITAN_DIP_CORE_H */
