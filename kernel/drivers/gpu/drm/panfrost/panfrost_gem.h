/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */

#ifndef __PANFROST_GEM_H__
#define __PANFROST_GEM_H__

#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>
#include <linux/dma-buf.h>
#include <linux/reservation.h>
#include <linux/dma-resv.h>

struct panfrost_mmu;

struct panfrost_gem_object {
	struct drm_gem_shmem_object base;
	struct sg_table *sgts;

	/*
	 * Use a list for now. If searching a mapping ever becomes the
	 * bottleneck, we should consider using an RB-tree, or even better,
	 * let the core store drm_gem_object_mapping entries (where we
	 * could place driver specific data) instead of drm_gem_object ones
	 * in its drm_file->object_idr table.
	 *
	 * struct drm_gem_object_mapping {
	 *	struct drm_gem_object *obj;
	 *	void *driver_priv;
	 * };
	 */
	struct {
		struct list_head list;
		struct mutex lock;
	} mappings;

	/*
	 * Count the number of jobs referencing this BO so we don't let the
	 * shrinker reclaim this object prematurely.
	 */
	atomic_t gpu_usecount;

	bool noexec		:1;
	bool is_heap		:1;

	/*
	 * 4.14 backport: struct drm_gem_object has no resv (5.2); implicit
	 * fences live here, or in the dma-buf for imported objects.
	 */
	struct reservation_object _resv;
};

struct panfrost_gem_mapping {
	struct list_head node;
	struct kref refcount;
	struct panfrost_gem_object *obj;
	struct drm_mm_node mmnode;
	struct panfrost_mmu *mmu;
	bool active		:1;
};

static inline
struct  panfrost_gem_object *to_panfrost_bo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct panfrost_gem_object, base);
}

/* the reservation object holding a BO's implicit fences */
static inline struct reservation_object *panfrost_gem_resv(struct drm_gem_object *obj)
{
	if (obj->import_attach)
		return obj->import_attach->dmabuf->resv;
	return &to_panfrost_bo(obj)->_resv;
}

static inline struct panfrost_gem_mapping *
drm_mm_node_to_panfrost_mapping(struct drm_mm_node *node)
{
	return container_of(node, struct panfrost_gem_mapping, mmnode);
}

struct drm_gem_object *panfrost_gem_create_object(struct drm_device *dev, size_t size);

struct drm_gem_object *
panfrost_gem_prime_import_sg_table(struct drm_device *dev,
				   struct dma_buf_attachment *attach,
				   struct sg_table *sgt);

struct panfrost_gem_object *
panfrost_gem_create_with_handle(struct drm_file *file_priv,
				struct drm_device *dev, size_t size,
				u32 flags,
				uint32_t *handle);

int panfrost_gem_open(struct drm_gem_object *obj, struct drm_file *file_priv);
void panfrost_gem_free_object(struct drm_gem_object *obj);
int panfrost_gem_pin(struct drm_gem_object *obj);
struct reservation_object *panfrost_gem_prime_res_obj(struct drm_gem_object *obj);
void panfrost_gem_close(struct drm_gem_object *obj,
			struct drm_file *file_priv);

struct panfrost_gem_mapping *
panfrost_gem_mapping_get(struct panfrost_gem_object *bo,
			 struct panfrost_file_priv *priv);
void panfrost_gem_mapping_put(struct panfrost_gem_mapping *mapping);
void panfrost_gem_teardown_mappings_locked(struct panfrost_gem_object *bo);

void panfrost_gem_shrinker_init(struct drm_device *dev);
void panfrost_gem_shrinker_cleanup(struct drm_device *dev);

#endif /* __PANFROST_GEM_H__ */
