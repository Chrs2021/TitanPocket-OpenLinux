#!/bin/bash
# Build the camera driver for Linux 6.12 (linux612/titan_camera.ko), out of
# tree against the 6.12 build, in the build container:
#   container exec titan_build_ubuntu bash /workspace/modules/camera/build612.sh
# It needs the kernel's media modules (videodev, mc, videobuf2-*), which
# tools/mainline/build.sh modules stages. The sources are mirrored to the
# container's disk and built there, so the 4.14 objects next to them stay
# out of the way. Output: modules/camera/out612/titan_camera.ko
set -euo pipefail
# KDIR/KSRC, O and OUT612 may be set to build against another 6.12 tree (another stable release)
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=/root/titan-camera-612
KDIR=${KDIR:-/workspace/kernel_vol_mnt/mainline/linux-6.12.118}
O=${O:-/root/titan-6.12.118-build}
export ARCH=arm64

rm -rf "$SRC"; mkdir -p "$SRC"
tar -C "$HERE" --exclude=out612 --exclude='*.o' --exclude='*.ko' --exclude='.*.cmd' \
	--exclude='*.mod' --exclude='*.mod.c' --exclude=Module.symvers --exclude=modules.order \
	-cf - linux612 dip camcap | tar -C "$SRC" -xf -
make -C "$KDIR" O="$O" M="$SRC/linux612" -j"$(nproc)" modules
mkdir -p "${OUT612:-$HERE/out612}"
cp "$SRC/linux612/titan_camera.ko" "${OUT612:-$HERE/out612}/"
ls -l "${OUT612:-$HERE/out612}/titan_camera.ko"

# The kernel modules it needs (the closure of its depends, from the staged
# modules.dep), for the phone's /lib/modules/<version>/: out612/media-modules.tar
V=$(ls "$O/staging/lib/modules")
M=$O/staging/lib/modules/$V
need=$(modinfo -F depends "${OUT612:-$HERE/out612}/titan_camera.ko" | tr ',' ' ')
files=
while [ -n "$need" ]; do
	next=
	for n in $need; do
		f=$(grep -E "(^|/)${n//_/[-_]}\.ko:" "$M/modules.dep" | head -1)
		[ -n "$f" ] || { echo "missing module $n" >&2; exit 1; }
		case " $files " in *" ${f%%:*} "*) continue ;; esac
		files="$files ${f%%:*}"
		for d in ${f#*:}; do next="$next $(basename "$d" .ko)"; done
	done
	need=$next
done
tar -C "$M" -cf "${OUT612:-$HERE/out612}/media-modules.tar" $files
echo "media modules ($V):$files"
