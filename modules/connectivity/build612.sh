#!/bin/bash
# Build the MediaTek connectivity modules for Linux 6.12 (docs/mainline.md),
# out of tree against the 6.12 build, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/connectivity/build612.sh [glue|wmt|...|all|clean]
# linux612/ (mtk_btif.ko, the BTIF driver) comes first; the WMT core
# links against its symbols. The vendor sources call the 6.12 interfaces
# directly (version-guarded where 4.14 lacks them).
# Output: modules/connectivity/out612/*.ko
#
# The sources are mirrored to the container's disk and built there: the
# 4.14 build (build.sh) puts its objects next to the sources, and Kbuild
# would take those for current.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=/root/titan-conn-612
KDIR=/workspace/kernel_vol_mnt/mainline/linux-6.12.115
O=/root/titan-6.12-build
JOBS=${JOBS:-$(nproc)}
OUT=$HERE/out612
export ARCH=arm64

mkdir -p "$SRC"
tar -C "$HERE" --exclude=out --exclude=out612 --exclude='*.o' --exclude='*.ko' \
	--exclude='.*.cmd' --exclude='*.mod' --exclude='*.mod.c' --exclude=Module.symvers \
	--exclude=modules.order -cf - . | tar -C "$SRC" -xf -

TOP=/tmp/titan-conn-top-612
mkdir -p $TOP/vendor/mediatek/kernel_modules
ln -sfn "$SRC" $TOP/vendor/mediatek/kernel_modules/connectivity

# What the vendor Makefiles read from the vendor kernel's config
VENDOR=(CONFIG_MTK_COMBO=y 'CONFIG_MTK_COMBO_CHIP="CONSYS_6771"'
	'CONFIG_MTK_PLATFORM="mt6771"' MTK_CONSYS_ADIE=MT6631 TOP=$TOP
	CONFIG_MTK_BTIF=y TITAN_LINUX612=y)
CFLAGS612="-I$SRC/linux612/include -DTITAN_LINUX612"

mk() {
	local dir=$1; shift
	make -C "$KDIR" O="$O" M="$SRC/$dir" -j"$JOBS" ${KEEP:+-k} "${VENDOR[@]}" \
		KCFLAGS="$CFLAGS612" "$@"
}

# build.sh builds no modules, so there is no Module.symvers; everything
# these modules use is built in, which vmlinux.symvers lists.
cp "$O/vmlinux.symvers" "$O/Module.symvers"
# and no scripts/module.lds, which linking a module needs
[ -f "$O/scripts/module.lds" ] || make -C "$KDIR" O="$O" modules_prepare

glue() { mk linux612 modules; }
wmt() {
	mk common KBUILD_EXTRA_SYMBOLS="$SRC/linux612/Module.symvers" modules
}

wifi() {
	mk wlan_adaptor KBUILD_EXTRA_SYMBOLS="$SRC/common/Module.symvers" modules
	mk wlan_gen3 KBUILD_EXTRA_SYMBOLS="$SRC/common/Module.symvers $SRC/wlan_adaptor/Module.symvers" \
		modules
}

others() {
	# Bluetooth: /dev/stpbt, FM: /dev/fm (MT6631), GPS: /dev/stpgps
	mk bt/legacy KBUILD_EXTRA_SYMBOLS="$SRC/common/Module.symvers" modules
	mk fm KBUILD_EXTRA_SYMBOLS="$SRC/common/Module.symvers" CONFIG_MTK_FMRADIO=y \
		'CONFIG_MTK_FM_CHIP="MT6631_FM"' CFG_FM_CHIP=mt6631 modules
	mk gps KBUILD_EXTRA_SYMBOLS="$SRC/common/Module.symvers" CONFIG_MTK_GPS_SUPPORT=y modules
}

case "${1:-all}" in
	clean) for d in linux612 common wlan_adaptor wlan_gen3 bt/legacy fm gps; do mk $d clean; done
		rm -rf "$OUT"; exit 0 ;;
	glue) glue ;;
	wmt) wmt ;;
	wifi) wifi ;;
	others) others ;;
	all) glue; wmt; wifi; others ;;
esac
mkdir -p "$OUT"
for m in linux612/mtk_btif.ko common/wmt_drv.ko wlan_adaptor/wmt_chrdev_wifi.ko \
	 wlan_gen3/wlan_drv_gen3.ko bt/legacy/bt_drv.ko fm/fmradio_drv.ko gps/gps_drv.ko; do
	# without debug info: they ship in the first-light ramdisk (32 MB image)
	[ -f "$SRC/$m" ] && aarch64-linux-gnu-strip --strip-debug -o "$OUT/$(basename "$m")" "$SRC/$m"
done
ls -la "$OUT"
