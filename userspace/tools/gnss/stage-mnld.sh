#!/bin/bash
# Stage the chroot that runs MediaTek's GNSS engine on Debian:
# out/android-gps/root, installed as /opt/android-gps by build-rootfs.sh.
#
# The engine is libmnl.so (closed, 32-bit bionic). Our own daemon,
# titan-mnld (userspace/titan-mnld), hosts it in place of stock mnld, so
# the chroot holds only what libmnl and the bionic linker need:
#   system/bin/linker, system/lib/{ld-android,libc,libc++,libdl,libm}.so,
#   vendor/lib/libmnl.so, vendor/etc/MNL_Config.xml
# all from the phone's own stock firmware (Firmware/super_ext); it is not
# redistributable. titan-mnld itself is added by build-rootfs.sh.
# Run on the Mac from the project root:
#   tools/gnss/stage-mnld.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=out/android-gps
mkdir -p $OUT/tmp
python3 - <<'PY'
import zipfile
with zipfile.ZipFile("Firmware/super_ext/system_ext/system/apex/com.android.runtime.apex") as z:
    z.extract("apex_payload.img", "out/android-gps/tmp/com.android.runtime")
PY
container exec titan_build_ubuntu bash -euo pipefail -c '
cd /workspace
V=Firmware/super_ext/vendor_ext; SY=Firmware/super_ext/system_ext/system
A=out/android-gps/apex/com.android.runtime; R=out/android-gps/root
rm -rf out/android-gps/apex $R; mkdir -p $A
debugfs -R "rdump / $A" out/android-gps/tmp/com.android.runtime/apex_payload.img 2>/dev/null
mkdir -p $R/system/bin $R/system/lib $R/vendor/bin $R/vendor/lib $R/vendor/etc \
	$R/dev $R/proc $R/sys $R/data/vendor/gps $R/tmp
cp $A/bin/linker $R/system/bin/linker
# bionic from the runtime APEX, matching its linker (its ld-android.so and
# libc++.so are links to /system/lib)
for l in libc.so libdl.so libm.so; do cp $A/lib/bionic/$l $R/system/lib/; done
cp $SY/lib/ld-android.so $SY/lib/libc++.so $R/system/lib/
cp $V/lib/libmnl.so $R/vendor/lib/
cp $V/etc/MNL_Config.xml $R/vendor/etc/
chmod 755 $R/system/bin/linker
# everything libmnl needs must be here (the linker reports anything missing)
for f in $R/vendor/lib/libmnl.so $R/system/lib/*.so; do
	for n in $(readelf -d $f | sed -n "s/.*Shared library: \[\(.*\)\]/\1/p"); do
		[ -e $R/system/lib/$n ] || [ -e $R/vendor/lib/$n ] || { echo "missing: $n (for $f)" >&2; exit 1; }
	done
done
echo "staged: $(find $R -type f | wc -l) files, $(du -sh $R | cut -f1)"'
rm -rf $OUT/tmp $OUT/apex
