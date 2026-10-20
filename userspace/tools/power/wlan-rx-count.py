#!/usr/bin/python3
# tools/power/wlan-rx-count.py [SECONDS]: run on the phone; counts incoming
# wlan0 frames by kind (broadcast/multicast/unicast) and protocol, skipping
# SSH (no tcpdump on the phone).
# count incoming frames on wlan0 by kind and protocol (skips our SSH)
import socket, struct, time, collections, sys
N = float(sys.argv[1]) if len(sys.argv) > 1 else 60
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
s.bind(("wlan0", 0)); s.settimeout(0.5)
my = open("/sys/class/net/wlan0/address").read().strip()
c = collections.Counter(); t0 = time.time()
while time.time() - t0 < N:
    try:
        f, addr = s.recvfrom(65535)
    except socket.timeout:
        continue
    if addr[2] == socket.PACKET_OUTGOING:
        continue
    dst = f[0:6]; et = struct.unpack("!H", f[12:14])[0]
    kind = "bcast" if dst == b"\xff" * 6 else ("mcast" if dst[0] & 1 else "ucast")
    what = "eth 0x%04x" % et
    if et == 0x0806:
        what = "ARP " + ("request" if f[21] == 1 else "reply")
    elif et == 0x0800:
        proto = f[23]; ihl = (f[14] & 15) * 4; p = 14 + ihl
        if proto in (6, 17):
            sp, dp = struct.unpack("!HH", f[p:p + 4])
            if proto == 6 and 22 in (sp, dp):
                continue                      # our SSH
            what = "IPv4 %s %d->%d" % ("TCP" if proto == 6 else "UDP", sp, dp)
        else:
            what = "IPv4 proto %d" % proto
    elif et == 0x86dd:
        nh = f[20]
        if nh == 58:
            what = "ICMPv6 type %d" % f[54]
        elif nh in (6, 17):
            sp, dp = struct.unpack("!HH", f[54:58])
            if nh == 6 and 22 in (sp, dp):
                continue
            what = "IPv6 %s %d->%d" % ("TCP" if nh == 6 else "UDP", sp, dp)
        else:
            what = "IPv6 nh %d" % nh
    c[(kind, what)] += 1
tot = sum(c.values())
print("%d frames in %.0f s (%.2f/s)" % (tot, N, tot / N))
for (k, w), n in c.most_common(25):
    print("%5d  %-5s  %s" % (n, k, w))
