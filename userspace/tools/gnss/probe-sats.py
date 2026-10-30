#!/usr/bin/env python3
"""
probe-sats.py: which satellites the chip acquired in a titan-gps-probe run.

    tools/gnss/probe-sats.py own.log [stale.log ...]

Reads the probe's log (`<ms> <type> <seq> <payload hex>`, XOR undone) and
decodes its 29 measurement records (meas-match.py's record()). Per
satellite: records, the highest C/N0, and when C/N0 first reached 20 dB-Hz
(the chip tracking it). The logs say where the phone was: keep them local.
"""
import collections
import importlib.util
import os
import sys


def load_matcher():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("meas_match", os.path.join(here, "meas-match.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mm = load_matcher()
    for path in sys.argv[1:]:
        sats = collections.defaultdict(lambda: [0, -99, None])
        for line in open(path):
            f = line.split()
            if len(f) != 4 or f[1] != "29":
                continue
            r = mm.record(bytes.fromhex(f[3]))
            if not r:
                continue
            s = sats[r["sat"]]
            s[0] += 1
            s[1] = max(s[1], r["cn0"])
            if r["cn0"] >= 20 and s[2] is None:
                s[2] = int(f[0]) / 1000
        tracked = {k: v for k, v in sats.items() if v[2] is not None}
        by = collections.Counter(k[0] for k in tracked)
        print("%s: tracked (C/N0 >= 20) GPS %d, GLONASS %d, BeiDou %d" % (
            os.path.basename(path), by["G"], by["R"], by["C"]))
        for k in sorted(sats):
            n, cn0, first = sats[k]
            if k[0] == "G":
                print("  %s%-3d records %4d  best C/N0 %3d  %s" % (
                    k[0], k[1], n, cn0, "tracked from %.1f s" % first if first is not None else "-"))


if __name__ == "__main__":
    main()
