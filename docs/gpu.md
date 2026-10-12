# GPU (Mali-G72 MP3) and the Panfrost backport

Status (2026-10-12): **all 3 phases done**. Panfrost drives the
Mali-G72 with Debian 12's Mesa: `GL_RENDERER: Mali-G72 (Panfrost)`,
**OpenGL ES 3.1 (Mesa 22.3.6)**, pixel-exact off-screen rendering, 20/20
long stress runs clean, ~1.8 Gpixel/s blended fill rate at 743 MHz. A small
KMS driver (`mtkfb_kms`, name "mediatek") puts it on the screen:
`kmscube` runs at 60 fps and **Weston** runs with the GL renderer.

## Hardware and what the BSP does

- GPU: Mali-G72 MP3, product id `0x6221`, r0p3 ("arch 6.2.2"), at
  `13040000.mali` (`compatible = "mediatek,mali", "arm,mali-midgard",
  "arm,mali-bifrost"`), interrupts `GPU`, `MMU`, `JOB`. The node has **no**
  clocks, regulators or power domains: MediaTek's `mt_gpufreq` driver powers
  the GPU (VGPU 0.9 V, VSRAM_GPU 1.0 V, MFG MTCMOS async + 3 cores).
- BSP driver: ARM's out-of-tree kbase **r26p0** (`/dev/mali0`), usable only
  with ARM's proprietary userspace (Android build in the vendor partition).
- Display: MediaTek legacy fbdev (`mtkfb`), no DRM/KMS.

## Why Panfrost, and which version

Mesa's open Panfrost driver supports the G72; Debian 12's **Mesa 22.3.6**
has the model entry (`MODEL(0x6221, "G72", "THEx", r0p3 ...)`). The kernel
half is backported from **Linux 5.10 LTS**: it has the G72 feature/issue
tables and every `DRM_PANFROST_PARAM_*` Mesa 22.3 requires
(`AFBC_FEATURES`, added in 5.12, is optional in Mesa), and it predates the
xarray-based scheduler/dependency APIs of 5.15+ that 4.14 lacks.

## Plan

1. **Infrastructure** (done): DRM core, GPU scheduler, GEM shmem helpers,
   Mali LPAE page tables. Test: `/dev/dri` + `drm-smoke`.
2. **Panfrost + MT6771 power glue** (done): see below.
3. **KMS over the MediaTek framebuffer** (done): a small DRM/KMS driver named
   `mediatek` (so Mesa's kmsro pairs it with Panfrost) with dumb buffers that
   can be shared with the GPU; page flip copies into `mtkfb`. Test: `kmscube`,
   Weston/sway. Later: real scanout through the display engine.

## Phase 1: what changed in the kernel

| Piece | Where | Notes |
|---|---|---|
| DRM core | `CONFIG_DRM=y` (fbdev emulation off, MediaTek DRM off) | `vgem` enabled as a smoke-test device (`/dev/dri/card0`) |
| GPU scheduler | `drivers/gpu/drm/scheduler/`, `include/drm/{gpu_scheduler,spsc_queue}.h` (5.10) | 4.14 only had amdgpu's private copy. Changes: `DRM_ERROR`/`DRM_WARN` from `drmP.h`; `sched_set_fifo_low()` → `sched_setscheduler(SCHED_FIFO, 1)` |
| GEM shmem helpers | `drivers/gpu/drm/drm_gem_shmem_helper.c`, `include/drm/drm_gem_shmem_helper.h` (5.10) | 4.14 has no `drm_gem_object_funcs`: the helpers are wired through `struct drm_driver` callbacks (`DRM_GEM_SHMEM_DRIVER_OPS`), `drm_gem_shmem_mmap()` is a file op (5.2-style: `drm_gem_mmap()`, pin pages, `VM_PFNMAP`→`VM_MIXEDMAP`, strip fake offset), `vm_insert_page()` instead of `vmf_insert_page()` |
| Compat shims | `include/drm/drm_backport.h` | `vm_fault_t`, `dma_{map,unmap}_sgtable()`, `drm_printf_indent()` |
| Mali page tables | `drivers/iommu/io-pgtable{,-arm}.[ch]` | `ARM_MALI_LPAE` format from 5.2: block-type leaves at level 3, no AF, stage-2 permissions with stage-1 attributes, 4-level table, `transtab`/`memattr` for the GPU MMU. Other formats unchanged. `CONFIG_IOMMU_IO_PGTABLE_LPAE=y` |

### ARM64 cache aliasing (found by the smoke test)

`drm-smoke` (`userspace/src/tests/drm-smoke.c`) writes a dumb buffer through
its mmap and reads it back. On `vgem` up to a third of the words came back as
zero, more on each run. Cause: shmem pages are zeroed through the kernel's
**cacheable** linear map, user mappings are **write-combined**; dirty zero
lines evicted later overwrite what userspace wrote straight to DRAM. Fix:
clean+invalidate the pages to the PoC (`__flush_dcache_area()`) before a WC
user mapping can see them, in `vgem`'s fault path and in the shmem helper's
`drm_gem_shmem_get_pages_locked()` (non-cached objects). Afterwards 9/9 runs
pass. Panfrost buffers also get this from `dma_map_sg()` when mapped for the
GPU, but CPU-only buffers (dumb buffers) would not.

## Phase 2: Panfrost on MT6771

Panfrost is `drivers/gpu/drm/panfrost/` from 5.10, built as a **module**
(`CONFIG_DRM_PANFROST=m`, loaded at boot via its `mediatek,mali` alias).
kbase is no longer built (`drivers/misc/mediatek/gpu/Makefile` skips
`gpu_mali/` when Panfrost is enabled); ged/hal and gpufreq stay.

### Porting to 4.14

| 5.10 assumes | Here |
|---|---|
| `drm_gem_object_funcs`, `obj->resv` | callbacks in `panfrost_drm_driver` (`DRM_GEM_SHMEM_DRIVER_OPS` + Panfrost's free/open/close/pin); a per-BO `reservation_object` (`panfrost_gem_resv()`, the dma-buf's for imports, exported via `gem_prime_res_obj`) |
| `drm_gem_lock_reservations()`, `drm_gem_objects_lookup()` | local copies in `panfrost_job.c` / `panfrost_drv.c` |
| `dma_resv_*`, `linux/io-pgtable.h`, `drm_utils.h`, `drm_dev_put`, `SZ_4G`, `for_each_sgtable_dma_sg`, ... | shims: `include/linux/{dma-resv,io-pgtable}.h`, `include/drm/{drm_utils,drm_backport}.h` |
| `iommu_flush_ops`, `map(..., gfp)`, `unmap(..., gather)` | 4.14 `iommu_gather_ops` (no-op add_flush/sync; Panfrost flushes explicitly), no gfp, no gather |
| `drm_syncobj_find_fence(file, h, point, flags, &f)` | 4.14 3-argument form |
| DT clocks/regulators/resets/power domains, devfreq OPPs | **`panfrost_mtk.c`**: power via MediaTek gpufreq (bucks, MFG MTCMOS, clock gate, timestamp enable) as kbase's `mali_kbase_config_mt6771.c` does; devfreq stubbed (gpufreq's own OPP stays) |
| lower-case interrupt names | `panfrost_get_irq_byname()` also tries `GPU`/`MMU`/`JOB` |

### Bugs found on the way (each crashed or hung the phone)

1. **Probe order.** Built in, Panfrost (`drivers/gpu`) probed before gpufreq
   (`drivers/misc`) and powered the GPU through NULL clocks -> panic loop.
   Fix: `mt_gpufreq_probed()` + `-EPROBE_DEFER`; Panfrost also became a
   module during bring-up so a GPU bug can never stop the phone booting.
2. **`dma_fence_init()` BUG.** 4.14 requires `.enable_signaling` and `.wait`
   in `dma_fence_ops` (optional since 4.18); the 5.10 scheduler's and
   Panfrost's fence ops lack them -> BUG() at the first submit.
   `DMA_FENCE_BACKPORT_OPS` adds the defaults the newer core assumes.
3. **MMU interrupts never unmasked.** 5.10 unmasks them only from
   `panfrost_device_reset()` (runtime resume / timeout). Here the first
   job's tiler-heap page fault was never seen -> job stuck until the
   timeout reset. Fix: `panfrost_mmu_reset()` at the end of
   `panfrost_mmu_init()`.
4. **Sparse page arrays.** Heap BOs are populated 2 MB at a time;
   4.14's `drm_gem_put_pages()` dereferenced the NULL entries when a heap BO
   was freed (every Mesa exit). Fix in `drm_gem.c`: skip NULL pages (as 5.x).
5. **MMU context freed under running jobs.** `postclose` only waited until
   jobs left the scheduler queue, then freed the page tables (fixed upstream
   in 5.14 by refcounting). `panfrost_job_close()` now waits for each
   entity's last job before teardown.
6. **Coherency.** This G72 integration reports `COHERENCY_FEATURES = 0` but
   `COHERENCY_ENABLE` resets to ACE-Lite; Panfrost never wrote it. Vendor
   quirk sets `COHERENCY_NONE` like kbase.
7. `WAIT_BO` returned the remaining jiffies (positive) instead of 0.

### Known limits

- GPU frequency: fixed by gpufreq (743 MHz OPP); no devfreq governor yet.
- 5.10's `panfrost_gpu_power_off()` writes 0 to the PWROFF registers (a
  no-op, fixed upstream later), so runtime suspend keeps L2/shaders on.
  If real power-off is added, also add kbase's G72 errata workarounds:
  GPU2017-1336 (keep L2 on) and TTRX_921 (flush L2 before shader power-off).
- MediaTek's GED DVFS no longer gets GPU load (that came from kbase).

### Debugging aids (module parameters)

- `panfrost debug_submit=1`: logs (and pauses after) each step of job
  submission, the MMU interrupt path and file teardown, so a hang can be
  pinned to one step from a live `dmesg -w` over SSH.
- `panfrost job_timeout_ms=N`: a long timeout keeps a stuck job
  inspectable (dump `JS*`/`AS*_FAULTSTATUS` with `busybox devmem`).
- Kernel panics leave no log here (pstore stays empty after the watchdog
  reset); `/proc/aed/reboot-reason` shows WDT/exception type. Stream
  `dmesg -w` to the Mac and set `kernel.panic_on_oops=0` while debugging.

### Tests (`userspace/src/tests/`)

| Test | What |
|---|---|
| `drm-smoke` | dumb buffer map/write/readback + PRIME export (any DRM node) |
| `panfrost-steps [n]` | raw-ioctl NULL job, one step at a time |
| `gpu-smoke [frames]` | Mesa EGL surfaceless + GLES2: renders a quad into an FBO, checks every pixel, times a draw loop (`BLEND=1` for real fill rate) |

The two Mesa/libdrm tests are built in the Debian arm64 chroot (glibc):

```sh
container exec titan_build_ubuntu bash -c 'R=/tmp/debian-rootfs; \
  chroot $R apt-get install -y gcc libegl-dev libgles-dev libdrm-dev; \
  cp /workspace/userspace/src/tests/{gpu-smoke.c,panfrost-steps.c,panfrost_drm.h} $R/tmp/; \
  chroot $R gcc -O2 -o /tmp/gpu-smoke /tmp/gpu-smoke.c -lEGL -lGLESv2'
```

## Phase 3: KMS on the MediaTek framebuffer

The BSP drives the display engine (OVL → RDMA → DSI) only through its
legacy fbdev driver `mtkfb`. Porting MediaTek's DRM display driver was out
of scope. Instead, `drivers/gpu/drm/mtkfb_kms/mtkfb_kms.c` (module,
`CONFIG_DRM_MTKFB_KMS=m`, about 400 lines) is a minimal KMS device on top
of mtkfb:

| Part | How |
|---|---|
| Device | registers its own platform device; finds `mtkfb` in `registered_fb[]`; DRM driver name **`mediatek`**, so Mesa's `kmsro` (`mediatek_dri.so`) pairs it with Panfrost |
| Pipeline | `drm_simple_display_pipe`: one CRTC/plane/encoder, a DSI connector that is always connected, one 720×720@60 mode, `XRGB8888`/`ARGB8888` |
| Buffers | GEM shmem helpers (dumb buffers, PRIME export/import) |
| Scanout | **copy**: each plane update copies the framebuffer into one of mtkfb's 3 buffers (its virtual area is 736×2208), then `fb_pan_display()`. mtkfb's pan triggers the display engine and waits for the frame, which paces flips at exactly 60 Hz. The page-flip event is sent after the pan |
| Sync | `drm_gem_fb_prepare_fb()`: waits for the GPU's exclusive fence on the buffer's dma-buf (Panfrost adds it to the imported dma-buf's reservation object) |
| Caches | GPU and user mappings write memory directly (write-combined); the copy reads through a cached `vmap()` after `dma_sync_sg_for_cpu()` invalidates it. To get non-coherent DMA ops without a DT node, the device calls `of_dma_configure(dev, NULL)`, with a 64-bit mask so nothing is bounced through swiotlb |
| Console | while the CRTC is on: `fb_set_suspend()` stops fbcon drawing, and mtkfb is switched to XRGB byte order (`red.offset = 16`; its native setting is R at bit 0). Both are restored on disable and on `lastclose` (last DRM client exits), so the text console comes back |

Card numbers depend on load order: at boot `mtkfb_kms` (modules-load.d)
usually comes before Panfrost (udev). Use the stable name
`/dev/dri/by-path/platform-mtkfb-kms-card`. Weston finds the only
KMS-capable device by itself.

Cost: a 2 MB copy per frame (720×720×4), read from cached memory and written
to write-combined memory. That's a few percent of one CPU core at 60 Hz.
Zero-copy scanout is possible later: mtkfb hands the display engine an
M4U (IOMMU) address per layer (`primary_display_config_input_multiple()`),
and `m4u_alloc_mva()` can map a dma-buf's sg table for the OVL port.

### Running it

```sh
kmscube -D /dev/dri/by-path/platform-mtkfb-kms-card   # 60 fps; quits on stdin input
weston --tty=2                          # from SSH, as root (XDG_RUNTIME_DIR=/run/user/0)
weston                                  # from the console on tty1
```

Debian's Weston 10 is built without libseat. Without a logind session it
uses the direct launcher, which needs a VT and root. With no session, nothing
creates `XDG_RUNTIME_DIR` either; `/etc/profile.d/xdg-runtime-dir.sh` (in
the overlay) sets it to `/run/user/<uid>` for login shells. Clients: `foot`,
`weston-terminal`, `weston-simple-egl` (GPU). Input goes through libinput.
Moving windows by hand under Weston works (checked 2026-10-12).

### Verifying without looking at the phone

`/dev/fb0` read() returns nothing on mtkfb. `userspace/src/tests/fbgrab.c` maps the
framebuffer and dumps the currently panned 720×720 area (XRGB) for
conversion to PNG. That's how the kmscube and Weston frames were checked.

### Phase 3 limitations

- The copy costs CPU and memory bandwidth; no zero-copy scanout yet.
- No vblank interrupts (`drmWaitVBlank` fails). Flip completion is
  signalled after the pan instead. Weston and kmscube cope with that.
- No hardware planes, cursor plane, gamma or rotation.
- No DPMS: the panel stays on while the CRTC is off. mtkfb's blank still
  works from the console.
