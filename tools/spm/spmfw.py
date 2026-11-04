#!/usr/bin/env python3
"""MT6771 SPM firmware (the spmfw partition): its container, its event
vectors, the PCM instruction set, and the program read back as routines
and conditions (docs/spm-firmware.md). Reads the vendor headers for
register, flag and bit names.

Usage: spmfw.py IMAGE [--build N] [--dis START END] [--annotate]
                 [--routines] [--flags] [--effects] [--debug]
  IMAGE       spmfw partition image (Firmware/spmfw-verified.img)
  --build     which pcm_allinone build (0 lp3_1866, 1 lp4_3200, 2 lp4_3733,
              3 lp4_2400; default 2, the one this phone runs: LPDDR4X at
              3600, the vendor kernel's index 0)
  --dis       plain disassembly of a range (word addresses, hex)
  --annotate  the whole program: routines, conditions in flag names
  --routines  one line per routine: what it calls, writes and tests
  --flags     every flag and status bit a branch tests, with the branches
  --effects   each flag-gated forward skip and what it skips
  --debug     where each SPM_SW_DEBUG bit is set
The listings are derived from a vendor image: keep them out of git.
"""
import argparse
import collections
import os
import re
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
VENDOR = os.path.join(HERE, "../../titan_kernel_ref/drivers/misc/mediatek/"
                      "base/power/include/spm_v4")

# blocks outside the SPM the program touches (vendor mt6771.dts)
REGIONS = [
    (0x0c530000, 0x2000, "MCUCFG"),
    (0x10012000, 0x1000, "DVFSRC"),
    (0x10017000, 0x1000, "SYS_TIMER"),
    (0x10219000, 0x1000, "EMI"),
    (0x10228000, 0x2000, "DDRPHY_AO_A"),
    (0x1022a000, 0x2000, "DRAMC_AO_A"),
    (0x1022c000, 0x2000, "DRAMC_NAO_A"),
    (0x10230000, 0x2000, "DDRPHY_AO_B"),
    (0x10232000, 0x2000, "DRAMC_AO_B"),
    (0x10234000, 0x2000, "DRAMC_NAO_B"),
]

# The routines of the 3733 build, named from what they do
# (docs/spm-firmware.md, "Routines")
NAMES = {
    0x000: "ev0_26m_wake",
    0x059: "ev1_26m_sleep",
    0x0c0: "ev2_infra_wake",
    0x0db: "ev3_infra_sleep",
    0x0f4: "ev4_apsrc_wake",
    0x139: "ev5_apsrc_sleep",
    0x1af: "ev8_dvfs",
    0x270: "ev6_vrf18_wake",
    0x28b: "ev7_vrf18_sleep",
    0x2a3: "ev12_ddren_wake",
    0x2ae: "ev13_ddren_sleep",
    0x2ee: "ddren_rearm",
    0x305: "pwr_con_on",
    0x318: "pwr_con_off",
    0x31e: "ddrphy_off",
    0x333: "ddrphy_on",
    0x357: "dram_phy_sleep",
    0x362: "dram_phy_wake",
    0x37e: "emi_clk_off",
    0x39a: "bus_pause",
    0x39f: "ulposc_on",
    0x3be: "ulposc_off",
    0x3d5: "pwrap_clk_sel",
    0x3e5: "pwrap_sleep",
    0x408: "debug_or",
    0x410: "dvfs_ack_wait",
    0x420: "drs_release",
    0x425: "tx_tracking_off",
    0x428: "tx_tracking_on",
    0x42f: "dram_mr4_read",
    0x489: "dramc_13fc_set",
    0x49b: "dramc_13fc_clr",
    0x4ad: "dramc_22c_clr",
    0x4bb: "dramc_22c_set",
    0x4cf: "dqsosc_save",
    0x52c: "dfs_target",
    0x540: "drs_hold",
    0x54d: "dvfs_requests",
    0x571: "dvfs_done",
    0x595: "dfs_prepare",
    0x5d1: "dfs_shuffle",
    0x76d: "dummy_read_on",
    0x77f: "dummy_read_off",
    0x790: "dvfs_cmd",
    0x79a: "dvfsrc_ack_chk",
    0x800: "main",
    0xba3: "armpll_clk_off",
}


def load_names():
    """SPM register names by address; bit names of r0, r7, r12, r13, the
    two flag words and the two debug words, from the vendor headers"""
    regs, bits = {}, collections.defaultdict(dict)

    def read(f):
        try:
            return open(os.path.join(VENDOR, f)).read()
        except OSError:
            return ""
    for n, o in re.findall(r"#define\s+(\w+)\s+\(SPM_BASE \+ 0x([0-9A-Fa-f]+)\)",
                           read("mtk_spm_reg_mt6771.h")):
        regs[0x10006000 + int(o, 16)] = n
    for f in ("pcm_def_mt6771.h", "sleep_def_mt6771.h"):
        h = read(f)
        for n, r, b in re.findall(r"#define\s+(R(0|7|12|13)_\w+)\s+\(1U << (\d+)\)", h):
            bits["r" + r][int(b)] = n
        for n, b in re.findall(r"#define\s+(SPM_FLAG_\w+)\s+\(1U << (\d+)\)", h):
            bits["SPM_SW_FLAG"][int(b)] = n
        for n, b in re.findall(r"#define\s+(SPM_FLAG1_\w+)\s+\(1U << (\d+)\)", h):
            bits["SPM_SW_RSV_2"][int(b)] = n
        for n, b in re.findall(r"#define\s+SPM_DBG_(?:DEBUG_IDX_)?(\w+)\s+\(1U << (\d+)\)", h):
            bits["SPM_SW_DEBUG"][int(b)] = n
        for n, b in re.findall(r"#define\s+SPM_DBG1_(\w+)\s+\(1U << (\d+)\)", h):
            bits["WDT_LATCH_SPARE0_FIX"][int(b)] = n
    # r6 is the firmware's copy of SPM_SW_FLAG; bits 27-31 its resource
    # state (1 = awake), set at entry and kept by the ladder's handlers
    bits["r6"] = dict(bits["SPM_SW_FLAG"])
    bits["r6"].update({27: "ST_26M", 28: "ST_INFRA", 29: "ST_APSRC",
                       30: "ST_VRF18", 31: "ST_DDREN"})
    return regs, bits


def builds(img):
    """the pcm_allinone records: (name, code words, size, session, vectors)"""
    out = []
    o = img.find(b"2MPS")
    while o >= 0:
        name = img[o + 8:o + 0x42].split(b"\0")[0].decode()
        size = struct.unpack_from("<H", img, o + 0x42)[0]
        code = struct.unpack_from("<%dI" % size, img, o + 0x44)
        d = o + 0x44 + 4 * size
        dsize, sess, rep, a2 = struct.unpack_from("<HBBH", img, d)
        vec = struct.unpack_from("<16I", img, d + 8)
        ver = img[d + 0x48:d + 0x68].split(b"\0")[0].decode()
        out.append(dict(name=name, code=code, sess=sess, vec=vec, ver=ver))
        o = img.find(b"2MPS", o + 4)
    return out


class Ins:
    """One instruction. Fields: op 31:28, imm 27 (the second operand is the
    next word), rd 26:22, inv 21, left 20, sh 19:15, rs2 14:10, rs3 9:5,
    rs1 4:0. Flow ops use 20:5 as the target."""

    def __init__(self, pc, w, imm=None):
        self.pc, self.w, self.imm = pc, w, imm
        self.op, self.i = w >> 28, w >> 27 & 1
        self.rd, self.inv, self.left = w >> 22 & 0x1f, w >> 21 & 1, w >> 20 & 1
        self.sh, self.rs2, self.rs3, self.rs1 = w >> 15 & 0x1f, w >> 10 & 0x1f, w >> 5 & 0x1f, w & 0x1f
        self.target = w >> 5 & 0xffff

    def size(self):
        return 3 if isinstance(self.imm, tuple) else 2 if self.imm is not None else 1

    def is_mov(self):
        """op 1 with the immediate: rd <- #imm, or [imm] with left (load)"""
        return self.op == 1 and self.i and not self.inv

    def is_load(self):
        return self.op == 1 and self.i and self.left

    def is_call(self):
        return self.op == 0xc

    def is_flow(self):
        return self.op in (0xc, 0xd, 0xf)

    def writes(self):
        """the register this instruction writes, if any"""
        return None if self.op >= 0xc else self.rd


def takes_imm(w):
    op, i, inv = w >> 28, w >> 27 & 1, w >> 21 & 1
    if not i:
        return 0
    if op == 0xe:
        return 2                    # e8: address and value follow
    if op == 1:
        return 0 if inv else 1      # with inv: rd <- [rs1], no word
    return 1 if op in (0, 6, 8, 0xa) else 0


def decode(code):
    pc, out = 0, []
    while pc < len(code):
        w = code[pc]
        n = takes_imm(w)
        if n == 2 and pc + 2 < len(code):
            out.append(Ins(pc, w, (code[pc + 1], code[pc + 2])))
        elif n == 1 and pc + 1 < len(code):
            out.append(Ins(pc, w, code[pc + 1]))
        else:
            n = 0
            out.append(Ins(pc, w))
        pc += 1 + n
    return out


class Names:
    def __init__(self, regs, bits):
        self.regs, self.bits = regs, bits
        self.names = NAMES

    def reg(self, a):
        if a in self.regs:
            return self.regs[a]
        for base, size, n in REGIONS:
            if base <= a < base + size:
                return "%s+%03x" % (n, a - base)
        return "%08x" % a

    def bit(self, src, b):
        n = self.bits.get(src, {}).get(b)
        return n if n else "%s bit %d" % (src, b)

    def routine(self, pc):
        return self.names.get(pc, "sub_%03x" % pc)


# ALU: op -> (symbol, symbol with inv). 4 compares in a direction the
# code does not settle; 3 and 9 fit XOR in each use (the two 9s toggle
# a bit with inv set, so inv is not a complement there).
ALU = {0: ("+", "-"), 1: ("+", "+"), 3: ("^", "^"), 4: ("<?", "<?"),
       6: ("==", "=="), 7: ("!=", "!="), 8: ("&", "& ~"), 9: ("^", "^"),
       0xa: ("|", "|"), 0xb: ("|", "|")}


def operand2(x):
    if x.i:
        return "#%08x" % x.imm if isinstance(x.imm, int) else "#?"
    if x.rs2 == 31:
        return "0"
    if x.rs2 == 1 and x.left:
        return "1 << %d" % x.sh
    if x.sh:
        return "r%d %s %d" % (x.rs2, "<<" if x.left else ">>", x.sh)
    return "r%d" % x.rs2


def fmt(x, nm):
    w = x.w
    if w == 0x17c07c1f:
        return "nop"
    if x.op == 0xf:
        return "return"
    if x.op == 0xc:
        t = nm.routine(x.target)
        if x.i:
            return "call %s if r%d %s" % (t, x.rs1, "clear" if x.inv else "set")
        return "call %s" % t
    if x.op == 0xd:
        if x.i:
            return "if r%d %s -> %03x" % (x.rs1, "clear" if x.inv else "set", x.target)
        return "jump %03x" % x.target
    if x.op == 0xe:
        if isinstance(x.imm, tuple):
            return "[%s] <- #%08x" % (nm.reg(x.imm[0]), x.imm[1])
        return "[r%d] <- %s" % (x.rd, "#%03x" % (w & 0x3ff) if x.inv else "r%d" % x.rs1)
    if x.op == 1 and x.i:
        if x.inv:
            return "r%d <- [r%d]" % (x.rd, x.rs1)
        if x.left:
            return "r%d <- [%s]" % (x.rd, nm.reg(x.imm))
        a = nm.reg(x.imm)
        return "r%d <- #%08x" % (x.rd, x.imm) + ("  (&%s)" % a if a != "%08x" % x.imm else "")
    if x.op not in ALU:
        return "?"
    if x.op == 8 and not x.i and x.rs1 == 1 and not x.left:
        return "r%d <- %sbit %d of r%d" % (x.rd, "!" if x.inv else "", x.sh, x.rs2)
    o = ALU[x.op][x.inv]
    b = operand2(x)
    if x.op == 0xb:
        # rd <- rs3 | (bit test of rs2)
        return "r%d <- r%d | %sbit %d of r%d" % (x.rd, x.rs3, "!" if x.inv else "", x.sh, x.rs2)
    if x.op in (0, 1) and x.rs1 == 31:
        e = b if not (x.op == 0 and x.inv) else "-" + b
    elif x.op in (0, 1) and b == "0":
        e = "r%d" % x.rs1
    elif x.op == 8 and x.inv:
        e = "r%d & ~(%s)" % (x.rs1, b)
    else:
        e = "r%d %s %s" % (x.rs1, o, b)
    note = ""
    if x.rd == x.rs1 and x.rs2 == 1 and x.left and x.op in (8, 0xa) and x.rd in (0, 7):
        note = "  (%s)" % nm.bit("r%d" % x.rd, x.sh)
    return "r%d <- %s%s" % (x.rd, e, note)


class Prog:
    def __init__(self, b, regs, bits):
        self.b, self.code = b, b["code"]
        self.ins = decode(self.code)
        self.at = {x.pc: k for k, x in enumerate(self.ins)}
        self.nm = Names(regs, bits)
        self.bits = bits
        # NAMES are the 3733 build's addresses; the other builds have the
        # same routines in the same order, shifted
        es = self.entries()
        if len(es) == len(NAMES):
            self.nm.names = {e: NAMES[n] for e, n in zip(es, sorted(NAMES))}

    # -- what a register holds at an instruction, as text and as terms

    def writer(self, k, r):
        # r12 is multiplexed by r7's WAKEUP_EXT_W_SEL/R_SEL: a write sets
        # the armed events (or clears the latches), a read returns the
        # live wake status, so a read never sees the last write
        if r in (0, 6, 7, 12, 13):
            # r13: live resource and ack status; r0, r6, r7 keep their
            # meaning across calls (outputs, flags)
            return None
        for j in range(k - 1, max(-1, k - 12), -1):
            x = self.ins[j]
            if x.writes() == r:
                return j
            if x.op == 0xf or x.is_call():
                # a call may return any register; a delay slot runs first
                return ("call", x.target) if x.is_call() and j != k - 1 else None
        return None

    def source(self, k, r):
        """a name for register r before instruction k: the register it was
        loaded from, or the r0/r6/r7/r12/r13 name"""
        j = self.writer(k, r)
        if isinstance(j, tuple):
            return "r%d from %s" % (r, self.nm.routine(j[1]))
        if j is not None and self.ins[j].is_load() and self.ins[j].imm is not None:
            return self.nm.reg(self.ins[j].imm)
        if j is not None and self.ins[j].op == 1 and self.ins[j].i and self.ins[j].inv:
            return "[r%d]" % self.ins[j].rs1
        return "r%d" % r

    def terms(self, k, r, depth=0):
        """r before instruction k as OR-ed terms: (source, bit, negated) for
        bit tests, or a text"""
        j = self.writer(k, r)
        if j is None or isinstance(j, tuple) or depth > 4:
            return [self.source(k, r)]
        x = self.ins[j]
        if x.op == 8 and not x.i and x.rs1 == 1 and not x.left:
            return [(self.source(j, x.rs2), x.sh, bool(x.inv))]
        if x.op == 8 and not x.i and x.rs2 == 1 and x.left and not x.inv:
            return [(self.source(j, x.rs1), x.sh, False)]
        if x.op == 0xb:
            return [(self.source(j, x.rs2), x.sh, bool(x.inv))] + self.terms(j, x.rs3, depth + 1)
        if x.op == 0xa and not x.i and not x.sh and x.rs1 != 1 and x.rs2 != 1:
            return self.terms(j, x.rs1, depth + 1) + self.terms(j, x.rs2, depth + 1)
        if x.op in (6, 7, 4):
            b = (self.nm.regs.get(x.imm, "#%x" % x.imm)) if x.i else self.text(j, x.rs2) + (
                " << %d" % x.sh if x.sh and x.left else " >> %d" % x.sh if x.sh else "")
            return ["%s %s %s" % (self.text(j, x.rs1), ALU[x.op][0], b)]
        if x.op == 8 and x.i:
            return ["%s & #%x" % (self.text(j, x.rs1), x.imm)]
        return ["r%d (%s)" % (r, fmt(x, self.nm))]

    def text(self, k, r):
        j = self.writer(k, r)
        if isinstance(j, int) and self.ins[j].is_mov() and not self.ins[j].left:
            return "#%x" % self.ins[j].imm
        return self.source(k, r)

    def term_text(self, t):
        if isinstance(t, str):
            return t
        src, b, neg = t
        key = src if src in self.bits else src
        n = self.bits.get(key, {}).get(b)
        s = n if n else "%s bit %d" % (src, b)
        return ("!" if neg else "") + s

    def cond(self, k):
        """a conditional flow instruction's condition as text"""
        x = self.ins[k]
        ts = [self.term_text(t) for t in self.terms(k, x.rs1)]
        e = " or ".join(ts)
        if x.inv:
            return ("not (%s)" % e) if len(ts) > 1 else (
                e[1:] if e.startswith("!") else "!" + e)
        return e

    # -- routines

    def entries(self):
        e = {0x800}
        for v in self.b["vec"]:
            if v:
                e.add(v >> 16)
        for x in self.ins:
            if x.is_call():
                e.add(x.target)
        return sorted(e)

    def routines(self):
        es = self.entries()
        return [(s, es[i + 1] if i + 1 < len(es) else len(self.code)) for i, s in enumerate(es)]

    def debug_marks(self):
        """SPM_SW_DEBUG bits set, by pc: calls to debug_or (which ORs r10
        into SPM_SW_DEBUG) with r10 set in the delay slot"""
        out = {}
        for k, x in enumerate(self.ins):
            if not x.is_call() or self.code[x.target + 1:x.target + 2] != (0x10006604,):
                continue
            d = self.ins[k + 1]
            if d.rd == 10 and d.op == 1:
                if d.rs2 == 1:
                    out[x.pc] = self.nm.bit("SPM_SW_DEBUG", d.sh)
                else:
                    out[x.pc] = "%s if r%d" % (self.nm.bit("SPM_SW_DEBUG", d.sh), d.rs2)
        return out

    def effects(self):
        """by pc: register writes (e8 stores; e0 stores through a register
        that last took an address) and calls"""
        out, const = {}, {}
        for x in self.ins:
            if x.is_mov():
                if x.left:
                    const.pop(x.rd, None)
                else:
                    const[x.rd] = x.imm
            elif x.op == 0xe and isinstance(x.imm, tuple):
                out[x.pc] = "%s=%x" % (self.nm.reg(x.imm[0]), x.imm[1])
            elif x.op == 0xe and x.rd in const:
                out[x.pc] = self.nm.reg(const[x.rd]) + (
                    "=%x" % (x.w & 0x3ff) if x.inv else "")
            elif x.is_call():
                out[x.pc] = "call " + self.nm.routine(x.target)
            elif x.writes() is not None and x.writes() in const:
                const.pop(x.writes())
        return out

    def summary(self, s, e):
        """what a routine does: calls, writes, r0/r7 bits, tests, waits"""
        eff = self.effects()
        calls, writes, bits, tests, loops = [], [], [], [], 0
        for k, x in enumerate(self.ins):
            if not s <= x.pc < e:
                continue
            ef = eff.get(x.pc)
            if ef and ef.startswith("call "):
                if ef[5:] not in calls:
                    calls.append(ef[5:])
            elif ef and ef not in writes:
                writes.append(ef)
            if x.op in (8, 0xa) and x.rd in (0, 7) and x.rd == x.rs1 and x.rs2 == 1 and x.left:
                bits.append(("-" if x.op == 8 else "+") + self.nm.bit("r%d" % x.rd, x.sh).split("_", 1)[1])
            if x.op in (0xc, 0xd) and x.i:
                c = self.cond(k)
                if c not in tests:
                    tests.append(c)
                if x.target <= x.pc and x.op == 0xd:
                    loops += 1
        return calls, writes, bits, tests, loops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--build", type=int, default=2)
    ap.add_argument("--dis", nargs=2)
    ap.add_argument("--annotate", action="store_true")
    ap.add_argument("--routines", action="store_true")
    ap.add_argument("--flags", action="store_true")
    ap.add_argument("--effects", action="store_true")
    ap.add_argument("--debug", action="store_true")
    a = ap.parse_args()
    img = open(a.image, "rb").read()
    regs, bits = load_names()
    bl = builds(img)
    for i, b in enumerate(bl):
        print("build %d: %s, %d words, session %d, %s" % (i, b["name"], len(b["code"]), b["sess"], b["ver"]))
    b = bl[a.build]
    p = Prog(b, regs, bits)
    nm = p.nm
    print("event vectors (%s):" % b["name"])
    for i, v in enumerate(b["vec"]):
        if v:
            print("  vec%-2d event %2d pc %03x%s" % (i, v & 0x1f, v >> 16,
                                                  " resume" if v & 0x20 else ""))
    undecoded = [x.pc for x in p.ins if fmt(x, nm) == "?"]
    print("%d instructions, %d undecoded" % (len(p.ins), len(undecoded)))
    marks = p.debug_marks()
    if a.dis or a.annotate:
        s, e = (int(a.dis[0], 16), int(a.dis[1], 16)) if a.dis else (0, len(p.code))
        starts = dict(p.routines())
        for k, x in enumerate(p.ins):
            if not s <= x.pc < e:
                continue
            if a.annotate and x.pc in starts:
                print("\n%03x-%03x %s" % (x.pc, starts[x.pc] - 1, nm.routine(x.pc)))
            im = (" ".join("%08x" % v for v in x.imm) if isinstance(x.imm, tuple)
                  else "%08x" % x.imm if x.imm is not None else "")
            t = fmt(x, nm)
            if a.annotate and x.op in (0xc, 0xd) and x.i:
                t += "    ; " + p.cond(k)
            if x.pc in marks:
                t += "    ; debug " + marks[x.pc]
            if a.annotate and t == "nop":
                continue
            print("%03x %08x %-17s %s" % (x.pc, x.w, im, t))
    if a.routines:
        for s, e in p.routines():
            calls, writes, rbits, tests, loops = p.summary(s, e)
            print("%03x-%03x %s" % (s, e - 1, nm.routine(s)))
            for k, v in (("calls", calls), ("writes", writes), ("bits", rbits), ("tests", tests)):
                if v:
                    print("    %-6s %s" % (k, ", ".join(v)))
            if loops:
                print("    waits  %d loop(s)" % loops)
    if a.debug:
        for pc, name in sorted(marks.items()):
            r = [s for s, e in p.routines() if s <= pc < e]
            print("%03x %-34s %s" % (pc, name, nm.routine(r[0]) if r else ""))
    if a.flags or a.effects:
        eff = p.effects()
        by = collections.defaultdict(list)
        for k, x in enumerate(p.ins):
            if x.op not in (0xc, 0xd) or not x.i:
                continue
            for t in p.terms(k, x.rs1):
                if not isinstance(t, str):
                    by[(t[0], t[1])].append("%03x" % x.pc)
            named = any(not isinstance(t, str) and p.bits.get(t[0], {}).get(t[1])
                        for t in p.terms(k, x.rs1))
            if a.effects and x.op == 0xd and x.target > x.pc and named:
                touched = collections.OrderedDict(
                    (v, 1) for pc, v in sorted(eff.items()) if x.pc + 1 < pc < x.target)
                print("%03x if %s: skip to %03x (%d words)%s" % (
                    x.pc, p.cond(k), x.target, x.target - x.pc - 2,
                    (": " + ", ".join(list(touched)[:8])) if touched else ""))
        if a.flags:
            for (src, bit), pcs in sorted(by.items(), key=lambda kv: (kv[0][0], kv[0][1])):
                print("%-26s bit %2d %-40s %s" % (src, bit, p.bits.get(src, {}).get(bit, ""),
                                                   " ".join(pcs)))


if __name__ == "__main__":
    main()
