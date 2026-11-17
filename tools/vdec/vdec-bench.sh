#!/bin/bash
# Play the same H.264 clip at its frame rate through the hardware decoder
# and through libavcodec, and report CPU time and battery current for each
# (docs/video-decoder.md). Runs on the phone; for current, unplugged.
#   vdec-bench.sh CLIP.mp4 [seconds idle between runs]
# Output: one line per run: wall time, system CPU busy (all online CPUs,
# so it includes the compositor and the kernel's decode worker), CPU-seconds,
# mean battery current_now (discharge; meaningless on the charger).
CLIP=$1 GAP=${2:-15}
D=$(dirname "$CLIP"); B=${CLIP%.mp4}
[ -f "$B.h264" ] || gst-launch-1.0 -q filesrc location="$CLIP" ! qtdemux ! h264parse ! \
	video/x-h264,stream-format=byte-stream,alignment=au ! filesink location="$B.h264"
FPS=30

busy() { awk '/^cpu /{print $2+$3+$4+$7+$8, $2+$3+$4+$5+$6+$7+$8}' /proc/stat; }
run() {
	local name=$1; shift
	local s0 s1 t0 t1 cur=0 k=0 pid c
	read -r s0 t0 < <(busy)
	local w0=$(date +%s.%N)
	"$@" >/dev/null 2>&1 &
	pid=$!
	while kill -0 $pid 2>/dev/null; do
		c=$(cat /sys/class/power_supply/oz8806-battery/current_now 2>/dev/null)
		[ -n "$c" ] && { cur=$((cur + c)); k=$((k + 1)); }
		sleep 1
	done
	local w1=$(date +%s.%N)
	read -r s1 t1 < <(busy)
	# /proc/stat ticks are summed over the online CPUs
	awk -v n="$name" -v w0="$w0" -v w1="$w1" -v s0="$s0" -v s1="$s1" -v t0="$t0" -v t1="$t1" \
	    -v c="$cur" -v k="$k" -v cpus="$(nproc)" 'BEGIN {
		b = (s1 - s0) / (t1 - t0); w = w1 - w0
		printf "%-8s wall %5.1f s  busy %5.1f %% of %d CPUs  %6.1f CPU-s  current %4d mA (%d samples)\n",
			n, w, 100 * b, cpus, b * cpus * w, k ? c / k / 1000 : 0, k }'
}
idle() { run idle sleep 30; }

idle; sleep "$GAP"
run hw gst-launch-1.0 -q filesrc location="$CLIP" ! qtdemux ! h264parse ! v4l2slh264dec ! fakesink sync=true
sleep "$GAP"
run hw+conv gst-launch-1.0 -q filesrc location="$CLIP" ! qtdemux ! h264parse ! v4l2slh264dec ! \
	videoconvert ! video/x-raw,format=I420 ! fakesink sync=true
sleep "$GAP"
run sw-1t /root/avref "$B.h264" - 1 $FPS
sleep "$GAP"
run sw-auto /root/avref "$B.h264" - 0 $FPS
sleep "$GAP"
idle
cat /sys/kernel/debug/titan_vdec/stats
