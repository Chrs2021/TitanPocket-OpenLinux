#!/usr/bin/python3
# tools/power/connlog.py [SECONDS]: run ON THE PHONE (as root) during normal
# use, e.g.
#   ssh root@172.16.42.1 systemd-run --unit=connlog --collect \
#       python3 /root/connlog.py 43200
# Whenever the phone is awake (at most every 20 s; it runs again right after
# each resume) it records to /root/connlog.csv: time, gauge charge/current/
# voltage, plugged, screen off flag, and how much of 0.5 s the connectivity
# chip held the SoC's 26 MHz clock (SPM PCM_REG13 bit 14, CONN_SRCCLKENA),
# with the Wi-Fi link (BSSID, frequency, signal, power save) and Bluetooth
# state. A clock hold near 100% with the screen off means Wi-Fi or Bluetooth
# has stopped sleeping: compare with the drain between rows.
import mmap, os, struct, subprocess, sys, time
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 43200
BAT = "/sys/class/power_supply/oz8806-battery/"
OUT = "/root/connlog.csv"

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
spm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x10006000)

def rd(path, default=""):
    try:
        return open(path).read().strip()
    except OSError:
        return default

def conn_hold(n=50):
    held = 0
    for _ in range(n):
        if struct.unpack("<I", spm[0x134:0x138])[0] >> 14 & 1:
            held += 1
        time.sleep(0.01)
    return held * 100 // n

def wifi():
    try:
        link = subprocess.run(["iw", "dev", "wlan0", "link"], capture_output=True,
                              text=True, timeout=3).stdout
        ps = subprocess.run(["iw", "dev", "wlan0", "get", "power_save"],
                            capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.TimeoutExpired):
        return "", "", "", ""
    bssid = freq = sig = ""
    for l in link.splitlines():
        l = l.strip()
        if l.startswith("Connected to"):
            bssid = l.split()[2]
        elif l.startswith("freq:"):
            freq = l.split()[1]
        elif l.startswith("signal:"):
            sig = l.split()[1]
    return bssid, freq, sig, "on" if "on" in ps else "off"

def bt():
    try:
        out = subprocess.run(["hciconfig", "hci0"], capture_output=True, text=True,
                             timeout=3).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return "up" if "UP RUNNING" in out else "down"

new = not os.path.exists(OUT)
log = open(OUT, "a")
if new:
    log.write("t,charge_mAh,mA,mV,plugged,screen,conn_pct,bssid,freq,signal,ps,bt\n")
end = time.time() + DUR
while time.time() < end:
    plugged = "1" if rd("/sys/class/power_supply/usb/online") == "1" or \
        rd("/sys/class/power_supply/ac/online") == "1" else "0"
    screen = "off" if os.path.exists("/run/titan-screen.off") else "on"
    pct = conn_hold()
    row = [int(time.time()), int(rd(BAT + "charge_now", "0")) // 1000,
           int(rd(BAT + "current_now", "0")) // 1000, int(rd(BAT + "voltage_now", "0")) // 1000,
           plugged, screen, pct] + list(wifi()) + [bt()]
    log.write(",".join(str(x) for x in row) + "\n"); log.flush()
    time.sleep(20)
