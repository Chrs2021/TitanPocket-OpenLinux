#!/usr/bin/env python3
"""
snapfix.py: a GPS position from titan-gps-probe's log, with no libmnl: the
chip's whole-chip code phases (docs/gnss.md, 29 records), broadcast
ephemeris, the probe's start time and a rough position.

    tools/gnss/snapfix.py PROBE.log BRDC.rnx START_UTC LAT LON HEIGHT [--truth LAT LON HEIGHT]

START_UTC (ISO) is when the probe started (own02.py's script says it);
LAT LON HEIGHT is the a-priori position, good to some tens of km (the chip
gives code phase only modulo 1 ms, 300 km of range). With --truth it prints
how far each fix is from that position, not where it is.

Method: a snapshot ("coarse-time") fix. The records of one batch (bytes
20-26 alike) are measured together. Each gives the satellite's transmit
time modulo 1 ms, in whole chips plus a per-batch offset common to all.
From the a-priori position and time, each satellite's whole milliseconds
are resolved against a reference satellite, and a least-squares solution
finds position, receiver clock bias and the error of the time stamp
(which moves the satellites). Batches with five satellites or more each
give a fix, and their median is printed; last, all batches are solved
together (one position, a clock bias and time error per batch), dropping
batches that don't fit, which is the fix to use.
"""
import collections
import datetime
import importlib.util
import math
import os
import sys

C = 299792458.0
MS = C * 1e-3                       # one millisecond of range, m
CHIP = MS / 1023
F_REL = -4.442807633e-10            # relativistic clock term, s/m^0.5


def _load(name, file):
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(name, os.path.join(here, file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ge = _load("gpseph", "gpseph.py")
mm = _load("meas_match", "meas-match.py")


def sat_clock(e, t):
    """satellite clock offset (s) at GPS seconds t, relativity included
    (GPS, BeiDou: polynomial, relativity and group delay TGD/TGD1; GLONASS:
    the broadcast -tau_n and gamma_n, relativity already in them)"""
    tk = t - e["t_toe"]
    if e.get("sys") == "R":
        return e["mtaun"] + e["gamma"] * tk
    a = e["sqrta"] ** 2
    n = math.sqrt(ge.MU / a ** 3) + e["dn"]
    m = e["m0"] + n * tk
    ek = m
    for _ in range(10):
        ek = m + e["e"] * math.sin(ek)
    return (e["af0"] + e["af1"] * tk + e["af2"] * tk * tk
            + F_REL * e["e"] * e["sqrta"] * math.sin(ek) - e["tgd"])


# atmosphere: IONO = Klobuchar alpha0-3, beta0-3 (GPS subframe 4 page 18,
# navsub.iono), None = no ionosphere; TROPO = troposphere on/off
IONO = None
TROPO = True
F_L1 = 1575.42e6


def carrier(e):
    if e.get("sys") == "R":
        return 1602e6 + e["k"] * 562500
    if e.get("sys") == "C":
        return 1561.098e6
    return F_L1


def klobuchar(a, b, lat, lon, el, az, t):
    """L1 ionospheric delay (s), IS-GPS-200 20.3.3.5.2.5; angles in radians,
    t GPS seconds"""
    sc = math.pi
    phu, lmu, E, A = lat / sc, lon / sc, el / sc, az
    psi = 0.0137 / (E + 0.11) - 0.022
    phi = phu + psi * math.cos(A)
    phi = max(-0.416, min(0.416, phi))
    lam = lmu + psi * math.sin(A) / math.cos(phi * sc)
    phm = phi + 0.064 * math.cos((lam - 1.617) * sc)
    tl = (4.32e4 * lam + t) % 86400
    F = 1.0 + 16.0 * (0.53 - E) ** 3
    amp = max(0.0, sum(a[i] * phm ** i for i in range(4)))
    per = max(72000.0, sum(b[i] * phm ** i for i in range(4)))
    x = 2 * math.pi * (tl - 50400) / per
    return F * (5e-9 + (amp * (1 - x * x / 2 + x ** 4 / 24) if abs(x) < 1.57 else 0.0))


def atmosphere(e, rx, d, r, t):
    """ionospheric + tropospheric delay (m) on the path rx -> satellite (d, r)"""
    lat, lon = ecef_latlon(rx)
    h = math.sqrt(sum(x * x for x in rx)) - math.sqrt(sum(x * x for x in ge.ecef(lat, lon, 0)))
    la, lo = math.radians(lat), math.radians(lon)
    en = (-math.sin(lo) * d[0] + math.cos(lo) * d[1],
          -math.sin(la) * math.cos(lo) * d[0] - math.sin(la) * math.sin(lo) * d[1] + math.cos(la) * d[2],
          math.cos(la) * math.cos(lo) * d[0] + math.cos(la) * math.sin(lo) * d[1] + math.sin(la) * d[2])
    el = math.asin(max(-1.0, min(1.0, en[2] / r)))
    if el <= 0.05:
        return 0.0
    out = 0.0
    if IONO:
        out += klobuchar(IONO[:4], IONO[4:], la, lo, el, math.atan2(en[0], en[1]), t) * C \
            * (F_L1 / carrier(e)) ** 2
    if TROPO:
        hk = max(-0.5, min(9.0, h / 1000))
        p = 1013.25 * (1 - 2.2557e-5 * hk * 1000) ** 5.2568       # hPa, standard atmosphere
        zhd = 0.0022768 * p / (1 - 0.00266 * math.cos(2 * la) - 0.00028 * hk)
        out += (zhd + 0.1) * 1.001 / math.sqrt(0.002001 + math.sin(el) ** 2)
    return out


def predict(e, t_rx, rx):
    """(geometric range m, unit vector rx->sat, range rate m/s, sat clock s)
    for a signal received at GPS seconds t_rx at rx (ECEF)"""
    pos = ge.position_any if e.get("sys") in ("R", "C") else ge.position
    tau = 0.075
    for _ in range(3):
        s = pos(e, t_rx - tau)
        th = ge.OMEGA_E * tau
        s = (s[0] * math.cos(th) + s[1] * math.sin(th), -s[0] * math.sin(th) + s[1] * math.cos(th), s[2])
        d = [s[k] - rx[k] for k in range(3)]
        r = math.sqrt(sum(x * x for x in d))
        tau = r / C
    s2 = pos(e, t_rx + 0.5 - tau)
    s1 = pos(e, t_rx - 0.5 - tau)
    v = [s2[k] - s1[k] for k in range(3)]
    u = [x / r for x in d]
    # the atmosphere delays the signal: the range it looks like
    rr = r + (atmosphere(e, rx, d, r, t_rx) if IONO or TROPO else 0.0)
    return rr, u, sum(v[k] * u[k] for k in range(3)), sat_clock(e, t_rx - tau)


def solve(meas, eph, t0, rx0, iters=8):
    """meas: [(prn, transmit time mod 1 ms in m)]; returns (rx, bias m, dt s, residuals)"""
    rx, b, dt = list(rx0), 0.0, 0.0
    res = None
    for _ in range(iters):
        rows, pred = [], []
        for prn, m in meas:
            e = ge.nearest(eph[prn], t0 + dt)
            r, u, rate, clk = predict(e, t0 + dt, rx)
            pred.append((r - clk * C, u, rate))
        # measured: range = (t_rx - t_tx) * c, known modulo 1 ms: m is the
        # transmit time modulo 1 ms, so range = -m (mod MS) + common
        y = [(-m) % MS for _, m in meas]
        ref = 0
        wrap = lambda x: (x + MS / 2) % MS - MS / 2
        base = wrap(y[ref] - pred[ref][0])
        full = [pred[i][0] + wrap(y[i] - pred[i][0] - base) + base for i in range(len(meas))]
        H, z = [], []
        for i, (p, u, rate) in enumerate(pred):
            H.append([-u[0], -u[1], -u[2], 1.0, rate])
            z.append(full[i] - (p + b))
        dx = lstsq(H, z)
        rx = [rx[k] + dx[k] for k in range(3)]
        b += dx[3]
        dt += dx[4]
        res = z
        if math.sqrt(sum(x * x for x in dx[:3])) < 0.1:
            break
    return rx, b, dt, res


def solve_joint(sets, eph, rx0, iters=10, drop=150.0):
    """sets: [(t0, meas)] batches; one position for all, a clock bias and a
    time error per batch. Each batch's two unknowns are eliminated from the
    normal equations (a Schur complement), leaving 3x3 for the position.
    Batches whose residual rms passes `drop` metres are left out and the
    rest solved again. Returns (rx, batches used, rms)."""
    rx = list(rx0)
    k = len(sets)
    b, dt = [0.0] * k, [0.0] * k
    use = list(range(k))
    wrap = lambda x: (x + MS / 2) % MS - MS / 2
    per = {}
    for rnd in range(3):
        for _ in range(iters):
            A = [[0.0] * 3 for _ in range(3)]
            v = [0.0] * 3
            local = {}
            for bi in use:
                t0, meas = sets[bi]
                rows, zs = [], []
                y = [(-m) % MS for _, m in meas]
                pred = []
                for prn, m in meas:
                    e = ge.nearest(eph[prn], t0 + dt[bi])
                    r, u, rate, clk = predict(e, t0 + dt[bi], rx)
                    pred.append((r - clk * C, u, rate))
                base = wrap(y[0] - pred[0][0])
                for i, (p, u, rate) in enumerate(pred):
                    full = p + wrap(y[i] - p - base) + base
                    zs.append(full - (p + b[bi]))
                    rows.append(([-u[0], -u[1], -u[2]], [1.0, rate]))
                per[bi] = zs
                # this batch's normal equations, split global (g) / local (l)
                Agg = [[sum(g[i] * g[j] for g, _ in rows) for j in range(3)] for i in range(3)]
                Agl = [[sum(g[i] * l[j] for g, l in rows) for j in range(2)] for i in range(3)]
                All = [[sum(l[i] * l[j] for _, l in rows) for j in range(2)] for i in range(2)]
                vg = [sum(g[i] * z for (g, _), z in zip(rows, zs)) for i in range(3)]
                vl = [sum(l[i] * z for (_, l), z in zip(rows, zs)) for i in range(2)]
                det = All[0][0] * All[1][1] - All[0][1] * All[1][0]
                if abs(det) < 1e-9:
                    continue
                inv = [[All[1][1] / det, -All[0][1] / det], [-All[1][0] / det, All[0][0] / det]]
                M = [[sum(Agl[i][m] * inv[m][j] for m in range(2)) for j in range(2)] for i in range(3)]
                for i in range(3):
                    for j in range(3):
                        A[i][j] += Agg[i][j] - sum(M[i][m] * Agl[j][m] for m in range(2))
                    v[i] += vg[i] - sum(M[i][m] * vl[m] for m in range(2))
                local[bi] = (inv, Agl, vl)
            dg = gauss(A, v)
            rx = [rx[i] + dg[i] for i in range(3)]
            for bi, (inv, Agl, vl) in local.items():
                r_ = [vl[j] - sum(Agl[i][j] * dg[i] for i in range(3)) for j in range(2)]
                b[bi] += inv[0][0] * r_[0] + inv[0][1] * r_[1]
                dt[bi] += inv[1][0] * r_[0] + inv[1][1] * r_[1]
            if math.sqrt(sum(x * x for x in dg)) < 0.05:
                break
        rms = {bi: math.sqrt(sum(z * z for z in per[bi]) / len(per[bi])) for bi in use}
        keep = [bi for bi in use if rms[bi] < drop]
        if len(keep) == len(use) or len(keep) < 3:
            break
        use = keep
    all_z = [z for bi in use for z in per[bi]]
    return rx, len(use), math.sqrt(sum(z * z for z in all_z) / len(all_z))


def solve_joint_t(sets, eph, rx0, iters=10, drop=150.0, height=None):
    """As solve_joint, but with one time error for all batches: their times
    relative to each other come from the probe's log and are good to a few
    ms; only the start time is uncertain. Each batch keeps just its clock
    bias, so batches of four satellites count and every batch has more
    redundancy. height = (metres above the ellipsoid, sigma m) adds that
    as a measurement: with the same four satellites in every batch the
    position and the time error can't be told apart without it (a live run
    gave fixes kilometres off). Returns (rx, batches used, rms, time error s)."""
    rx = list(rx0)
    dt = 0.0
    k = len(sets)
    b = [0.0] * k
    use = list(range(k))
    wrap = lambda x: (x + MS / 2) % MS - MS / 2
    per = {}
    for rnd in range(3):
        for _ in range(iters):
            A = [[0.0] * 4 for _ in range(4)]
            v = [0.0] * 4
            local = {}
            for bi in use:
                t0, meas = sets[bi]
                y = [(-m) % MS for _, m in meas]
                pred = []
                for prn, m in meas:
                    e = ge.nearest(eph[prn], t0 + dt)
                    r, u, rate, clk = predict(e, t0 + dt, rx)
                    pred.append((r - clk * C, u, rate))
                base = wrap(y[0] - pred[0][0])
                rows, zs = [], []
                for i, (p, u, rate) in enumerate(pred):
                    full = p + wrap(y[i] - p - base) + base
                    zs.append(full - (p + b[bi]))
                    rows.append([-u[0], -u[1], -u[2], rate])
                per[bi] = zs
                # the batch's bias (local, coefficient 1) eliminated: subtract
                # each row's and residual's mean
                n = len(rows)
                mg = [sum(g[i] for g in rows) / n for i in range(4)]
                mz = sum(zs) / n
                for g, z in zip(rows, zs):
                    gc = [g[i] - mg[i] for i in range(4)]
                    for i in range(4):
                        v[i] += gc[i] * (z - mz)
                        for j in range(4):
                            A[i][j] += gc[i] * gc[j]
                local[bi] = (mg, mz)
            if height:
                # h(rx) ~ h0, weighted against ~85 m code noise
                up = [x / math.sqrt(sum(y * y for y in rx)) for x in rx]
                la, lo = ecef_latlon(rx)
                h_now = math.sqrt(sum(x * x for x in rx)) - math.sqrt(
                    sum(x * x for x in ge.ecef(la, lo, 0)))
                w = 85.0 / height[1]
                g = [up[0] * w, up[1] * w, up[2] * w, 0.0]
                z = (height[0] - h_now) * w
                for i in range(4):
                    v[i] += g[i] * z
                    for j in range(4):
                        A[i][j] += g[i] * g[j]
            dg = gauss(A, v)
            rx = [rx[i] + dg[i] for i in range(3)]
            dt += dg[3]
            for bi, (mg, mz) in local.items():
                b[bi] += mz - sum(mg[i] * dg[i] for i in range(4))
            if math.sqrt(sum(x * x for x in dg[:3])) < 0.05:
                break
        rms = {bi: math.sqrt(sum(z * z for z in per[bi]) / len(per[bi])) for bi in use}
        keep = [bi for bi in use if rms[bi] < drop]
        if len(keep) == len(use) or len(keep) < 3:
            break
        use = keep
    all_z = [z for bi in use for z in per[bi]]
    return rx, len(use), math.sqrt(sum(z * z for z in all_z) / len(all_z)), dt


def sat_bias(sets, eph, rx, dt):
    """{prn: (mean residual m, count)} at a solution: each batch's residuals
    less their mean (its clock bias). A satellite whose mean stays well off
    zero is biased (a reflected signal, indoors), not noisy: whole chips
    alone scatter by ~85 m rms around the truth."""
    wrap = lambda x: (x + MS / 2) % MS - MS / 2
    acc = collections.defaultdict(list)
    for t0, meas in sets:
        y = [(-m) % MS for _, m in meas]
        pred = []
        for prn, m in meas:
            e = ge.nearest(eph[prn], t0 + dt)
            r, u, rate, clk = predict(e, t0 + dt, rx)
            pred.append(r - clk * C)
        base = wrap(y[0] - pred[0])
        z = [wrap(y[i] - pred[i] - base) + base for i in range(len(meas))]
        mz = sum(z) / len(z)
        for (prn, _), zi in zip(meas, z):
            acc[prn].append(zi - mz)
    return {p: (sum(v) / len(v), len(v)) for p, v in acc.items()}


def solve_robust(sets, eph, rx0, limit=120.0, min_sats=4, height=None):
    """solve_joint_t, then leave out the satellite with the largest steady
    bias while one passes `limit` metres, as long as enough batches keep
    `min_sats` satellites. Returns (rx, batches, rms, dt, dropped prns)."""
    dropped = []
    rx, used, rms, dt = solve_joint_t(sets, eph, rx0, height=height)
    while True:
        bias = sat_bias(sets, eph, rx, dt)
        worst = max(((abs(b), p) for p, (b, n) in bias.items() if n >= 5), default=None)
        if not worst or worst[0] < limit:
            break
        trial = [(t, [m for m in meas if m[0] != worst[1]]) for t, meas in sets]
        trial = [s for s in trial if len(s[1]) >= min_sats]
        if len(trial) < 3:
            break
        sets = trial
        dropped.append(worst[1])
        rx, used, rms, dt = solve_joint_t(sets, eph, rx, height=height)
    return rx, used, rms, dt, dropped


def gauss(A, v):
    n = len(v)
    A = [row[:] for row in A]
    v = v[:]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(A[r][i]))
        A[i], A[p], v[i], v[p] = A[p], A[i], v[p], v[i]
        if not A[i][i]:
            continue
        for r in range(n):
            if r != i and A[r][i]:
                f = A[r][i] / A[i][i]
                A[r] = [A[r][c] - f * A[i][c] for c in range(n)]
                v[r] -= f * v[i]
    return [v[i] / A[i][i] if A[i][i] else 0.0 for i in range(n)]


def lstsq(H, z):
    n = len(H[0])
    A = [[sum(H[k][i] * H[k][j] for k in range(len(H))) for j in range(n)] for i in range(n)]
    v = [sum(H[k][i] * z[k] for k in range(len(H))) for i in range(n)]
    for i in range(n):                              # Gauss-Jordan
        p = max(range(i, n), key=lambda r: abs(A[r][i]))
        A[i], A[p], v[i], v[p] = A[p], A[i], v[p], v[i]
        for r in range(n):
            if r != i and A[i][i]:
                f = A[r][i] / A[i][i]
                A[r] = [A[r][c] - f * A[i][c] for c in range(n)]
                v[r] -= f * v[i]
    return [v[i] / A[i][i] if A[i][i] else 0.0 for i in range(n)]


def batches(path, min_cn0):
    """{batch key: (ms, [(prn, transmit time mod 1 ms in m)])} from the probe log"""
    out = collections.OrderedDict()
    for line in open(path):
        f = line.split()
        if len(f) != 4 or f[1] != "29":
            continue
        p = bytes.fromhex(f[3])
        r = mm.record(p)
        if not r or r["sat"][0] != "G" or p[5] != 0xA0 or r["cn0"] < min_cn0:
            continue
        ms = int(f[0])
        key = (p[20:27], ms // 400)
        ms0, l = out.setdefault(key, (ms, []))
        if r["sat"][1] not in [x[0] for x in l]:
            l.append((r["sat"][1], r["codephase"] * CHIP))
    return out


def identify(sat, gnss, t, rx, lat, lon):
    """the ephemeris key of a record's satellite ("G", 5), ("R", 13)...: a
    GLONASS record names a frequency channel (k + 8) and a BeiDou one its
    PRN modulo 32, so the satellite above the horizon is taken; None if
    there's none"""
    sy, n = sat
    if sy == "G":
        cands = [("G", n)]
    elif sy == "C":
        cands = [("C", n), ("C", n + 32)]
    else:
        cands = [k for k, l in gnss.items() if k[0] == "R" and l[0]["k"] == n - 8]
    best = None
    for k in cands:
        if k not in gnss:
            continue
        _, el = ge.look_any(ge.nearest(gnss[k], t), t, rx, lat, lon)
        if el > 0 and (best is None or el > best[0]):
            best = (el, k)
    return best[1] if best else None


def batches_gnss(path, min_cn0, gnss, t_start, rx, lat, lon, systems="GRC", doppler=False):
    """as batches(), for GPS, GLONASS and BeiDou: {batch key: (ms, [(key,
    transmit time mod 1 ms in m)])}, keys from identify(). Each batch is one
    constellation's, with its own clock bias in the solution, so the offsets
    between the systems' times and receiver paths need no unknowns of
    their own."""
    out = collections.OrderedDict()
    ident = {}
    for line in open(path):
        f = line.split()
        if len(f) != 4 or f[1] != "29":
            continue
        p = bytes.fromhex(f[3])
        r = mm.record(p)
        if not r or r["sat"][0] not in systems or p[5] != 0xA0 or r["cn0"] < min_cn0:
            continue
        ms = int(f[0])
        # which satellite: once a minute is enough
        ik = (r["sat"], ms // 60000)
        if ik not in ident:
            ident[ik] = identify(r["sat"], gnss, t_start + ms / 1000, rx, lat, lon)
        key = ident[ik]
        if key is None:
            continue
        bk = (r["sat"][0], p[20:27], ms // 400)
        ms0, l = out.setdefault(bk, (ms, []))
        if key not in [x[0] for x in l]:
            m = r["codephase"] / r["chips"] * MS
            l.append((key, m, r["doppler"], MS / r["chips"]) if doppler else (key, m))
    return out


def enu(d, lat, lon):
    la, lo = math.radians(lat), math.radians(lon)
    e = -math.sin(lo) * d[0] + math.cos(lo) * d[1]
    n = -math.sin(la) * math.cos(lo) * d[0] - math.sin(la) * math.sin(lo) * d[1] + math.cos(la) * d[2]
    u = math.cos(la) * math.cos(lo) * d[0] + math.cos(la) * math.sin(lo) * d[1] + math.sin(la) * d[2]
    return e, n, u


def main():
    if len(sys.argv) < 7:
        sys.exit(__doc__)
    log, brdc, start = sys.argv[1:4]
    lat, lon, h = map(float, sys.argv[4:7])
    truth = None
    if "--truth" in sys.argv:
        i = sys.argv.index("--truth")
        truth = tuple(map(float, sys.argv[i + 1:i + 4]))
    min_cn0 = 20
    eph = ge.load(brdc)
    t_start = ge.gps_seconds(datetime.datetime.fromisoformat(start).replace(tzinfo=datetime.timezone.utc))
    rx0 = ge.ecef(lat, lon, h)
    fixes = []
    for key, (ms, meas) in batches(log, min_cn0).items():
        meas = [m for m in meas if m[0] in eph]
        if len(meas) < 5:
            continue
        rx, b, dt, res = solve(meas, eph, t_start + ms / 1000, rx0)
        rms = math.sqrt(sum(x * x for x in res) / len(res))
        fixes.append((ms, rx, len(meas), rms, dt))
    if not fixes:
        sys.exit("no batch with five satellites or more")
    for ms, rx, n, rms, dt in fixes:
        line = "%6.1f s  %d sats  residual rms %6.0f m  time error %+.3f s" % (ms / 1000, n, rms, dt)
        if truth:
            e, nn, u = enu([rx[k] - ge.ecef(*truth)[k] for k in range(3)], truth[0], truth[1])
            line += "  off by %5.0f m horizontal, %+6.0f m up" % (math.hypot(e, nn), u)
        print(line)
    good = [f for f in fixes if f[3] < 150]
    if good:
        med = [sorted(f[1][k] for f in good)[len(good) // 2] for k in range(3)]
        line = "median of %d fixes (residual rms < 150 m)" % len(good)
        if truth:
            e, nn, u = enu([med[k] - ge.ecef(*truth)[k] for k in range(3)], truth[0], truth[1])
            line += ": off by %.0f m horizontal, %+.0f m up" % (math.hypot(e, nn), u)
        print(line)
    # all batches at once, starting from the median (or the a-priori position)
    sets = [(t_start + ms / 1000, [m for m in meas if m[0] in eph])
            for ms, meas in batches(log, min_cn0).values()]
    sets = [s for s in sets if len(s[1]) >= 5]
    rx, used, rms = solve_joint(sets, eph, med if good else rx0)
    lat_, lon_ = ecef_latlon(rx)
    line = "joint fix over %d batches (residual rms %.0f m)" % (used, rms)
    if truth:
        e, nn, u = enu([rx[k] - ge.ecef(*truth)[k] for k in range(3)], truth[0], truth[1])
        line += ": off by %.0f m horizontal, %+.0f m up" % (math.hypot(e, nn), u)
    else:
        line += ": %.5f %.5f" % (lat_, lon_)
    print(line)
    # one time error for all batches; four satellites are enough then
    sets4 = [(t_start + ms / 1000, [m for m in meas if m[0] in eph])
             for ms, meas in batches(log, min_cn0).values()]
    sets4 = [s for s in sets4 if len(s[1]) >= 4]
    rx0j = rx
    rx, used, rms, dt = solve_joint_t(sets4, eph, rx0j)
    lat_, lon_ = ecef_latlon(rx)
    line = "joint fix, one time error (%+.3f s), over %d batches (residual rms %.0f m)" % (dt, used, rms)
    if truth:
        e, nn, u = enu([rx[k] - ge.ecef(*truth)[k] for k in range(3)], truth[0], truth[1])
        line += ": off by %.0f m horizontal, %+.0f m up" % (math.hypot(e, nn), u)
    else:
        line += ": %.5f %.5f" % (lat_, lon_)
    print(line)
    bias = sat_bias(sets4, eph, rx, dt)
    print("  steady bias per satellite: " + ", ".join(
        "G%d %+.0f m" % (p, b) for p, (b, n) in sorted(bias.items(), key=lambda kv: -abs(kv[1][0]))))
    rx, used, rms, dt, dropped = solve_robust(sets4, eph, rx0j)
    lat_, lon_ = ecef_latlon(rx)
    line = "robust fix, without %s, over %d batches (residual rms %.0f m)" % (
        " ".join("G%d" % p for p in dropped) or "none", used, rms)
    if truth:
        e, nn, u = enu([rx[k] - ge.ecef(*truth)[k] for k in range(3)], truth[0], truth[1])
        line += ": off by %.0f m horizontal, %+.0f m up" % (math.hypot(e, nn), u)
    else:
        line += ": %.5f %.5f" % (lat_, lon_)
    print(line)


def ecef_latlon(r):
    a, f = 6378137.0, 1 / 298.257223563
    e2 = f * (2 - f)
    lon = math.atan2(r[1], r[0])
    p = math.hypot(r[0], r[1])
    lat = math.atan2(r[2], p * (1 - e2))
    for _ in range(5):
        n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        lat = math.atan2(r[2] + e2 * n * math.sin(lat), p)
    return math.degrees(lat), math.degrees(lon)


if __name__ == "__main__":
    main()
