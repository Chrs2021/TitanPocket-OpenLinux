#!/bin/sh
# Encode a synthetic clip with x264enc, decode it with titan_vdec
# (v4l2slh264dec) and with libavcodec (avref), and compare the frames
# (docs/video-decoder.md). Runs on the phone:
#   vdec-test.sh NAME W H FRAMES "x264enc options" [profile]
set -e
N=$1 W=$2 H=$3 F=$4 OPTS=$5 PROF=${6:-high}
D=/tmp/vd; mkdir -p $D; cd $D
gst-launch-1.0 -q videotestsrc num-buffers=$F pattern=ball motion=sweep background-color=0xff204060 ! \
	video/x-raw,width=$W,height=$H,framerate=30/1 ! x264enc $OPTS ! \
	video/x-h264,profile=$PROF,stream-format=byte-stream ! filesink location=$N.h264
before=$(cat /sys/kernel/debug/titan_vdec/stats)
timeout 120 gst-launch-1.0 -q filesrc location=$N.h264 ! h264parse ! v4l2slh264dec ! \
	videoconvert ! video/x-raw,format=I420 ! filesink location=$N.hw.yuv
/root/avref $N.h264 $N.av.yuv 2>/dev/null
echo "$N: $(stat -c %s $N.h264) bytes; before: $before"
echo "  after: $(cat /sys/kernel/debug/titan_vdec/stats)"
python3 - $N.hw.yuv $N.av.yuv $W $H <<'PY'
import sys, math
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
w, h = int(sys.argv[3]), int(sys.argv[4]); fs = w * h * 3 // 2
na, nb = len(a) // fs, len(b) // fs
bad = []
for f in range(min(na, nb)):
    x, y = a[f*fs:(f+1)*fs], b[f*fs:(f+1)*fs]
    if x != y:
        n = sum(1 for i in range(0, fs, 3) if x[i] != y[i])
        mse = sum((x[i]-y[i])**2 for i in range(0, fs, 3)) / (fs / 3)
        bad.append((f, n * 3, round(10*math.log10(65025/mse), 1) if mse else 99))
print(f"  frames hw {na} ref {nb}; identical {min(na,nb)-len(bad)}; differing {len(bad)}")
for f, n, p in bad[:8]:
    print(f"    frame {f}: ~{n} samples differ, PSNR {p} dB")
PY
