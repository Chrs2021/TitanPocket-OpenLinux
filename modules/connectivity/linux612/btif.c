// SPDX-License-Identifier: GPL-2.0
/*
 * BTIF for Linux 6.12: the UART-like link between the AP and CONNSYS that
 * carries STP (WMT commands, ROM patches, BT, FM and GPS; Wi-Fi data goes
 * over its own AHB interface). It exports the mtk_wcn_btif_* calls the
 * WMT core (modules/connectivity/common, stp_btif.c) uses.
 *
 * Registers and the init sequence are the vendor driver's
 * (drivers/misc/mediatek/btif in the 4.14 tree: btif_priv.h,
 * hal_btif_hw_init): FIFO depth 16 out, 8 in; IIR reports rx data or rx
 * timeout; WAK is the AP-wakes-CONNSYS line, pulsed low for > 1/32 kHz.
 *
 * Data moves by APDMA when the node has the two channels (reg 1 tx, 2 rx;
 * interrupts 1, 2; clock "apdma"), as the vendor driver does
 * (btif_dma_plat.c), else by PIO. Each channel is a virtual FIFO: a ring
 * in memory with hardware write and read pointers (bit 16 the wrap bit).
 * The rx ring interrupts at one byte, so every STP packet is handed on at
 * once; the tx ring interrupts when 7 bytes or fewer are left, which DMA
 * moves only on a flush. Upstream's mtk-uart-apdma drives the same
 * channels for 8250 UARTs, but fills rx to 3/4 and relies on the UART's
 * rx timeout, which BTIF in DMA mode does not raise.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/wait.h>

#include "mtk_btif_exp.h"

#define BTIF_RBR		0x00
#define BTIF_THR		0x00
#define BTIF_IER		0x04
#define  BTIF_IER_TXEEN		BIT(1)
#define  BTIF_IER_RXFEN		BIT(0)
#define BTIF_IIR		0x08
#define  BTIF_IIR_RX		BIT(2)
#define  BTIF_IIR_RX_TIMEOUT	(0x11 << 2)
#define BTIF_FIFOCTRL		0x08
#define  BTIF_FIFOCTRL_CLR_TX	BIT(2)
#define  BTIF_FIFOCTRL_CLR_RX	BIT(1)
#define BTIF_FAKELCR		0x0c
#define BTIF_LSR		0x14
#define  BTIF_LSR_DR		BIT(0)
#define  BTIF_LSR_OE		BIT(1)
#define  BTIF_LSR_THRE		BIT(5)
#define  BTIF_LSR_TEMT		BIT(6)
#define BTIF_SLEEP_EN		0x48
#define BTIF_DMA_EN		0x4c
#define  BTIF_DMA_EN_RX		BIT(0)
#define  BTIF_DMA_EN_TX		BIT(1)
#define  BTIF_DMA_EN_AUTORST	BIT(2)
#define BTIF_RTOCNT		0x54
#define BTIF_TRI_LVL		0x60
#define  BTIF_TRI_LVL_TX(x)	((x) & 0xf)
#define  BTIF_TRI_LVL_RX(x)	(((x) & 0x7) << 4)
#define BTIF_WAK		0x64
#define BTIF_HANDSHAKE		0x6c

/* APDMA virtual FIFO channel registers */
#define VFF_INT_FLAG		0x00
#define  VFF_RX_INT_THRE	BIT(0)
#define  VFF_RX_INT_DONE	BIT(1)
#define VFF_INT_EN		0x04
#define VFF_EN			0x08
#define VFF_RST			0x0c
#define  VFF_WARM_RST		BIT(0)
#define  VFF_HARD_RST		BIT(1)
#define VFF_STOP		0x10
#define VFF_FLUSH		0x14
#define VFF_ADDR		0x1c
#define VFF_LEN			0x24
#define VFF_THRE		0x28
#define VFF_WPT			0x2c
#define VFF_RPT			0x30
#define VFF_INT_BUF_SIZE	0x38
#define VFF_VALID_SIZE		0x3c
#define VFF_LEFT_SIZE		0x40
#define VFF_ADDR_H		0x54
#define VFF_PTR_MASK		0xffff
#define VFF_PTR_WRAP		BIT(16)

/*
 * Ring sizes: multiples of 8, below 64 KB. STP keeps at most 7 packets of
 * up to 2 KB unacknowledged, so a write always fits the tx ring.
 */
#define BTIF_TX_VFF_SIZE	SZ_32K
#define BTIF_RX_VFF_SIZE	SZ_16K
#define BTIF_TX_VFF_THRE	(BTIF_TX_VFF_SIZE - 7)

#define BTIF_TX_FIFO_SIZE	16
#define BTIF_TX_FIFO_THRE	(BTIF_TX_FIFO_SIZE / 2)
#define BTIF_RX_KFIFO_SIZE	(64 * 1024)
/* STP keeps at most 7 packets of up to 2 KB unacknowledged */
#define BTIF_TX_KFIFO_SIZE	(32 * 1024)
/* the only user: STP; mtk_wcn_btif_open() hands out this id */
#define BTIF_USER_ID		1

struct btif {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	int irq;

	/* APDMA: tx and rx virtual FIFO channels and their rings */
	bool dma;
	void __iomem *tx_dma, *rx_dma;
	int tx_irq, rx_irq;
	struct clk *dma_clk;
	u8 *tx_vff, *rx_vff;
	dma_addr_t tx_vff_addr, rx_vff_addr;
	unsigned int tx_flushes;	/* tx interrupts in a row asking to flush */

	struct mutex lock;		/* open/close */
	bool opened;
	MTK_WCN_BTIF_RX_CB rx_cb;

	DECLARE_KFIFO_PTR(rx_fifo, u8);
	spinlock_t rx_lock;		/* rx_fifo, between irq and thread */
	struct task_struct *rx_thread;
	wait_queue_head_t rx_wq;

	/*
	 * PIO: tx_fifo is drained into the 16-byte hardware FIFO by the writer
	 * and the tx-empty interrupt. STP writes with a spinlock held and
	 * interrupts off (CFG_STP_CORE_CTX_SPIN_LOCK), so writes never sleep,
	 * in either mode.
	 */
	DECLARE_KFIFO_PTR(tx_fifo, u8);
	spinlock_t tx_lock;		/* tx_fifo, the THR and IER's TXEEN; the tx ring */

	/* rx bytes taken and FIFO overruns seen (rx_stats in sysfs) */
	unsigned long rx_bytes;
	unsigned long rx_overruns;
};

static struct btif *g_btif;

/*
 * Debug trace of the link: the last BTIF_TRACE_N transfers (writes and
 * received batches) with their first bytes, dumped when STP times out
 * (mtk_wcn_btif_dbg_ctrl).
 */
#define BTIF_TRACE_N		48
#define BTIF_TRACE_BYTES	24
static struct btif_trace {
	u64 us;
	u16 len;
	bool tx;
	u8 data[BTIF_TRACE_BYTES];
} btif_trace[BTIF_TRACE_N];
static unsigned int btif_trace_pos;
static DEFINE_SPINLOCK(btif_trace_lock);

static void btif_trace_add(bool tx, const u8 *p, unsigned int len)
{
	struct btif_trace *t;
	unsigned long flags;

	spin_lock_irqsave(&btif_trace_lock, flags);
	t = &btif_trace[btif_trace_pos++ % BTIF_TRACE_N];
	t->us = ktime_to_us(ktime_get());
	t->tx = tx;
	t->len = len;
	memcpy(t->data, p, min_t(unsigned int, len, BTIF_TRACE_BYTES));
	spin_unlock_irqrestore(&btif_trace_lock, flags);
}

static void btif_trace_dump(struct device *dev)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&btif_trace_lock, flags);
	for (i = 0; i < BTIF_TRACE_N; i++) {
		struct btif_trace *t = &btif_trace[(btif_trace_pos + i) % BTIF_TRACE_N];

		if (t->len)
			dev_info(dev, "trace %llu.%06llu %s %4u: %*ph\n",
				 t->us / 1000000, t->us % 1000000,
				 t->tx ? "tx" : "rx", t->len,
				 min_t(int, t->len, BTIF_TRACE_BYTES), t->data);
	}
	spin_unlock_irqrestore(&btif_trace_lock, flags);
}

/*
 * The last BTIF_RXLOG_SIZE bytes received, in order, as the bin attribute
 * rx_log: CONNSYS sends its assert text and coredump header as STP
 * packets, which the STP core doesn't log in full.
 */
#define BTIF_RXLOG_SIZE		SZ_64K
static u8 btif_rxlog[BTIF_RXLOG_SIZE];
static unsigned long btif_rxlog_pos;	/* total bytes logged */
static DEFINE_SPINLOCK(btif_rxlog_lock);

static void btif_rxlog_add(const u8 *p, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&btif_rxlog_lock, flags);
	while (len--)
		btif_rxlog[btif_rxlog_pos++ % BTIF_RXLOG_SIZE] = *p++;
	spin_unlock_irqrestore(&btif_rxlog_lock, flags);
}

static void btif_hw_init(struct btif *b)
{
	writel(0, b->base + BTIF_FAKELCR);		/* normal mode */
	writel(1, b->base + BTIF_HANDSHAKE);		/* new handshake */
	writel(BTIF_FIFOCTRL_CLR_RX, b->base + BTIF_FIFOCTRL);
	writel(0, b->base + BTIF_FIFOCTRL);
	writel(BTIF_FIFOCTRL_CLR_TX, b->base + BTIF_FIFOCTRL);
	writel(0, b->base + BTIF_FIFOCTRL);
	/* tx threshold half the FIFO, rx on every byte, no loopback */
	writel(BTIF_TRI_LVL_TX(BTIF_TX_FIFO_THRE) | BTIF_TRI_LVL_RX(1),
	       b->base + BTIF_TRI_LVL);
	if (b->dma) {
		writel(0, b->base + BTIF_IER);
		writel(BTIF_DMA_EN_AUTORST | BTIF_DMA_EN_TX | BTIF_DMA_EN_RX,
		       b->base + BTIF_DMA_EN);
	} else {
		writel(BTIF_DMA_EN_AUTORST, b->base + BTIF_DMA_EN);
		writel(BTIF_IER_RXFEN, b->base + BTIF_IER);
	}
}

/* Warm reset, as hal_btif_dma_hw_init(); hard reset if that doesn't stop it */
static void btif_vff_reset(struct btif *b, void __iomem *ch)
{
	u32 en;

	writel(VFF_WARM_RST, ch + VFF_RST);
	if (readl_poll_timeout_atomic(ch + VFF_EN, en, !(en & 1), 10, 1000)) {
		writel(VFF_HARD_RST, ch + VFF_RST);
		writel(0, ch + VFF_RST);
		dev_warn(b->dev, "APDMA channel hard reset\n");
	}
}

static void btif_vff_init(struct btif *b, void __iomem *ch, dma_addr_t addr,
			  u32 len, u32 thre)
{
	btif_vff_reset(b, ch);
	writel(lower_32_bits(addr), ch + VFF_ADDR);
	writel(upper_32_bits(addr), ch + VFF_ADDR_H);
	writel(len, ch + VFF_LEN);
	writel(0, ch + VFF_WPT);
	writel(0, ch + VFF_RPT);
	writel(thre, ch + VFF_THRE);
}

/* Sets up both rings before BTIF is switched to DMA mode */
static void btif_dma_init(struct btif *b)
{
	btif_vff_init(b, b->rx_dma, b->rx_vff_addr, BTIF_RX_VFF_SIZE, 1);
	writel(VFF_RX_INT_THRE | VFF_RX_INT_DONE, b->rx_dma + VFF_INT_FLAG);
	writel(VFF_RX_INT_THRE | VFF_RX_INT_DONE, b->rx_dma + VFF_INT_EN);
	writel(1, b->rx_dma + VFF_EN);

	btif_vff_init(b, b->tx_dma, b->tx_vff_addr, BTIF_TX_VFF_SIZE,
		      BTIF_TX_VFF_THRE);
	writel(0, b->tx_dma + VFF_INT_FLAG);
	writel(0, b->tx_dma + VFF_INT_EN);
	writel(1, b->tx_dma + VFF_EN);
	b->tx_flushes = 0;
}

/* Stops a channel after its current transfer, as btif_*_dma_ctrl() */
static void btif_vff_stop(struct btif *b, void __iomem *ch)
{
	u32 stop;

	writel(0, ch + VFF_INT_EN);
	writel(1, ch + VFF_STOP);
	if (readl_poll_timeout_atomic(ch + VFF_STOP, stop, !(stop & 1), 10, 10000))
		dev_warn(b->dev, "APDMA channel did not stop\n");
	writel(0, ch + VFF_EN);
}

/* The tx ring keeps up to 7 bytes until flushed; tx_lock held */
static void btif_tx_flush(struct btif *b)
{
	u32 valid = readl(b->tx_dma + VFF_VALID_SIZE);

	if (valid && valid < 8 && !(readl(b->tx_dma + VFF_STOP) & 1) &&
	    !(readl(b->tx_dma + VFF_FLUSH) & 1))
		writel(1, b->tx_dma + VFF_FLUSH);
}

/* Copies into the tx ring and starts DMA; tx_lock held. Returns bytes taken. */
static unsigned int btif_dma_write(struct btif *b, const u8 *p, unsigned int len)
{
	unsigned int n, off, first, i;
	u32 wpt, wrap;

	n = min_t(unsigned int, len, readl(b->tx_dma + VFF_LEFT_SIZE));
	if (!n)
		return 0;
	/* the write pointer must not move while a flush runs */
	for (i = 0; i < 100 && (readl(b->tx_dma + VFF_FLUSH) & 1); i++)
		udelay(1);

	wpt = readl(b->tx_dma + VFF_WPT);
	off = wpt & VFF_PTR_MASK;
	wrap = wpt & VFF_PTR_WRAP;
	first = min(n, BTIF_TX_VFF_SIZE - off);
	memcpy(b->tx_vff + off, p, first);
	memcpy(b->tx_vff, p + first, n - first);
	off += n;
	if (off >= BTIF_TX_VFF_SIZE) {
		off -= BTIF_TX_VFF_SIZE;
		wrap ^= VFF_PTR_WRAP;
	}
	dma_wmb();	/* the ring before the pointer */
	writel(off | wrap, b->tx_dma + VFF_WPT);
	btif_tx_flush(b);
	writel(1, b->tx_dma + VFF_INT_EN);
	return n;
}

static irqreturn_t btif_tx_dma_irq(int irq, void *data)
{
	struct btif *b = data;
	unsigned long flags;
	u32 left, valid;

	spin_lock_irqsave(&b->tx_lock, flags);
	valid = readl(b->tx_dma + VFF_VALID_SIZE);
	left = readl(b->tx_dma + VFF_LEFT_SIZE);
	if (valid && valid < 8) {
		/* the flag stays set until the flush has moved the tail */
		btif_tx_flush(b);
		if (++b->tx_flushes >= 512) {
			writel(0, b->tx_dma + VFF_INT_FLAG);
			writel(0, b->tx_dma + VFF_INT_EN);
			dev_err_ratelimited(b->dev, "tx flush does not complete\n");
		}
	} else if (left == BTIF_TX_VFF_SIZE) {
		b->tx_flushes = 0;
		writel(0, b->tx_dma + VFF_INT_FLAG);
		writel(0, b->tx_dma + VFF_INT_EN);
	} else {
		writel(0, b->tx_dma + VFF_INT_FLAG);
	}
	spin_unlock_irqrestore(&b->tx_lock, flags);
	return IRQ_HANDLED;
}

/* Moves what the rx ring holds into rx_fifo for the rx thread */
static irqreturn_t btif_rx_dma_irq(int irq, void *data)
{
	struct btif *b = data;
	unsigned long flags;
	bool rx = false;

	spin_lock_irqsave(&b->rx_lock, flags);
	writel(VFF_RX_INT_THRE | VFF_RX_INT_DONE, b->rx_dma + VFF_INT_FLAG);
	while (readl(b->rx_dma + VFF_VALID_SIZE)) {
		u32 rpt = readl(b->rx_dma + VFF_RPT);
		u32 wpt = readl(b->rx_dma + VFF_WPT);
		unsigned int off = rpt & VFF_PTR_MASK, len, first, n;

		if ((rpt ^ wpt) & VFF_PTR_WRAP)
			len = BTIF_RX_VFF_SIZE - off + (wpt & VFF_PTR_MASK);
		else
			len = (wpt & VFF_PTR_MASK) - off;
		if (!len)
			break;
		dma_rmb();	/* the pointer before the ring */
		first = min(len, BTIF_RX_VFF_SIZE - off);
		n = kfifo_in(&b->rx_fifo, b->rx_vff + off, first);
		n += kfifo_in(&b->rx_fifo, b->rx_vff, len - first);
		if (n < len) {
			b->rx_overruns++;
			dev_err_ratelimited(b->dev, "rx overrun, %u bytes lost\n",
					    len - n);
		}
		b->rx_bytes += len;
		off += len;
		rpt &= VFF_PTR_WRAP;
		if (off >= BTIF_RX_VFF_SIZE) {
			off -= BTIF_RX_VFF_SIZE;
			rpt ^= VFF_PTR_WRAP;
		}
		writel(off | rpt, b->rx_dma + VFF_RPT);
		rx = true;
	}
	spin_unlock_irqrestore(&b->rx_lock, flags);

	if (rx)
		wake_up(&b->rx_wq);
	return IRQ_HANDLED;
}

/* Moves queued bytes into the hardware FIFO; tx_lock held */
static void btif_tx_fill(struct btif *b)
{
	u32 ier;

	while (!kfifo_is_empty(&b->tx_fifo)) {
		u32 lsr = readl(b->base + BTIF_LSR);
		unsigned int room;
		u8 c;

		if (lsr & BTIF_LSR_TEMT)
			room = BTIF_TX_FIFO_SIZE;
		else if (lsr & BTIF_LSR_THRE)
			room = BTIF_TX_FIFO_SIZE - BTIF_TX_FIFO_THRE;
		else
			break;
		while (room-- && kfifo_get(&b->tx_fifo, &c))
			writeb(c, b->base + BTIF_THR);
	}

	/* the tx-empty interrupt refills while anything is queued */
	ier = readl(b->base + BTIF_IER);
	if (kfifo_is_empty(&b->tx_fifo))
		ier &= ~BTIF_IER_TXEEN;
	else
		ier |= BTIF_IER_TXEEN;
	writel(ier, b->base + BTIF_IER);
}

static irqreturn_t btif_irq(int irq, void *data)
{
	struct btif *b = data;
	unsigned long flags;
	bool rx = false;

	spin_lock_irqsave(&b->rx_lock, flags);
	while (readl(b->base + BTIF_IIR) & (BTIF_IIR_RX | BTIF_IIR_RX_TIMEOUT)) {
		u8 c;

		if (readl(b->base + BTIF_LSR) & BTIF_LSR_OE) {
			b->rx_overruns++;
			dev_warn_ratelimited(b->dev, "rx FIFO overrun (%lu)\n",
					     b->rx_overruns);
		}
		c = readb(b->base + BTIF_RBR);
		b->rx_bytes++;
		if (!kfifo_put(&b->rx_fifo, c))
			dev_err_ratelimited(b->dev, "rx overrun\n");
		rx = true;
	}
	spin_unlock_irqrestore(&b->rx_lock, flags);

	spin_lock_irqsave(&b->tx_lock, flags);
	if (readl(b->base + BTIF_IER) & BTIF_IER_TXEEN)
		btif_tx_fill(b);
	spin_unlock_irqrestore(&b->tx_lock, flags);

	if (rx)
		wake_up(&b->rx_wq);
	return IRQ_HANDLED;
}

/* Hands received bytes to STP in process context, as the vendor rx thread */
static int btif_rx_thread(void *data)
{
	struct btif *b = data;
	u8 buf[256];

	while (!kthread_should_stop()) {
		unsigned long flags;
		unsigned int n;

		wait_event_interruptible(b->rx_wq, !kfifo_is_empty(&b->rx_fifo) ||
					 kthread_should_stop());
		for (;;) {
			spin_lock_irqsave(&b->rx_lock, flags);
			n = kfifo_out(&b->rx_fifo, buf, sizeof(buf));
			spin_unlock_irqrestore(&b->rx_lock, flags);
			if (!n)
				break;
			btif_trace_add(false, buf, n);
			btif_rxlog_add(buf, n);
			if (READ_ONCE(b->rx_cb))
				b->rx_cb(buf, n);
		}
	}
	return 0;
}

static struct btif *btif_get(unsigned long id)
{
	if (!g_btif || id != BTIF_USER_ID)
		return NULL;
	return g_btif;
}

int mtk_wcn_btif_open(char *p_owner, unsigned long *p_id)
{
	struct btif *b = g_btif;
	int ret;

	if (!b)
		return -ENODEV;
	mutex_lock(&b->lock);
	if (b->opened) {
		ret = -EBUSY;
		goto out;
	}
	ret = clk_prepare_enable(b->clk);
	if (ret)
		goto out;
	if (b->dma) {
		ret = clk_prepare_enable(b->dma_clk);
		if (ret) {
			clk_disable_unprepare(b->clk);
			goto out;
		}
		btif_dma_init(b);
	}
	kfifo_reset(&b->tx_fifo);
	btif_hw_init(b);
	if (b->dma) {
		enable_irq(b->rx_irq);
		enable_irq(b->tx_irq);
	} else {
		enable_irq(b->irq);
	}
	spin_lock_irq(&b->tx_lock);
	b->opened = true;
	spin_unlock_irq(&b->tx_lock);
	*p_id = BTIF_USER_ID;
	dev_info(b->dev, "opened by %s\n", p_owner);
out:
	mutex_unlock(&b->lock);
	return ret;
}
EXPORT_SYMBOL(mtk_wcn_btif_open);

int mtk_wcn_btif_close(unsigned long u_id)
{
	struct btif *b = btif_get(u_id);

	if (!b)
		return -EINVAL;
	mutex_lock(&b->lock);
	if (b->opened) {
		spin_lock_irq(&b->tx_lock);
		b->opened = false;
		writel(0, b->base + BTIF_IER);
		spin_unlock_irq(&b->tx_lock);
		if (b->dma) {
			disable_irq(b->tx_irq);
			disable_irq(b->rx_irq);
			btif_vff_stop(b, b->tx_dma);
			btif_vff_stop(b, b->rx_dma);
			writel(BTIF_DMA_EN_AUTORST, b->base + BTIF_DMA_EN);
			clk_disable_unprepare(b->dma_clk);
		} else {
			disable_irq(b->irq);
		}
		clk_disable_unprepare(b->clk);
		b->rx_cb = NULL;
	}
	mutex_unlock(&b->lock);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_btif_close);

/*
 * Queues the buffer and starts sending; never sleeps (see tx_fifo).
 * Returns the bytes queued: all of them unless STP outran the 32 KB queue
 * (the tx ring with DMA).
 */
int mtk_wcn_btif_write(unsigned long u_id, const unsigned char *p_buf,
		       unsigned int len)
{
	struct btif *b = btif_get(u_id);
	unsigned long flags;
	unsigned int n;

	if (!b || !p_buf)
		return -EINVAL;
	spin_lock_irqsave(&b->tx_lock, flags);
	if (!b->opened) {
		spin_unlock_irqrestore(&b->tx_lock, flags);
		return -EIO;
	}
	if (b->dma) {
		n = btif_dma_write(b, p_buf, len);
	} else {
		n = kfifo_in(&b->tx_fifo, p_buf, len);
		btif_tx_fill(b);
	}
	btif_trace_add(true, p_buf, n);
	spin_unlock_irqrestore(&b->tx_lock, flags);
	if (n < len)
		dev_err_ratelimited(b->dev, "tx queue full, %u of %u bytes\n",
				    n, len);
	return n;
}
EXPORT_SYMBOL(mtk_wcn_btif_write);

int mtk_wcn_btif_read(unsigned long u_id, unsigned char *p_buf,
		      unsigned int max_len)
{
	return -EOPNOTSUPP;	/* STP takes its data through the callback */
}
EXPORT_SYMBOL(mtk_wcn_btif_read);

int mtk_wcn_btif_rx_cb_register(unsigned long u_id, MTK_WCN_BTIF_RX_CB rx_cb)
{
	struct btif *b = btif_get(u_id);

	if (!b)
		return -EINVAL;
	WRITE_ONCE(b->rx_cb, rx_cb);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_btif_rx_cb_register);

/* Pulse the AP-to-CONNSYS wake line low, as hal_btif_raise_wak_sig() */
int mtk_wcn_btif_wakeup_consys(unsigned long u_id)
{
	struct btif *b = btif_get(u_id);

	if (!b || !b->opened)
		return -EINVAL;
	writel(readl(b->base + BTIF_WAK) & ~1, b->base + BTIF_WAK);
	usleep_range(128, 160);
	writel(readl(b->base + BTIF_WAK) | 1, b->base + BTIF_WAK);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_btif_wakeup_consys);

/* Deep idle needs no action with the clock kept on while open */
int mtk_wcn_btif_dpidle_ctrl(unsigned long u_id, enum _ENUM_BTIF_DPIDLE_ en_flag)
{
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_btif_dpidle_ctrl);

int mtk_wcn_btif_loopback_ctrl(unsigned long u_id,
			       enum _ENUM_BTIF_LPBK_MODE_ enable)
{
	struct btif *b = btif_get(u_id);
	u32 v;

	if (!b || !b->opened)
		return -EINVAL;
	v = readl(b->base + BTIF_TRI_LVL) & ~BIT(7);
	writel(v | (enable == BTIF_LPBK_ENABLE ? BIT(7) : 0), b->base + BTIF_TRI_LVL);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_btif_loopback_ctrl);

int mtk_wcn_btif_dbg_ctrl(unsigned long u_id, enum _ENUM_BTIF_DBG_ID_ flag)
{
	struct btif *b = btif_get(u_id);

	if (!b || !b->opened)
		return -EINVAL;
	dev_info(b->dev, "dbg %d: IER 0x%x IIR 0x%x LSR 0x%x TRI 0x%x WAK 0x%x\n",
		 flag, readl(b->base + BTIF_IER), readl(b->base + BTIF_IIR),
		 readl(b->base + BTIF_LSR), readl(b->base + BTIF_TRI_LVL),
		 readl(b->base + BTIF_WAK));
	if (b->dma) {
		dev_info(b->dev, "tx VFF EN %u WPT 0x%x RPT 0x%x valid %u left %u int %u/%u flush %u\n",
			 readl(b->tx_dma + VFF_EN), readl(b->tx_dma + VFF_WPT),
			 readl(b->tx_dma + VFF_RPT), readl(b->tx_dma + VFF_VALID_SIZE),
			 readl(b->tx_dma + VFF_LEFT_SIZE), readl(b->tx_dma + VFF_INT_FLAG),
			 readl(b->tx_dma + VFF_INT_EN), readl(b->tx_dma + VFF_FLUSH));
		dev_info(b->dev, "rx VFF EN %u WPT 0x%x RPT 0x%x valid %u int %u/%u DMA_EN 0x%x\n",
			 readl(b->rx_dma + VFF_EN), readl(b->rx_dma + VFF_WPT),
			 readl(b->rx_dma + VFF_RPT), readl(b->rx_dma + VFF_VALID_SIZE),
			 readl(b->rx_dma + VFF_INT_FLAG), readl(b->rx_dma + VFF_INT_EN),
			 readl(b->base + BTIF_DMA_EN));
	}
	btif_trace_dump(b->dev);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_btif_dbg_ctrl);

/* The vendor driver searches its rx log for a WMT event; no log kept here */
bool mtk_wcn_btif_parser_wmt_evt(unsigned long u_id, const char *sub_str,
				 unsigned int str_len)
{
	return false;
}
EXPORT_SYMBOL(mtk_wcn_btif_parser_wmt_evt);

int mtk_btif_exp_rx_has_pending_data(unsigned long u_id)
{
	struct btif *b = btif_get(u_id);

	if (!b)
		return 0;
	return !kfifo_is_empty(&b->rx_fifo) ||
	       (b->dma && b->opened && readl(b->rx_dma + VFF_VALID_SIZE));
}
EXPORT_SYMBOL(mtk_btif_exp_rx_has_pending_data);

int mtk_btif_exp_tx_has_pending_data(unsigned long u_id)
{
	struct btif *b = btif_get(u_id);

	if (!b || !b->opened)
		return 0;
	if (b->dma && (readl(b->tx_dma + VFF_VALID_SIZE) ||
		       readl(b->tx_dma + VFF_INT_BUF_SIZE)))
		return 1;
	return !kfifo_is_empty(&b->tx_fifo) ||
	       !(readl(b->base + BTIF_LSR) & BTIF_LSR_TEMT);
}
EXPORT_SYMBOL(mtk_btif_exp_tx_has_pending_data);

struct task_struct *mtk_btif_exp_rx_thread_get(unsigned long u_id)
{
	struct btif *b = btif_get(u_id);

	return b ? b->rx_thread : NULL;
}
EXPORT_SYMBOL(mtk_btif_exp_rx_thread_get);

static ssize_t rx_stats_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct btif *b = dev_get_drvdata(dev);

	return sysfs_emit(buf, "bytes %lu overruns %lu\n", b->rx_bytes,
			  b->rx_overruns);
}
static DEVICE_ATTR_RO(rx_stats);

static ssize_t rx_log_read(struct file *f, struct kobject *kobj,
			   struct bin_attribute *attr, char *buf,
			   loff_t off, size_t count)
{
	unsigned long flags, len, start, i;

	spin_lock_irqsave(&btif_rxlog_lock, flags);
	len = min_t(unsigned long, btif_rxlog_pos, BTIF_RXLOG_SIZE);
	start = btif_rxlog_pos - len;
	if (off >= len) {
		count = 0;
	} else {
		count = min_t(size_t, count, len - off);
		for (i = 0; i < count; i++)
			buf[i] = btif_rxlog[(start + off + i) % BTIF_RXLOG_SIZE];
	}
	spin_unlock_irqrestore(&btif_rxlog_lock, flags);
	return count;
}
static BIN_ATTR_RO(rx_log, BTIF_RXLOG_SIZE);

static struct attribute *btif_attrs[] = {
	&dev_attr_rx_stats.attr,
	NULL
};

static struct bin_attribute *btif_bin_attrs[] = {
	&bin_attr_rx_log,
	NULL
};

static const struct attribute_group btif_group = {
	.attrs = btif_attrs,
	.bin_attrs = btif_bin_attrs,
};
__ATTRIBUTE_GROUPS(btif);

/*
 * DMA when the node has both APDMA channels, their interrupts and clock;
 * PIO otherwise. The rings are coherent and below 4 GB, so VFF_ADDR_H
 * stays 0 (the vendor driver's "4G mode" is not needed).
 */
static int btif_dma_probe(struct platform_device *pdev, struct btif *b)
{
	struct device *dev = &pdev->dev;
	int ret;

	if (!platform_get_resource(pdev, IORESOURCE_MEM, 2))
		return 0;
	b->tx_dma = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(b->tx_dma))
		return PTR_ERR(b->tx_dma);
	b->rx_dma = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(b->rx_dma))
		return PTR_ERR(b->rx_dma);
	b->tx_irq = platform_get_irq(pdev, 1);
	if (b->tx_irq < 0)
		return b->tx_irq;
	b->rx_irq = platform_get_irq(pdev, 2);
	if (b->rx_irq < 0)
		return b->rx_irq;
	b->dma_clk = devm_clk_get(dev, "apdma");
	if (IS_ERR(b->dma_clk))
		return dev_err_probe(dev, PTR_ERR(b->dma_clk), "no APDMA clock\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	b->tx_vff = dmam_alloc_coherent(dev, BTIF_TX_VFF_SIZE, &b->tx_vff_addr,
					GFP_KERNEL);
	b->rx_vff = dmam_alloc_coherent(dev, BTIF_RX_VFF_SIZE, &b->rx_vff_addr,
					GFP_KERNEL);
	if (!b->tx_vff || !b->rx_vff)
		return -ENOMEM;
	b->dma = true;
	return 0;
}

static int btif_probe(struct platform_device *pdev)
{
	struct btif *b;
	int ret;

	b = devm_kzalloc(&pdev->dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = &pdev->dev;
	mutex_init(&b->lock);
	spin_lock_init(&b->rx_lock);
	init_waitqueue_head(&b->rx_wq);
	spin_lock_init(&b->tx_lock);

	b->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(b->base))
		return PTR_ERR(b->base);
	b->clk = devm_clk_get(&pdev->dev, "main");
	if (IS_ERR(b->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(b->clk), "no clock\n");
	b->irq = platform_get_irq(pdev, 0);
	if (b->irq < 0)
		return b->irq;
	ret = btif_dma_probe(pdev, b);
	if (ret)
		return ret;
	ret = kfifo_alloc(&b->rx_fifo, BTIF_RX_KFIFO_SIZE, GFP_KERNEL);
	if (ret)
		return ret;
	ret = kfifo_alloc(&b->tx_fifo, BTIF_TX_KFIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto err_fifo;
	/* kept off until STP opens the link: the clock is gated until then */
	ret = devm_request_irq(&pdev->dev, b->irq, btif_irq, IRQF_NO_AUTOEN,
			       dev_name(&pdev->dev), b);
	if (!ret && b->dma)
		ret = devm_request_irq(&pdev->dev, b->tx_irq, btif_tx_dma_irq,
				       IRQF_NO_AUTOEN, "btif-tx-dma", b);
	if (!ret && b->dma)
		ret = devm_request_irq(&pdev->dev, b->rx_irq, btif_rx_dma_irq,
				       IRQF_NO_AUTOEN, "btif-rx-dma", b);
	if (ret)
		goto err_fifo;
	b->rx_thread = kthread_run(btif_rx_thread, b, "btif_rxd");
	if (IS_ERR(b->rx_thread)) {
		ret = PTR_ERR(b->rx_thread);
		goto err_fifo;
	}
	sched_set_fifo(b->rx_thread);

	platform_set_drvdata(pdev, b);
	g_btif = b;
	if (b->dma)
		dev_info(&pdev->dev, "DMA mode, tx ring %u, rx ring %u bytes\n",
			 BTIF_TX_VFF_SIZE, BTIF_RX_VFF_SIZE);
	else
		dev_info(&pdev->dev, "PIO mode, irq %d\n", b->irq);
	return 0;

err_fifo:
	kfifo_free(&b->tx_fifo);
	kfifo_free(&b->rx_fifo);
	return ret;
}

/*
 * The SPM's deep sleep resets the BTIF and its APDMA channels (infra); an
 * open BTIF is then set up again as mtk_wcn_btif_open() does, or every
 * WMT/STP exchange times out (Bluetooth "Opcode 0x0c03 failed: -110",
 * Wi-Fi gone; measured). After s2idle the registers survive and nothing is
 * touched. Before the CONNSYS drivers thaw.
 */
static int btif_resume_noirq(struct device *dev)
{
	struct btif *b = dev_get_drvdata(dev);
	u32 dma_en = b->dma ? BTIF_DMA_EN_AUTORST | BTIF_DMA_EN_TX | BTIF_DMA_EN_RX :
			      BTIF_DMA_EN_AUTORST;

	if (!b->opened || readl(b->base + BTIF_DMA_EN) == dma_en)
		return 0;

	dev_info(dev, "reset in suspend, setting it up again\n");
	if (b->dma)
		btif_dma_init(b);
	kfifo_reset(&b->tx_fifo);
	btif_hw_init(b);
	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(btif_pm_ops, NULL, btif_resume_noirq);

static void btif_remove(struct platform_device *pdev)
{
	struct btif *b = platform_get_drvdata(pdev);

	g_btif = NULL;
	kthread_stop(b->rx_thread);
	if (b->opened)
		mtk_wcn_btif_close(BTIF_USER_ID);
	kfifo_free(&b->tx_fifo);
	kfifo_free(&b->rx_fifo);
}

static const struct of_device_id btif_of_match[] = {
	{ .compatible = "mediatek,mt6771-btif" },
	{ }
};
MODULE_DEVICE_TABLE(of, btif_of_match);

static struct platform_driver btif_driver = {
	.probe = btif_probe,
	.remove_new = btif_remove,
	.driver = {
		.name = "mtk-btif",
		.of_match_table = btif_of_match,
		.dev_groups = btif_groups,
		.pm = pm_sleep_ptr(&btif_pm_ops),
	},
};
module_platform_driver(btif_driver);

MODULE_DESCRIPTION("MediaTek BTIF (APDMA or PIO) for the connectivity modules");
MODULE_LICENSE("GPL");
