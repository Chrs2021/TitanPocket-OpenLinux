#!/usr/bin/env python3
"""
emu_hal.py: load several AArch64 Android shared libraries into one Unicorn
address space and call into them, so stock HAL code (MediaTek's DIP
PostProcPipe and friends) can run on a desktop.

Each library is mapped at its own base with its RELA, RELR and PLT
relocations applied; imports resolve across the loaded libraries by name,
then to Python shims (libc, logging, properties, pthreads, a fake device for
open/ioctl/mmap), and anything else to a stub that logs its name and returns
0. Static constructors run at load.

Used as a module by experiment scripts (tools/re/emu_dip_config.py):

    emu = Hal(verbose=False)
    emu.load(path) ...; emu.start()
    emu.call(emu.sym("..."), args...)
"""
import re
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UcError
from unicorn.arm64_const import (UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                 UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5,
                                 UC_ARM64_REG_X6, UC_ARM64_REG_X7, UC_ARM64_REG_X8,
                                 UC_ARM64_REG_TPIDR_EL0)

PAGE = 0x1000
X = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
     UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7]
LIB_BASE = 0x10000000           # first library; each next one 256 MB higher
STACK, STACK_SIZE = 0x7f000000, 0x800000
TLS = 0x7e000000
RET = 0x7d000000
HOOKS = 0x7c000000              # one slot per import
DATA = 0x7b000000               # data imports, misc
HEAP, HEAP_SIZE = 0x80000000, 0x40000000
FAKE_IO = 0x70000000            # what mmap of the device returns

R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027


def align_up(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


class Lib:
    def __init__(self, path, base):
        self.path, self.base = path, base
        self.d = d = open(path, "rb").read()
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3a)
        sh = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
        names = sh[shstrndx]
        def nm(o):
            s = names[4] + o
            return d[s:d.index(b"\0", s)].decode()
        self.sec = {nm(s[0]): s for s in sh}
        self.syms = self._dynsym()
        self.exports = {n: base + v for n, v, shndx in self.syms if shndx != 0 and v}

    def relas(self):
        """(offset, info, addend) from .rela.dyn (plain or Android APS2-packed) and .rela.plt"""
        for name in (".rela.dyn", ".rela.plt"):
            off, size, _ = self.section(name)
            if not size:
                continue
            if self.d[off:off + 4] == b"APS2":
                yield from self._aps2(off + 4, off + size)
            else:
                for i in range(size // 24):
                    yield struct.unpack_from("<QQq", self.d, off + 24 * i)

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
        count = sleb()
        r_offset = sleb()
        r_info = r_addend = 0
        done = 0
        while done < count:
            gsize = sleb()
            flags = sleb()
            if flags & 2:                       # grouped by offset delta
                odelta = sleb()
            if flags & 1:                       # grouped by info
                r_info = sleb()
            if flags & 4 and flags & 8:         # grouped by addend
                r_addend += sleb()
            elif not flags & 8:
                r_addend = 0
            for _ in range(gsize):
                r_offset += odelta if flags & 2 else sleb()
                if not flags & 1:
                    r_info = sleb()
                if flags & 8 and not flags & 4:
                    r_addend += sleb()
                yield (r_offset, r_info & 0xffffffffffffffff, r_addend)
            done += gsize

    def section(self, name):
        s = self.sec.get(name)
        return (s[4], s[5], s[3]) if s else (0, 0, 0)       # file offset, size, vaddr

    def _dynsym(self):
        off, size, _ = self.section(".dynsym")
        stro, _, _ = self.section(".dynstr")
        out = []
        for i in range(size // 24):
            name, info, other, shndx, value, sz = struct.unpack_from("<IBBHQQ", self.d, off + 24 * i)
            s = stro + name
            out.append((self.d[s:self.d.index(b"\0", s)].decode(), value, shndx))
        return out


class Hal:
    def __init__(self, verbose=False):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.verbose = verbose
        self.libs = []
        self.imports = {}           # hook address -> name
        self.hook_by_name = {}
        self.counts = {}
        self.stub_seen = set()
        self.next_hook = HOOKS
        self.next_data = DATA
        self.heap = HEAP
        self.next_base = LIB_BASE
        self.shims = {}             # name -> fn(self, args) -> ret
        self.log = []
        for base, size in ((STACK - STACK_SIZE, STACK_SIZE + PAGE), (TLS - PAGE, 3 * PAGE),
                           (RET, PAGE), (HOOKS, 0x100000), (DATA, 0x100000),
                           (HEAP, HEAP_SIZE), (FAKE_IO, 0x1000000)):
            self.uc.mem_map(base, size)
        self.uc.mem_write(TLS + 0x28, struct.pack("<Q", 0x1234567890abcdef))
        self.uc.reg_write(UC_ARM64_REG_TPIDR_EL0, TLS)
        self.uc.hook_add(UC_HOOK_CODE, self._on_hook, begin=HOOKS, end=HOOKS + 0x100000)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_bad)
        install_default_shims(self)

    # ---- loading ----
    def load(self, path):
        lib = Lib(path, self.next_base)
        d = lib.d
        phoff, = struct.unpack_from("<Q", d, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
        top = 0
        mapped = set()
        for i in range(phnum):
            typ, flags, off, vaddr, paddr, filesz, memsz, align = struct.unpack_from(
                "<IIQQQQQQ", d, phoff + i * phentsize)
            if typ != 1:
                continue
            for p in range((lib.base + vaddr) & ~(PAGE - 1), align_up(lib.base + vaddr + memsz), PAGE):
                if p not in mapped:
                    self.uc.mem_map(p, PAGE)
                    mapped.add(p)
            self.uc.mem_write(lib.base + vaddr, d[off:off + filesz])
            top = max(top, vaddr + memsz)
        self.next_base = align_up(lib.base + top, 0x10000000)
        self.libs.append(lib)
        return lib

    def sym(self, name):
        for lib in self.libs:
            if name in lib.exports:
                return lib.exports[name]
        raise KeyError(name)

    def _resolve(self, name):
        for lib in self.libs:
            if name in lib.exports:
                return lib.exports[name]
        return None

    def _hook(self, name):
        if name not in self.hook_by_name:
            a = self.next_hook
            self.next_hook += 4
            self.uc.mem_write(a, b"\xc0\x03\x5f\xd6")
            self.imports[a] = name
            self.hook_by_name[name] = a
        return self.hook_by_name[name]

    def link(self):
        """apply relocations of every loaded library (after all are loaded)"""
        for lib in self.libs:
            b = lib.base
            for r_off, info, addend in lib.relas():
                    typ, si = info & 0xffffffff, info >> 32
                    if typ == R_RELATIVE:
                        val = b + addend
                    elif typ in (R_GLOB_DAT, R_JUMP_SLOT, R_ABS64):
                        name, value, shndx = lib.syms[si]
                        if name in self.shims:
                            val = self._hook(name)
                        else:
                            t = self._resolve(name)
                            if t is None:
                                if typ == R_GLOB_DAT and not name.startswith("_Z") and name not in FUNC_NAMES:
                                    val = self.next_data          # unknown data object
                                    self.next_data += 0x1000
                                else:
                                    val = self._hook(name)
                            else:
                                val = t
                        val += addend if typ != R_RELATIVE else 0
                    else:
                        continue
                    self.uc.mem_write(b + r_off, struct.pack("<Q", val & 0xffffffffffffffff))
            off, size, _ = lib.section(".relr.dyn")
            where = 0
            for i in range(size // 8):
                e, = struct.unpack_from("<Q", lib.d, off + 8 * i)
                if e & 1 == 0:
                    where = e
                    self._relr(b, where)
                    where += 8
                else:
                    bits = e >> 1
                    for k in range(63):
                        if bits & (1 << k):
                            self._relr(b, where + 8 * k)
                    where += 8 * 63

    def _relr(self, base, off):
        v, = struct.unpack("<Q", self.uc.mem_read(base + off, 8))
        self.uc.mem_write(base + off, struct.pack("<Q", v + base))

    def start(self):
        """link and run static constructors"""
        self.link()
        for lib in self.libs:
            off, size, vaddr = lib.section(".init_array")
            for i in range(size // 8):
                fn, = struct.unpack("<Q", self.uc.mem_read(lib.base + vaddr + 8 * i, 8))
                if fn and fn != 0xffffffffffffffff:
                    try:
                        self.call(fn)
                    except RuntimeError as e:
                        print("init %s: %s" % (lib.path.split("/")[-1], e))

    # ---- memory helpers ----
    def malloc(self, n, align=16):
        p = align_up(self.heap, align)
        self.heap = p + max(n, 1)
        if self.heap > HEAP + HEAP_SIZE:
            raise MemoryError("heap exhausted")
        self.uc.mem_write(p, b"\0" * max(n, 1))
        return p

    def cstr(self, a, limit=4096):
        if not a:
            return b""
        try:
            b = bytes(self.uc.mem_read(a, limit))
        except UcError:
            b = bytes(self.uc.mem_read(a, 64))
        return b[:b.index(b"\0")] if b"\0" in b else b

    def new_cstr(self, s):
        b = s.encode() + b"\0"
        p = self.malloc(len(b))
        self.uc.mem_write(p, b)
        return p

    def r32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def r64(self, a):
        return struct.unpack("<Q", self.uc.mem_read(a, 8))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xffffffff))

    def w64(self, a, v):
        self.uc.mem_write(a, struct.pack("<Q", v & 0xffffffffffffffff))

    def where(self, addr):
        for lib in self.libs:
            if lib.base <= addr < lib.base + 0x10000000:
                return "%s+%#x" % (lib.path.split("/")[-1], addr - lib.base)
        return "%#x" % addr

    # ---- execution ----
    lenient = False		# map zero pages on faulting access and continue

    def _on_bad(self, uc, access, address, size, value, user):
        if self.lenient and 0x1000 <= address < 0x7a000000:
            page = address & ~(PAGE - 1)
            try:
                uc.mem_map(page, PAGE)
                self.faults = getattr(self, "faults", 0) + 1
                return True		# retry the access
            except UcError:
                return True
        pc = uc.reg_read(UC_ARM64_REG_PC)
        print("bad access %#x (size %d) at %s, lr %s" % (address, size, self.where(pc),
              self.where(uc.reg_read(UC_ARM64_REG_LR))))
        return False

    def _on_hook(self, uc, address, size, user):
        name = self.imports.get(address)
        if name is None:
            return
        a = [uc.reg_read(r) for r in X]
        self.counts[name] = self.counts.get(name, 0) + 1
        fn = self.shims.get(name)
        if fn is None:
            if name not in self.stub_seen:
                self.stub_seen.add(name)
                if self.verbose:
                    print("  stub: %s (from %s)" % (name, self.where(uc.reg_read(UC_ARM64_REG_LR))))
            ret = 0
        else:
            ret = fn(self, a)
        if ret is not None:
            uc.reg_write(UC_ARM64_REG_X0, ret & 0xffffffffffffffff)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def call(self, addr, *args, limit=500_000_000, x8=None):
        """call a function (reentrant: shims may call back in)"""
        saved = {r: self.uc.reg_read(r) for r in X + [UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC]}
        nested = hasattr(self, "_depth") and self._depth > 0
        sp = (saved[UC_ARM64_REG_SP] - 0x1000) & ~0xf if nested else STACK - 0x100
        self.uc.reg_write(UC_ARM64_REG_SP, sp)
        self.uc.reg_write(UC_ARM64_REG_LR, RET)
        for r, v in zip(X, args):
            self.uc.reg_write(r, v & 0xffffffffffffffff)
        if x8 is not None:
            self.uc.reg_write(UC_ARM64_REG_X8, x8)
        self._depth = getattr(self, "_depth", 0) + 1
        try:
            self.uc.emu_start(addr, RET, count=limit)
        except UcError as e:
            pc = self.uc.reg_read(UC_ARM64_REG_PC)
            raise RuntimeError("stopped at %s (lr %s): %s" % (
                self.where(pc), self.where(self.uc.reg_read(UC_ARM64_REG_LR)), e))
        finally:
            self._depth -= 1
        ret = self.uc.reg_read(UC_ARM64_REG_X0)
        if nested:
            for r, v in saved.items():
                self.uc.reg_write(r, v)
        return ret

    def format(self, fmt_addr, regs, va=None):
        """printf: integer/string args from registers then the stack (or a va_list)"""
        fmt = self.cstr(fmt_addr).decode(errors="replace")
        if va is not None:
            args = self._va_args(va)
        else:
            sp = self.uc.reg_read(UC_ARM64_REG_SP)
            args = list(regs) + [self.r64(sp + 8 * i) for i in range(32)]
        k = 0
        def sub(m):
            nonlocal k
            c = m.group(1)
            if c == "%":
                return "%"
            v = args[k] if k < len(args) else 0
            k += 1
            if c == "s":
                try:
                    return self.cstr(v, 256).decode(errors="replace")
                except UcError:
                    return "<%#x>" % v
            if c in "di":
                v &= 0xffffffff
                return str(v - (1 << 32) if v & 0x80000000 else v)
            if c in "xXp":
                return "%x" % v
            if c == "c":
                return chr(v & 0xff)
            if c in "fg":
                return "<f>"
            return str(v & 0xffffffff)
        return re.sub(r"%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|h|hh|z|j|t)?([diuxXspcfg%])", sub, fmt)

    def _va_args(self, va):
        """AArch64 va_list: stack, gr_top, vr_top, gr_offs, vr_offs"""
        stack, gr_top, vr_top = self.r64(va), self.r64(va + 8), self.r64(va + 16)
        gr_offs = self.r32(va + 24)
        gr_offs = gr_offs - (1 << 32) if gr_offs & 0x80000000 else gr_offs
        out = []
        while gr_offs < 0 and len(out) < 8:
            out.append(self.r64(gr_top + gr_offs))
            gr_offs += 8
        out += [self.r64(stack + 8 * i) for i in range(32)]
        return out


FUNC_NAMES = set()


def install_default_shims(h):
    S = h.shims

    def ret(v):
        return lambda h, a: v
    def memset(h, a):
        if a[2]:
            h.uc.mem_write(a[0], bytes([a[1] & 0xff]) * a[2])
        return a[0]
    def memcpy(h, a):
        if a[2]:
            h.uc.mem_write(a[0], bytes(h.uc.mem_read(a[1], a[2])))
        return a[0]
    def memcmp(h, a):
        x, y = bytes(h.uc.mem_read(a[0], a[2])), bytes(h.uc.mem_read(a[1], a[2]))
        return 0 if x == y else (-1 if x < y else 1)
    def strlen(h, a):
        return len(h.cstr(a[0], 0x10000))
    def strcmp(h, a):
        x, y = h.cstr(a[0]), h.cstr(a[1])
        return (x > y) - (x < y)
    def strncmp(h, a):
        x, y = h.cstr(a[0])[:a[2]], h.cstr(a[1])[:a[2]]
        return (x > y) - (x < y)
    def strcpy(h, a):
        h.uc.mem_write(a[0], h.cstr(a[1]) + b"\0")
        return a[0]
    def strncpy(h, a):
        s = h.cstr(a[1])[:a[2]]
        h.uc.mem_write(a[0], s + b"\0" * (a[2] - len(s)))
        return a[0]
    def strcat(h, a):
        d = h.cstr(a[0])
        h.uc.mem_write(a[0] + len(d), h.cstr(a[1]) + b"\0")
        return a[0]
    def strncat(h, a):
        d = h.cstr(a[0])
        h.uc.mem_write(a[0] + len(d), h.cstr(a[1])[:a[2]] + b"\0")
        return a[0]
    def strchr(h, a):
        s = h.cstr(a[0])
        i = s.find(bytes([a[1] & 0xff]))
        return a[0] + i if i >= 0 else 0
    def strdup(h, a):
        s = h.cstr(a[0])
        p = h.malloc(len(s) + 1)
        h.uc.mem_write(p, s + b"\0")
        return p
    def malloc(h, a):
        return h.malloc(a[0])
    def calloc(h, a):
        return h.malloc(a[0] * a[1])
    def realloc(h, a):
        p = h.malloc(a[1])
        if a[0]:
            h.uc.mem_write(p, bytes(h.uc.mem_read(a[0], a[1])))
        return p
    def memalign(h, a):
        return h.malloc(a[1], max(a[0], 16))
    def posix_memalign(h, a):
        h.w64(a[0], h.malloc(a[2], max(a[1], 16)))
        return 0
    def vsnprintf(h, a):
        s = h.format(a[2], None, va=a[3]).encode()
        if a[1]:
            h.uc.mem_write(a[0], s[:a[1] - 1] + b"\0")
        return len(s)
    def vsnprintf_chk(h, a):          # (buf, len, flags, slen, fmt, va)
        s = h.format(a[4], None, va=a[5]).encode()
        if a[1]:
            h.uc.mem_write(a[0], s[:a[1] - 1] + b"\0")
        return len(s)
    def snprintf(h, a):
        s = h.format(a[2], a[3:]).encode()
        if a[1]:
            h.uc.mem_write(a[0], s[:a[1] - 1] + b"\0")
        return len(s)
    def sprintf(h, a):
        s = h.format(a[1], a[2:]).encode()
        h.uc.mem_write(a[0], s + b"\0")
        return len(s)
    def log_print(h, a):                # (prio, tag, fmt, ...)
        msg = h.format(a[2], a[3:]).rstrip()
        h.log.append(msg)
        if h.verbose:
            print("  log[%s]: %s" % (h.cstr(a[1]).decode(errors="replace"), msg))
        return 0
    def log_vprint(h, a):               # (prio, tag, fmt, va)
        msg = h.format(a[2], None, va=a[3]).rstrip()
        h.log.append(msg)
        if h.verbose:
            print("  log[%s]: %s" % (h.cstr(a[1]).decode(errors="replace"), msg))
        return 0
    def property_get(h, a):             # (key, value, default)
        d = h.cstr(a[2]) if a[2] else b""
        v = PROPS.get(h.cstr(a[0]).decode(), d.decode()).encode()
        h.uc.mem_write(a[1], v + b"\0")
        return len(v)
    def property_get_int(h, a):         # (key, default)
        k = h.cstr(a[0]).decode()
        return int(PROPS[k]) if k in PROPS else a[1]
    def guard_acquire(h, a):
        return 0 if h.uc.mem_read(a[0], 1)[0] else 1
    def guard_release(h, a):
        h.uc.mem_write(a[0], b"\1")
        return 0
    def pthread_once(h, a):
        if h.r32(a[0]) == 0:
            h.w32(a[0], 1)
            h.call(a[1])
        return 0
    def errno_(h, a):
        return DATA + 0x80000
    def mmap(h, a):                     # (addr, len, prot, flags, fd, off)
        if a[4] != 0xffffffff and a[4] < 0x100:
            p = FAKE_IO + (h.fake_io_used if hasattr(h, "fake_io_used") else 0)
            h.fake_io_used = getattr(h, "fake_io_used", 0) + align_up(a[1])
            if h.verbose:
                print("  mmap fd %d len %#x off %#x -> %#x" % (a[4], a[1], a[5], p))
            h.mmaps = getattr(h, "mmaps", []) + [(p, a[1], a[5])]
            return p
        return h.malloc(a[1], PAGE)
    def open_(h, a):
        path = h.cstr(a[0] if a[0] > 0x1000 else a[1]).decode(errors="replace")
        h.opened = getattr(h, "opened", []) + [path]
        if h.verbose:
            print("  open %s" % path)
        return 3 + len(h.opened)
    def ioctl(h, a):
        h.ioctls = getattr(h, "ioctls", []) + [(a[0], a[1], a[2])]
        fn = IOCTLS.get(a[1])
        if fn:
            return fn(h, a)
        if h.verbose:
            print("  ioctl fd %d cmd %#x arg %#x" % (a[0], a[1], a[2]))
        return 0
    def stack_chk(h, a):
        if not h.lenient:
            print("  !! stack check failure / abort, lr %s" % h.where(h.uc.reg_read(UC_ARM64_REG_LR)))
            h.uc.emu_stop()
        h.stackchk = getattr(h, "stackchk", 0) + 1	# spurious under stubs; continue
        return None

    for n in ("memset", "__memset_chk"):
        S[n] = memset
    for n in ("memcpy", "memmove", "__memcpy_chk", "__memmove_chk"):
        S[n] = memcpy
    S["memcmp"] = memcmp
    for n in ("strlen", "__strlen_chk"):
        S[n] = strlen
    S["strcmp"] = strcmp
    S["strncmp"] = strncmp
    for n in ("strcpy", "__strcpy_chk"):
        S[n] = strcpy
    for n in ("strncpy", "__strncpy_chk", "__strncpy_chk2"):
        S[n] = strncpy
    S["strcat"] = strcat
    for n in ("strncat", "__strncat_chk", "__strlcat_chk", "strlcat"):
        S[n] = strncat
    S["strchr"] = strchr
    S["strdup"] = strdup
    S["malloc"] = malloc
    S["calloc"] = calloc
    S["realloc"] = realloc
    S["memalign"] = memalign
    S["posix_memalign"] = posix_memalign
    for n in ("free", "munmap", "close", "usleep", "sleep", "nanosleep", "sched_yield",
              "__cxa_atexit", "__cxa_finalize", "__register_atfork", "atrace_begin_body",
              "atrace_end_body", "atrace_get_enabled_tags", "sem_init", "sem_post", "sem_wait",
              "sem_destroy", "sem_timedwait", "fsync", "fflush", "fclose"):
        S[n] = ret(0)
    for n in ("pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_init",
              "pthread_mutex_destroy", "pthread_mutex_trylock", "pthread_cond_init",
              "pthread_cond_signal", "pthread_cond_broadcast", "pthread_cond_destroy",
              "pthread_cond_wait", "pthread_cond_timedwait", "pthread_attr_init",
              "pthread_attr_destroy", "pthread_condattr_init", "pthread_condattr_setclock",
              "pthread_condattr_destroy", "pthread_mutexattr_init", "pthread_mutexattr_settype",
              "pthread_mutexattr_destroy", "pthread_key_create", "pthread_setspecific",
              "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock",
              "pthread_rwlock_init", "pthread_rwlock_destroy"):
        S[n] = ret(0)
    S["pthread_getspecific"] = ret(0)
    S["pthread_self"] = ret(1)
    S["pthread_once"] = pthread_once
    for n in ("gettid", "getpid", "getuid"):
        S[n] = ret(1000)
    for n in ("gettimeofday", "clock_gettime", "time"):
        S[n] = ret(0)
    for n in ("vsnprintf",):
        S[n] = vsnprintf
    S["__vsnprintf_chk"] = vsnprintf_chk
    S["snprintf"] = snprintf
    S["sprintf"] = sprintf
    S["__android_log_print"] = log_print
    S["__android_log_vprint"] = log_vprint
    S["__android_log_buf_print"] = lambda h, a: log_print(h, a[1:] + [0])
    S["__android_log_is_loggable"] = ret(0)
    S["property_get"] = property_get
    for n in ("property_get_int32", "property_get_int64", "property_get_bool"):
        S[n] = property_get_int
    S["property_set"] = ret(0)
    S["mtkcam_testLog"] = ret(0)
    S["__cxa_guard_acquire"] = guard_acquire
    S["__cxa_guard_release"] = guard_release
    S["__errno"] = errno_
    S["mmap"] = mmap
    S["mmap64"] = mmap
    for n in ("open", "open64", "__open_2", "openat"):
        S[n] = open_
    S["ioctl"] = ioctl
    S["__stack_chk_fail"] = stack_chk
    S["abort"] = stack_chk
    FUNC_NAMES.update(S)


PROPS = {}
IOCTLS = {}
