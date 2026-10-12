// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 (Helio P60/P70) power glue for the backported Panfrost driver.
 *
 * The BSP device tree gives the Mali node no clocks, regulators or power
 * domains: MediaTek's gpufreq driver (drivers/misc/mediatek/base/power/
 * mt6771/mtk_gpufreq_core.c) owns the VGPU/VSRAM_GPU bucks, the MFG MTCMOS
 * domains (async + 3 shader cores) and the MFG clock gate. This follows the
 * power sequence of kbase's MT6771 platform code
 * (gpu_mali/.../platform/mt6771/mali_kbase_config_mt6771.c).
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "panfrost_device.h"
#include "panfrost_mtk.h"
#include "panfrost_regs.h"

/* mtk_gpufreq_core.c */
extern bool mt_gpufreq_probed(void);
extern unsigned int mt_gpufreq_voltage_enable_set(unsigned int enable);
extern void mt_gpufreq_enable_MTCMOS(void);
extern void mt_gpufreq_disable_MTCMOS(void);
extern void mt_gpufreq_enable_CG(void);
extern void mt_gpufreq_disable_CG(void);

/* MFGCFG registers */
#define MFG_TIMESTAMP		0x130	/* bit 0: enable the GPU TIMESTAMP counter */
#define MFG_QCHANNEL_CON	0x0b4
#define MFG_DEBUG_SEL		0x180
#define MFG_DEBUG_TOP		0x188
#define   MFG_DEBUG_TOP_BUS_IDLE	BIT(2)

static void __iomem *mfg_base;
static bool mfg_on;		/* MTCMOS + clock gate */
static bool bucks_on;		/* VGPU / VSRAM_GPU (regulator use counts) */

static void panfrost_mtk_wait_mfg_idle(void)
{
	int i;

	writel(0x1, mfg_base + MFG_QCHANNEL_CON);
	writel(0x3, mfg_base + MFG_DEBUG_SEL);
	for (i = 0; i < 1000; i++) {
		if (readl(mfg_base + MFG_DEBUG_TOP) & MFG_DEBUG_TOP_BUS_IDLE)
			return;
		udelay(10);
	}
	pr_warn("panfrost: MFG bus not idle before power off\n");
}

/*
 * Vendor quirk (applied after every GPU reset): the MT6771 integration
 * reports no coherency support (COHERENCY_FEATURES = 0) but the register
 * resets to ACE-Lite; select "none" as kbase does.
 */
void panfrost_mtk_quirks(struct panfrost_device *pfdev)
{
	gpu_write(pfdev, GPU_COHERENCY_ENABLE, COHERENCY_NONE);
}

/*
 * Called at probe and on every runtime resume (Panfrost autosuspends ~50 ms
 * after the last job), like kbase's pm_callback_power_on. The GPU state is
 * lost while MFG is off; panfrost_device_resume() resets and reinitializes it.
 */
void panfrost_mtk_power_on(struct panfrost_device *pfdev)
{
	if (!bucks_on) {
		mt_gpufreq_voltage_enable_set(1);
		bucks_on = true;
	}
	if (mfg_on)
		return;
	mt_gpufreq_enable_MTCMOS();
	mt_gpufreq_enable_CG();
	writel(0x1, mfg_base + MFG_TIMESTAMP);
	mfg_on = true;
}

/* runtime suspend: MFG off, bucks stay on (kbase pm_callback_power_off) */
void panfrost_mtk_power_off(struct panfrost_device *pfdev)
{
	if (!mfg_on)
		return;
	panfrost_mtk_wait_mfg_idle();
	mt_gpufreq_disable_CG();
	mt_gpufreq_disable_MTCMOS();
	mfg_on = false;
}

/* system suspend, after MFG is off: bucks off too (pm_callback_power_suspend) */
void panfrost_mtk_suspend(struct panfrost_device *pfdev)
{
	panfrost_mtk_power_off(pfdev);
	if (bucks_on) {
		mt_gpufreq_voltage_enable_set(0);
		bucks_on = false;
	}
}

int panfrost_mtk_init(struct panfrost_device *pfdev)
{
	struct device_node *np;

	/* gpufreq (drivers/misc) owns the clocks; it probes after us */
	if (!mt_gpufreq_probed())
		return -EPROBE_DEFER;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mfgcfg");
	if (!np) {
		dev_err(pfdev->dev, "no mediatek,mfgcfg node\n");
		return -ENODEV;
	}
	mfg_base = of_iomap(np, 0);
	of_node_put(np);
	if (!mfg_base) {
		dev_err(pfdev->dev, "cannot map MFGCFG\n");
		return -ENOMEM;
	}

	panfrost_mtk_power_on(pfdev);
	dev_info(pfdev->dev, "MT6771 GPU powered via gpufreq\n");
	return 0;
}

void panfrost_mtk_fini(struct panfrost_device *pfdev)
{
	panfrost_mtk_power_off(pfdev);
	iounmap(mfg_base);
	mfg_base = NULL;
}
