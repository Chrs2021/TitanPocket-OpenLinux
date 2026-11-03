#!/usr/bin/env python3
"""Symbols of the modem firmware from MediaTek's DbgInfo file (shipped
with the firmware package, `DbgInfo_<build>`; vendor data: read it in
place, never commit it). Not the trace database: that one (MDDB, `.EDB`
and md1img's `md1_mddb`) is encrypted (docs/modem.md, "Modem debug data").

Layout: "CATICTNR", version, size; "CATI", version, build strings; a u32
count and a u32 length, then entries of name, NUL, start and end address
(u32 little endian), then the source file paths.

Usage: dbginfo.py DBGINFO ADDR...   the function holding each address
       dbginfo.py DBGINFO -g REGEX  functions whose names match
"""
import bisect
import re
import struct
import sys


def symbols(path):
    d = open(path, "rb").read()
    if d[:8] != b"CATICTNR":
        raise ValueError("not a DbgInfo file")
    o = d.index(b"INT_Vectors\0")      # the first entry
    out = []
    while True:
        z = d.find(b"\0", o)
        name = d[o:z]
        if not name or not all(32 <= c < 127 for c in name):
            break
        start, end = struct.unpack_from("<II", d, z + 1)
        out.append((start, end, name.decode()))
        o = z + 9
    out.sort()
    return out


def main():
    syms = symbols(sys.argv[1])
    if sys.argv[2] == "-g":
        rx = re.compile(sys.argv[3])
        for s, e, n in syms:
            if rx.search(n):
                print("%08x-%08x %s" % (s, e, n))
        return
    starts = [s for s, _, _ in syms]
    for a in sys.argv[2:]:
        addr = int(a, 16)
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and syms[i][0] <= addr < syms[i][1]:
            s, _, n = syms[i]
            print("%08x %s+%#x" % (addr, n, addr - s))
        else:
            print("%08x ?" % addr)


if __name__ == "__main__":
    main()
