#!/usr/bin/env python3
"""
emu32.py: load 32-bit ARM Android shared libraries into one Unicorn address
space and call into them (the 32-bit sibling of emu_hal.py), for vendor code
that only ships as armeabi, such as MediaTek's video encoder driver
(libvcodecdrv.so, docs/video-encoder.md).

Each library is mapped at its own base with its REL (plain or APS2-packed),
RELR and PLT relocations applied; imports resolve across the loaded
libraries by name, then to Python shims, and anything else to a stub that
returns 0. Functions are entered in ARM or Thumb state from the address's
bit 0, as the ABI does.

    emu = Emu32()
    emu.load(path); emu.start()
    emu.call(emu.sym("..."), args...)
"""
import struct

from collections import deque

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_BLOCK,
                     UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, UcError)
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
                               UC_ARM_REG_C13_C0_3, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC)

PAGE = 0x1000
R = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
LIB_BASE = 0x10000000           # first library; each next one 16 MB higher
STACK, STACK_SIZE = 0x7f000000, 0x800000
TLS = 0x7e000000
RET = 0x7d000000
HOOKS = 0x7c000000              # one slot per import
DATA = 0x7b000000               # unknown data imports
HEAP, HEAP_SIZE = 0x80000000, 0x40000000
IO = 0x70000000                 # fake register windows

R_ABS32, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 2, 21, 22, 23
BX_LR = struct.pack("<I", 0xe12fff1e)


def align_up(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


class Lib:
    def __init__(self, path, base):
        self.path, self.base = path, base
        self.d = d = open(path, "rb").read()
        assert d[4] == 1 and struct.unpack_from("<H", d, 0x12)[0] == 40, "not ELF32 ARM"
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2e)
        sh = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
        names = sh[shstrndx]
        def nm(o):
            s = names[4] + o
            return d[s:d.index(b"\0", s)].decode()
        self.sec = {nm(s[0]): s for s in sh}
        self.syms = self._dynsym()
        self.exports = {n: base + v for n, v, shndx in self.syms if shndx != 0 and v}

    def section(self, name):
        s = self.sec.get(name)
        return (s[4], s[5], s[3]) if s else (0, 0, 0)       # file offset, size, vaddr

    def _dynsym(self):
        off, size, _ = self.section(".dynsym")
        stro, _, _ = self.section(".dynstr")
        out = []
        for i in range(size // 16):
            name, value, sz, info, other, shndx = struct.unpack_from("<IIIBBH", self.d, off + 16 * i)
            s = stro + name
            out.append((self.d[s:self.d.index(b"\0", s)].decode(), value, shndx))
        return out

    def rels(self):
        """(offset, info) from .rel.dyn (plain or APS2) and .rel.plt"""
        for name in (".rel.dyn", ".rel.plt"):
            off, size, _ = self.section(name)
            if not size:
                continue
            if self.d[off:off + 4] == b"APS2":
                yield from self._aps2(off + 4, off + size)
            else:
                for i in range(size // 8):
                    yield struct.unpack_from("<II", self.d, off + 8 * i)

    def _aps2(self, p, end):
        d = self.d
        def sleb():
            nonlocal p
            v = shift = 0
            while True:
                b = d[p]
                p += 1
                v |= (b & 0x7f) << shift
                shift += 7
                if not b & 0x80:
                    if b & 0x40:
                        v -= 1 << shift
                    return v
        count, r_offset = sleb(), sleb()
        r_info, done, odelta = 0, 0, 0
        while done < count:
            gsize, flags = sleb(), sleb()
            if flags & 2:
                odelta = sleb()
            if flags & 1:
                r_info = sleb()
            if flags & 4 and flags & 8:
                sleb()                          # addend (REL: none expected)
            for _ in range(gsize):
                r_offset += odelta if flags & 2 else sleb()
                if not flags & 1:
                    r_info = sleb()
                if flags & 8 and not flags & 4:
                    sleb()
                yield (r_offset & 0xffffffff, r_info & 0xffffffff)
            done += gsize


class Emu32:
    def __init__(self, verbose=False):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.verbose = verbose
        self.libs, self.imports, self.hook_by_name = [], {}, {}
        self.counts, self.stub_seen, self.shims = {}, set(), {}
        self.next_hook, self.next_data, self.heap = HOOKS, DATA, HEAP
        self.next_base = LIB_BASE
        self.io = {}                # base -> (size, name, regs dict)
        self.io_log = []            # (op, name, offset, value, caller)
        for base, size in ((STACK - STACK_SIZE, STACK_SIZE + PAGE), (TLS - PAGE, 3 * PAGE),
                           (RET, PAGE), (HOOKS, 0x100000), (DATA, 0x400000),
                           (HEAP, HEAP_SIZE)):
            uc.mem_map(base, size)
        uc.mem_write(RET, BX_LR)
        # stack guard in TLS slot 5 (bionic), FPU on
        uc.mem_write(TLS + 5 * 4, struct.pack("<I", 0x12345678))
        uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        uc.hook_add(UC_HOOK_CODE, self._on_hook, begin=HOOKS, end=HOOKS + 0x100000)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_bad)
        install_shims(self)

    # ---- loading ----
    def load(self, path):
        lib = Lib(path, self.next_base)
        d = lib.d
        phoff, = struct.unpack_from("<I", d, 0x1c)
        phentsize, phnum = struct.unpack_from("<HH", d, 0x2a)
        top, mapped = 0, set()
        for i in range(phnum):
            typ, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from(
                "<8I", d, phoff + i * phentsize)
            if typ != 1:
                continue
            for p in range((lib.base + vaddr) & ~(PAGE - 1), align_up(lib.base + vaddr + memsz), PAGE):
                if p not in mapped:
                    self.uc.mem_map(p, PAGE)
                    mapped.add(p)
            self.uc.mem_write(lib.base + vaddr, d[off:off + filesz])
            top = max(top, vaddr + memsz)
        self.next_base = align_up(lib.base + top, 0x1000000)
        self.libs.append(lib)
        return lib

    def sym(self, name):
        for lib in self.libs:
            if name in lib.exports:
                return lib.exports[name]
        raise KeyError(name)

    def _hook(self, name):
        if name not in self.hook_by_name:
            a = self.next_hook
            self.next_hook += 4
            self.uc.mem_write(a, BX_LR)
            self.imports[a] = name
            self.hook_by_name[name] = a
        return self.hook_by_name[name]

    def link(self):
        for lib in self.libs:
            b = lib.base
            for r_off, info in lib.rels():
                typ, si = info & 0xff, info >> 8
                old, = struct.unpack("<I", self.uc.mem_read(b + r_off, 4))
                if typ == R_RELATIVE:
                    val = b + old
                elif typ in (R_GLOB_DAT, R_JUMP_SLOT, R_ABS32):
                    name = lib.syms[si][0].split("@")[0]
                    t = None if name in self.shims else next(
                        (l.exports[name] for l in self.libs if name in l.exports), None)
                    if t is not None:
                        val = t
                    elif typ == R_JUMP_SLOT or name in self.shims or name in FUNC_NAMES:
                        val = self._hook(name)
                    else:                       # unknown data object
                        val = self.next_data
                        self.next_data += 0x1000
                    if typ == R_ABS32:
                        val += old
                else:
                    continue
                self.uc.mem_write(b + r_off, struct.pack("<I", val & 0xffffffff))
            off, size, _ = lib.section(".relr.dyn")
            where = 0
            for i in range(size // 4):
                e, = struct.unpack_from("<I", lib.d, off + 4 * i)
                if e & 1 == 0:
                    where = e
                    self._relr(b, where)
                    where += 4
                else:
                    for k in range(31):
                        if (e >> 1) & (1 << k):
                            self._relr(b, where + 4 * k)
                    where += 4 * 31

    def _relr(self, base, off):
        v, = struct.unpack("<I", self.uc.mem_read(base + off, 4))
        self.uc.mem_write(base + off, struct.pack("<I", (v + base) & 0xffffffff))

    def start(self):
        self.link()
        for lib in self.libs:
            off, size, vaddr = lib.section(".init_array")
            for i in range(size // 4):
                fn = self.r32(lib.base + vaddr + 4 * i)
                if fn and fn != 0xffffffff:
                    try:
                        self.call(fn)
                    except RuntimeError as e:
                        print("init %s: %s" % (lib.path.split("/")[-1], e))

    # ---- fake register windows ----
    def add_io(self, size, name):
        base = IO + 0x100000 * len(self.io)
        self.uc.mem_map(base, align_up(size))
        regs = {}
        self.io[base] = (size, name, regs)
        def on_w(uc, access, addr, sz, value, user):
            self.io_log.append(("W", name, addr - base, value & 0xffffffff, self.where(self.lr())))
            regs[addr - base] = value & 0xffffffff
        def on_r(uc, access, addr, sz, value, user):
            v = self.io_read(name, addr - base, regs.get(addr - base, 0))
            self.uc.mem_write(addr, struct.pack("<I", v & 0xffffffff))
            self.io_log.append(("R", name, addr - base, v, self.where(self.lr())))
        self.uc.hook_add(UC_HOOK_MEM_WRITE, on_w, begin=base, end=base + size - 1)
        self.uc.hook_add(UC_HOOK_MEM_READ, on_r, begin=base, end=base + size - 1)
        return base

    def io_read(self, name, off, last):
        """override: value a register read returns (default: the last write)"""
        return last

    # ---- memory helpers ----
    def malloc(self, n, align=16):
        p = align_up(self.heap, align)
        self.heap = p + max(n, 1)
        if self.heap > HEAP + HEAP_SIZE:
            raise MemoryError("heap exhausted")
        self.uc.mem_write(p, b"\0" * max(n, 1))
        return p

    def r32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xffffffff))

    def cstr(self, a, limit=512):
        if not a:
            return b""
        try:
            b = bytes(self.uc.mem_read(a, limit))
        except UcError:
            b = bytes(self.uc.mem_read(a, 16))
        return b[:b.index(b"\0")] if b"\0" in b else b

    def new_cstr(self, s):
        b = s.encode() + b"\0"
        p = self.malloc(len(b))
        self.uc.mem_write(p, b)
        return p

    def lr(self):
        return self.uc.reg_read(UC_ARM_REG_LR)

    def where(self, addr):
        addr &= ~1
        for lib in self.libs:
            if lib.base <= addr < lib.base + 0x1000000:
                return "%s+%#x" % (lib.path.split("/")[-1], addr - lib.base)
        return "%#x" % addr

    def stack_arg(self, i):
        """argument i (0-based over all arguments, 4 and up live on the stack)"""
        return self.r32(self.uc.reg_read(UC_ARM_REG_SP) + 4 * (i - 4))

    # ---- execution ----
    lenient = False

    def trace_blocks(self, n=40):
        """keep the last n basic blocks; printed when an access faults"""
        self.blocks = deque(maxlen=n)
        self.uc.hook_add(UC_HOOK_BLOCK, lambda uc, a, s, u: self.blocks.append(a))

    def _on_bad(self, uc, access, address, size, value, user):
        if self.lenient and 0x1000 <= address < 0x70000000:
            try:
                uc.mem_map(address & ~(PAGE - 1), PAGE)
                return True
            except UcError:
                return True
        print("bad access %#x (size %d) at %s, lr %s" % (
            address, size, self.where(uc.reg_read(UC_ARM_REG_PC)), self.where(self.lr())))
        if getattr(self, "blocks", None):
            print("  blocks: " + " ".join(self.where(b) for b in self.blocks))
        return False

    def _on_hook(self, uc, address, size, user):
        name = self.imports.get(address)
        if name is None:
            return
        a = [uc.reg_read(r) for r in R]
        self.counts[name] = self.counts.get(name, 0) + 1
        fn = self.shims.get(name)
        if fn is None:
            if name not in self.stub_seen:
                self.stub_seen.add(name)
                if self.verbose:
                    print("  stub: %s (from %s)" % (name, self.where(self.lr())))
            ret = 0
        else:
            ret = fn(self, a)
        if ret is not None:
            uc.reg_write(UC_ARM_REG_R0, ret & 0xffffffff)
            uc.reg_write(UC_ARM_REG_R1, (ret >> 32) & 0xffffffff)
        uc.reg_write(UC_ARM_REG_PC, self.lr())     # bit 0 selects Thumb

    def call(self, addr, *args, limit=200_000_000):
        regs = R + [UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR]
        saved = {r: self.uc.reg_read(r) for r in regs}
        nested = getattr(self, "_depth", 0) > 0
        sp = (saved[UC_ARM_REG_SP] - 0x1000) & ~0xf if nested else STACK - 0x1000
        extra = list(args[4:])
        sp -= 4 * len(extra)
        sp &= ~7
        for i, v in enumerate(extra):
            self.w32(sp + 4 * i, v)
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_LR, RET)
        for r, v in zip(R, args[:4]):
            self.uc.reg_write(r, v & 0xffffffff)
        self._depth = getattr(self, "_depth", 0) + 1
        try:
            self.uc.emu_start(addr, RET, count=limit)
        except UcError as e:
            raise RuntimeError("stopped at %s (lr %s): %s" % (
                self.where(self.uc.reg_read(UC_ARM_REG_PC)), self.where(self.lr()), e))
        finally:
            self._depth -= 1
        ret = self.uc.reg_read(UC_ARM_REG_R0)
        if nested:
            for r, v in saved.items():
                self.uc.reg_write(r, v)
        return ret


FUNC_NAMES = set()      # data-looking GLOB_DAT imports that are functions


def install_shims(e):
    s = e.shims

    def mem(a):
        return bytes(e.uc.mem_read(a[0], 1)) and None
    def memset(e, a):
        e.uc.mem_write(a[0], bytes([a[1] & 0xff]) * a[2])
        return a[0]
    def memcpy(e, a):
        if a[2]:
            e.uc.mem_write(a[0], bytes(e.uc.mem_read(a[1], a[2])))
        return a[0]
    def memclr(e, a):                   # __aeabi_memclr(dst, n)
        e.uc.mem_write(a[0], b"\0" * a[1])
    def aeabi_memset(e, a):             # __aeabi_memset(dst, n, c)
        e.uc.mem_write(a[0], bytes([a[2] & 0xff]) * a[1])
    def memcmp(e, a):
        x, y = bytes(e.uc.mem_read(a[0], a[2])), bytes(e.uc.mem_read(a[1], a[2]))
        return 0 if x == y else (1 if x > y else 0xffffffff)
    def strlen(e, a):
        return len(e.cstr(a[0], 4096))
    def malloc(e, a):
        return e.malloc(a[0])
    def calloc(e, a):
        return e.malloc(a[0] * a[1])
    def memalign(e, a):
        return e.malloc(a[1], max(a[0], 16))
    def realloc(e, a):
        p = e.malloc(a[1])
        if a[0]:
            e.uc.mem_write(p, bytes(e.uc.mem_read(a[0], a[1])))
        return p
    def property_get(e, a):             # (key, value, default)
        v = e.cstr(a[2]) if a[2] else b""
        e.uc.mem_write(a[1], v + b"\0")
        return len(v)
    def atoi(e, a):
        try:
            return int(e.cstr(a[0]) or b"0") & 0xffffffff
        except ValueError:
            return 0
    def ret(v):
        return lambda e, a: v

    for n, f in (("memset", memset), ("memcpy", memcpy), ("memmove", memcpy),
                 ("__aeabi_memcpy", memcpy), ("__aeabi_memcpy4", memcpy),
                 ("__aeabi_memcpy8", memcpy), ("__aeabi_memmove", memcpy),
                 ("__aeabi_memmove4", memcpy), ("__aeabi_memclr", memclr),
                 ("__aeabi_memclr4", memclr), ("__aeabi_memclr8", memclr),
                 ("__aeabi_memset", aeabi_memset), ("memcmp", memcmp), ("strlen", strlen),
                 ("malloc", malloc), ("calloc", calloc), ("memalign", memalign),
                 ("realloc", realloc), ("free", ret(0)), ("property_get", property_get),
                 ("atoi", atoi), ("syscall", ret(1)), ("gettid", ret(1)), ("getpid", ret(1)),
                 ("__stack_chk_fail", None), ("abort", None)):
        if f is None:
            def die(e, a, n=n):
                raise UcError(0) if False else RuntimeError("%s called from %s" % (n, e.where(e.lr())))
            f = die
        s[n] = f
    FUNC_NAMES.update(s)
