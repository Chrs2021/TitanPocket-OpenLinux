// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 modem (MD1): power it on, start the image LK left in memory, do
 * the boot handshake, and pass the modem's packets to userspace.
 *
 * LK loads and checks the modem image, sets the MD's memory protection
 * and address remap, and describes all of it in a tag block that /chosen's
 * ccci,modem_info_v2 points to (the vendor's ccci_util parses the same).
 * What is left to the kernel, as in the vendor eccci driver (mt6771/
 * md_sys1_platform.c, hif/ccci_hif_ccif.c, ccci_modem.c):
 *
 *  - the CCIF ring buffers in the non-cached AP/MD shared memory (the
 *    modem gen 6293's control, AT, file and log channels all use CCIF;
 *    CLDMA carries only network data),
 *  - the MD1 power domain, the modem's clock mode and 26 MHz request,
 *    its PLLs, and the boot slave release,
 *  - handshake 1: the modem posts its feature query in the CCIF SRAM and
 *    raises CCIF channel 15; the AP answers with its runtime data (where
 *    each shared memory region is, the time, a random seed, ...) the same
 *    way. Handshake 2 is a control channel message once the modem has
 *    finished starting, which needs the AP to serve its NVRAM files.
 *
 * Everything after handshake 1 is a CCCI packet (a 16-byte header with
 * the channel, then the payload) on one of eight CCIF rings; /dev/titan-
 * modem reads and writes them, one packet per call, for a userspace
 * daemon (the file server, RPC). The kernel keeps the control channel's
 * boot and exception messages and forwards the other packets, except
 * the AT channel once the daemon asks (TITAN_MD_AT_PORT, after its own
 * start-up commands): from then it is a WWAN AT port (wwan0at0) for
 * ModemManager.
 *
 * Load by hand; unloading powers the modem off again.
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/wwan.h>

/* SPM (scpsys) */
#define MD1_PWR_CON		0x320
#define PWR_STATUS		0x180
#define PWR_STATUS_2ND		0x184
#define MD_SRAM_ISO_CON		0x394
#define MD_EXTRA_PWR_CON	0x398
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define MD1_SRAM_PDN		BIT(8)
#define MD1_PWR_STA		BIT(0)

/* INFRACFG_AO */
#define INFRA_RST0		0x150	/* PD domain resets: CCIF0 bit 8 */
#define INFRA_RST1		0x154
#define CCIF0_RST		BIT(8)
#define PROTECTEN_STA1		0x228
#define PROTECTEN_1_STA1	0x258
#define PROTECTEN_SET		0x2a0
#define PROTECTEN_CLR		0x2a4
#define PROTECTEN_1_SET		0x2a8
#define PROTECTEN_1_CLR		0x2ac
#define AP2MD_DUMMY		0x370
#define MD_SRCCLKENA		0xf0c
/* bus protection: AP->MD is bit 7 of PROTECTEN, MD->AP bit 6 of _1 */
#define PROT_PERI2MD		BIT(7)
#define PROT_MD1_STEP2		(BIT(3) | BIT(4))
#define PROT_MD2PERI		BIT(6)

/* TOPCKGEN CLK_MODE: bits 8, 9 gate the modem's clocks */
#define CLK_MODE		0x000
#define CLK_MODE_MD_GATE	(BIT(8) | BIT(9))

/* modem-side registers (offsets into the DT reg windows) */
#define MD_BOOT_VECTOR_EN_OFF	0x24	/* into "md-boot" */
#define WDT_MD_MODE_KEY		0x55000030	/* "md-rgu" + 0 */

/* CCIF */
#define APCCIF_BUSY		0x04
#define APCCIF_TCHNUM		0x0c
#define APCCIF_RCHNUM		0x10
#define APCCIF_ACK		0x14
#define APCCIF_CHDATA		0x100
#define CCIF_SRAM_SIZE		512
#define CCIF_SRAM		15	/* the handshake, both ways */

/* CCCI channels (ccci_core.h) */
#define CCCI_CONTROL_RX		0
#define CCCI_CONTROL_TX		1
#define CCCI_SYSTEM_TX		3
#define CCCI_PCM_TX		5
#define CCCI_UART1_TX		8
#define CCCI_UART2_RX		10
#define CCCI_UART2_TX		12
#define CCCI_FS_TX		15
#define CCCI_RPC_TX		33
#define CCCI_IPC_TX		36
#define CCCI_MD_LOG_TX		61
#define CCCI_STATUS_TX		67
#define CCCI_CH_MAX		128

/* control channel messages (mtk_ccci_common.h) */
#define MD_INIT_START_BOOT	0x0
#define MD_EX			0x4
#define MD_EX_REC_OK		0x6
#define MD_EX_PASS		0x8
#define MD_INIT_CHK_ID		0x5555ffff

struct ccci_header {
	u32 data[2];
	u16 channel;
	u16 seq_num:15;
	u16 assert_bit:1;
	u32 reserved;
} __packed;

/* the handshake (ccci_modem.h); feature masks are support:4, version:4 */
#define FEATURE_COUNT		64
#define MD_FEATURE_QUERY_PATTERN 0x49434343
#define AP_FEATURE_QUERY_PATTERN 0x43434349

enum { F_NOT_EXIST, F_NOT_SUPPORT, F_MUST, F_OPTIONAL, F_BACKWARD };

enum {
	BOOT_INFO = 0, EXCEPTION_SHARE_MEMORY, CCIF_SHARE_MEMORY,
	SMART_LOGGING_SHARE_MEMORY, MD1MD3_SHARE_MEMORY,
	MISC_INFO_HIF_DMA_REMAP, MISC_INFO_RTC_32K_LESS,
	MISC_INFO_RANDOM_SEED_NUM, MISC_INFO_GPS_COCLOCK, MISC_INFO_SBP_ID,
	MISC_INFO_CCCI, MISC_INFO_CLIB_TIME, MISC_INFO_C2K,
	MD_IMAGE_START_MEMORY, CCISM_SHARE_MEMORY, CCB_SHARE_MEMORY,
	DHL_RAW_SHARE_MEMORY, DT_NETD_SHARE_MEMORY, DT_USB_SHARE_MEMORY,
	EE_AFTER_EPOF, AP_CCMNI_MTU, MISC_INFO_CUSTOMER_VAL = 21,
	CCCI_FAST_HEADER = 22, MISC_INFO_C2K_MEID = 24, LWA_SHARE_MEMORY = 25,
	AUDIO_RAW_SHARE_MEMORY = 26, MULTI_MD_MPU = 27,
	CCISM_SHARE_MEMORY_EXP = 28, MD_PHY_CAPTURE = 29,
	MD_CONSYS_SHARE_MEMORY = 30, MD_USIP_SHARE_MEMORY = 31,
	MD_MTEE_SHARE_MEMORY_ENABLE = 32, MD_POS_SHARE_MEMORY = 33,
	UDC_RAW_SHARE_MEMORY = 34,
};

struct md_query_ap_feature {
	u32 head_pattern;
	u8 feature_set[FEATURE_COUNT];
	u32 tail_pattern;
	u8 reserved[152];
} __packed;

struct ap_query_md_feature_v2_1 {
	u32 head_pattern;
	u8 feature_set[FEATURE_COUNT];
	u32 share_memory_support;	/* 2: multiple MD MPU regions */
	u32 ap_runtime_data_addr;
	u32 ap_runtime_data_size;
	u32 md_runtime_data_addr;
	u32 md_runtime_data_size;
	u32 noncached_mpu_start_addr;
	u32 noncached_mpu_total_size;
	u32 cached_mpu_start_addr;
	u32 cached_mpu_total_size;
	u32 reserve_addr[12];
	u32 tail_pattern;
} __packed;

/* the CCIF SRAM during handshake 1 */
#define SRAM_DL_HEADER		0
#define SRAM_MD_QUERY		16
#define SRAM_UP_HEADER		(SRAM_MD_QUERY + sizeof(struct md_query_ap_feature))
#define SRAM_AP_DATA		(SRAM_UP_HEADER + 16)

struct rt_feature {
	u8 feature_id;
	u8 support_info;
	u8 reserved[2];
	u32 data_len;
} __packed;

/* the CCIF ring buffers (hif/ccci_ringbuf.c) */
#define CCCI_RBF_HEADER		0xee0000ee
#define CCCI_RBF_FOOTER		0xff0000ff
#define CCIF_PKG_HEADER		0xaabbaabb
#define CCIF_PKG_FOOTER		0xccddeeff
/* rx read, write, length, then tx: 24 bytes after the 8-byte guard */
#define RB_RX_READ		0
#define RB_RX_WRITE		4
#define RB_RX_LEN		8
#define RB_TX_READ		12
#define RB_TX_WRITE		16
#define RB_TX_LEN		20
#define RB_BUF			24
#define RINGBUF_CTL_LEN		(8 + RB_BUF + 8)
#define QUEUE_NUM		8

static const int rx_queue_size[QUEUE_NUM] = {
	80 * 1024, 80 * 1024, 40 * 1024, 80 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
};
static const int tx_queue_size[QUEUE_NUM] = {
	128 * 1024, 40 * 1024, 8 * 1024, 40 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
};
static const int rx_exp_size[QUEUE_NUM] = {
	12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0,
};
static const int tx_exp_size[QUEUE_NUM] = {
	12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0,
};

/* the non-cached shared memory (ccci_modem.c md1_6293_noncacheable_fat) */
#define SMEM_MDCCCI_DBG		0
#define SMEM_MDSS_DBG		(2 * 1024)
#define SMEM_MDSS_DBG_SIZE	(10 * 1024)
#define SMEM_RUNTIME_DATA	(58 * 1024)
#define SMEM_RUNTIME_AP_SIZE	0x800
#define SMEM_RUNTIME_MD_SIZE	0x800
#define SMEM_DBM_SIZE		160
#define SMEM_DBM		(64 * 1024 - SMEM_DBM_SIZE - 2 * 8)
#define SMEM_CCISM_SCP		(64 * 1024)
#define SMEM_CCISM_SCP_SIZE	(32 * 1024)
#define SMEM_CCB_CTRL		(96 * 1024)
#define SMEM_CCB_CTRL_SIZE	(4 * 1024)
#define SMEM_NETD		(100 * 1024)
#define SMEM_USB		(104 * 1024)
#define SMEM_AUDIO		(108 * 1024)
#define SMEM_AUDIO_SIZE		(52 * 1024)
#define SMEM_CCISM_MCU		(160 * 1024)
#define SMEM_CCISM_EXP		(881 * 1024)
#define SMEM_CCISM_EXP_SIZE	(121 * 1024)
#define SMEM_LWA		(1024 * 1024)
#define EE_SMEM_TOTAL_SIZE	(64 * 1024)
/* the cached part: CCB users in the first 2 MB, the DHL log after */
#define CCB_CTRL_PART		(2 * 1024 * 1024)
#define CCB_USERS_SIZE		0x178000	/* DHL 0x70000, MDM, META */
#define CCB_HDR_PATTERN		0xeeff0011

/*
 * The CCB buffers (MT6771 ccb_configs): page and buffer sizes, modem to
 * AP (DL) and AP to modem (UL). The DHL user's control, exception, PS
 * and three hardware-logger buffers, padding, then the MD monitor and
 * META users; the data follows the same order from the CCB start.
 */
static const struct { u32 dl_page, ul_page, dl_size, ul_size; } ccb_bufs[] = {
	{ 1024, 1024, 32 * 1024, 32 * 1024 },		/* DHL control */
	{ 20 * 1024, 128, 6 * 20 * 1024, 128 },		/* exception */
	{ 512, 1024, 72 * 512, 32 * 1024 },		/* PS */
	{ 512, 128, 64 * 512, 128 },			/* hw logger 1 */
	{ 512, 128, 64 * 512, 128 },			/* hw logger 2 */
	{ 512, 128, 256 * 512, 128 },			/* hw logger 3 */
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 5 * 128, 128 },
	{ 512, 1024, 32 * 512, 16 * 1024 },		/* MD monitor */
	{ 65 * 1024, 63 * 1024, 8 * 65 * 1024, 8 * 63 * 1024 },	/* META */
};
#define CCMNI_MTU		(3584 - 16)

/* LK's tags (ccci_util_lib_fo.c), with the vendor structs' padding */
struct ccci_tag_v2 {
	char name[64];
	u32 data_offset;
	u32 data_size;
	u32 next_tag_offset;
};

struct lk_modem_info {
	u64 base;
	u32 size;
	u8 md_id, err, md_type, ver;
	u32 reserved[2];
} __packed;

struct lk_smem_layout {
	u64 base;
	u32 ap_md1_offset, ap_md1_size;
	u32 ap_md3_offset, ap_md3_size;
	u32 md1_md3_offset, md1_md3_size;
	u32 total;
};

struct lk_ccb_layout {
	u64 base;
	u32 size;
};

/* /chosen ccci,modem_info_v2: LK writes its struct as is (little endian) */
struct lk_info_v2 {
	__le64 base;
	__le32 size;
	__le32 err;
	__le32 version;
	__le32 tag_num;
	__le32 ld_flag;
} __packed;

#define RX_QUEUE_MAX		512	/* packets waiting for userspace */
#define AT_TX_MAX		1024	/* AT payload bytes per packet */
/* /dev/titan-modem: hand the AT channel to a WWAN port (after handshake 2) */
#define TITAN_MD_AT_PORT	_IO('t', 0x01)

struct titan_md {
	struct device *dev;
	struct regmap *spm, *infra, *topck, *apmixed;
	void __iomem *ap_ccif, *md_ccif;
	/* MD-internal blocks the boot sequence reaches (all AP-visible) */
	void __iomem *md_pll, *md_clksw, *md_rgu, *md_boot, *md_cfg;
	int irq, irq_ex;

	phys_addr_t img_base, smem_base, ccb_base;
	u32 img_size, img_raw_size, smem_size, ccb_size, ccb_md_offset;
	u32 smem_md_view, ccb_md_view;
	void __iomem *smem;		/* write-combined alias */
	void __iomem *rings[QUEUE_NUM];
	u32 ccism_size, ccism_exp_size;

	bool powered, hs1_done, ready;
	unsigned long rchnum;
	struct delayed_work watch;
	int watch_ticks;
	struct work_struct rx_work;
	spinlock_t tx_lock;
	u16 tx_seq[CCCI_CH_MAX];

	struct dentry *debugfs;
	struct debugfs_blob_wrapper smem_blob, sram_blob, ccb_blob, dsp_blob;
	u8 img_type;		/* LK's modem type (12: ULWCTG) */
	void *ccb;		/* the cached region, write-combined */
	u8 sram_copy[CCIF_SRAM_SIZE];

	struct miscdevice misc;
	struct sk_buff_head rxq;
	wait_queue_head_t rx_wait;
	atomic_t open;

	/* the AT channel as a WWAN port, from TITAN_MD_AT_PORT */
	struct work_struct at_work;
	struct wwan_port *at_port;
	bool at_open;
};

static struct titan_md *tmd;

/*
 * The booting start id (ccci_modem.c get_booting_start_id): boot mode in
 * bits 0-7 (NORMAL_BOOT_ID 0, META_BOOT_ID 1, factory 2), logging mode in
 * 8-15, the logger wait time in 16-31. Only for experiments: META mode
 * tolerates a blank NVRAM (no IMEI, no calibration).
 */
static uint boot_id;
module_param(boot_id, uint, 0444);

/* offer the CCB (see ap_feature); and its users' total size */
static bool ccb = true;
module_param(ccb, bool, 0444);
static uint ccb_users = CCB_USERS_SIZE;
module_param(ccb_users, uint, 0444);

/* optional features to answer "not supported" (bit = feature id), to test */
static unsigned long long decline;
module_param(decline, ullong, 0444);

/* the CLDMA and CCIF0-2 infra clocks, by the DT node's clock-names */
static const char * const clk_names[] = {
	"cldma", "ccif-ap", "ccif-md",
	"ccif1-ap", "ccif1-md", "ccif2-ap", "ccif2-md",
};

/* ---- LK's description of the modem ---- */

static int find_tag(void *blk, u32 len, const char *name, void *out, u32 size)
{
	u32 off = 0;

	while (off + sizeof(struct ccci_tag_v2) <= len) {
		struct ccci_tag_v2 *t = blk + off;

		if (!strncmp(t->name, name, sizeof(t->name))) {
			if (t->data_size != size ||
			    t->data_offset + size > len)
				return -EINVAL;
			memcpy(out, blk + t->data_offset, size);
			return 0;
		}
		if (t->next_tag_offset <= off)
			break;
		off = t->next_tag_offset;
	}
	return -ENOENT;
}

static int read_lk_tags(struct titan_md *md)
{
	struct device *dev = md->dev;
	const struct lk_info_v2 *info;
	struct lk_smem_layout smem;
	struct lk_ccb_layout ccb;
	struct lk_modem_info hdr;
	struct device_node *np;
	u32 ld_flag, len;
	void *blk;
	int ret, plen;

	np = of_find_node_by_path("/chosen");
	info = of_get_property(np, "ccci,modem_info_v2", &plen);
	of_node_put(np);
	if (!info || plen < sizeof(*info))
		return dev_err_probe(dev, -ENODEV, "no ccci,modem_info_v2\n");
	if (le32_to_cpu(info->err) || !(le32_to_cpu(info->ld_flag) & 1))
		return dev_err_probe(dev, -ENODEV,
				     "LK did not load the modem (%#x, %#x)\n",
				     le32_to_cpu(info->err),
				     le32_to_cpu(info->ld_flag));

	len = le32_to_cpu(info->size);
	blk = memremap(le64_to_cpu(info->base), len, MEMREMAP_WB);
	if (!blk)
		return -ENOMEM;
	ret = find_tag(blk, len, "hdr_tbl_inf", &hdr, sizeof(hdr)) ?:
	      find_tag(blk, len, "smem_layout", &smem, sizeof(smem)) ?:
	      find_tag(blk, len, "ccb_info", &ccb, sizeof(ccb)) ?:
	      find_tag(blk, len, "md1_smem_cahce_offset", &md->ccb_md_offset,
		       sizeof(u32)) ?:
	      find_tag(blk, len, "md1img", &md->img_raw_size, sizeof(u32)) ?:
	      find_tag(blk, len, "hdr_count", &ld_flag, sizeof(ld_flag));
	memunmap(blk);
	if (ret)
		return dev_err_probe(dev, ret, "LK tags\n");
	if (hdr.err || !ld_flag)
		return dev_err_probe(dev, -ENODEV, "modem image error %d\n",
				     hdr.err);

	md->img_base = hdr.base;
	md->img_size = hdr.size;
	md->img_type = hdr.md_type;
	md->smem_base = smem.base + smem.ap_md1_offset;
	md->smem_size = smem.ap_md1_size;
	md->ccb_base = ccb.base;
	md->ccb_size = ccb.size;
	/* the modem's bank 4 view (ccci_modem.c ccci_md_config) */
	md->smem_md_view = 0x40000000 + md->smem_base -
			   round_down(md->smem_base, SZ_32M);
	md->ccb_md_view = 0x40000000 + md->ccb_md_offset + md->ccb_base -
			  round_down(md->ccb_base, SZ_1M);
	dev_info(dev, "image %pa+%#x (%#x, type %u); shared %pa+%#x at %#x, cached %pa+%#x at %#x\n",
		 &md->img_base, md->img_size, md->img_raw_size, hdr.md_type,
		 &md->smem_base, md->smem_size, md->smem_md_view,
		 &md->ccb_base, md->ccb_size, md->ccb_md_view);
	if (md->smem_size < SMEM_CCISM_EXP + SMEM_CCISM_EXP_SIZE ||
	    md->ccb_size < CCB_CTRL_PART)
		return -EINVAL;
	return 0;
}

/*
 * The shared memory is ordinary RAM that LK reserves without no-map, so
 * it is in the kernel's cacheable linear map and cannot be ioremapped.
 * The modem does not snoop the AP's caches, and each ring's control words
 * for both directions share a cache line, so it is used through a
 * write-combined alias, as the vendor driver does (vmap_reserved_mem),
 * after dropping whatever the linear alias holds of it.
 */
static void *wc_map(struct device *dev, phys_addr_t base, size_t size)
{
	unsigned int i, n = size >> PAGE_SHIFT;
	struct page **pages;
	dma_addr_t dma;
	void *va;

	dma = dma_map_page(dev, phys_to_page(base), 0, size, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dma))
		return NULL;
	dma_unmap_page(dev, dma, size, DMA_FROM_DEVICE);

	pages = kmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;
	for (i = 0; i < n; i++)
		pages[i] = phys_to_page(base + i * PAGE_SIZE);
	va = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kfree(pages);
	return va;
}

static int map_smem(struct titan_md *md)
{
	md->smem = (void __iomem *)wc_map(md->dev, md->smem_base,
					  md->smem_size);
	/*
	 * The cached region (CCB users, the DHL log): cleared before each
	 * boot, and in debugfs, the way the modem wrote it.
	 */
	md->ccb = wc_map(md->dev, md->ccb_base, md->ccb_size);
	return md->smem && md->ccb ? 0 : -ENOMEM;
}

/* ---- the ring buffers ---- */

static void __iomem *ring_create(void __iomem *buf, int rx, int tx)
{
	int len = RINGBUF_CTL_LEN + rx + tx;

	memset_io(buf, 0, len);
	writel(CCCI_RBF_HEADER, buf);
	writel(CCCI_RBF_HEADER, buf + 4);
	writel(CCCI_RBF_FOOTER, buf + len - 4);
	writel(CCCI_RBF_FOOTER, buf + len - 8);
	writel(rx, buf + 8 + RB_RX_LEN);
	writel(tx, buf + 8 + RB_TX_LEN);
	return buf + 8;
}

static u32 init_ring_set(void __iomem *p, const int *rx, const int *tx,
			 void __iomem **rings)
{
	u32 total = 0;
	int i;

	for (i = 0; i < QUEUE_NUM; i++) {
		void __iomem *rb = ring_create(p + total, rx[i], tx[i]);

		if (rings)
			rings[i] = rb;
		total += RINGBUF_CTL_LEN + rx[i] + tx[i];
	}
	return total;
}

/*
 * The CCB buffer headers, 64 bytes each in the control block: a DL half
 * and a UL half, each ending with the page size, the buffer size and a
 * pattern. Android's ccci_mdinit writes them (libccci_util
 * ccci_ccb_init_users) before it starts the modem; without them the
 * modem's buffer manager waits and asserts.
 */
static void init_ccb_headers(struct titan_md *md)
{
	void __iomem *h = md->smem + SMEM_CCB_CTRL;
	int i;

	for (i = 0; i < ARRAY_SIZE(ccb_bufs); i++, h += 64) {
		writel(ccb_bufs[i].dl_page, h + 0x14);
		writel(ccb_bufs[i].dl_size, h + 0x18);
		writel(CCB_HDR_PATTERN, h + 0x1c);
		writel(ccb_bufs[i].ul_page, h + 0x34);
		writel(ccb_bufs[i].ul_size, h + 0x38);
		writel(CCB_HDR_PATTERN, h + 0x3c);
	}
}

/*
 * The speech shared memory (AUDIO_RAW_SHARE_MEMORY), formatted as the
 * stock audio HAL does once per boot (SpeechExtMemCCCI::formatShareMemory):
 * 32-byte guards of 0x0a at both ends, the AP and modem flags, then three
 * rings (offset, size, read and write index): speech parameters, AP data
 * and modem data; the AP flag's bit 0 says the AP side is formatted.
 */
static const u32 sph_shm_rings[][2] = {
	{ 0x80, 0x3000 }, { 0x3080, 0x2000 }, { 0x5080, 0x7f60 },
};

static void init_audio_shm(struct titan_md *md)
{
	void __iomem *p = md->smem + SMEM_AUDIO;
	int i;

	for (i = 0; i < 32; i += 4) {
		writel(0x0a0a0a0a, p + i);
		writel(0x0a0a0a0a, p + SMEM_AUDIO_SIZE - 32 + i);
	}
	for (i = 0; i < ARRAY_SIZE(sph_shm_rings); i++) {
		writel(sph_shm_rings[i][0], p + 0x28 + i * 16);
		writel(sph_shm_rings[i][1], p + 0x2c + i * 16);
	}
	writel(0x7c, p + 0x7c);
	writel(1, p + 0x20);
}

static void init_rings(struct titan_md *md)
{
	/* the modem clears the shared memory itself after its first boot */
	memset_io(md->smem, 0, md->smem_size);
	/*
	 * The cached region too, as the vendor's first boot does
	 * (clear_smem_region): the CCB users' buffer headers live there,
	 * and left as LK found the DRAM (mostly 0xff), the modem's
	 * shared-memory buffer manager asserted (ccci_shm_bm.c:584).
	 */
	if (md->ccb)
		memset(md->ccb, 0, md->ccb_size);
	if (ccb)
		init_ccb_headers(md);
	init_audio_shm(md);
	md->ccism_size = init_ring_set(md->smem + SMEM_CCISM_MCU,
				       rx_queue_size, tx_queue_size, md->rings);
	/* used by the modem for its exception dump only */
	md->ccism_exp_size = init_ring_set(md->smem + SMEM_CCISM_EXP,
					   rx_exp_size, tx_exp_size, NULL);
}

static void ring_copy_from(void __iomem *buf, u32 len, u32 pos, void *dst,
			   u32 n)
{
	u32 first = min(n, len - pos);

	memcpy_fromio(dst, buf + pos, first);
	if (n > first)
		memcpy_fromio(dst + first, buf, n - first);
}

static void ring_copy_to(void __iomem *buf, u32 len, u32 pos, const void *src,
			 u32 n)
{
	u32 first = min(n, len - pos);

	memcpy_toio(buf + pos, src, first);
	if (n > first)
		memcpy_toio(buf, src + first, n - first);
}

/*
 * The next packet the modem wrote: its length, or a negative error. Each
 * packet is 0xaabbaabb, its length, the data padded to 8 bytes, and a
 * footer of two 0xccddeeff.
 */
static int ring_peek(void __iomem *rb, u32 *pos)
{
	u32 read = readl(rb + RB_RX_READ), write = readl(rb + RB_RX_WRITE);
	u32 len = readl(rb + RB_RX_LEN), hdr[2], ftr[2], full, fpos;
	int avail = write - read;

	if (avail < 0)
		avail += len;
	if (avail < 16 + sizeof(struct ccci_header))
		return -EAGAIN;
	ring_copy_from(rb + RB_BUF, len, read, hdr, 8);
	if (hdr[0] != CCIF_PKG_HEADER)
		return -EBADMSG;
	full = ALIGN(hdr[1], 8) + 16;
	if (full > avail)
		return -EAGAIN;
	fpos = (read + full - 8) % len;
	ring_copy_from(rb + RB_BUF, len, fpos, ftr, 8);
	if (ftr[0] != CCIF_PKG_FOOTER || ftr[1] != CCIF_PKG_FOOTER)
		return -EBADMSG;
	*pos = (read + 8) % len;
	return hdr[1];
}

static void ring_consume(void __iomem *rb, u32 n)
{
	u32 read = readl(rb + RB_RX_READ), len = readl(rb + RB_RX_LEN);

	writel((read + ALIGN(n, 8) + 16) % len, rb + RB_RX_READ);
}

static int ring_write(void __iomem *rb, const void *data, u32 n)
{
	u32 read = readl(rb + RB_TX_READ), write = readl(rb + RB_TX_WRITE);
	u32 len = readl(rb + RB_TX_LEN), rx_len = readl(rb + RB_RX_LEN);
	u32 hdr[2] = { CCIF_PKG_HEADER, n };
	u32 ftr[2] = { CCIF_PKG_FOOTER, CCIF_PKG_FOOTER };
	void __iomem *buf = rb + RB_BUF + rx_len;
	u32 need = ALIGN(n, 8) + 16;
	int room;

	if (!len || n > len)
		return -EINVAL;
	room = (read <= write ? len - write + read : read - write) - 1;
	if (need >= room)
		return -EBUSY;
	ring_copy_to(buf, len, write, hdr, 8);
	write = (write + 8) % len;
	ring_copy_to(buf, len, write, data, n);
	write = (write + ALIGN(n, 8)) % len;
	ring_copy_to(buf, len, write, ftr, 8);
	write = (write + 8) % len;
	wmb();
	writel(write, rb + RB_TX_WRITE);
	return 0;
}

/* ---- CCIF ---- */

static void ccif_reset(struct titan_md *md)
{
	int i;

	/* clears CCIF0's busy, channel and interrupt state, not its SRAM */
	regmap_write(md->infra, INFRA_RST0, CCIF0_RST);
	regmap_write(md->infra, INFRA_RST1, CCIF0_RST);
	for (i = 0; i < CCIF_SRAM_SIZE; i += 4) {
		writel(0, md->ap_ccif + APCCIF_CHDATA + i);
		writel(0, md->md_ccif + APCCIF_CHDATA + i);
	}
	/* the modem's boot trace goes to the first 60 bytes; the end says
	 * where its debug area is (ccci_platform.c ccci_reset_ccif_hw)
	 */
	writel(0x7274626e, md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_SIZE - 12);
	writel(md->smem_md_view + SMEM_MDSS_DBG,
	       md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_SIZE - 8);
	writel(SMEM_MDSS_DBG_SIZE,
	       md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_SIZE - 4);
}

static void ccif_send(struct titan_md *md, int ch)
{
	if (readl(md->ap_ccif + APCCIF_BUSY) & BIT(ch))
		return;	/* still pending: the modem reads the ring anyway */
	writel(BIT(ch), md->ap_ccif + APCCIF_BUSY);
	writel(ch, md->ap_ccif + APCCIF_TCHNUM);
}

static irqreturn_t ccif_isr(int irq, void *data)
{
	struct titan_md *md = data;
	u32 ch = readl(md->ap_ccif + APCCIF_RCHNUM);

	/* the data queues here; the exception channels on irq_ex */
	writel(ch & 0xffff, md->ap_ccif + APCCIF_ACK);
	set_mask_bits(&md->rchnum, 0, ch);
	if (ch & BIT(CCIF_SRAM))
		mod_delayed_work(system_wq, &md->watch, 0);
	if (ch & GENMASK(QUEUE_NUM - 1, 0))
		queue_work(system_highpri_wq, &md->rx_work);
	/* the vendor's other channels (sequence error, CCB wake-up, ...) */
	if (ch & GENMASK(14, QUEUE_NUM))
		dev_info_ratelimited(md->dev, "CCIF channels %#x\n",
				     ch & (u32)GENMASK(14, QUEUE_NUM));
	return IRQ_HANDLED;
}

/* the CCIF SRAM takes 32-bit accesses only: byte stores are lost */
static void sram_write(void __iomem *dst, const void *src, size_t n)
{
	const u32 *p = src;
	size_t i;

	for (i = 0; i < n / 4; i++)
		writel(p[i], dst + 4 * i);
}

static void sram_read(void *dst, const void __iomem *src, size_t n)
{
	u32 *p = dst;
	size_t i;

	for (i = 0; i < n / 4; i++)
		p[i] = readl(src + 4 * i);
}

/* CCIF0's second interrupt: the modem's exception channels (16 and up) */
static irqreturn_t ccif_ex_isr(int irq, void *data)
{
	struct titan_md *md = data;
	u32 ch = readl(md->ap_ccif + APCCIF_RCHNUM);

	writel(ch & ~0x7fffU, md->ap_ccif + APCCIF_ACK);
	set_mask_bits(&md->rchnum, 0, ch & ~0x7fffU);
	mod_delayed_work(system_wq, &md->watch, 0);
	return IRQ_HANDLED;
}

/* ---- handshake 1: the AP's runtime data ---- */

static u8 ap_feature(int id)
{
	switch (id) {
	case BOOT_INFO: case EXCEPTION_SHARE_MEMORY: case CCIF_SHARE_MEMORY:
	case CCISM_SHARE_MEMORY: case CCISM_SHARE_MEMORY_EXP:
	case MD_CONSYS_SHARE_MEMORY: case DHL_RAW_SHARE_MEMORY:
	case LWA_SHARE_MEMORY:
	case DT_NETD_SHARE_MEMORY: case DT_USB_SHARE_MEMORY:
	case AUDIO_RAW_SHARE_MEMORY: case MISC_INFO_HIF_DMA_REMAP:
	case MULTI_MD_MPU: case MISC_INFO_RANDOM_SEED_NUM:
	case MISC_INFO_GPS_COCLOCK: case MISC_INFO_SBP_ID: case MISC_INFO_CCCI:
	case MISC_INFO_CLIB_TIME: case MISC_INFO_C2K: case EE_AFTER_EPOF:
	case AP_CCMNI_MTU:
		return F_MUST;
	case MD_IMAGE_START_MEMORY: case MD_MTEE_SHARE_MEMORY_ENABLE:
		return F_OPTIONAL;
	case CCB_SHARE_MEMORY:
		/*
		 * The CCB (cross-core buffers for the DHL log, the MD monitor
		 * and META), with its buffer headers (init_ccb_headers):
		 * without them the modem's buffer manager asserted
		 * (ccci_shm_bm.c:584). ccb=0 leaves it out.
		 */
		return ccb ? F_MUST : F_NOT_SUPPORT;
	case MD_PHY_CAPTURE: case MD1MD3_SHARE_MEMORY: case UDC_RAW_SHARE_MEMORY:
	case MD_POS_SHARE_MEMORY: case MISC_INFO_CUSTOMER_VAL:
	case MISC_INFO_C2K_MEID: case SMART_LOGGING_SHARE_MEMORY:
	case MD_USIP_SHARE_MEMORY:
		return F_NOT_SUPPORT;
	case MISC_INFO_RTC_32K_LESS:
		/* this board has the 32 kHz crystal (RTC_SPAR0 bit 6) */
		return F_NOT_SUPPORT;
	default:
		return F_NOT_EXIST;
	}
}

/*
 * The RATs of a "universal binary" image by its type, from modem_ultg
 * (8) on, in the modem's bit order (0 G, 1 T, 2 C, 3 W, 4 Lt, 5 Lf,
 * 6 NR): ccci_util_lib_fo.c legacy_ubin_rat_map, compatible_convert and
 * ap_rat_bitmap_to_md_bitmap folded together. Sent with the SBP ID.
 */
static const u8 ubin_rats[] = {
	0x33, 0x39, 0x3b, 0x3d, 0x3f,		/* ultg ulwg ulwtg ulwcg ulwctg */
	0x13, 0x29, 0x2d, 0x37, 0x17,		/* ulttg ulfwg ulfwcg ulctg ultctg */
	0x19, 0x1d, 0x23, 0x27,			/* ultwg ultwcg ulftg ulfctg */
	0x79, 0x7b, 0x7f, 0x7d, 0x57,		/* unlwg unlwtg unlwctg unlwcg unltctg */
};

/* one feature's data, or a negative error; len is its size in bytes */
static int feature_data(struct titan_md *md, int id, u32 *d)
{
	struct timespec64 ts;

	switch (id) {
	case BOOT_INFO:
		/* boot channel, start id (normal boot unless boot_id) */
		d[0] = CCCI_CONTROL_RX;
		d[1] = boot_id;
		d[2] = d[3] = 0;
		return 16;
	case EXCEPTION_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_MDCCCI_DBG;
		d[1] = EE_SMEM_TOTAL_SIZE;
		return 8;
	case CCIF_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_CCISM_MCU;
		d[1] = md->ccism_size;
		return 8;
	case CCISM_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_CCISM_SCP;
		d[1] = SMEM_CCISM_SCP_SIZE;
		return 8;
	case CCISM_SHARE_MEMORY_EXP:
		d[0] = md->smem_md_view + SMEM_CCISM_EXP;
		d[1] = md->ccism_exp_size;
		return 8;
	case CCB_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_CCB_CTRL;
		d[1] = SMEM_CCB_CTRL_SIZE;
		d[2] = md->ccb_md_view;
		d[3] = ccb_users;
		return 16;
	case DHL_RAW_SHARE_MEMORY:
		d[0] = md->ccb_md_view + CCB_CTRL_PART;
		d[1] = md->ccb_size - CCB_CTRL_PART;
		return 8;
	case MD_CONSYS_SHARE_MEMORY:
		/*
		 * Empty, at the end of the cached area: this LK has no cached
		 * layout table (md1_bank4_cache_info), so the vendor gives the
		 * whole area to the CCB and CONSYS size 0 after it
		 * (cshare_memory_info_parsing, OLD_LK_CSMEM). Overlapping the
		 * CCB users, it made the modem's shared-memory buffer manager
		 * assert (ccci_shm_bm.c:584, measured).
		 */
		d[0] = md->ccb_md_view + md->ccb_size;
		d[1] = 0;
		return 8;
	case LWA_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_LWA;
		d[1] = 0;
		return 8;
	case DT_NETD_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_NETD;
		d[1] = 4096;
		return 8;
	case DT_USB_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_USB;
		d[1] = 4096;
		return 8;
	case AUDIO_RAW_SHARE_MEMORY:
		d[0] = md->smem_md_view + SMEM_AUDIO;
		d[1] = SMEM_AUDIO_SIZE;
		return 8;
	case MD_IMAGE_START_MEMORY:
		d[0] = md->img_base;
		d[1] = md->img_raw_size;
		return 8;
	case MULTI_MD_MPU:
		return 0;
	case MISC_INFO_RANDOM_SEED_NUM:
		d[0] = get_random_u32();
		d[1] = d[2] = d[3] = 0;
		return 16;
	case MISC_INFO_CCCI:
		/* sequence check, MD status polling */
		d[0] = BIT(0) | BIT(1);
		d[1] = d[2] = d[3] = 0;
		return 16;
	case MISC_INFO_CLIB_TIME:
		ktime_get_real_ts64(&ts);
		d[0] = lower_32_bits(ts.tv_sec);
		d[1] = upper_32_bits(ts.tv_sec);
		d[2] = d[3] = 0;	/* time zone, DST */
		return 16;
	case MISC_INFO_C2K:
		/* the image (type 12) has CDMA2000 */
		d[0] = BIT(2);
		d[1] = d[2] = d[3] = 0;
		return 16;
	case AP_CCMNI_MTU:
		d[0] = CCMNI_MTU;
		return 4;
	case MD_MTEE_SHARE_MEMORY_ENABLE:
		d[0] = 0;
		return 4;
	case MISC_INFO_SBP_ID:
		/* SBP code 0 (none set), the image's RATs (the vendor's world
		 * mode bitmap; 0 told the modem it had none)
		 */
		d[0] = 0;
		d[1] = md->img_type >= 8 &&
		       md->img_type - 8 < ARRAY_SIZE(ubin_rats) ?
		       ubin_rats[md->img_type - 8] : 0;
		d[2] = d[3] = 0;
		return 16;
	case MISC_INFO_HIF_DMA_REMAP: case MISC_INFO_RTC_32K_LESS:
	case MISC_INFO_GPS_COCLOCK: case EE_AFTER_EPOF:
		d[0] = d[1] = d[2] = d[3] = 0;
		return 16;
	default:
		/* asked for, but nothing to say: an empty region */
		d[0] = d[1] = 0;
		return 8;
	}
}

static int send_runtime_data(struct titan_md *md)
{
	void __iomem *sram = md->ap_ccif + APCCIF_CHDATA;
	void __iomem *rt = md->smem + SMEM_RUNTIME_DATA;
	struct ap_query_md_feature_v2_1 ap = { };
	struct md_query_ap_feature q;
	struct ccci_header h;
	u32 off = 0, d[4];
	int i, len;

	sram_read(&h, sram + SRAM_DL_HEADER, sizeof(h));
	sram_read(&q, sram + SRAM_MD_QUERY, sizeof(q));
	dev_info(md->dev, "HS1: %08x %08x ch %u res %08x\n",
		 h.data[0], h.data[1], h.channel, h.reserved);
	print_hex_dump(KERN_INFO, "titan-modem HS1 features: ",
		       DUMP_PREFIX_OFFSET, 32, 1, q.feature_set,
		       FEATURE_COUNT, false);
	if (q.head_pattern != MD_FEATURE_QUERY_PATTERN ||
	    q.tail_pattern != MD_FEATURE_QUERY_PATTERN) {
		dev_err(md->dev, "HS1 patterns %08x %08x\n",
			q.head_pattern, q.tail_pattern);
		return -EPROTO;
	}

	/* the feature list, in the AP half of the runtime data region */
	for (i = 0; i < FEATURE_COUNT; i++) {
		u8 mdf = q.feature_set[i], mask = mdf & 0xf, ver = mdf >> 4;
		struct rt_feature f = { .feature_id = i };

		switch (mask) {
		case F_NOT_EXIST:
			f.support_info = mdf;
			break;
		case F_MUST:
			if (ap_feature(i) < F_MUST) {
				dev_err(md->dev,
					"modem needs feature %d\n", i);
				return -EOPNOTSUPP;
			}
			f.support_info = mdf;
			break;
		case F_OPTIONAL:
			/* the AP's versions are all 0 */
			f.support_info = (ver == 0 && ap_feature(i) >= F_MUST &&
					  !(decline & BIT_ULL(i))) ?
					 F_MUST : F_NOT_SUPPORT;
			break;
		case F_BACKWARD:
			f.support_info = F_MUST;
			break;
		}
		len = (f.support_info & 0xf) == F_MUST ?
		      feature_data(md, i, d) : 0;
		if (off + sizeof(f) + len > SMEM_RUNTIME_AP_SIZE)
			return -ENOSPC;
		f.data_len = len;
		memcpy_toio(rt + off, &f, sizeof(f));
		memcpy_toio(rt + off + sizeof(f), d, len);
		off += sizeof(f) + len;
	}

	/* the database of the modem's power budget: guards, zeros */
	writel(0x44444444, md->smem + SMEM_DBM);
	writel(0x44444444, md->smem + SMEM_DBM + 4);
	memset_io(md->smem + SMEM_DBM + 8, 0, SMEM_DBM_SIZE);
	writel(0x44444444, md->smem + SMEM_DBM + 8 + SMEM_DBM_SIZE);
	writel(0x44444444, md->smem + SMEM_DBM + 12 + SMEM_DBM_SIZE);

	/* where all that is, in the CCIF SRAM */
	ap.head_pattern = AP_FEATURE_QUERY_PATTERN;
	ap.share_memory_support = 2;
	ap.ap_runtime_data_addr = md->smem_md_view + SMEM_RUNTIME_DATA;
	ap.ap_runtime_data_size = SMEM_RUNTIME_AP_SIZE;
	ap.md_runtime_data_addr = ap.ap_runtime_data_addr +
				  SMEM_RUNTIME_AP_SIZE;
	ap.md_runtime_data_size = SMEM_RUNTIME_MD_SIZE;
	ap.noncached_mpu_start_addr = md->smem_md_view;
	ap.noncached_mpu_total_size = md->smem_size;
	ap.cached_mpu_start_addr = md->ccb_md_view;
	ap.cached_mpu_total_size = md->ccb_size;
	ap.feature_set[1] = 1 << 4;	/* MPU size includes MD1-MD3 smem */
	ap.tail_pattern = AP_FEATURE_QUERY_PATTERN;
	sram_write(sram + SRAM_AP_DATA, &ap, sizeof(ap));

	memset(&h, 0, sizeof(h));
	h.data[1] = sizeof(ap) + sizeof(h);
	h.channel = CCCI_CONTROL_TX;
	h.reserved = MD_INIT_CHK_ID;
	sram_write(sram + SRAM_UP_HEADER, &h, sizeof(h));
	wmb();
	ccif_send(md, CCIF_SRAM);
	dev_info(md->dev, "runtime data sent (%u bytes of features)\n",
		 off);
	return 0;
}

/* ---- packets ---- */

static int tx_queue(u16 ch)
{
	switch (ch) {
	case CCCI_CONTROL_TX: case CCCI_SYSTEM_TX: case CCCI_STATUS_TX:
	case CCCI_PCM_TX:
		return 0;
	case CCCI_MD_LOG_TX:
		return 2;
	case CCCI_FS_TX:
		return 4;
	case CCCI_UART2_TX:
		return 5;
	default:
		return 1;
	}
}

static int md_send(struct titan_md *md, struct ccci_header *h, u32 len)
{
	unsigned long flags;
	int q, ret;

	if (h->channel >= CCCI_CH_MAX)
		return -EINVAL;
	q = tx_queue(h->channel);
	spin_lock_irqsave(&md->tx_lock, flags);
	h->seq_num = md->tx_seq[h->channel];
	/* file and RPC replies are asserted only while the modem boots */
	h->assert_bit = !(md->ready && (h->channel == CCCI_FS_TX ||
					h->channel == CCCI_RPC_TX));
	ret = ring_write(md->rings[q], h, len);
	if (!ret) {
		md->tx_seq[h->channel]++;
		ccif_send(md, q);
	}
	spin_unlock_irqrestore(&md->tx_lock, flags);
	return ret;
}

static void control_rx(struct titan_md *md, struct ccci_header *h, u32 len)
{
	switch (h->data[1]) {
	case MD_INIT_START_BOOT:
		if (h->reserved != MD_INIT_CHK_ID && !md->ready) {
			md->ready = true;
			dev_info(md->dev, "HS2: modem ready\n");
		}
		break;
	case MD_EX:
	case MD_EX_REC_OK:
	case MD_EX_PASS:
		dev_err(md->dev, "modem exception message %#x\n",
			h->data[1]);
		break;
	default:
		dev_info(md->dev, "control %08x %08x res %08x\n",
			 h->data[0], h->data[1], h->reserved);
	}
}

static void rx_work(struct work_struct *w)
{
	struct titan_md *md = container_of(w, struct titan_md, rx_work);
	int q, n;
	u32 pos;

	/* look at every queue: an interrupt can cover several packets */
	set_mask_bits(&md->rchnum, GENMASK(QUEUE_NUM - 1, 0), 0);
	for (q = 0; q < QUEUE_NUM; q++) {
		void __iomem *rb = md->rings[q];

		while ((n = ring_peek(rb, &pos)) >= 0) {
			struct sk_buff *skb = alloc_skb(n, GFP_KERNEL);
			struct ccci_header *h;

			if (!skb)
				return;
			ring_copy_from(rb + RB_BUF, readl(rb + RB_RX_LEN), pos,
				       skb_put(skb, n), n);
			ring_consume(rb, n);
			h = (struct ccci_header *)skb->data;
			if (h->channel == CCCI_CONTROL_RX)
				control_rx(md, h, n);
			if (h->channel == CCCI_UART2_RX && md->at_port) {
				/* dropped while nobody has the port open */
				skb_pull(skb, sizeof(*h));
				if (READ_ONCE(md->at_open))
					wwan_port_rx(md->at_port, skb);
				else
					kfree_skb(skb);
				continue;
			}
			if (skb_queue_len(&md->rxq) >= RX_QUEUE_MAX)
				kfree_skb(skb_dequeue(&md->rxq));
			skb_queue_tail(&md->rxq, skb);
			wake_up_interruptible(&md->rx_wait);
		}
		if (n == -EBADMSG)
			dev_err_ratelimited(md->dev,
					    "queue %d: bad packet\n", q);
	}
}

static int at_start(struct wwan_port *port)
{
	struct titan_md *md = wwan_port_get_drvdata(port);

	WRITE_ONCE(md->at_open, true);
	return 0;
}

static void at_stop(struct wwan_port *port)
{
	struct titan_md *md = wwan_port_get_drvdata(port);

	WRITE_ONCE(md->at_open, false);
}

/* AT text to the modem, as CCCI packets on UART2_TX */
static int at_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct titan_md *md = wwan_port_get_drvdata(port);
	struct ccci_header *h;
	u32 off, n;
	int ret = 0;

	h = kmalloc(sizeof(*h) + AT_TX_MAX, GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	for (off = 0; off < skb->len && !ret; off += n) {
		n = min_t(u32, skb->len - off, AT_TX_MAX);
		memset(h, 0, sizeof(*h));
		h->data[1] = sizeof(*h) + n;
		h->channel = CCCI_UART2_TX;
		memcpy(h + 1, skb->data + off, n);
		ret = md_send(md, h, sizeof(*h) + n);
	}
	kfree(h);
	if (!ret)
		consume_skb(skb);
	return ret;
}

static const struct wwan_port_ops at_ops = {
	.start = at_start,
	.stop = at_stop,
	.tx = at_tx,
};

static void at_work(struct work_struct *w)
{
	struct titan_md *md = container_of(w, struct titan_md, at_work);
	struct wwan_port *port;

	if (md->at_port)
		return;
	port = wwan_create_port(md->dev, WWAN_PORT_AT, &at_ops, NULL, md);
	if (IS_ERR(port)) {
		dev_err(md->dev, "AT port: %pe\n", port);
		return;
	}
	md->at_port = port;
}

static int md_open(struct inode *inode, struct file *f)
{
	if (atomic_cmpxchg(&tmd->open, 0, 1))
		return -EBUSY;
	f->private_data = tmd;
	return stream_open(inode, f);
}

static int md_release(struct inode *inode, struct file *f)
{
	atomic_set(&tmd->open, 0);
	return 0;
}

static ssize_t md_read(struct file *f, char __user *buf, size_t count,
		       loff_t *ppos)
{
	struct titan_md *md = f->private_data;
	struct sk_buff *skb;
	ssize_t ret;

	while (!(skb = skb_dequeue(&md->rxq))) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(md->rx_wait,
					     !skb_queue_empty(&md->rxq)))
			return -ERESTARTSYS;
	}
	if (count < skb->len) {
		skb_queue_head(&md->rxq, skb);
		return -EMSGSIZE;
	}
	ret = copy_to_user(buf, skb->data, skb->len) ? -EFAULT : skb->len;
	kfree_skb(skb);
	return ret;
}

static ssize_t md_write(struct file *f, const char __user *buf, size_t count,
			loff_t *ppos)
{
	struct titan_md *md = f->private_data;
	void *pkt;
	int ret;

	if (count < sizeof(struct ccci_header) || count > SZ_64K)
		return -EINVAL;
	if (!md->hs1_done)
		return -EAGAIN;
	pkt = memdup_user(buf, count);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);
	ret = md_send(md, pkt, count);
	kfree(pkt);
	return ret ?: count;
}

static long md_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct titan_md *md = f->private_data;

	if (cmd != TITAN_MD_AT_PORT)
		return -ENOTTY;
	if (!md->ready)
		return -EAGAIN;
	schedule_work(&md->at_work);
	flush_work(&md->at_work);
	return md->at_port ? 0 : -EIO;
}

static __poll_t md_poll(struct file *f, poll_table *wait)
{
	struct titan_md *md = f->private_data;

	poll_wait(f, &md->rx_wait, wait);
	return (skb_queue_empty(&md->rxq) ? 0 : EPOLLIN | EPOLLRDNORM) |
	       EPOLLOUT | EPOLLWRNORM;
}

/*
 * The speech shared memory (AUDIO_RAW_SHARE_MEMORY) for the speech
 * daemon, as Android's /dev/ccci_raw_audio: it writes the speech
 * parameters into the parameter ring there.
 */
static int md_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct titan_md *md = f->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || size > SMEM_AUDIO_SIZE)
		return -EINVAL;
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       PHYS_PFN(md->smem_base + SMEM_AUDIO), size,
			       vma->vm_page_prot);
}

static const struct file_operations md_fops = {
	.owner = THIS_MODULE,
	.open = md_open,
	.release = md_release,
	.read = md_read,
	.write = md_write,
	.poll = md_poll,
	.unlocked_ioctl = md_ioctl,
	.mmap = md_mmap,
};

/*
 * Decode the modem's exception record. On a crash the modem fills an
 * overview at the start of the MDSS_DBG shared-memory region (the vendor's
 * mdee_dumper_v3.h layout): a per-core reason array, then a brief-main-info
 * block whose type says assert or fatal, then the details. This turns it
 * into one log line instead of hand offsets in debugfs.
 */
#define EX_OVERVIEW		SMEM_MDSS_DBG	/* the record's start in smem */
#define EX_CORE_NUM		8		/* MD_CORE_TOTAL_NUM (gen 6293) */
#define EX_CORE_NAME		11		/* MD_CORE_NAME_LEN */
#define EX_INFO_OFF		(8 + EX_CORE_NUM * (EX_CORE_NAME + 1))
#define EX_CONTENT_OFF		(EX_INFO_OFF + 8)
#define EX_TYPE_ASSERT		0x50		/* ASSERT_FAIL_EXCEPTION */

static void decode_exception(struct titan_md *md)
{
	void __iomem *ov = md->smem + EX_OVERVIEW;
	char core[EX_CORE_NAME + 1] = {};
	char file[256];
	u16 ex_type;
	int i;

	ex_type = readw(ov + EX_INFO_OFF);
	/* the offending core's name, from the per-core reason array */
	for (i = 0; i < EX_CORE_NUM; i++) {
		void __iomem *r = ov + 8 + i * (EX_CORE_NAME + 1);

		if (readb(r + EX_CORE_NAME)) {	/* is_offender */
			memcpy_fromio(core, r, EX_CORE_NAME);
			break;
		}
	}
	if (ex_type == EX_TYPE_ASSERT) {
		u32 line = readl(ov + EX_CONTENT_OFF + 256);
		u32 p1 = readl(ov + EX_CONTENT_OFF + 260);
		u32 p2 = readl(ov + EX_CONTENT_OFF + 264);
		u32 p3 = readl(ov + EX_CONTENT_OFF + 268);

		memcpy_fromio(file, ov + EX_CONTENT_OFF, sizeof(file));
		file[sizeof(file) - 1] = 0;
		dev_err(md->dev,
			"modem assert on %s: %s:%u params %#x %#x %#x\n",
			core[0] ? core : "?", file, line, p1, p2, p3);
	} else {
		u32 c1 = readl(ov + EX_CONTENT_OFF);
		u32 c2 = readl(ov + EX_CONTENT_OFF + 4);
		u32 c3 = readl(ov + EX_CONTENT_OFF + 8);
		u32 pc = readl(ov + EX_CONTENT_OFF + 0x24);
		u32 lr = readl(ov + EX_CONTENT_OFF + 0x28);

		dev_err(md->dev,
			"modem fatal on %s (type %#x): codes %#x %#x %#x pc %#x lr %#x\n",
			core[0] ? core : "?", ex_type, c1, c2, c3, pc, lr);
	}
}

/* the modem's progress until handshake 1, then the handshake itself */
static void watch_work(struct work_struct *w)
{
	struct titan_md *md = container_of(w, struct titan_md, watch.work);
	u32 trace[6], s0, s1;

	if (md->rchnum & ~0xffffUL) {
		sram_read(md->sram_copy, md->ap_ccif + APCCIF_CHDATA,
			  CCIF_SRAM_SIZE);
		dev_err(md->dev, "modem exception: CCIF %#lx\n", md->rchnum);
		decode_exception(md);
		set_mask_bits(&md->rchnum, ~0xffffUL, 0);
		return;
	}
	if (test_and_clear_bit(CCIF_SRAM, &md->rchnum) && !md->hs1_done) {
		md->hs1_done = !send_runtime_data(md);
		return;
	}
	s0 = readl(md->md_cfg);		/* MD1_CFG boot status 0, 1 */
	s1 = readl(md->md_cfg + 4);
	sram_read(trace, md->ap_ccif + APCCIF_CHDATA, sizeof(trace));
	dev_info(md->dev, "t+%d.%ds: boot stats %08x %08x, trace %*ph\n",
		 md->watch_ticks / 2, md->watch_ticks % 2 * 5, s0, s1,
		 (int)sizeof(trace), trace);
	if (++md->watch_ticks < 40)
		schedule_delayed_work(&md->watch, HZ / 2);
	else
		dev_warn(md->dev, "no HS1 after 20 s\n");
}

/* ---- MD1 power and boot (md_sys1_platform.c, clk-mt6771-pg.c) ---- */

static int prot_set(struct titan_md *md, u32 set, u32 sta, u32 mask)
{
	u32 v;

	regmap_write(md->infra, set, mask);
	return regmap_read_poll_timeout(md->infra, sta, v,
					(v & mask) == mask, 10, 100000);
}

static int prot_clr(struct titan_md *md, u32 clr, u32 sta, u32 mask)
{
	u32 v;

	regmap_write(md->infra, clr, mask);
	return regmap_read_poll_timeout(md->infra, sta, v,
					!(v & mask), 10, 100000);
}

static int md1_domain_on(struct titan_md *md)
{
	u32 a, b;
	int ret;

	regmap_clear_bits(md->spm, MD1_PWR_CON, PWR_RST_B);
	regmap_set_bits(md->spm, MD1_PWR_CON, PWR_ON);
	regmap_set_bits(md->spm, MD1_PWR_CON, PWR_ON_2ND);
	ret = regmap_read_poll_timeout(md->spm, PWR_STATUS, a,
				       a & MD1_PWR_STA, 10, 100000) ?:
	      regmap_read_poll_timeout(md->spm, PWR_STATUS_2ND, b,
				       b & MD1_PWR_STA, 10, 100000);
	if (ret)
		return ret;
	regmap_clear_bits(md->spm, MD1_PWR_CON, MD1_SRAM_PDN);
	regmap_set_bits(md->spm, MD_SRAM_ISO_CON, BIT(0));
	regmap_clear_bits(md->spm, MD1_PWR_CON, PWR_ISO);
	regmap_clear_bits(md->spm, MD1_PWR_CON, PWR_CLK_DIS);
	regmap_set_bits(md->spm, MD1_PWR_CON, PWR_RST_B);
	regmap_clear_bits(md->spm, MD_EXTRA_PWR_CON, BIT(0));
	/* release bus protection; the vendor sequence does not wait here */
	regmap_write(md->infra, PROTECTEN_CLR, PROT_MD1_STEP2);
	regmap_write(md->infra, PROTECTEN_1_CLR, PROT_MD2PERI);
	regmap_write(md->infra, PROTECTEN_CLR, PROT_PERI2MD);
	return 0;
}

static int md1_domain_off(struct titan_md *md)
{
	u32 a, b;
	int ret;

	ret = prot_set(md, PROTECTEN_SET, PROTECTEN_STA1, PROT_PERI2MD) ?:
	      prot_set(md, PROTECTEN_SET, PROTECTEN_STA1, PROT_MD1_STEP2) ?:
	      prot_set(md, PROTECTEN_1_SET, PROTECTEN_1_STA1, PROT_MD2PERI);
	if (ret)
		return ret;
	regmap_set_bits(md->spm, MD_EXTRA_PWR_CON, BIT(0));
	regmap_set_bits(md->spm, MD1_PWR_CON, PWR_CLK_DIS);
	regmap_set_bits(md->spm, MD1_PWR_CON, PWR_ISO);
	regmap_clear_bits(md->spm, MD_SRAM_ISO_CON, BIT(0));
	regmap_set_bits(md->spm, MD1_PWR_CON, MD1_SRAM_PDN);
	regmap_clear_bits(md->spm, MD1_PWR_CON, PWR_ON);
	regmap_clear_bits(md->spm, MD1_PWR_CON, PWR_ON_2ND);
	return regmap_read_poll_timeout(md->spm, PWR_STATUS, a,
					!(a & MD1_PWR_STA), 10, 100000) ?:
	       regmap_read_poll_timeout(md->spm, PWR_STATUS_2ND, b,
					!(b & MD1_PWR_STA), 10, 100000);
}

static int md1_pll_init(struct titan_md *md)
{
	void __iomem *pll = md->md_pll, *clksw = md->md_clksw;
	u32 v;
	int ret, i;

	/* the PLL block reads its version once it is out of reset */
	ret = readl_poll_timeout(pll, v, v, 1000, 200000);
	if (ret)
		return ret;
	dev_info(md->dev, "MD PLL version %#x\n", v);

	/* clock square 1's low-pass filter, for the 26 MHz quality */
	regmap_set_bits(md->apmixed, 0x0, BIT(1));
	udelay(100);
	/* md_srclkena_ack settle time: 147 32k periods */
	writel(0x02020e93, pll + 0x4);
	writel(0x801713b1, pll + 0x60);
	writel(0x80171400, pll + 0x58);
	writel(0x80229e00, pll + 0x50);
	writel(0x80204e00, pll + 0x48);
	writel(0x80213c00, pll + 0x40);
	ret = readl_poll_timeout(pll + 0xc00, v, !(v & BIT(14)), 10, 100000);
	if (ret)
		return ret;
	writel(readl(pll + 0x64) & ~BIT(7), pll + 0x64);
	writel(0x04c43100, pll + 0x104);
	for (i = 0; i < 50; i++) {
		writel(0x100010, pll + 0x10);
		msleep(20);
		if (readl(pll + 0x10) == 0x100010)
			break;
	}
	if (i == 50)
		return -ETIMEDOUT;
	ret = readl_poll_timeout(clksw + 0x84, v, v & BIT(15), 1000, 1000000);
	if (ret)
		return ret;
	writel(readl(clksw + 0x24) | 0x3, clksw + 0x24);
	writel(readl(clksw + 0x24) | 0x58103fc, clksw + 0x24);
	writel(readl(clksw + 0x28) | 0x10, clksw + 0x28);
	writel(0x1, clksw + 0x20);
	writel(0xffff, pll + 0x314);
	writel(0xffff, pll + 0x318);
	/* the vendor's record that the MD PLLs are set up */
	writel(0x62930000, pll + 0xf00);
	return 0;
}

static int md_power_on(struct titan_md *md)
{
	int ret;

	/* the modem's clocks out of their gated mode */
	regmap_clear_bits(md->topck, CLK_MODE, CLK_MODE_MD_GATE);

	ret = md1_domain_on(md);
	if (ret)
		return dev_err_probe(md->dev, ret, "MD1 domain\n");
	md->powered = true;

	/* MD->AP closed while the AP sets the modem up */
	regmap_clear_bits(md->infra, AP2MD_DUMMY, BIT(0));
	ret = prot_set(md, PROTECTEN_1_SET, PROTECTEN_1_STA1, PROT_MD2PERI);
	if (ret)
		return dev_err_probe(md->dev, ret, "MD2PERI\n");

	/* the modem's 26 MHz request: srclkena 0 and its ack (0x21) */
	regmap_update_bits(md->infra, MD_SRCCLKENA, 0xff, 0x21);

	ret = md1_pll_init(md);
	if (ret)
		return dev_err_probe(md->dev, ret, "MD PLLs\n");

	/* the modem's watchdog off until it runs (WDT mode at md-rgu + 0) */
	writel(WDT_MD_MODE_KEY, md->md_rgu);
	return 0;
}

static int md_let_go(struct titan_md *md)
{
	int ret;

	writel(1, md->md_boot + MD_BOOT_VECTOR_EN_OFF);

	/* from here the AP keeps out of the modem; the modem may reach in */
	ret = prot_set(md, PROTECTEN_SET, PROTECTEN_STA1, PROT_PERI2MD) ?:
	      prot_clr(md, PROTECTEN_1_CLR, PROTECTEN_1_STA1, PROT_MD2PERI);
	regmap_set_bits(md->infra, AP2MD_DUMMY, BIT(0));
	return ret;
}

static void md_power_off(struct titan_md *md)
{
	int ret;

	ret = md1_domain_off(md);
	if (ret) {
		dev_err(md->dev, "MD1 off: %d, left on\n", ret);
		return;
	}
	regmap_update_bits(md->infra, MD_SRCCLKENA, 0xff, 0);
	regmap_set_bits(md->topck, CLK_MODE, CLK_MODE_MD_GATE);
	md->powered = false;
}

/* ---- setup ---- */

static struct regmap *syscon_phandle(struct device *dev, const char *name)
{
	struct regmap *map = syscon_regmap_lookup_by_phandle(dev->of_node,
							     name);

	if (IS_ERR(map))
		dev_err(dev, "syscon %s: %pe\n", name, map);
	return map;
}

static int get_resources(struct titan_md *md)
{
	struct platform_device *pdev = to_platform_device(md->dev);
	void __iomem **io[] = { &md->ap_ccif, &md->md_ccif, &md->md_pll,
				&md->md_clksw, &md->md_rgu, &md->md_boot,
				&md->md_cfg };
	static const char * const io_names[] = { "ap-ccif", "md-ccif",
		"md-pll", "md-clksw", "md-rgu", "md-boot", "md-cfg" };
	int i;

	for (i = 0; i < ARRAY_SIZE(io); i++) {
		*io[i] = devm_platform_ioremap_resource_byname(pdev,
							       io_names[i]);
		if (IS_ERR(*io[i]))
			return PTR_ERR(*io[i]);
	}

	md->spm = syscon_phandle(md->dev, "mediatek,scpsys");
	md->infra = syscon_phandle(md->dev, "mediatek,infracfg");
	md->topck = syscon_phandle(md->dev, "mediatek,topckgen");
	md->apmixed = syscon_phandle(md->dev, "mediatek,apmixedsys");
	if (IS_ERR(md->spm) || IS_ERR(md->infra) || IS_ERR(md->topck) ||
	    IS_ERR(md->apmixed))
		return -ENODEV;

	/* CLDMA and the CCIF0-2 infra clocks, enabled for the driver's life */
	for (i = 0; i < ARRAY_SIZE(clk_names); i++) {
		struct clk *c = devm_clk_get_enabled(md->dev, clk_names[i]);

		if (IS_ERR(c))
			return dev_err_probe(md->dev, PTR_ERR(c),
					     "clock %s\n", clk_names[i]);
	}

	md->irq = platform_get_irq_byname(pdev, "ccif");
	md->irq_ex = platform_get_irq_byname(pdev, "ccif-ex");
	if (md->irq < 0 || md->irq_ex < 0)
		return -EINVAL;
	return 0;
}

static int titan_md_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct titan_md *md;
	int ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = dev;
	platform_set_drvdata(pdev, md);
	INIT_DELAYED_WORK(&md->watch, watch_work);
	INIT_WORK(&md->rx_work, rx_work);
	INIT_WORK(&md->at_work, at_work);
	spin_lock_init(&md->tx_lock);
	skb_queue_head_init(&md->rxq);
	init_waitqueue_head(&md->rx_wait);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = read_lk_tags(md) ?: get_resources(md) ?: map_smem(md);
	if (ret)
		return ret;
	init_rings(md);
	ccif_reset(md);

	ret = devm_request_irq(dev, md->irq, ccif_isr, 0,
			       "titan-modem-ccif", md);
	if (!ret)
		ret = devm_request_irq(dev, md->irq_ex, ccif_ex_isr, 0,
				       "titan-modem-ccif-ex", md);
	if (ret)
		goto err_smem;

	md->misc.minor = MISC_DYNAMIC_MINOR;
	md->misc.name = "titan-modem";
	md->misc.fops = &md_fops;
	md->misc.mode = 0600;
	ret = misc_register(&md->misc);
	if (ret)
		goto err_smem;

	/* the shared memory and the CCIF SRAM, for the modem's debug areas */
	md->debugfs = debugfs_create_dir("titan-modem", NULL);
	md->smem_blob.data = (void __force *)md->smem;
	md->smem_blob.size = md->smem_size;
	debugfs_create_blob("smem", 0400, md->debugfs, &md->smem_blob);
	md->sram_blob.data = md->sram_copy;
	md->sram_blob.size = CCIF_SRAM_SIZE;
	debugfs_create_blob("sram", 0400, md->debugfs, &md->sram_blob);
	if (md->ccb) {
		md->ccb_blob.data = md->ccb;
		md->ccb_blob.size = md->ccb_size;
		debugfs_create_blob("ccb", 0400, md->debugfs, &md->ccb_blob);
	}
	/* the start of the DSP image's place (md1rom check header: +0x7000000) */
	md->dsp_blob.data = devm_memremap(dev, md->img_base + 0x7000000, SZ_64K,
					  MEMREMAP_WB);
	if (!IS_ERR(md->dsp_blob.data)) {
		md->dsp_blob.size = SZ_64K;
		debugfs_create_blob("dsp", 0400, md->debugfs, &md->dsp_blob);
	}

	ret = md_power_on(md);
	if (ret)
		goto err_misc;
	ret = md_let_go(md);
	dev_info(dev, "modem released (%d)\n", ret);
	tmd = md;			/* the misc device's single instance */
	schedule_delayed_work(&md->watch, HZ / 2);
	return 0;

err_misc:
	md_power_off(md);
	debugfs_remove_recursive(md->debugfs);
	misc_deregister(&md->misc);
err_smem:
	vunmap((void __force *)md->smem);
	if (md->ccb)
		vunmap(md->ccb);
	return ret;
}

static void titan_md_remove(struct platform_device *pdev)
{
	struct titan_md *md = platform_get_drvdata(pdev);

	/* stop new events, flush pending work, then bring the modem down */
	disable_irq(md->irq);
	disable_irq(md->irq_ex);
	cancel_delayed_work_sync(&md->watch);
	cancel_work_sync(&md->rx_work);
	cancel_work_sync(&md->at_work);
	if (md->at_port)
		wwan_remove_port(md->at_port);
	if (md->powered)
		md_power_off(md);
	tmd = NULL;
	misc_deregister(&md->misc);
	debugfs_remove_recursive(md->debugfs);
	vunmap((void __force *)md->smem);
	if (md->ccb)
		vunmap(md->ccb);
	skb_queue_purge(&md->rxq);
}

static const struct of_device_id titan_md_of_match[] = {
	{ .compatible = "mediatek,mt6771-modem" },
	{ }
};
MODULE_DEVICE_TABLE(of, titan_md_of_match);

static struct platform_driver titan_md_driver = {
	.probe = titan_md_probe,
	.remove = titan_md_remove,
	.driver = {
		.name = "titan-modem",
		.of_match_table = titan_md_of_match,
	},
};
module_platform_driver(titan_md_driver);

MODULE_DESCRIPTION("MT6771 modem: power, boot handshake, CCCI packets");
MODULE_LICENSE("GPL");
