#!/bin/sh
# venc-test.sh [FRAMES] [W H KBPS FPS GOP]: on the phone, encode a synthetic
# NV12 clip (a moving gradient and box) with titan_venc and write
# /tmp/venc-test.h264; prints one status line a frame. Needs
# /lib/firmware/titan/venc_rc.bin (extracted locally, docs/video-encoder.md)
# and the titan_venc module. Check the clip on a computer:
#   ffprobe -v error -show_streams venc-test.h264; ffmpeg -i venc-test.h264 f%03d.png
set -e
N=${1:-60}; W=${2:-1280}; H=${3:-720}; KBPS=${4:-4000}; FPS=${5:-30}; GOP=${6:-30}
D=/sys/kernel/debug/titan_venc
[ -d $D ] || modprobe titan_venc || insmod /tmp/titan_venc.ko
echo "$W $H $KBPS $FPS $GOP" > $D/config
: > /tmp/venc-test.h264
python3 - "$N" "$W" "$H" <<'PY'
import sys, numpy as np, subprocess
n, w, h = map(int, sys.argv[1:])
pw, ph = (w + 15) & ~15, (h + 15) & ~15
yy, xx = np.mgrid[0:ph, 0:pw]
D = "/sys/kernel/debug/titan_venc"
for f in range(n):
    y = ((xx + yy + 4 * f) % 256).astype(np.uint8)
    bx = (40 + 12 * f) % (pw - 160)
    y[ph // 3:ph // 3 + 160, bx:bx + 160] = 235
    uv = np.empty((ph // 2, pw), np.uint8)
    uv[:, 0::2] = (128 + 60 * np.sin(f / 10)).astype(np.uint8)    # U
    uv[:, 1::2] = 128                                              # V
    open(D + "/in", "wb").write(y.tobytes() + uv.tobytes())
    open(D + "/run", "w").write("1")
    print(open(D + "/run").read().strip(), flush=True)
    with open("/tmp/venc-test.h264", "ab") as o:
        o.write(open(D + "/out", "rb").read())
PY
echo stop > $D/config
ls -l /tmp/venc-test.h264
