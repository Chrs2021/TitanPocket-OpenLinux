#!/bin/sh
# nvcopy.sh ROOT: the phone's modem NVRAM into ROOT for mdd.py, as the
# modem's drives: Z/ = nvdata md/, X/ = protect1 md/, Y/ = protect2 md/
# (Android's ccci_fsd mapping). The partitions are mounted read-only
# without journal replay, so they are never modified; the modem's writes
# go to the copy. ROOT should be a tmpfs (/run): per-phone data (IMEI,
# calibration), never to be copied off the phone, committed or published.
set -e
root=${1:?root}
rm -rf "$root"
umask 077
mkdir -p "$root"
for map in Z:nvdata X:protect1 Y:protect2; do
	drive=${map%%:*} part=${map#*:}
	mnt=$(mktemp -d)
	mount -o ro,noload "/dev/disk/by-partlabel/$part" "$mnt"
	cp -a "$mnt/md" "$root/$drive"
	umount "$mnt"
	rmdir "$mnt"
done
chmod 700 "$root"
echo "nvcopy: $(find "$root" -type f | wc -l) files"
