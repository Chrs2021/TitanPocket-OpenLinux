#!/bin/sh
# venc-test.sh [FRAMES] [W H KBPS FPS GOP]: on the phone, check titan_venc as a
# V4L2 encoder (docs/video-encoder.md): v4l2-compliance when it is installed,
# then GStreamer encodes a moving test pattern through v4l2h264enc into
# /tmp/venc-test.mp4 and decodes it again (the stream must play to its last
# frame; libavcodec, or openh264 when gst-libav is missing). PATTERN= picks
# videotestsrc's pattern (ball, or a busy one such as smpte or snow). Needs
# /lib/firmware/titan/venc_rc.bin (extracted locally) and the titan_venc module. The encoder's per-frame line: /sys/kernel/debug/titan_venc/stats.
set -e
N=${1:-300}; W=${2:-1280}; H=${3:-720}; KBPS=${4:-4000}; FPS=${5:-30}; GOP=${6:-30}
OUT=/tmp/venc-test.mp4
PATTERN=${PATTERN:-ball}

grep -q '^titan_venc ' /proc/modules || modprobe titan_venc ||
	{ modprobe -a v4l2-mem2mem videobuf2-dma-contig && insmod /tmp/titan_venc.ko; }
DEV=
for d in /sys/class/video4linux/video*; do
	[ "$(cat "$d/name")" = titan-venc ] && DEV=/dev/$(basename "$d")
done
[ -n "$DEV" ] || { echo "no titan-venc video device"; exit 1; }
echo "encoder: $DEV"

if command -v v4l2-compliance >/dev/null; then
	v4l2-compliance -d "$DEV" 2>&1 | tail -n 3
fi

T0=$(date +%s.%N)
gst-launch-1.0 -q videotestsrc num-buffers="$N" pattern="$PATTERN" ! \
	"video/x-raw,format=NV12,width=$W,height=$H,framerate=$FPS/1" ! \
	v4l2h264enc \
		extra-controls="encode,video_bitrate=$((KBPS * 1000)),video_gop_size=$GOP" ! \
	"video/x-h264,profile=constrained-baseline" ! h264parse ! mp4mux ! filesink location="$OUT"
T1=$(date +%s.%N)
cat /sys/kernel/debug/titan_venc/stats 2>/dev/null || true
SIZE=$(stat -c %s "$OUT")
python3 -c "
n, s, t = $N, $SIZE, $T1 - $T0
print(f'{n} frames in {t:.1f} s ({n / t:.0f} fps), {s} bytes, {s * 8 / (n / $FPS) / 1000:.0f} kbit/s')"

# decode it again, on the CPU (an independent decoder), counting frames
DECODER=avdec_h264
gst-inspect-1.0 --exists avdec_h264 || DECODER=openh264dec
DEC=$(gst-launch-1.0 -v filesrc location="$OUT" ! qtdemux ! h264parse ! $DECODER ! \
	fakesink silent=false 2>&1 | grep -c 'last-message = chain' || true)
echo "decoded frames ($DECODER): $DEC of $N"
