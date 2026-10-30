#!/usr/bin/env python3
"""
flip-steps.py: what a bit of the chip's 29 records means to libmnl, from
titan-mnld's bit-flip experiments (TITAN_MNLD_FLIP, docs/gnss.md).

    tools/gnss/flip-steps.py flip-28:10.log [flip-sats.log ...]

Each log is a measurement log with F lines (tools/gnss/flip-run.sh). For each
GPS satellite, the step in libmnl's received SV time from one second to the
next, beyond the receiver clock's own step, is compared with whether that
satellite's records were flipped just before: a flip switched on or off shows
as a step of that bit's weight, signed by the bit's new value. Printed per
flipped bit (byte.bit) in ns and in C/A chips (1 ms = 1023 chips), with how
the steps spread: steps of 0 are seconds whose measurement didn't come from a
flipped record. The logs say where the phone was: keep them local.
"""
import bisect
import collections
import re
import struct
import sys

import numpy as np


def load(path):
    clock, meas, flips = {}, collections.defaultdict(list), collections.defaultdict(list)
    m = re.search(r"flip-(\d+):([0-9a-f]+)", path)
    for line in open(path):
        f = line.split()
        if f[0] == "C":
            clock[int(f[1])] = struct.unpack_from("<q", bytes.fromhex(f[2]), 8)[0]
        elif f[0] == "M":
            b = bytes.fromhex(f[4])
            if b[10] == 1 and struct.unpack_from("<I", b, 0x14)[0] & 1:     # GPS, code lock
                meas[struct.unpack_from("<h", b, 8)[0]].append(
                    (int(f[1]), struct.unpack_from("<q", b, 0x18)[0]))
        elif f[0] == "F":
            b1, b2, old, new = (int(x, 16) for x in f[2:6])
            byte = int(f[6]) if len(f) > 6 else int(m.group(1))
            if b2 == 8:
                flips[b1 >> 3].append((int(f[1]), byte, old ^ new, new & (old ^ new) != 0))
    return clock, meas, flips


def steps(clock, meas, flips):
    """{(byte, mask): {True: [ns], False: [ns]}}: steps when a flip set or cleared the bit"""
    out = collections.defaultdict(lambda: {True: [], False: []})
    for sv, l in meas.items():
        ft = [x[0] for x in flips[sv]]

        def state(ms):          # the flip in force at ms, if any
            i = bisect.bisect_right(ft, ms) - 1
            return flips[sv][i] if i >= 0 and ms - ft[i] < 1500 else None

        ys, ss = [], []
        for (t0, s0), (t1, s1) in zip(l, l[1:]):
            if t0 in clock and t1 in clock and t1 - t0 < 1500:
                ys.append((s1 - s0) - (clock[t1] - clock[t0]))
                ss.append((state(t0), state(t1)))
        quiet = [y for y, (a, b) in zip(ys, ss) if a is None and b is None]
        if not quiet:
            continue
        bg = np.median(quiet)   # the satellite's own code drift
        wrap = lambda d: (d + 500000) % 1000000 - 500000
        for y, (a, b) in zip(ys, ss):
            if a is None and b is not None:
                out[b[1], b[2]][b[3]].append(wrap(y - bg))
            elif b is None and a is not None:
                out[a[1], a[2]][a[3]].append(wrap(bg - y))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        for (byte, mask), s in sorted(steps(*load(path)).items()):
            bit = byte * 8 + mask.bit_length() - 1
            for setting in (True, False):
                v = np.array(s[setting])
                v = v[np.abs(v) > 100]          # drop seconds the flip didn't reach
                if len(v):
                    ns = np.median(v)
                    print("bit %3d (%d.%d) set %d: %+11.1f ns  %+9.3f chips  n %3d  spread %.0f ns" % (
                        bit, byte, mask.bit_length() - 1, setting, ns, ns * 1.023e-3, len(v),
                        np.subtract(*np.percentile(v, [75, 25]))))


if __name__ == "__main__":
    main()
