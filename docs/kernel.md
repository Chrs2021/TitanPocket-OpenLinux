# Kernel

## Source tree

| | |
|---|---|
| Location | `kernel_vol_mnt/kernel_workspace` (on a case-sensitive APFS sparse image, `kernel_vol.sparseimage`; must be mounted) |
| Upstream | https://github.com/ubuntu-touch-unihertz-titan/kernel-alps-mt6771 (MediaTek alps 4.14.186 BSP for Titan Pocket/Slim) |
| Base branch | `origin/halium-11.0` (adds the AW9523 keyboard, Titan Slim bits, Ubuntu AppArmor) on top of `master` (GT1151QM touch, LCM, `agold_gpio_set`) |
| Local branch | `titanpocket` — all our work is **uncommitted working-tree changes** on this branch |
| Compiler | native gcc 9.4 inside the `titan_build_ubuntu` container (Ubuntu 20.04 arm64) |

Other trees in the project root, used only as references: `titan_kernel_ref`
(pristine copy of upstream master), `oppo_kernel_ref` (OPPO's MT6771 4.14
release; useful for drivers Unihertz didn't publish, e.g. exFAT).

## Configuration

`arch/arm64/configs/titanpocket_defconfig` (upstream, regenerated from the
stock `/proc/config.gz`) **plus** our fragment
`arch/arm64/configs/titanpocket_linux.config`, merged by
`build_kernel.sh config`:

| Option(s) | Why |
|---|---|
| `LOCALVERSION="-titanpocket"`, `CROSS_COMPILE=""` | identify our build; native gcc instead of the Android toolchain prefix |
| `CUSTOM_KERNEL_IMGSENSOR=""`, `CUSTOM_KERNEL_CAM_CAL_DRV=""` | camera sensor drivers have no source |
| `MTK_AW9523`, `BATTERY_OZ8806`, `SND_SOC_FS1860`, `INPUT_TITANPOCKET_TOUCHPAD` | our device drivers |
| `RMI4_CORE`, `RMI4_I2C`, `RMI4_2D_SENSOR`, `RMI4_F11`, `RMI4_F12` (F55 off) | Synaptics touchpad |
| `DEVTMPFS(_MOUNT)`, `FHANDLE`, `AUTOFS4_FS`, `TMPFS_*`, `SECCOMP(_FILTER)`, `CGROUP_BPF`, `BPF_SYSCALL`, `INOTIFY_USER`, ... | systemd / generic distro requirements |
| `VT`, `VT_CONSOLE`, `FRAMEBUFFER_CONSOLE`, `FONT_8x16` (+`EXPERT`) | text console on the MTK framebuffer |
| `USB_CONFIGFS_{NCM,ECM,RNDIS,ACM}` | USB serial + network gadget for bring-up |
| `I2C_CHARDEV` | `i2cget`/`i2cset` from userspace |
| `DEBUG_FS`, `DEVMEM`, `!STRICT_DEVMEM` | `/sys/kernel/debug/mtksocaudio`, `devmem` register access |
| `!IKHEADERS` | needs the Android prebuilt `cpio`; not useful here |
| `DRM`, `DRM_SCHED`, `DRM_GEM_SHMEM_HELPER`, `DRM_VGEM`, `IOMMU_IO_PGTABLE_LPAE`, `DRM_PANFROST=m` | Panfrost (replaces kbase) ([gpu.md](gpu.md)) |

## Changes to the source tree

Everything below is relative to `origin/halium-11.0`. See `git diff` in the
tree for exact hunks.

### Fixes required to boot / run

| File | Change | Why |
|---|---|---|
| `drivers/devfreq/helio-dvfsrc.c` | `#include "mtk_dvfsrc_reg.h"` (local) instead of `<mtk_dvfsrc_reg.h>` | With GCC's include order the angle-bracket form picked `spm_v4`'s header, where register names are **absolute addresses** (`DVFSRC_BASE + off`) instead of offsets. The display idle manager's bandwidth request then oopsed in `commit_data()` at ~19 s -> bootloop. |
| `drivers/devfreq/helio-dvfsrc_v1.h` | removed `(unsigned int)` cast in `DVFSRC_REG` | That cast was an earlier attempt to silence the resulting "pointer + pointer" compile error; it produced the garbage address. |
| `drivers/misc/mediatek/video/mt6771/videox/disp_helper.c` | `DISP_OPT_IDLE_MGR = 0` | The idle manager switches to "decouple" mode, which scans out a buffer only Android's SurfaceFlinger refreshes; fbcon output then became garbage. |
| `drivers/tty/vt/selection.c` | cherry-pick of upstream `61af072b8` | duplicate `set_selection` definition breaks `CONFIG_VT` builds |

### New / changed drivers

| File | Change | Doc |
|---|---|---|
| `drivers/power/supply/oz8806_battery.c` (+ Kconfig/Makefile) | new OZ8806 fuel-gauge driver | [drivers/oz8806.md](drivers/oz8806.md) |
| `drivers/pinctrl/mediatek/pinctrl-paris.c` | `agold_gpio_get_in()` (exported) | board strap reads (GPIO 5) |
| `drivers/misc/mediatek/aw9523/aw9523_key.c` | active at probe; idempotent suspend/resume; stock keymap | [drivers/aw9523-keyboard.md](drivers/aw9523-keyboard.md) |
| `sound/soc/codecs/fs1860.[ch]` (+ Kconfig/Makefile) | new FS1860 ASoC codec | [drivers/fs1860.md](drivers/fs1860.md) |
| `drivers/input/misc/titanpocket-touchpad.c` (+ Kconfig/Makefile) | touchpad reset + RMI4 instantiation | [drivers/touchpad.md](drivers/touchpad.md) |
| `sound/soc/mediatek/common_int/mtk-soc-speaker-amp.[ch]` | `MTK_SPK_FOURSEMI_FS1860` hook | [audio.md](audio.md) |
| `sound/soc/mediatek/common_int/mtk-soc-afe-control.c` | `mtk_mem_dlblk_copy()` honours ALSA `pos` | [audio.md](audio.md) |
| `drivers/gpu/drm/scheduler/`, `drm_gem_shmem_helper.c`, `include/drm/{gpu_scheduler,spsc_queue,drm_gem_shmem_helper,drm_backport}.h` | DRM infrastructure backported from 5.10 | [gpu.md](gpu.md) |
| `drivers/iommu/io-pgtable{,-arm}.[ch]` | `ARM_MALI_LPAE` page-table format (5.2) | [gpu.md](gpu.md) |
| `drivers/clk/mediatek/clk-mt6771-pg.c`, `base/power/spm_v4/mtk_sleep.c` | a power domain still on at suspend refuses the suspend (-EBUSY) instead of `BUG_ON` (#33) | [power.md](power.md#the-suspend-crash) |
| `drivers/power/supply/oz8806_battery.c` | the gauge seeds from the cell voltage when the OCV register is stale, and resyncs when the count and the voltage disagree by > 25 % for a minute (#35) | [drivers/oz8806.md](drivers/oz8806.md) |
| `base/power/spm_v4/mtk_spm_sodi.c`, `pmic/mt6358/v1/pmic_irq.c`, `mmc/host/mediatek/ComboA/sd.c` | quieter logs: no SODI line per non-timer wake (summary once a minute), PMIC IRQ enable and SD clock lines at debug (#34) | [power.md](power.md#log-volume) |
| `drivers/gpu/drm/vgem/vgem_drv.c` | clean pages to PoC before the WC user mapping (arm64) | [gpu.md](gpu.md) |
| `drivers/gpu/drm/panfrost/` (5.10 + `panfrost_mtk.c`), `drivers/gpu/drm/drm_gem.c` (sparse `put_pages`), `drivers/misc/mediatek/gpu/Makefile` (no kbase), `base/power/mt6771/mtk_gpufreq_core.c` (`mt_gpufreq_probed()` + exports), `drivers/iommu/io-pgtable.c` (exports) | Panfrost on MT6771 | [gpu.md](gpu.md) |

### GCC 9 build fixes (inherited)

The upstream tree is built with clang. An earlier session made it build with
gcc 9.4: ~90 files, mostly duplicate symbol definitions (`static` / `inline`
mismatches), include-path fixes (`<x.h>` -> `"x.h"` or explicit paths) and
format/cast warnings promoted to errors. A copy of that patch as it was
before this work: `kernel_vol_mnt/gcc9_build_fixes.patch`.

**Audit lesson:** an include-path "fix" can silently select a *different*
header with the same name (that is exactly what caused the DVFSRC bootloop).
When touching includes in this tree, check for duplicates with
`find . -name <header>`. The remaining include rewrites were checked and
select the mt6771/local copies.

### Leftovers from earlier sessions (not used by the build)

`arch/arm64/configs/titanpocket_stock_defconfig`,
`arch/arm64/boot/dts/mediatek/titanpocket_stock.dts`, and several `elf.h`
copies (`elf.h`, `include/elf.h`, `scripts/mod/elf.h`,
`scripts/selinux/genheaders/elf.h`, `include/uapi/asm`) from an attempt to
build on macOS directly. Safe to delete once confirmed unneeded.

## Building

```sh
container exec titan_build_ubuntu bash /workspace/build_kernel.sh config   # merge defconfig + fragment
container exec titan_build_ubuntu bash /workspace/build_kernel.sh build    # Image.gz-dtb + modules
```
Output: `arch/arm64/boot/Image.gz` (what goes into the boot image; the
`-dtb` variant is not used). Full build ~15 min, incremental ~3 min.

## Committing

Nothing is committed yet. Suggested split when ready: (1) GCC build fixes,
(2) boot fixes (DVFSRC include, idle manager, VT), (3) keyboard, (4) OZ8806 +
`agold_gpio_get_in`, (5) FS1860 + MTK hook, (6) DL copy fix, (7) touchpad
glue, (8) config fragment.
