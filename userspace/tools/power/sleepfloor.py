#!/usr/bin/python3
# tools/power/sleepfloor.py: run ON THE PHONE (as root), e.g.
#   scp tools/power/sleepfloor.py root@172.16.42.1:/root/ &&
#   ssh root@172.16.42.1 systemd-run --unit=titan-sleepfloor --collect \
#       python3 /root/sleepfloor.py 480
# then unplug the USB cable; plug it back in when the screen comes back on.
#
# Sleep current by component: waits for the USB cable to come out, then keeps
# the phone in forced suspend (300 s RTC alarm, re-suspending at once after
# any other wake) for PHASE seconds in each of
#   both     Wi-Fi associated (firmware suspend mode) and Bluetooth on: the
#            normal screen-off state
#   wifi     Bluetooth off
#   bt       Bluetooth on, Wi-Fi chip powered off
#   none     both off (the connectivity chip idle)
# and reads the fuel gauge's coulomb counter (0.5 mAh steps) across each phase.
# Results: /root/sleepfloor/phases.csv (phase, seconds, seconds suspended,
# suspends, mAh used, average mA) and cycles.csv (every suspend).
import os, subprocess, sys, time
PHASE = float(sys.argv[1]) if len(sys.argv) > 1 else 480
OUT = "/root/sleepfloor"
BAT = "/sys/class/power_supply/oz8806-battery/"
os.makedirs(OUT, exist_ok=True)
cycles = open(OUT + "/cycles.csv", "a")
phases = open(OUT + "/phases.csv", "a")
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
def charge():
    # the driver re-reads the counter right after resume and every 5 s
    time.sleep(1.5)
    return int(open(BAT + "charge_now").read()) / 1000

out = 0
while out < 5:
    out = out + 1 if not plugged() else 0
    time.sleep(1)
phases.write("# start %d\nphase,seconds,suspended_s,suspends,mAh,avg_mA\n" % time.time())
phases.flush()
try:
    sh("systemctl", "stop", "titan-powerkey")
    open("/run/titan-screen.off", "w").close()
    open("/sys/class/graphics/fb0/blank", "w").write("4")
    sh("/usr/local/bin/titan-wlan-cmd", "SETSUSPENDMODE", "1")
    sh("systemctl", "stop", "systemd-timesyncd")
    for phase in ("both", "wifi", "bt", "none"):
        if plugged():
            break
        if phase == "wifi":
            sh("systemctl", "stop", "titan-bt")
        elif phase == "bt":
            sh("systemctl", "start", "titan-bt")
            sh("systemctl", "stop", "wpa_supplicant@wlan0")
            open("/dev/wmtWifi", "w").write("0")
        elif phase == "none":
            sh("systemctl", "stop", "titan-bt")
        time.sleep(5)
        c0 = charge(); t0 = time.time(); asleep = 0.0; n = 0
        while time.time() - t0 < PHASE and not plugged():
            open("/sys/class/rtc/rtc0/wakealarm", "w").write("0")
            open("/sys/class/rtc/rtc0/wakealarm", "w").write("+300")
            s0 = time.time()
            try:
                open("/sys/power/state", "w").write("mem")
                ok = 1
            except OSError:
                ok = 0
            s1 = time.time()
            asleep += s1 - s0; n += 1
            cycles.write("%s,%.3f,%.3f,%d\n" % (phase, s0, s1 - s0, ok)); cycles.flush()
            time.sleep(0.3)
        if plugged():
            break
        c1 = charge(); dt = time.time() - t0
        phases.write("%s,%.0f,%.0f,%d,%.1f,%.1f\n" % (
            phase, dt, asleep, n, c0 - c1, (c0 - c1) * 3600 / dt))
        phases.flush()
finally:
    for step in (
        lambda: open("/sys/class/rtc/rtc0/wakealarm", "w").write("0"),
        lambda: sh("systemctl", "start", "titan-bt"),
        lambda: sh("systemctl", "restart", "titan-wifi"),
        lambda: sh("systemctl", "start", "wpa_supplicant@wlan0"),
        lambda: sh("systemctl", "start", "systemd-timesyncd"),
        lambda: sh("/usr/local/bin/titan-wlan-cmd", "SETSUSPENDMODE", "0"),
        lambda: open("/sys/class/graphics/fb0/blank", "w").write("0"),
        # the backlight PWM loses its enable bit in deep sleep; level 0 makes
        # the driver set it again (see titan-powerkey brightness())
        lambda: open("/sys/class/leds/lcd-backlight/brightness", "w").write("0"),
        lambda: sh("/usr/local/bin/titan-brightness", "apply"),
        lambda: os.unlink("/run/titan-screen.off"),
        lambda: sh("systemctl", "start", "titan-powerkey")):
        try:
            step()
        except OSError:
            pass
    phases.write("# end %d\n" % time.time()); phases.flush()
