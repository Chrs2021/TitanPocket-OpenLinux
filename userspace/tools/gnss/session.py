#!/usr/bin/env python3
"""
session.py: a whole titan-gps-probe session script made from scratch, with
no libmnl capture (docs/gnss-open.md, "Replacing libmnl").

    tools/gnss/session.py SECONDS > session.script     # then own02.py-style 02/03

The frames that every captured libmnl session sends identically (the 08
header, the 05 settings, 04, the 08 controls, 1C, 05 0x0029, the short 03)
are protocol constants, kept here. The counters are generated:

    00  00 00 [u16 ms]               the session's milliseconds, about T - 250
    01  [u32 0x60][u32 0x111f][u16 the previous 00's ms][u16 0x0425][00]
        [u16 seconds in 1.5 s units][00 00 00]
    10  the clock-correction values of a captured session, unchanged (changing
        them changed nothing in tracking)
    02  one placeholder per constellation and write group, with its time
        field: GPS [0400][u16 1.5 s count, as 01's][u16 1/24 s], GLONASS
        [0000][u16 s][u16 1/24 s], BeiDou [0600][u16 s][u16 1/24 s], and the
        search-mode tail (bytes 12-19) of libmnl's first frame
    03  the satellite list: a placeholder "00 00"

own02.regenerate then fills the 02 frames with the satellites above the
horizon and their Doppler, and the 03 list. The schedule is libmnl's: a
setup burst at 722-761 ms, then a cycle every 500 ms for the first four,
every second after: 00/10/01 at T, the 02 group at T + 11 ms, and
03/1C/05 0x0029 at T + 55 ms. The SBAS search rotation (07), the 0A
frames and the 05 0x000c/0x010c messages are left out.
"""
import struct
import sys

HEADER_08 = ("1a0000000000c301a502000000000078008004807b003100000000000500c8006f3ca45fe705"
             "0000d200b7002800404a1e00b0372800404a8f65070001002800ff008000470064005000d800"
             "5000bb0003003c006f00890088000200fb00010000009b08000001004e4f")
SETUP_05 = ["0a0000000000000000", "3200000100", "460200010beeff986e", "3600000000",
            "2a00000000", "3400000100", "0f0000e803", "1500000200", "0f0000f401", "1900000300"]
CLOCK_10 = "01486a0000fffffd6a686d0000ffffbf6c88700000ffff816e"
LIST_SHORT_03 = "0100"
ALL_FF_04 = "ff" * 30 + "0000"
# the 02 frames' constellation byte, bytes 2-3, and search-mode tail (12-19)
TEMPLATE_02 = {0x01: ("0400", "e601000201004100"),
               0x10: ("0000", "9606000100000100"),
               0x20: ("0600", "cb01ff0300000100")}


def u16(v):
    return struct.pack("<H", v & 0xFFFF).hex()


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF).hex()


def time_field(con, t):
    """bytes 4-7 of a 02 frame for session time t (s); counters start at 0"""
    if con == 0x01:                           # GPS: 1.5 s units, then 1/24 s
        return u16(int(t / 1.5)) + u16(int((t % 1.5) * 24))
    return u16(int(t)) + u16(int((t % 1) * 24))


def generate(seconds, epoch=0.0):
    """the script's lines for a session of `seconds`; epoch (s) starts the
    02/01 time counters (any value: libmnl's differ from run to run)"""
    out = ["0 08 " + HEADER_08, "3 PATCH", "722 05 3200000100"]
    T, cycle, prev_ms = 761, 0, 12
    while T < seconds * 1000:
        ms = T - 250
        t = epoch + T / 1000
        g = []
        if cycle == 0:
            g += [("05", x) for x in SETUP_05]
        elif cycle < 4:
            g.append(("05", "3400000100"))
        g += [("00", "0000" + u16(ms)), ("10", CLOCK_10),
              ("01", u32(0x60) + u32(0x111F) + u16(prev_ms) + "2504" + "00" + u16(int(t / 1.5)) + "000000")]
        prev_ms = ms
        out += [("+" if i else "") + "%d %s %s" % (T, k, p) for i, (k, p) in enumerate(g)]
        t2 = epoch + (T + 11) / 1000
        out += [("+" if i else "") + "%d 02 %s" % (T + 11, "00%02x%s%s00000000%s" % (
            con, b23, time_field(con, t2), tail)) for i, (con, (b23, tail)) in enumerate(TEMPLATE_02.items())]
        g = [("03", "0000"), ("03", LIST_SHORT_03)]
        if cycle == 0:
            g += [("04", ALL_FF_04), ("08", "10001000")]
        g += [("1c", ""), ("05", "2900")]
        if T > 7000:
            g.append(("05", "1c00006400"))
        out += [("+" if i else "") + ("%d %s %s" % (T + 55, k, p)).rstrip() for i, (k, p) in enumerate(g)]
        if cycle == 0:
            out.append("%d 08 1800" % (T + 60))
        T += 500 if cycle < 4 else 1000
        cycle += 1
    return out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print("\n".join(generate(float(sys.argv[1]))))
