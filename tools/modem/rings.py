#!/usr/bin/env python3
"""rings.py: the titan_modem CCIF rings' pointers from debugfs
(titan-modem/smem), to see whether the modem consumed the AP's packets.
Ring control as titan_modem.c: header (8), rx read/write/len, tx
read/write/len, rx buffer, tx buffer, footer (8); the set starts at the
non-cached region + 160 KB."""
import struct

with open("/sys/kernel/debug/titan-modem/smem", "rb") as f:
    f.seek(160 * 1024)
    b = f.read(1024 * 1024 - 160 * 1024)
off = 0
for q in range(8):
    rr, rw, rl, tr, tw, tl = struct.unpack_from("<6I", b, off + 8)
    print("q%d  md->ap read %6d write %6d (len %6d)   ap->md read %6d write %6d (len %6d)%s"
          % (q, rr, rw, rl, tr, tw, tl,
             "  AP PACKETS UNREAD" if tr != tw else ""))
    off += 8 + 24 + 8 + rl + tl
