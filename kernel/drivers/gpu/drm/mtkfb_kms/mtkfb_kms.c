// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal DRM/KMS driver on top of MediaTek's legacy framebuffer (mtkfb).
 *
 * The BSP drives the MT6771 display engine (OVL/RDMA/DSI) only through
 * mtkfb. This driver gives userspace a KMS device so that Wayland
 * compositors and Mesa work: one CRTC/plane/connector with the panel's
 * single mode, GEM shmem dumb buffers that can be shared with Panfrost
 * (Mesa's "kmsro" pairs a KMS driver named "mediatek" with the GPU).
 *
 * Scanout is by copy: on every plane update the framebuffer is copied into
 * one of mtkfb's three buffers (its virtual area is 3 x the screen), which
 * is then shown with fb_pan_display(). 720x720x4 = 2 MB per frame.
 *
 * While the CRTC is on, the fbdev console is suspended and mtkfb switched to
 * XRGB8888 byte order (red at bit 16); both are restored when it goes off,
 * including when the last DRM client exits.
 */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/console.h>
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define DRIVER_NAME	"mediatek"	/* the name Mesa's kmsro looks for */
#define NBUF		3

struct mtkfb_kms {
	struct drm_device *drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct fb_info *info;
	void __iomem *fbmem;		/* write-combined map of the fb memory */
	struct fb_var_screeninfo saved_var;
	unsigned int front;		/* buffer being shown */
	bool on;
};

static struct platform_device *mtkfb_kms_pdev;

static inline struct mtkfb_kms *to_mk(struct drm_device *drm)
{
	return drm->dev_private;
}

/* ---- mtkfb access (same locking as the FBIOPAN/FBIOPUT ioctls) ---- */

static int mk_fb_set(struct mtkfb_kms *mk, struct fb_var_screeninfo *var, bool pan)
{
	struct fb_info *info = mk->info;
	int ret = -ENODEV;

	console_lock();
	if (lock_fb_info(info)) {
		ret = pan ? fb_pan_display(info, var) : fb_set_var(info, var);
		unlock_fb_info(info);
	}
	console_unlock();
	return ret;
}

static void mk_fb_suspend(struct mtkfb_kms *mk, bool suspend)
{
	console_lock();
	fb_set_suspend(mk->info, suspend ? FBINFO_STATE_SUSPENDED : FBINFO_STATE_RUNNING);
	console_unlock();
}

/* ---- copy a framebuffer to the screen ---- */

static int mk_copy_fb(struct mtkfb_kms *mk, struct drm_framebuffer *fb,
		      unsigned int sx, unsigned int sy, void __iomem *dst)
{
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	unsigned int line = mk->info->fix.line_length;
	unsigned int w = mk->mode.hdisplay, h = mk->mode.vdisplay, y;
	struct sg_table *sgt;
	void *src, *vaddr;
	bool imported = obj->import_attach;

	if (imported) {
		/* exporter's mapping (Panfrost: write-combined) */
		vaddr = dma_buf_vmap(obj->import_attach->dmabuf);
		if (!vaddr)
			return -ENOMEM;
	} else {
		sgt = drm_gem_shmem_get_pages_sgt(obj);
		if (IS_ERR(sgt))
			return PTR_ERR(sgt);
		/*
		 * Read through a cached mapping: much faster than the
		 * write-combined one. The GPU or a user WC mapping wrote
		 * memory directly, so invalidate stale lines first.
		 */
		dma_sync_sg_for_cpu(mk->drm->dev, sgt->sgl, sgt->orig_nents,
				    DMA_BIDIRECTIONAL);
		vaddr = vmap(shmem->pages, obj->size >> PAGE_SHIFT, VM_MAP, PAGE_KERNEL);
		if (!vaddr)
			return -ENOMEM;
	}

	src = vaddr + fb->offsets[0] + sy * fb->pitches[0] + sx * 4;
	for (y = 0; y < h; y++)
		memcpy((void __force *)dst + y * line, src + y * fb->pitches[0], w * 4);

	if (imported)
		dma_buf_vunmap(obj->import_attach->dmabuf, vaddr);
	else
		vunmap(vaddr);
	return 0;
}

/*
 * mtkfb resets its pixel format to the native order (red at bit 0) on
 * unblank/resume; put XRGB8888 back before showing a frame.
 */
static void mk_fb_ensure_xrgb(struct mtkfb_kms *mk)
{
	struct fb_var_screeninfo var = mk->info->var;

	if (var.red.offset == 16 && var.blue.offset == 0)
		return;
	var.red.offset = 16;
	var.blue.offset = 0;
	var.activate = FB_ACTIVATE_NOW;
	if (mk_fb_set(mk, &var, false))
		DRM_ERROR("cannot switch mtkfb to XRGB8888\n");
}

static void mk_show(struct mtkfb_kms *mk, struct drm_plane_state *state)
{
	struct fb_var_screeninfo var;
	unsigned int back = (mk->front + 1) % NBUF;
	unsigned int yoff = back * mk->info->var.yres_virtual / NBUF;

	mk_fb_ensure_xrgb(mk);
	if (mk_copy_fb(mk, state->fb, state->src_x >> 16, state->src_y >> 16,
		       mk->fbmem + yoff * mk->info->fix.line_length))
		return;

	var = mk->info->var;
	var.xoffset = 0;
	var.yoffset = yoff;
	if (!mk_fb_set(mk, &var, true))
		mk->front = back;
}

/* ---- display pipe ---- */

static void mk_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state)
{
	struct mtkfb_kms *mk = to_mk(pipe->crtc.dev);

	mk_fb_suspend(mk, true);
	mk->saved_var = mk->info->var;
	mk_fb_ensure_xrgb(mk);
	mk->front = 0;
	mk->on = true;

	/* the atomic helpers update planes before enabling the CRTC */
	if (pipe->plane.state->fb && pipe->plane.state->visible)
		mk_show(mk, pipe->plane.state);
}

static void mk_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct mtkfb_kms *mk = to_mk(pipe->crtc.dev);
	struct fb_var_screeninfo var = mk->saved_var;

	mk->on = false;
	var.xoffset = var.yoffset = 0;
	var.activate = FB_ACTIVATE_NOW;
	mk_fb_set(mk, &var, false);
	mk_fb_set(mk, &var, true);
	mk_fb_suspend(mk, false);	/* fbcon redraws the console */
}

static void mk_pipe_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct mtkfb_kms *mk = to_mk(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;

	if (mk->on && state->fb && state->visible)
		mk_show(mk, state);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

static int mk_pipe_prepare_fb(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *state)
{
	/* implicit sync: wait for the GPU's write fence on the buffer */
	return drm_gem_fb_prepare_fb(&pipe->plane, state);
}

static const struct drm_simple_display_pipe_funcs mk_pipe_funcs = {
	.enable = mk_pipe_enable,
	.disable = mk_pipe_disable,
	.update = mk_pipe_update,
	.prepare_fb = mk_pipe_prepare_fb,
};

static const uint32_t mk_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,	/* alpha ignored */
};

/* ---- connector: the built-in panel ---- */

static int mk_conn_get_modes(struct drm_connector *connector)
{
	struct mtkfb_kms *mk = to_mk(connector->dev);
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &mk->mode);

	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	return 1;
}

static const struct drm_connector_helper_funcs mk_conn_helper_funcs = {
	.get_modes = mk_conn_get_modes,
};

static enum drm_connector_status mk_conn_detect(struct drm_connector *connector,
					      bool force)
{
	return connector_status_connected;	/* built-in panel */
}

static const struct drm_connector_funcs mk_conn_funcs = {
	.detect = mk_conn_detect,
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs mk_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* ---- driver ---- */

static void mk_lastclose(struct drm_device *drm)
{
	/* no fbdev emulation to restore: turn the pipe off, fbcon comes back */
	drm_atomic_helper_shutdown(drm);
}

DEFINE_DRM_GEM_SHMEM_FOPS(mk_fops);

static struct drm_driver mk_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_PRIME,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.lastclose = mk_lastclose,
	.fops = &mk_fops,
	.name = DRIVER_NAME,
	.desc = "KMS on the MediaTek legacy framebuffer",
	.date = "20261012",
	.major = 1,
	.minor = 0,
};

static struct fb_info *mk_find_mtkfb(void)
{
	int i;

	for (i = 0; i < FB_MAX; i++)
		if (registered_fb[i] && !strcmp(registered_fb[i]->fix.id, "mtkfb"))
			return registered_fb[i];
	return NULL;
}

static int mk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_display_mode *m;
	struct mtkfb_kms *mk;
	struct fb_info *info;
	struct drm_device *drm;
	int ret;

	info = mk_find_mtkfb();
	if (!info)
		return -EPROBE_DEFER;
	if (info->var.bits_per_pixel != 32 ||
	    info->var.yres_virtual < NBUF * info->var.yres) {
		dev_err(dev, "unexpected mtkfb layout %ux%u-%u\n",
			info->var.xres_virtual, info->var.yres_virtual,
			info->var.bits_per_pixel);
		return -ENODEV;
	}

	/*
	 * No DT node: give the device non-coherent DMA ops (needed for the
	 * cache maintenance on shared buffers) and a 64-bit mask, so nothing
	 * is bounced; the device itself never does DMA.
	 */
	dev->coherent_dma_mask = DMA_BIT_MASK(64);
	dev->dma_mask = &dev->coherent_dma_mask;
	ret = of_dma_configure(dev, NULL);
	if (ret)
		return ret;

	mk = devm_kzalloc(dev, sizeof(*mk), GFP_KERNEL);
	if (!mk)
		return -ENOMEM;
	mk->info = info;
	mk->fbmem = devm_ioremap_wc(dev, info->fix.smem_start, info->fix.smem_len);
	if (!mk->fbmem)
		return -ENOMEM;

	m = &mk->mode;
	m->hdisplay = m->hsync_start = m->hsync_end = m->htotal = info->var.xres;
	m->vdisplay = m->vsync_start = m->vsync_end = m->vtotal = info->var.yres;
	m->clock = m->htotal * m->vtotal * 60 / 1000;
	m->width_mm = info->var.width > 0 && info->var.width < 1000 ? info->var.width : 56;
	m->height_mm = info->var.height > 0 && info->var.height < 1000 ? info->var.height : 56;
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(m);

	drm = drm_dev_alloc(&mk_driver, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);
	mk->drm = drm;
	drm->dev_private = mk;
	platform_set_drvdata(pdev, mk);

	drm_mode_config_init(drm);
	drm->mode_config.min_width = m->hdisplay;
	drm->mode_config.min_height = m->vdisplay;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &mk_mode_config_funcs;

	drm_connector_helper_add(&mk->connector, &mk_conn_helper_funcs);
	ret = drm_connector_init(drm, &mk->connector, &mk_conn_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		goto err;
	ret = drm_simple_display_pipe_init(drm, &mk->pipe, &mk_pipe_funcs,
					   mk_formats, ARRAY_SIZE(mk_formats),
					   NULL, &mk->connector);
	if (ret)
		goto err;
	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err;
	DRM_INFO("%ux%u on %s (fb%d)\n", m->hdisplay, m->vdisplay,
		 info->fix.id, info->node);
	return 0;

err:
	drm_mode_config_cleanup(drm);
	drm_dev_unref(drm);
	return ret;
}

static int mk_remove(struct platform_device *pdev)
{
	struct mtkfb_kms *mk = platform_get_drvdata(pdev);

	drm_dev_unregister(mk->drm);
	drm_atomic_helper_shutdown(mk->drm);
	drm_mode_config_cleanup(mk->drm);
	drm_dev_unref(mk->drm);
	return 0;
}

static struct platform_driver mk_platform_driver = {
	.probe = mk_probe,
	.remove = mk_remove,
	.driver = { .name = "mtkfb-kms" },
};

static int __init mk_init(void)
{
	int ret = platform_driver_register(&mk_platform_driver);

	if (ret)
		return ret;
	mtkfb_kms_pdev = platform_device_register_simple("mtkfb-kms", -1, NULL, 0);
	if (IS_ERR(mtkfb_kms_pdev)) {
		platform_driver_unregister(&mk_platform_driver);
		return PTR_ERR(mtkfb_kms_pdev);
	}
	return 0;
}

static void __exit mk_exit(void)
{
	platform_device_unregister(mtkfb_kms_pdev);
	platform_driver_unregister(&mk_platform_driver);
}

module_init(mk_init);
module_exit(mk_exit);
MODULE_DESCRIPTION("KMS on the MediaTek legacy framebuffer");
MODULE_LICENSE("GPL v2");
