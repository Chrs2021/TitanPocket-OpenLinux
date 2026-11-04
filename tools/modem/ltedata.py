#!/usr/bin/python3
# Mobile data on the attached modem, as Android's RIL and NetAgent: allow data, set the
# APN table, activate the PDN, and answer the modem's +EIF report by bringing the interface
# up. ModemManager's generic plugin cannot do this; it stays on cmd1 and noti.
#   ltedata.py [APN] [--default]
# The modem must be registered (mmcli -m any -e). --default also adds a default route behind
# Wi-Fi and the network's DNS servers.
# A PDN that is already active gets no second report: restart titan-modem first. Deactivating
# the only PDN detaches the modem from the network. Addresses are not printed.
import os, re, select, subprocess, sys, time
args = [a for a in sys.argv[1:] if not a.startswith("--")]
APN = args[0] if args else "fast.t-mobile.com"
DEFAULT = "--default" in sys.argv
CONF = ("type=default,mms,supl,hipri,ia;protocol=IPV4V6;roaming_protocol=IPV4V6;authtype=0;"
        "carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;bearer_bitmask=0;inactive_timer=0")
PORTS = {n: os.open("/dev/radio/" + n, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
         for n in ("cmd4", "nwurc", "nwcmd")}
NAMES = {f: n for n, f in PORTS.items()}
buf = {f: "" for f in NAMES}
last, net = {}, {}

def ip(*c):
    return subprocess.run(("ip",) + c, capture_output=True, text=True).returncode

def v6(s):
    p = s.split(".")
    if len(p) != 16:
        return s
    return ":".join("%02x%02x" % (int(p[i]), int(p[i + 1])) for i in range(0, 16, 2))

def send(n, c):
    os.write(PORTS[n], (c + "\r").encode())

def ifup(i, rest):
    """+EIF: 100, ifup, <type>, <address>[, <address>], ...: type 1 IPv4, 2 IPv6, 3 both"""
    f = [x.strip().strip('"') for x in rest.split(",")]
    dev = "ccmni%d" % (int(i) % 100)
    net["dev"] = dev
    for k, v in (("accept_ra", "2"), ("autoconf", "1")):
        open("/proc/sys/net/ipv6/conf/%s/%s" % (dev, k), "w").write(v)
    ip("link", "set", dev, "up")
    for a in f[1:]:
        addr = a.split("/")[0]
        if addr.count(".") == 3:
            ip("addr", "add", addr + "/32", "dev", dev)
            net["v4"] = True
        elif addr.count(".") == 15 or ":" in addr:
            ip("-6", "addr", "add", v6(addr) + "/64", "dev", dev)
    send("nwcmd", 'AT+EIF=%s, "ifst", "up", %s' % (i, f[0]))
    print("%s up, address type %s" % (dev, f[0]), flush=True)

def pump(secs, stop=None):
    end = time.time() + secs
    while time.time() < end:
        if stop and stop():
            return
        for f in select.select(list(NAMES), [], [], 0.1)[0]:
            try:
                buf[f] += os.read(f, 4096).decode(errors="replace")
            except BlockingIOError:
                continue
            while "\r\n" in buf[f]:
                l, buf[f] = buf[f].split("\r\n", 1)
                l = l.strip()
                if l == "OK" or "ERROR" in l:
                    last[NAMES[f]] = l
                c = re.match(r"\+CGEV: ME PDN ACT (\d+)", l)
                if c:
                    net["cid"] = c.group(1)
                m = re.match(r"\+EIF: (\d+), (\w+), (.*)", l)
                if m and m.group(2) == "ifup":
                    ifup(m.group(1), m.group(3))
                elif m and m.group(2) == "mtu" and "dev" in net:
                    ip("link", "set", net["dev"], "mtu", m.group(3).strip())

def at(c, wait=6):
    last.pop("cmd4", None)
    send("cmd4", c)
    pump(wait, lambda: "cmd4" in last)
    return last.get("cmd4", "no answer")

for c in ("ATE0", "AT+CMEE=1", "AT+EDALLOW=1", "AT+EAPNLOCK=1", "AT+EAPNSET",
          'AT+EAPNSET="%s",1,"",""' % APN, 'AT+EAPNSET="%s",2,"%s"' % (APN, CONF),
          'AT+EAPNSET="ims",1,"",""', 'AT+EAPNSET="ims",2,"type=ims;%s"' % CONF.split(";", 1)[1], "AT+EAPNLOCK=0"):
    r = at(c)
    if r != "OK":
        sys.exit("%s: %s" % (c.split("=")[0], r))
send("nwcmd", "ATE0")
r = at('AT+EAPNACT=1,"%s","default",0' % APN, 15)
if r != "OK":
    sys.exit("activation: %s" % r)
pump(10, lambda: "dev" in net)
if "dev" not in net:
    sys.exit("the modem reported no interface")
def global_addr():
    return subprocess.run(("ip", "-o", "addr", "show", "dev", net["dev"], "scope", "global"),
                          capture_output=True, text=True).stdout

pump(15, global_addr)
if not global_addr():
    sys.exit("%s has no global address" % net["dev"])
# behind Wi-Fi (metric 600): traffic stays on Wi-Fi while it has a route
note = ""
if DEFAULT:
    ip("-6", "route", "replace", "default", "dev", net["dev"], "metric", "1024")
    if "v4" in net:
        ip("-4", "route", "replace", "default", "dev", net["dev"], "metric", "1024")
    b = ""
    send("cmd4", "AT+CGCONTRDP=%s" % net.get("cid", "0"))
    end = time.time() + 5
    while time.time() < end and "OK\r\n" not in b:
        if select.select([PORTS["cmd4"]], [], [], 0.2)[0]:
            try:
                b += os.read(PORTS["cmd4"], 4096).decode(errors="replace")
            except BlockingIOError:
                pass
    # after the APN and the address fields: DNS servers, either family
    dns = [v6(x) for x in re.findall(r'"([^"]*)"', b)
           if x.count(".") in (3, 15) and "/" not in x and "mnc" not in x]
    r = 1
    if dns:
        r = subprocess.run(("resolvectl", "dns", net["dev"], *dns), capture_output=True).returncode
        subprocess.run(("resolvectl", "default-route", net["dev"], "yes"), capture_output=True)
    note = ", default route, %d DNS servers%s" % (len(dns), "" if r == 0 else " NOT SET")
    note += ", IPv4" if "v4" in net else ", IPv6 only"
print("%s ready: global address%s" % (net["dev"], note), flush=True)
