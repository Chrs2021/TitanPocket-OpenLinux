#!/bin/sh
# sphloop.sh: the modem's speech path without sound. A 1 kHz tone
# (-20 dBFS, at RATE, default 32000 Hz) goes from Playback_2 (DL2) into the modem's uplink
# (PCM 2), the modem loops it back (mdd.py SPH_TEST=loopback), and
# Capture_2 (UL2) records its downlink. Nothing is routed to the codec or
# the amp. Run while mdmock.sh runs with SPH_TEST=loopback: speech is on
# from 8 s after handshake 2 for SPH_SECS. Writes /root/sphloop.wav and
# prints the tone's level in the capture.
set -e
rate=${RATE:-32000}
# PLAY seconds of tone; the capture starts after WAIT seconds
play=${PLAY:-8} wait=${WAIT:-0.5}
mix() { amixer -q -c 0 cset name="$1" "$2"; }
mix 'UL2_CH1 CONNSYS_I2S_CH1' off; mix 'UL2_CH2 CONNSYS_I2S_CH2' off
mix 'PCM_2_PB_CH1 DL2_CH1' on; mix 'PCM_2_PB_CH2 DL2_CH2' on
mix 'UL2_CH1 PCM_2_CAP_CH1' on; mix 'UL2_CH2 PCM_2_CAP_CH2' on
python3 -c '
import math, struct, sys
f = b"".join(struct.pack("<hh", s, s) for s in
             (int(3277 * math.sin(2 * math.pi * 1000 * i / '$rate'))
              for i in range('$rate')))
sys.stdout.buffer.write(f * '$play')' |
	aplay -q -D hw:0,1 -t raw -f S16_LE -r $rate -c 2 &
sleep $wait
arecord -q -D hw:0,3 -f S16_LE -r $rate -c 2 -d 5 /root/sphloop.wav
wait
mix 'PCM_2_PB_CH1 DL2_CH1' off; mix 'PCM_2_PB_CH2 DL2_CH2' off
mix 'UL2_CH1 PCM_2_CAP_CH1' off; mix 'UL2_CH2 PCM_2_CAP_CH2' off
mix 'UL2_CH1 CONNSYS_I2S_CH1' on; mix 'UL2_CH2 CONNSYS_I2S_CH2' on
RATE=$rate python3 - <<'PY'
import math, os, struct, wave
w = wave.open("/root/sphloop.wav")
n = w.getnframes(); d = w.readframes(n)
y = struct.unpack("<%dh" % (2 * n), d)
def goertzel(x, f, fs=int(os.environ['RATE'])):
    k = 2 * math.cos(2 * math.pi * f / fs); s1 = s2 = 0
    for v in x: s1, s2 = v + k * s1 - s2, s1
    return math.sqrt(max(s1 * s1 + s2 * s2 - k * s1 * s2, 0)) * 2 / len(x)
def db(v): return 20 * math.log10(max(v, 1e-9) / 32768)
out = []
for ch in (0, 1):
    x = y[ch::2]
    rms = math.sqrt(sum(v * v for v in x) / len(x))
    out.append("ch%d RMS %.1f dBFS, 1 kHz %.1f dBFS, peak %d" % (
        ch + 1, db(rms), db(goertzel(x, 1000)), max(abs(v) for v in x)))
print("capture: %d frames, %s" % (n, "; ".join(out)))
PY
