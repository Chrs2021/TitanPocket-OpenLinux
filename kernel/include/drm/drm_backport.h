/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Small compatibility layer for DRM code backported from Linux 5.10 to this
 * 4.14 tree (scheduler, GEM shmem helpers, Panfrost). Each item is what the
 * newer kernel provides, expressed with 4.14 primitives.
 */
#ifndef __DRM_BACKPORT_H__
#define __DRM_BACKPORT_H__

#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <drm/drmP.h>		/* DRM_DEBUG_*, DRM_ERROR, ... */
#include <drm/drm_print.h>

/* 4.17 (this tree has no vm_fault_t) */
typedef int vm_fault_t;

/* 5.8: map/unmap a whole sg_table, nents updated, 0 or -errno */
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
				  enum dma_data_direction dir, unsigned long attrs)
{
	int nents = dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);

	if (nents <= 0)
		return -EINVAL;
	sgt->nents = nents;
	return 0;
}

static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
				     enum dma_data_direction dir, unsigned long attrs)
{
	dma_unmap_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
}

/* 4.20 */
#ifndef drm_printf_indent
#define drm_printf_indent(printer, indent, fmt, ...) \
	drm_printf((printer), "%.*s" fmt, (indent), "\t\t\t\t\t\t\t\t\t\t", ##__VA_ARGS__)
#endif

/* 5.8 */
#define for_each_sgtable_dma_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

/* 4.16 */
#include <linux/sizes.h>
#ifndef SZ_4G
#define SZ_4G			_AC(0x100000000, ULL)
#endif

/* 5.1 / 4.18 (used by generic Panfrost paths, not on MT6771) */
#include <linux/clk.h>
#include <linux/err.h>
static inline struct clk *devm_clk_get_optional(struct device *dev, const char *id)
{
	struct clk *clk = devm_clk_get(dev, id);

	if (PTR_ERR(clk) == -ENOENT)
		return NULL;
	return clk;
}

struct device;
static inline struct device *dev_pm_domain_attach_by_name(struct device *dev,
							  const char *name)
{
	return ERR_PTR(-EOPNOTSUPP);
}

/*
 * 4.18/4.19: &dma_fence_ops.enable_signaling and .wait became optional. 4.14's
 * dma_fence_init() BUG()s without them, so backported fence ops add these,
 * which are what the newer core assumes when they are absent.
 */
#include <linux/dma-fence.h>
static inline bool dma_fence_backport_enable_signaling(struct dma_fence *fence)
{
	return true;
}
#define DMA_FENCE_BACKPORT_OPS \
	.enable_signaling = dma_fence_backport_enable_signaling, \
	.wait = dma_fence_default_wait

/* 4.15 */
#include <drm/drm_drv.h>
#define drm_dev_put		drm_dev_unref

#endif /* __DRM_BACKPORT_H__ */
