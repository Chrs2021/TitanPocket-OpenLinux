#!/usr/bin/python3
# tools/power/sleeptest.py: run ON THE PHONE (as root), e.g.
#   scp tools/power/sleeptest.py root@172.16.42.1:/root/ &&
#   ssh root@172.16.42.1 systemd-run --unit=titan-sleeptest --collect \
#       python3 /root/sleeptest.py 90
# then unplug the USB cable; plug it back in when the screen comes back on.
# Analysis: /root/sleeptest/cycles.csv (phase, suspend start, end, length,
# ok), rx.csv (every wlan0 frame, kernel timestamp, OUT- for outgoing), and
# the kernel log for the window (journalctl -k --since @start --until @end).
#
# Unattended sleep test: waits for the USB cable to come out, then sleeps the
# phone in a loop (only the connectivity chip or a 60 s RTC alarm end a
# sleep) for PHASE seconds with Bluetooth on, then PHASE with it off, while a
# raw socket timestamps every frame wlan0 receives. Results: /root/sleeptest/
import os, socket, struct, subprocess, sys, threading, time
PHASE = float(sys.argv[1]) if len(sys.argv) > 1 else 180
OUT = "/root/sleeptest"
os.makedirs(OUT, exist_ok=True)
log = open(OUT + "/cycles.csv", "a")
def note(*a):
    log.write(",".join(str(x) for x in a) + "\n"); log.flush()
def sh(*cmd):
    return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def plugged():
    for s in ("usb", "ac"):
        try:
            if open("/sys/class/power_supply/%s/online" % s).read().strip() == "1":
                return True
        except OSError:
            pass
    return False

def sniff():
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
    s.bind(("wlan0", 0))
    SO_TIMESTAMPNS = 35          # not in this Python's socket module
    s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
    f = open(OUT + "/rx.csv", "a")
    while True:
        data, anc, _, addr = s.recvmsg(2048, 64)
        out = addr[2] == socket.PACKET_OUTGOING
        ts = time.time()
        for lvl, typ, d in anc:
            if lvl == socket.SOL_SOCKET and typ == SO_TIMESTAMPNS:
                sec, ns = struct.unpack("qq", d[:16]); ts = sec + ns / 1e9
        dst = data[0:6]; et = struct.unpack("!H", data[12:14])[0]
        kind = "b" if dst == b"\xff" * 6 else ("m" if dst[0] & 1 else "u")
        extra = ""
        if et == 0x0806:
            extra = socket.inet_ntoa(data[38:42])
        elif et == 0x86dd:
            extra = "nh%d/%d" % (data[20], data[54] if data[20] == 58 else 0)
        elif et == 0x0800:
            extra = "p%d" % data[23]
            if data[23] in (6, 17):
                ihl = (data[14] & 15) * 4; q = 14 + ihl
                sp, dp = struct.unpack("!HH", data[q:q + 4])
                extra += " %s:%d>%s:%d %s" % (socket.inet_ntoa(data[26:30]), sp,
                                             socket.inet_ntoa(data[30:34]), dp,
                                             data[q + 8:q + 24].hex())
        f.write("%.6f,%s%s,%04x,%s\n" % (ts, "OUT-" if out else "", kind, et, extra)); f.flush()

# the cable out for 5 s in a row (online lingers a moment after unplugging)
out = 0
while out < 5:
    out = out + 1 if not plugged() else 0
    time.sleep(1)
note("# start", time.time())
try:
    # everything that changes the phone happens inside try: a failure
    # anywhere still restores the screen and titan-powerkey
    sh("systemctl", "stop", "titan-powerkey")
    open("/run/titan-screen.off", "w").close()
    open("/sys/class/graphics/fb0/blank", "w").write("4")
    sh("/usr/local/bin/titan-wlan-cmd", "SETSUSPENDMODE", "1")
    sh("systemctl", "stop", "systemd-timesyncd")
    # verbose Wi-Fi driver logging (skipped if the driver has no debug file)
    try:
        for mod in ("0x02", "0x05", "0x1e", "0x00", "0x01"):
            open("/proc/net/wlan/dbgLevel", "w").write(
                "%s:%s" % (mod, "0x7f" if mod == "0x05" else "0x3f"))
    except OSError as e:
        note("# no driver debug level", e)
    threading.Thread(target=sniff, daemon=True).start()
    for phase, bt in (("wlan-debug", True),):
        if plugged():
            break
        end = time.time() + PHASE
        while time.time() < end and not plugged():
            open("/sys/class/rtc/rtc0/wakealarm", "w").write("0")
            open("/sys/class/rtc/rtc0/wakealarm", "w").write("+60")
            t0 = time.time()
            try:
                open("/sys/power/state", "w").write("mem")
                ok = 1
            except OSError:
                ok = 0
            t1 = time.time()
            note(phase, "%.6f" % t0, "%.6f" % t1, "%.3f" % (t1 - t0), ok)
            time.sleep(0.3)         # let the wake's packets reach the sniffer
finally:
    for step in (
        lambda: open("/proc/net/wlan/dbgLevel", "w").write("0xff:0x03"),
        lambda: sh("systemctl", "start", "titan-bt"),
        lambda: sh("systemctl", "start", "systemd-timesyncd"),
        lambda: sh("/usr/local/bin/titan-wlan-cmd", "SETSUSPENDMODE", "0"),
        lambda: open("/sys/class/graphics/fb0/blank", "w").write("0"),
        lambda: os.unlink("/run/titan-screen.off"),
        lambda: sh("systemctl", "start", "titan-powerkey")):
        try:
            step()
        except OSError:
            pass
    note("# end", time.time())
