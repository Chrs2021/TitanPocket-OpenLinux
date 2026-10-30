#!/usr/bin/env python3
"""
navsub.py: GPS navigation subframes from the chip's 2B frames, and broadcast
ephemeris decoded from them (docs/gnss-research.md, section 6).

    tools/gnss/navsub.py PROBE.log            # subframes and the ephemeris found
    tools/gnss/navsub.py PROBE.log --rinex    # the ephemeris as RINEX 3 GPS records

Every byte of a chip payload is descrambled as rotr8(b, 3) ^ 0x59 (libmnl,
FUN_0001d5e0), which is rotr8(b ^ 0xCA, 3): the probe's log and
stpgps-decode.py already undo the 0xCA, so a logged byte only needs the
rotation. A 2B payload is then (libmnl's FUN_00015ee4):

    bytes 0-1  status, u16 little-endian: bit i set = word i failed the
               chip's check (0x3FF: none usable); 0 = all good
    byte 2     PRN          byte 4  subframe id (1-5)     byte 5  unknown
    bytes 6-45 ten words of 4 bytes [data 9-16][parity][data 17-24][data 1-8]:
               word = q1 << 24 | q0 << 16 | q3 << 8 | q2  ->  [00 pppppp][d1..d24]

The data bits are the satellite's source data (the D30 inversion already
undone), what IS-GPS-200 lays out. Subframes 1-3 make an ephemeris when
their IODC (low 8 bits) and IODE agree. The log says where the phone was
and when: keep it local.
"""
import collections
import math
import sys

PI = 3.1415926535898          # IS-GPS-200's value


def rotr(b, n):
    return ((b >> n) | (b << (8 - n))) & 0xFF


def decode(payload, xored=False):
    """(prn, subframe id, status, [10 words]) of a 2B payload: as logged by
    the probe (0xCA already undone) or, with xored, as on the wire"""
    b = bytes(rotr(x ^ 0xCA if xored else x, 3) for x in payload)
    words = [b[7 + 4 * i] << 24 | b[6 + 4 * i] << 16 | b[9 + 4 * i] << 8 | b[8 + 4 * i]
             for i in range(10)]
    return b[2], b[4], b[0] | b[1] << 8, words


def _s(v, n):
    return v - (1 << n) if v >> (n - 1) else v


def ephemeris(sf1, sf2, sf3):
    """IS-GPS-200 20.3.3.3/4: an ephemeris dict in gpseph.py's form (angles
    in radians, as RINEX) from the 24-bit data of subframes 1-3, or None if
    their issues of data don't agree"""
    d1 = [w & 0xFFFFFF for w in sf1]
    d2 = [w & 0xFFFFFF for w in sf2]
    d3 = [w & 0xFFFFFF for w in sf3]
    iodc = ((d1[2] >> 6) & 3) << 8 | d1[7] >> 16
    iode2, iode3 = d2[2] >> 16, d3[9] >> 16
    if not (iode2 == iode3 == iodc & 0xFF):
        return None
    e = {}
    e["week"] = d1[2] >> 14                         # 10 bits, modulo 1024
    e["sva"] = (d1[2] >> 8) & 0xF
    e["svh"] = (d1[2] >> 2) & 0x3F
    e["iodc"] = iodc
    e["tgd"] = _s(d1[6] & 0xFF, 8) * 2 ** -31
    e["toc"] = (d1[7] & 0xFFFF) * 16
    e["af2"] = _s(d1[8] >> 16, 8) * 2 ** -55
    e["af1"] = _s(d1[8] & 0xFFFF, 16) * 2 ** -43
    e["af0"] = _s(d1[9] >> 2, 22) * 2 ** -31
    e["iode"] = iode2
    e["crs"] = _s(d2[2] & 0xFFFF, 16) * 2 ** -5
    e["dn"] = _s(d2[3] >> 8, 16) * 2 ** -43 * PI
    e["m0"] = _s((d2[3] & 0xFF) << 24 | d2[4], 32) * 2 ** -31 * PI
    e["cuc"] = _s(d2[5] >> 8, 16) * 2 ** -29
    e["e"] = ((d2[5] & 0xFF) << 24 | d2[6]) * 2 ** -33
    e["cus"] = _s(d2[7] >> 8, 16) * 2 ** -29
    e["sqrta"] = ((d2[7] & 0xFF) << 24 | d2[8]) * 2 ** -19
    e["toe"] = (d2[9] >> 8) * 16
    e["fit"] = (d2[9] >> 7) & 1
    e["cic"] = _s(d3[2] >> 8, 16) * 2 ** -29
    e["omega0"] = _s((d3[2] & 0xFF) << 24 | d3[3], 32) * 2 ** -31 * PI
    e["cis"] = _s(d3[4] >> 8, 16) * 2 ** -29
    e["i0"] = _s((d3[4] & 0xFF) << 24 | d3[5], 32) * 2 ** -31 * PI
    e["crc"] = _s(d3[6] >> 8, 16) * 2 ** -5
    e["w"] = _s((d3[6] & 0xFF) << 24 | d3[7], 32) * 2 ** -31 * PI
    e["omegadot"] = _s(d3[8], 24) * 2 ** -43 * PI
    e["idot"] = _s((d3[9] >> 2) & 0x3FFF, 14) * 2 ** -43 * PI
    return e


def iono(words):
    """Klobuchar coefficients (alpha0-3, beta0-3; IS-GPS-200 20.3.3.5.1.7) from
    subframe 4 page 18 (SV ID 56), or None for any other page"""
    d = [w & 0xFFFFFF for w in words]
    if (d[2] >> 16) & 0x3F != 56:
        return None
    b8 = lambda v, sh: _s((v >> sh) & 0xFF, 8)
    a = (b8(d[2], 8) * 2.0 ** -30, b8(d[2], 0) * 2.0 ** -27,
         b8(d[3], 16) * 2.0 ** -24, b8(d[3], 8) * 2.0 ** -24)
    bt = (b8(d[3], 0) * 2.0 ** 11, b8(d[4], 16) * 2.0 ** 14,
          b8(d[4], 8) * 2.0 ** 16, b8(d[4], 0) * 2.0 ** 16)
    return a + bt


def iono_from_lines(lines):
    """the newest Klobuchar coefficients in probe-log lines, or None"""
    found = None
    for line in lines:
        f = line.split()
        if len(f) == 4 and f[1] == "2b":
            prn, sfid, status, words = decode(bytes.fromhex(f[3]))
            if sfid == 4 and not status:
                found = iono(words) or found
    return found


def resolve_week(e, t_gps):
    """full GPS week (the subframe gives it modulo 1024) and t_toe, picking
    the week whose toe is nearest t_gps (GPS seconds)"""
    base = int(t_gps // 604800)
    best = min((base - 1, base, base + 1),
               key=lambda wk: abs(wk * 604800 + e["toe"] - t_gps) if wk % 1024 == e["week"] % 1024
               else 1e18)
    e["week"] = best
    e["t_toe"] = best * 604800 + e["toe"]
    return e


def from_lines(lines, t_gps=None):
    """{prn: [ephemeris]} from probe-log lines (`<ms> 2b <seq> <payload hex>`)"""
    sub = collections.defaultdict(dict)        # prn -> {subframe id: words}
    out = collections.defaultdict(list)
    for line in lines:
        f = line.split()
        if len(f) != 4 or f[1] != "2b":
            continue
        prn, sfid, status, words = decode(bytes.fromhex(f[3]))
        if status or not 1 <= sfid <= 3 or not 1 <= prn <= 32:
            continue
        if (words[1] >> 2) & 7 != sfid:         # HOW's subframe id must agree
            continue
        sub[prn][sfid] = words
        s = sub[prn]
        if len(s) == 3:
            e = ephemeris(s[1], s[2], s[3])
            if e and not any(x["iode"] == e["iode"] and x["toe"] == e["toe"] for x in out[prn]):
                if t_gps is not None:
                    resolve_week(e, t_gps)
                out[prn].append(e)
    return out


# BeiDou D1 navigation (B1I), from the chip's 4B frames (2026-10-30): a
# 47-byte payload, after rotr8(b, 3): bytes 0-1 status, byte 3 PRN, byte 5
# subframe (FraID), bytes 7-46 ten 30-bit words packed like 2B's (word =
# q1 << 24 | q0 << 16 | q3 << 8 | q2), already decoded: word 1 is 26
# information bits + 4 parity, words 2-10 are 22 information bits + 8 parity
# (the two BCH(15,11) codewords de-interleaved and their parity moved to the
# end; with parity between, subframe SOWs don't step by 6 s). Fields per the
# BeiDou ICD (B1I) 5.2.

def decode_bds(payload):
    """(prn, subframe, status, information bits as an int, their count 224)"""
    b = bytes(rotr(x, 3) for x in payload)
    w = [(b[8 + 4 * i] << 24 | b[7 + 4 * i] << 16 | b[10 + 4 * i] << 8 | b[9 + 4 * i]) & 0x3FFFFFFF
         for i in range(10)]
    info = (w[0] >> 4) & ((1 << 26) - 1)
    for v in w[1:]:
        info = (info << 22) | ((v >> 8) & 0x3FFFFF)
    return b[3], b[5], b[0] | b[1] << 8, info


def _field(info, start, n, signed=False):
    """n bits of the 224 information bits from bit `start` (0 = first)"""
    v = (info >> (224 - start - n)) & ((1 << n) - 1)
    return _s(v, n) if signed else v


def bds_ephemeris(i1, i2, i3):
    """a gpseph-style BeiDou ephemeris from subframes 1-3 (information bits)"""
    f = _field
    sc = PI
    e = {"svh": f(i1, 38, 1), "aodc": f(i1, 39, 5), "sva": f(i1, 44, 4),
         "week": f(i1, 48, 13), "toc": f(i1, 61, 17) * 8,
         "tgd": f(i1, 78, 10, True) * 0.1e-9, "tgd2": f(i1, 88, 10, True) * 0.1e-9,
         "af2": f(i1, 162, 11, True) * 2.0 ** -66, "af0": f(i1, 173, 24, True) * 2.0 ** -33,
         "af1": f(i1, 197, 22, True) * 2.0 ** -50, "aode": f(i1, 219, 5)}
    e.update({"dn": f(i2, 38, 16, True) * 2.0 ** -43 * sc, "cuc": f(i2, 54, 18, True) * 2.0 ** -31,
              "m0": f(i2, 72, 32, True) * 2.0 ** -31 * sc, "e": f(i2, 104, 32) * 2.0 ** -33,
              "cus": f(i2, 136, 18, True) * 2.0 ** -31, "crc": f(i2, 154, 18, True) * 2.0 ** -6,
              "crs": f(i2, 172, 18, True) * 2.0 ** -6, "sqrta": f(i2, 190, 32) * 2.0 ** -19})
    e["toe"] = (f(i2, 222, 2) << 15 | f(i3, 38, 15)) * 8
    e.update({"i0": f(i3, 53, 32, True) * 2.0 ** -31 * sc, "cic": f(i3, 85, 18, True) * 2.0 ** -31,
              "omegadot": f(i3, 103, 24, True) * 2.0 ** -43 * sc,
              "cis": f(i3, 127, 18, True) * 2.0 ** -31, "idot": f(i3, 145, 14, True) * 2.0 ** -43 * sc,
              "omega0": f(i3, 159, 32, True) * 2.0 ** -31 * sc, "w": f(i3, 191, 32, True) * 2.0 ** -31 * sc})
    if not 5000 < e["sqrta"] < 6600 or e["e"] > 0.1:
        return None
    # gpseph's conventions (load_gnss): BDT week 0 = GPS week 1356, BDT = GPST - 14 s
    e["t_toe"] = (e["week"] + 1356) * 604800 + e["toe"] + 14
    e["sys"], e["freq"] = "C", 1561.098e6
    return e


def bds_from_lines(lines):
    """{("C", prn): [ephemeris]} from probe-log lines' 4B frames: subframes
    1, 2 and 3 of one 30 s frame (their SOW 0, 6, 12 s apart)"""
    sub = collections.defaultdict(dict)        # prn -> {subframe: (sow, info)}
    out = collections.defaultdict(list)
    for line in lines:
        f = line.split()
        if len(f) != 4 or f[1] != "4b":
            continue
        prn, fra, status, info = decode_bds(bytes.fromhex(f[3]))
        if status or not 1 <= fra <= 3 or _field(info, 0, 11) != 0x712 or _field(info, 15, 3) != fra:
            continue
        sow = _field(info, 18, 20)
        sub[prn][fra] = (sow, info)
        s = sub[prn]
        if fra == 3 and all(k in s for k in (1, 2)) and \
                s[2][0] - s[1][0] == 6 and s[3][0] - s[2][0] == 6:
            e = bds_ephemeris(s[1][1], s[2][1], s[3][1])
            if e and not any(x["aode"] == e["aode"] and x["toe"] == e["toe"] for x in out[("C", prn)]):
                e["prn"] = prn
                out[("C", prn)].append(e)
    return out


# GLONASS navigation strings (L1OF), from the chip's 3B frames (2026-10-30):
# a 14-byte payload, after rotr8(b, 3): byte 0 the channel (as in the 29
# records), byte 2 the string number m (1-15), bytes 4-13 the string's bits
# 80..1 as five 16-bit little-endian words, most significant bit first (bit
# 85, the idle bit, and m are not repeated). Fields per the GLONASS ICD 5.1
# 4.4, magnitudes with a sign bit.

def decode_glo(payload):
    """(channel, string number, the string's bits 80..1 as an int)"""
    b = bytes(rotr(x, 3) for x in payload)
    v = 0
    for i in range(4, 14, 2):
        v = (v << 16) | b[i + 1] << 8 | b[i]
    return b[0], b[2], v


def _g(v, hi, lo, sm=False):
    """string bits hi..lo (ICD numbering, 1 = last); sm: sign-magnitude"""
    n = hi - lo + 1
    x = (v >> (lo - 1)) & ((1 << n) - 1)
    if sm and x >> (n - 1):
        return -(x & ((1 << (n - 1)) - 1))
    return x


def glo_ephemeris(s1, s2, s3, s4, k, t_gps):
    """a gpseph-style GLONASS ephemeris from strings 1-4 (bits as ints), for
    frequency channel k, received near GPS seconds t_gps"""
    import datetime
    e = {"vx": _g(s1, 64, 41, True) * 2.0 ** -20, "ax": _g(s1, 40, 36, True) * 2.0 ** -30,
         "x": _g(s1, 35, 9, True) * 2.0 ** -11,
         "vy": _g(s2, 64, 41, True) * 2.0 ** -20, "ay": _g(s2, 40, 36, True) * 2.0 ** -30,
         "y": _g(s2, 35, 9, True) * 2.0 ** -11, "svh": _g(s2, 80, 78) >> 2,
         "gamma": _g(s3, 79, 69, True) * 2.0 ** -40,
         "vz": _g(s3, 64, 41, True) * 2.0 ** -20, "az": _g(s3, 40, 36, True) * 2.0 ** -30,
         "z": _g(s3, 35, 9, True) * 2.0 ** -11,
         "mtaun": -_g(s4, 80, 59, True) * 2.0 ** -30, "age": _g(s4, 53, 49),
         "slot": _g(s4, 15, 11), "k": k}
    tk = _g(s1, 76, 72) * 3600 + _g(s1, 71, 66) * 60 + _g(s1, 65, 65) * 30
    tb = _g(s2, 76, 70) * 900                       # Moscow time of day
    e["tk"] = (tk - 10800) % 86400
    if not 1 <= e["slot"] <= 24 or not 2.4e4 < (e["x"] ** 2 + e["y"] ** 2 + e["z"] ** 2) ** 0.5 < 2.7e4:
        return None
    # tb in UTC on the day that puts it nearest the reception time
    gps0 = datetime.datetime(1980, 1, 6, tzinfo=datetime.timezone.utc)
    rx = gps0 + datetime.timedelta(seconds=t_gps - 18)
    day = datetime.datetime(rx.year, rx.month, rx.day, tzinfo=datetime.timezone.utc)
    cands = [day + datetime.timedelta(days=d, seconds=tb - 10800) for d in (-1, 0, 1)]
    utc = min(cands, key=lambda c: abs((c - rx).total_seconds()))
    e["t_toe"] = (utc - gps0).total_seconds() + 18
    e["sys"], e["freq"] = "R", 1602e6 + k * 0.5625e6
    return e


def glo_from_lines(lines, t_gps):
    """{("R", slot): [ephemeris]} from probe-log lines: 3B strings 1-4 of a
    satellite received within 10 s, kept once the same content (all but
    t_k) has come in two 30 s frames: the strings carry no check we can
    apply (the chip leaves out bits the Hamming code covers), and a few are
    corrupted. The frame's first byte is the frequency channel k + 8, as in
    the 29 records."""
    strings = collections.defaultdict(dict)     # channel -> {m: (ms, bits)}
    seen = collections.Counter()
    out = collections.defaultdict(list)
    for line in lines:
        f = line.split()
        if len(f) != 4 or f[1] != "3b":
            continue
        ch, m, bits = decode_glo(bytes.fromhex(f[3]))
        ms = int(f[0])
        strings[ch][m] = (ms, bits)
        st = strings[ch]
        if m == 4 and all(i in st and ms - st[i][0] <= 10000 for i in (1, 2, 3)):
            # leave out t_k and every string's Hamming bits (1-8): t_k
            # changes them each frame
            sig = (ch, (st[1][1] >> 8) & ((1 << 56) - 1), st[2][1] >> 8, st[3][1] >> 8, st[4][1] >> 8)
            seen[sig] += 1
            if seen[sig] != 2:
                continue
            e = glo_ephemeris(st[1][1], st[2][1], st[3][1], st[4][1], ch - 8, t_gps + ms / 1000)
            if e and not any(abs(x["t_toe"] - e["t_toe"]) < 1 for x in out[("R", e["slot"])]):
                out[("R", e["slot"])].append(e)
    return out


def rinex(prn, e):
    """one RINEX 3 GPS navigation record (as gpseph.load reads it)"""
    import datetime
    # RINEX gives a GPS satellite's clock epoch in GPS time: no leap seconds
    # (gpseph.load keeps no toc: a GPS satellite's toc equals its toe in practice)
    t = datetime.datetime(1980, 1, 6) + datetime.timedelta(
        seconds=e["week"] * 604800 + e.get("toc", e["toe"]))
    f = lambda v: ("%19.12E" % v).replace("E", "D").replace("D", "E")
    rows = [[e["af0"], e["af1"], e["af2"]], [e["iode"], e["crs"], e["dn"], e["m0"]],
            [e["cuc"], e["e"], e["cus"], e["sqrta"]], [e["toe"], e["cic"], e["omega0"], e["cis"]],
            [e["i0"], e["crc"], e["w"], e["omegadot"]], [e["idot"], 0, e["week"], 0],
            [0, e["svh"], e["tgd"], e["iodc"]], [e["toe"], 4 if e["fit"] else 0]]
    lines = ["G%02d %04d %02d %02d %02d %02d %02d" % (prn, t.year, t.month, t.day, t.hour, t.minute,
                                                     t.second) + "".join(f(v) for v in rows[0])]
    lines += ["    " + "".join(f(v) for v in r) for r in rows[1:]]
    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    import time
    t_gps = time.time() - 315964800 + 18
    eph = from_lines(open(sys.argv[1]), t_gps)
    if "--rinex" in sys.argv:
        print("     3.04           N: GNSS NAV DATA    G: GPS              RINEX VERSION / TYPE")
        print("                                                            END OF HEADER")
        for prn in sorted(eph):
            for e in eph[prn]:
                print(rinex(prn, e))
        return
    for prn in sorted(eph):
        for e in eph[prn]:
            print("G%02d IODE %3d toe %6d sqrtA %.6f e %.9f health %d" % (
                prn, e["iode"], e["toe"], e["sqrta"], e["e"], e["svh"]))


if __name__ == "__main__":
    main()
