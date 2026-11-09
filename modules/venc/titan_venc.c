// SPDX-License-Identifier: GPL-2.0
/*
 * titan_venc: the MT6771's H.264 hardware encoder (VENC, 0x17020000), first
 * test driver. One NV12 frame in, H.264 out, through debugfs:
 *
 *   echo "1280 720 4000 30 30" > /sys/kernel/debug/titan_venc/config
 *                                   width height kbit/s fps GOP (Baseline)
 *   cat frame.nv12 > .../in        luma pitch x height, then interleaved UV
 *   echo 1 > .../run; cat .../run  "frame N type bytes status us"
 *   cat .../out >> clip.h264       SPS + PPS before each I frame, then slice
 *   cat .../regs                   0x000-0x1fc
 *
 * The register sequence is the one MediaTek's driver library writes, found
 * by running it in emulation (tools/re/emu_venc.py; docs/video-encoder.md):
 * reset, sequence and frame settings, buffers, the start, then the frame-done
 * interrupt and the byte count. SPS and PPS are written here, with the
 * library's field choices, since the hardware writes slice headers that
 * depend on them. The rate control runs on the encoder's own processor,
 * from code the library carries: request_firmware("titan/venc_rc.bin"),
 * extracted from a phone's own vendor files (emu_venc.py --rc-out); it is
 * not distributed.
 */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#define VENC_MODE	0x000	/* mode word; written last before the start */
#define VENC_SEQ0	0x004	/* GOP, initial QP */
#define VENC_SEQ1	0x008	/* search and mode enables */
#define VENC_SEQ2	0x00c
#define VENC_FRM	0x010	/* frame type */
#define VENC_FRMNUM	0x014
#define VENC_FRMNUM2	0x018
#define VENC_LASTI	0x01c
#define VENC_SIZE	0x024	/* picture in macroblocks */
#define VENC_PITCH	0x028	/* pitch, crop */
#define VENC_PROFILE	0x030	/* entropy, profile, level */
#define VENC_RC0	0x048	/* rate control on, kbit/s */
#define VENC_RC1	0x04c	/* fps, QP steps */
#define VENC_RC2	0x050	/* QP limits */
#define VENC_START	0x058
#define VENC_IRQ_STATUS	0x05c
#define VENC_IRQ_ACK	0x060
#define VENC_BS_ADDR	0x064
#define VENC_BS_SIZE	0x068	/* in 128-byte units */
#define VENC_Y		0x06c
#define VENC_UV		0x070
#define VENC_REC_Y	0x074	/* recon luma, chroma; reference luma, chroma; MV out, in */
#define VENC_PLANE3	0x094
#define VENC_BS_BYTES	0x098
#define VENC_RC_CODE	0x08c
#define VENC_RC_STATE	0x090
#define VENC_IRQ_EN	0x0a4
#define VENC_RESET	0x0a8	/* 0: in reset */
#define VENC_PAUSE	0x0ac
#define VENC_CLKON	0x0ec
#define VENC_FRM2	0x124
#define VENC_FRM3	0x128
#define VENC_RCFG	0x12c
#define VENC_CMP	0x164	/* reference compression */
#define VENC_CMP_SIZE	0x168

#define IRQ_FRM		0x04
#define IRQ_DRAM	0x08	/* bitstream buffer full */
#define IRQ_PAUSE	0x10
#define IRQ_ALL		0xbf

#define RC_CODE_SIZE	0x4040
#define RC_STATE_SIZE	0x400

struct vbuf {
	void *va;
	dma_addr_t da;
	size_t size;
};

struct venc {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;
	struct completion done;
	u32 irq_status;
	struct mutex lock;
	struct dentry *dbg;

	bool active;
	u32 w, h, pw, ph, mbw, mbh, kbps, fps, gop, level;
	u32 frame, itoggle;
	struct vbuf in, bs, rc, rcst, rec_y[2], rec_c[2], mv[2], out;
	size_t out_len;
	char last[96];
};

static inline void vw(struct venc *v, u32 off, u32 val)
{
	writel(val, v->regs + off);
}

static inline u32 vr(struct venc *v, u32 off)
{
	return readl(v->regs + off);
}

static irqreturn_t venc_irq(int irq, void *data)
{
	struct venc *v = data;
	u32 st = vr(v, VENC_IRQ_STATUS);
	int b;

	for (b = 0; b < 8; b++)		/* acknowledged a bit at a time, as the vendor driver does */
		if (st & BIT(b))
			vw(v, VENC_IRQ_ACK, BIT(b));
	v->irq_status = st;
	complete(&v->done);
	return IRQ_HANDLED;
}

/* ---- buffers ---- */

static int vb_alloc(struct venc *v, struct vbuf *b, size_t size)
{
	b->size = ALIGN(size, 4096);
	b->va = dma_alloc_coherent(v->dev, b->size, &b->da, GFP_KERNEL);
	return b->va ? 0 : -ENOMEM;
}

static void vb_free(struct venc *v, struct vbuf *b)
{
	if (b->va)
		dma_free_coherent(v->dev, b->size, b->va, b->da);
	b->va = NULL;
}

static void venc_free(struct venc *v)
{
	int i;

	vb_free(v, &v->in);
	vb_free(v, &v->bs);
	vb_free(v, &v->rc);
	vb_free(v, &v->rcst);
	vb_free(v, &v->out);
	for (i = 0; i < 2; i++) {
		vb_free(v, &v->rec_y[i]);
		vb_free(v, &v->rec_c[i]);
		vb_free(v, &v->mv[i]);
	}
}

/* compressed-reference row sizes (the library's fields +0x110/+0x114) */
static u32 cmp_a(struct venc *v)
{
	return (0x815 * v->mbw + 0x3ff) >> 10;
}

static u32 cmp_b(struct venc *v)
{
	return (0x40f * v->mbw + 0x3ff) >> 10;
}

/* ---- SPS / PPS, with the library's field choices (Baseline, progressive) ---- */

struct bw {
	u8 *p;
	u32 bits;
};

static void put(struct bw *b, u32 val, int n)
{
	while (n--) {
		u32 byte = b->bits >> 3, bit = 7 - (b->bits & 7);

		if (bit == 7)
			b->p[byte] = 0;
		if ((val >> n) & 1)
			b->p[byte] |= BIT(bit);
		b->bits++;
	}
}

static void ue(struct bw *b, u32 val)
{
	u32 x = val + 1;
	int n = fls(x);

	put(b, 0, n - 1);
	put(b, x, n);
}

static void trailing(struct bw *b)
{
	put(b, 1, 1);
	while (b->bits & 7)
		put(b, 0, 1);
}

static size_t write_sps_pps(struct venc *v, u8 *p)
{
	struct bw b = { p, 0 };
	u32 area = v->w * v->h, prim, xfer, mat;
	u32 crop_r = v->pw - v->w, crop_b = v->ph - v->h;
	size_t sps;

	put(&b, 1, 32);			/* start code */
	put(&b, 0x67, 8);		/* SPS, nal_ref_idc 3 */
	put(&b, 66, 8);			/* Baseline */
	put(&b, 0, 8);			/* constraint flags, reserved */
	put(&b, v->level, 8);		/* level_idc */
	ue(&b, 0);			/* seq_parameter_set_id */
	ue(&b, 12);			/* log2_max_frame_num - 4 */
	ue(&b, 0);			/* pic_order_cnt_type */
	ue(&b, 12);			/* log2_max_pic_order_cnt_lsb - 4 */
	ue(&b, 1);			/* max_num_ref_frames */
	put(&b, 0, 1);			/* gaps */
	ue(&b, v->mbw - 1);
	ue(&b, v->mbh - 1);
	put(&b, 1, 1);			/* frame_mbs_only */
	put(&b, 1, 1);			/* direct_8x8_inference */
	if (crop_r || crop_b) {
		put(&b, 1, 1);
		ue(&b, 0);
		ue(&b, crop_r >> 1);
		ue(&b, 0);
		ue(&b, crop_b >> 1);
	} else {
		put(&b, 0, 1);
	}
	put(&b, 1, 1);			/* VUI */
	put(&b, 0, 1);			/* aspect ratio */
	put(&b, 0, 1);			/* overscan */
	put(&b, 1, 1);			/* video signal type */
	put(&b, 5, 3);			/* format unspecified */
	put(&b, 0, 1);			/* limited range */
	put(&b, 1, 1);			/* colour description */
	prim = area < 1280 * 720 ? 5 : area < 3840 * 2160 ? 1 : 9;
	xfer = area < 1280 * 720 ? 6 : 1;
	mat = area < 1280 * 720 ? 5 : 1;
	put(&b, prim, 8);
	put(&b, xfer, 8);
	put(&b, mat, 8);
	put(&b, 0, 1);			/* chroma location */
	put(&b, 0, 1);			/* timing */
	put(&b, 0, 1);			/* NAL HRD */
	put(&b, 0, 1);			/* VCL HRD */
	put(&b, 0, 1);			/* pic_struct */
	put(&b, 1, 1);			/* bitstream restriction */
	put(&b, 1, 1);			/* motion vectors over picture boundaries */
	ue(&b, 0);
	ue(&b, 0);
	ue(&b, 16);
	ue(&b, 16);
	ue(&b, 0);			/* max_num_reorder_frames */
	ue(&b, 2);			/* max_dec_frame_buffering */
	trailing(&b);
	sps = b.bits >> 3;

	put(&b, 1, 32);
	put(&b, 0x68, 8);		/* PPS */
	ue(&b, 0);
	ue(&b, 0);
	put(&b, 0, 1);			/* CAVLC */
	put(&b, 0, 1);			/* bottom_field_pic_order */
	ue(&b, 0);			/* slice groups */
	ue(&b, 1);			/* num_ref_idx_l0_default - 1 */
	ue(&b, 1);			/* num_ref_idx_l1_default - 1 */
	put(&b, 0, 1);			/* weighted_pred */
	put(&b, 0, 2);			/* weighted_bipred_idc */
	ue(&b, 0);			/* pic_init_qp - 26 (se 0) */
	ue(&b, 0);			/* pic_init_qs - 26 */
	ue(&b, 0);			/* chroma_qp_index_offset */
	put(&b, 1, 1);			/* deblocking filter control */
	put(&b, 0, 1);			/* constrained intra */
	put(&b, 0, 1);			/* redundant_pic_cnt */
	trailing(&b);
	(void)sps;
	return b.bits >> 3;
}

/* ---- session ---- */

static int venc_start(struct venc *v, u32 w, u32 h, u32 kbps, u32 fps, u32 gop)
{
	const struct firmware *fw;
	u32 luma, chroma, mv, rows;
	int i, ret;

	if (w < 64 || h < 64 || w > 1920 || h > 1088 || !fps || !gop || !kbps)
		return -EINVAL;
	v->w = w;
	v->h = h;
	v->pw = ALIGN(w, 16);
	v->ph = ALIGN(h, 16);
	v->mbw = v->pw / 16;
	v->mbh = v->ph / 16;
	v->kbps = kbps;
	v->fps = fps;
	v->gop = gop;
	v->level = v->mbw * v->mbh > 3600 ? 40 : 31;	/* above 720p: 4.0 */
	v->frame = 0;
	v->itoggle = 0;

	ret = request_firmware(&fw, "titan/venc_rc.bin", v->dev);
	if (ret)
		return ret;
	if (fw->size != RC_CODE_SIZE) {
		release_firmware(fw);
		return -EINVAL;
	}

	/* the library's sizes (H264_AllocWorkBuf), with reference compression */
	rows = ((v->ph + 0x1f) >> 4) & ~1u;
	luma = v->mbh * (cmp_a(v) + cmp_b(v)) * 0x80 + 0x200;
	chroma = rows * v->mbw * 0x80;
	mv = ALIGN(((w + 0x7f) >> 7) * v->mbh * 0x20 * 2, 128);
	ret = vb_alloc(v, &v->in, v->pw * v->ph * 3 / 2) ?:
	      vb_alloc(v, &v->bs, v->pw * v->ph) ?:
	      vb_alloc(v, &v->out, v->pw * v->ph + 256) ?:
	      vb_alloc(v, &v->rc, RC_CODE_SIZE) ?:
	      vb_alloc(v, &v->rcst, RC_STATE_SIZE);
	for (i = 0; i < 2 && !ret; i++)
		ret = vb_alloc(v, &v->rec_y[i], luma + 4096) ?:
		      vb_alloc(v, &v->rec_c[i], chroma + 4096) ?:
		      vb_alloc(v, &v->mv[i], mv + 4096);
	if (ret) {
		release_firmware(fw);
		venc_free(v);
		return ret;
	}
	memcpy(v->rc.va, fw->data, RC_CODE_SIZE);
	release_firmware(fw);

	ret = pm_runtime_resume_and_get(v->dev);
	if (ret) {
		venc_free(v);
		return ret;
	}
	ret = clk_prepare_enable(v->clk);
	if (ret) {
		pm_runtime_put(v->dev);
		venc_free(v);
		return ret;
	}
	v->active = true;
	return 0;
}

static void venc_stop(struct venc *v)
{
	int i;

	if (!v->active)
		return;
	/* the vendor's stop: pause, wait for the pause interrupt, reset */
	vw(v, VENC_PAUSE, 1);
	for (i = 0; i < 100 && !(vr(v, VENC_IRQ_STATUS) & IRQ_PAUSE); i++)
		udelay(10);
	vw(v, VENC_PAUSE, 0);
	vw(v, VENC_RESET, 0);
	vr(v, VENC_RESET);
	vw(v, VENC_RESET, 1);
	vw(v, VENC_IRQ_ACK, IRQ_PAUSE);
	clk_disable_unprepare(v->clk);
	pm_runtime_put(v->dev);
	venc_free(v);
	v->active = false;
}

/* one frame; returns the bytes written to v->out, or < 0 */
static int venc_frame(struct venc *v)
{
	bool idr = v->frame % v->gop == 0;
	u32 cur = v->frame & 1, ref = cur ^ 1, frm, st, n;
	size_t hdr = 0;
	ktime_t t0 = ktime_get();
	long left;

	/* reset */
	vw(v, VENC_RESET, 0);
	vw(v, VENC_RESET, 0);
	vr(v, VENC_RESET);
	vw(v, VENC_RESET, 1);
	vr(v, VENC_RESET);

	/* sequence (H264_SeqLvlMMRSetting) */
	vw(v, VENC_SEQ0, v->gop + (v->gop <= 30 ? v->gop << 14 : 0) + (22 << 26));
	vw(v, VENC_SEQ1, 0x626e5fae);
	vw(v, VENC_SEQ2, 0x104e0000);
	vw(v, VENC_SIZE, ((v->mbw << 16) + 0x100) * v->mbh + v->mbw);
	vw(v, VENC_PITCH, v->mbw << 21 | v->mbw << 11 | (v->pw - v->w) << 5 | (v->ph - v->h));
	vw(v, VENC_PROFILE, v->level << 22);	/* CAVLC, Baseline */
	vw(v, VENC_RC0, 0x40000 | v->kbps);
	vw(v, VENC_RC1, v->fps << 16 | 5 << 8 | 3);
	vw(v, VENC_RC2, 0x3308);
	vw(v, VENC_RCFG, 0x1f4);
	vw(v, VENC_CMP, 0x11);
	vw(v, VENC_CMP_SIZE, (cmp_a(v) + cmp_b(v)) << 16 | cmp_a(v));

	/* frame (H264_FrmLvlMMRSetting) */
	if (idr) {
		frm = 0x108400 | 0x3000 | 4 | 2 | v->itoggle << 3;
		v->itoggle ^= 1;
	} else {
		frm = 0x108400 | (v->frame % v->gop != 1 ? 0x100 : 0);
	}
	vw(v, VENC_FRM, frm);
	vw(v, VENC_FRMNUM, v->frame);
	vw(v, VENC_FRMNUM2, v->frame);
	vw(v, VENC_LASTI, v->frame / v->gop * v->gop);
	vw(v, VENC_RC0, 0x40000 | v->kbps);
	vw(v, VENC_RC1, v->fps << 16 | 5 << 8 | 3);
	vw(v, VENC_RC2, 0x3308);
	vw(v, VENC_FRM2, 0);
	vw(v, VENC_FRM3, 0);

	/* buffers */
	vw(v, VENC_RC_CODE, v->rc.da >> 4);
	vw(v, VENC_RC_STATE, v->rcst.da >> 4);
	vw(v, VENC_BS_SIZE, v->bs.size >> 7);
	vw(v, VENC_BS_ADDR, v->bs.da >> 4);
	vw(v, VENC_Y, v->in.da >> 4);
	vw(v, VENC_UV, (v->in.da + v->pw * v->ph) >> 4);
	vw(v, VENC_PLANE3, 0);
	vw(v, VENC_REC_Y + 0x00, v->rec_y[ref].da >> 4);	/* as the trace: frame 0 writes set 1 first */
	vw(v, VENC_REC_Y + 0x04, v->rec_c[ref].da >> 4);
	vw(v, VENC_REC_Y + 0x08, v->rec_y[cur].da >> 4);
	vw(v, VENC_REC_Y + 0x0c, v->rec_c[cur].da >> 4);
	vw(v, VENC_REC_Y + 0x10, v->mv[cur].da >> 4);
	vw(v, VENC_REC_Y + 0x14, v->mv[ref].da >> 4);

	/* go */
	reinit_completion(&v->done);
	v->irq_status = 0;
	vw(v, VENC_IRQ_EN, 0x3f);
	vw(v, VENC_MODE, 0x0f20bff0 | (v->frame ? BIT(23) : BIT(22)));
	vw(v, VENC_CLKON, 1);
	vw(v, VENC_START, 4);
	left = wait_for_completion_timeout(&v->done, msecs_to_jiffies(200));
	st = v->irq_status;
	n = vr(v, VENC_BS_BYTES);
	vw(v, VENC_CLKON, 0);

	snprintf(v->last, sizeof(v->last), "frame %u %c bytes %u status %#x us %lld%s\n",
		 v->frame, idr ? 'I' : 'P', n, st, ktime_us_delta(ktime_get(), t0),
		 left ? "" : " TIMEOUT");
	if (!left || !(st & IRQ_FRM) || n > v->bs.size)
		return -EIO;
	if (idr)
		hdr = write_sps_pps(v, v->out.va);
	memcpy(v->out.va + hdr, v->bs.va, n);
	v->out_len = hdr + n;
	v->frame++;
	return v->out_len;
}

/* ---- debugfs ---- */

static ssize_t config_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	struct venc *v = f->private_data;
	u32 w, h, kbps, fps, gop;
	char buf[64];
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	mutex_lock(&v->lock);
	venc_stop(v);
	if (sscanf(buf, "%u %u %u %u %u", &w, &h, &kbps, &fps, &gop) == 5)
		ret = venc_start(v, w, h, kbps, fps, gop);
	else
		ret = strncmp(buf, "stop", 4) ? -EINVAL : 0;
	mutex_unlock(&v->lock);
	return ret ?: len;
}

static ssize_t in_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	struct venc *v = f->private_data;
	ssize_t ret = -ENODEV;

	mutex_lock(&v->lock);
	if (v->active) {
		size_t room = v->pw * v->ph * 3 / 2;

		if (*pos >= room)
			ret = -ENOSPC;
		else if (copy_from_user(v->in.va + *pos, ubuf, min(len, room - (size_t)*pos)))
			ret = -EFAULT;
		else {
			ret = min(len, room - (size_t)*pos);
			*pos += ret;
		}
	}
	mutex_unlock(&v->lock);
	return ret;
}

static ssize_t run_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	struct venc *v = f->private_data;
	int ret = -ENODEV;

	mutex_lock(&v->lock);
	if (v->active)
		ret = venc_frame(v);
	mutex_unlock(&v->lock);
	return ret < 0 ? ret : len;
}

static ssize_t run_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	struct venc *v = f->private_data;

	return simple_read_from_buffer(ubuf, len, pos, v->last, strlen(v->last));
}

static ssize_t out_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	struct venc *v = f->private_data;
	ssize_t ret = 0;

	mutex_lock(&v->lock);
	if (v->active)
		ret = simple_read_from_buffer(ubuf, len, pos, v->out.va, v->out_len);
	mutex_unlock(&v->lock);
	return ret;
}

static ssize_t regs_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	struct venc *v = f->private_data;
	char *buf;
	int i, n = 0;
	ssize_t ret;

	buf = kzalloc(4096, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	mutex_lock(&v->lock);
	if (v->active)		/* only while powered: an unpowered read can hang the bus */
		for (i = 0; i < 0x200; i += 16)
			n += scnprintf(buf + n, 4096 - n, "%03x: %08x %08x %08x %08x\n", i,
				       vr(v, i), vr(v, i + 4), vr(v, i + 8), vr(v, i + 12));
	mutex_unlock(&v->lock);
	ret = simple_read_from_buffer(ubuf, len, pos, buf, n);
	kfree(buf);
	return ret;
}

static const struct file_operations config_fops = {
	.open = simple_open, .write = config_write,
};
static const struct file_operations in_fops = {
	.open = simple_open, .write = in_write,
};
static const struct file_operations run_fops = {
	.open = simple_open, .write = run_write, .read = run_read,
};
static const struct file_operations out_fops = {
	.open = simple_open, .read = out_read,
};
static const struct file_operations regs_fops = {
	.open = simple_open, .read = regs_read,
};

/* ---- platform ---- */

static int venc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct venc *v;
	int irq, ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->dev = dev;
	mutex_init(&v->lock);
	init_completion(&v->done);
	v->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(v->regs))
		return PTR_ERR(v->regs);
	v->clk = devm_clk_get(dev, "venc");
	if (IS_ERR(v->clk))
		return PTR_ERR(v->clk);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, venc_irq, 0, "titan-venc", v);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/* larb 4 powers and clocks with us */
	np = of_parse_phandle(dev->of_node, "mediatek,larb", 0);
	if (np) {
		struct platform_device *larb = of_find_device_by_node(np);

		of_node_put(np);
		if (!larb || !platform_get_drvdata(larb))
			return -EPROBE_DEFER;
		if (!device_link_add(dev, &larb->dev, DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS))
			return -EINVAL;
	}
	pm_runtime_enable(dev);
	platform_set_drvdata(pdev, v);

	v->dbg = debugfs_create_dir("titan_venc", NULL);
	debugfs_create_file("config", 0200, v->dbg, v, &config_fops);
	debugfs_create_file("in", 0200, v->dbg, v, &in_fops);
	debugfs_create_file("run", 0600, v->dbg, v, &run_fops);
	debugfs_create_file("out", 0400, v->dbg, v, &out_fops);
	debugfs_create_file("regs", 0400, v->dbg, v, &regs_fops);
	dev_info(dev, "H.264 encoder ready (debugfs titan_venc)\n");
	return 0;
}

static void venc_remove(struct platform_device *pdev)
{
	struct venc *v = platform_get_drvdata(pdev);

	debugfs_remove_recursive(v->dbg);
	mutex_lock(&v->lock);
	venc_stop(v);
	mutex_unlock(&v->lock);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id venc_of_match[] = {
	{ .compatible = "mediatek,mt6771-venc" },
	{ }
};
MODULE_DEVICE_TABLE(of, venc_of_match);

static struct platform_driver venc_driver = {
	.probe = venc_probe,
	.remove_new = venc_remove,
	.driver = { .name = "titan-venc", .of_match_table = venc_of_match },
};
module_platform_driver(venc_driver);

MODULE_DESCRIPTION("MT6771 H.264 hardware encoder (test driver)");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("titan/venc_rc.bin");
