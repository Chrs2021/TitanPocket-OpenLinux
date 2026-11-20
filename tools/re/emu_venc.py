#!/usr/bin/env python3
"""
emu_venc.py: run MediaTek's H.264 hardware encoder driver (the 32-bit vendor
libvcodecdrv.so) in emu32 and log every VENC register access, for
docs/video-encoder.md. The driver's utility layer (memory, cache, lock,
interrupt wait, register mmap) is replaced by shims: buffers get device
addresses equal to their emulated addresses, the "interrupt" always reports
frame done, and register reads return the last value written unless
--read OFF=VAL says otherwise.

  tools/re/emu_venc.py LIB [--size W H] [--fmt N] [--profile N] [--level N]
                       [--kbps N] [--fps N] [--gop N] [--frames N] [-v]

Prints, per driver call, the register writes in order (offset, value,
caller). Nothing from the library itself is printed or stored.
"""
import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from emu32 import Emu32  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("lib")
ap.add_argument("--size", nargs=2, type=int, default=[1280, 720])
ap.add_argument("--fmt", type=int, default=1, help="driver input format enum")
ap.add_argument("--profile", type=lambda x: int(x, 0), default=1, help="1 baseline, 4 main, 0x10 high")
ap.add_argument("--level", type=lambda x: int(x, 0), default=0xb, help="enum; 0xb = 3.1, 0xd = 4.0")
ap.add_argument("--kbps", type=int, default=4000)
ap.add_argument("--fps", type=int, default=30)
ap.add_argument("--gop", type=int, default=30)
ap.add_argument("--frames", type=int, default=3)
ap.add_argument("--read", action="append", default=[], help="OFF=VAL for register reads")
ap.add_argument("--dump-handle", action="store_true")
ap.add_argument("--rc-out", help="write the rate-control code the driver loads (0x4040 bytes; vendor data, keep local)")
ap.add_argument("-v", "--verbose", action="store_true")
a = ap.parse_args()

e = Emu32(verbose=a.verbose)
lib = e.load(a.lib)
venc = e.add_io(0x1000, "venc")
reads = {int(k, 0): int(v, 0) for k, v in (x.split("=") for x in a.read)}
e.io_read = lambda name, off, last: reads.get(off, last)
allocs = []


def shim(name):
    def deco(f):
        e.shims[name] = f
        return f
    return deco


def r(name, v=0):
    e.shims[name] = lambda e, args: v


for n in ("eHalCmdProc", "eHalInit", "eHalDeInit", "eValInit", "eValDeInit", "eVideoLockHW",
          "eVideoUnLockHW", "eVideoFlushCache", "eVideoInvalidateCache", "eVideoGetParam",
          "eVideoSetParam", "eVideoMemFree", "eVideoIncLogCount", "eVideoVcodecSetThreadID",
          "VCodecDrvFlushCachedBuffer", "VCodecDrvInvalidateCachedBuffer",
          "VCodecDrvFlushCachedBufferAll", "VCodecDrvInvalidateCachedBufferAll",
          "VCodecDrvFlushInvalidateCacheBufferAll", "VCodecDrvMemFree", "VCodecDrvMemFree_NC",
          "VCodecDrvLockHW", "VCodecDrvUnLockHW", "PowerHal_Wrap_scnReg", "PowerHal_Wrap_scnEnable",
          "PowerHal_Wrap_scnDisable", "PowerHal_Wrap_scnUnreg", "PowerHal_Wrap_scnConfig",
          "atrace_get_enabled_tags", "fopen", "gettimeofday", "eHalEMICtrlForRecordSize"):
    r(n)


@shim("eHalGetMMAP")
def _(e, args):
    return venc


@shim("eVideoMemAlloc")             # (VAL_MEMORY_T *, size of it)
def _(e, args):
    m = args[0]
    size, align = e.r32(m + 8), e.r32(m + 0x20) or 16
    p = e.malloc(size + align, max(align, 16))
    for off in (0x10, 0x18, 0x28, 0x30):    # va, pa, aligned va, aligned pa
        e.w32(m + off, p)
        e.w32(m + off + 4, 0)
    allocs.append((p, size, e.where(e.lr())))
    return 0


@shim("VCodecDrvMemAllocAligned")
def _(e, args):                     # (void **out, size, align, ...)? best effort
    p = e.malloc(args[1] + 64, 64)
    if args[0]:
        e.w32(args[0], p)
    return p


@shim("VCodecDrvIntMalloc")
def _(e, args):
    return e.malloc(args[0] if args[0] < 0x10000000 else args[1])


@shim("eVideoMemCpy")               # (dst mem, dst size, src mem, src size, n)
def _(e, args):
    n = e.stack_arg(4)
    dst, src = e.r32(args[0] + 0x10), e.r32(args[2] + 0x10)
    if n:
        e.uc.mem_write(dst, bytes(e.uc.mem_read(src, n)))
    return 0


@shim("eVideoMemSet")               # (mem, size, value, n)
def _(e, args):
    dst = e.r32(args[0] + 0x10)
    n = e.stack_arg(3) if False else args[3]
    if dst and n < 0x4000000:
        e.uc.mem_write(dst, bytes([args[2] & 0xff]) * n)
    return 0


@shim("WaitISR")                    # (VAL_ISR_T *, size): report "frame done"
def _(e, args):
    e.w32(args[0] + 0x20, 4)
    return 0


def section(title, fn):
    start = len(e.io_log)
    try:
        rv = fn()
    except RuntimeError as ex:
        rv = "STOPPED: %s" % ex
    print("== %s -> %s" % (title, rv if isinstance(rv, str) else "%#x" % rv))
    for op, name, off, val, caller in e.io_log[start:]:
        print("  %s %03x %08x  %s" % (op, off, val, caller))
    return rv


e.start()
if a.verbose:
    e.trace_blocks()
W, H = a.size
pw, ph = (W + 15) & ~15, (H + 15) & ~15
hp = e.malloc(4)
drv = e.malloc(0x200)               # the caller's context: +0xb0 driver type
e.w32(drv + 0xb0, 5)                # VAL_DRIVER_TYPE_H264_ENC
section("h264_enc_init", lambda: e.call(e.sym("h264_enc_init"), hp, drv))
h = e.r32(hp)
print("handle %#x chip %#x regs %#x" % (h, e.r32(h + 0x7bc), e.r32(h + 8)))

rc = e.malloc(0x40)
e.w32(rc + 0, a.gop)
e.w32(rc + 4, a.kbps * 1000)
e.uc.mem_write(rc + 10, struct.pack("<H", a.fps))
p = e.malloc(0x100)
for i, v in enumerate((a.fmt, a.profile, a.level, W, H, pw, ph)):
    e.w32(p + 4 * i, v)
e.w32(p + 0x30, rc)
section("setParameter(6, enc param)", lambda: e.call(e.sym("h264_enc_setParameter"), h, 6, p, 0))
for t in (0xe,):
    section("setParameter(%#x)" % t, lambda: e.call(e.sym("h264_enc_setParameter"), h, t, 0, 0))
print("fields: w %d h %d scan %d pitch %d bufh %d gop %d kbps %d fps %d profile %#x level %d" % tuple(
    e.r32(h + o) for o in (0x18, 0x1c, 0x20, 0x24, 0x28, 0xcc, 0x98, 0x9c, 0xc4, 0xc8)))


def vbuf(size):
    """a VAL_MEM_ADDR_T-like {va, pa, size} (64-bit fields) and its memory"""
    m = e.malloc(size + 0x100, 0x100)
    d = e.malloc(0x40)
    for off, v in ((0, m), (8, m), (0x10, size)):
        e.w32(d + off, v)
    return d


bs = e.malloc(0x80)
bsbuf = e.malloc(pw * ph + 0x1000, 0x100)
for off, v in ((0, bsbuf), (8, bsbuf), (0x10, pw * ph)):
    e.w32(bs + off, v)
res = e.malloc(0x40)
if a.dump_handle:
    words = [e.r32(h + o) for o in range(0, 0x1e10, 4)]
    print("handle (nonzero words):")
    print("  " + " ".join("%x=%x" % (4 * i, w) for i, w in enumerate(words)
                          if w and not 0x80000000 <= w < 0xc0000000))
for kind, name in ((2, "SPS"), (3, "PPS")):
    e.w32(bs + 0x28, 0)
    section("encode %s" % name, lambda: e.call(e.sym("h264_enc_encode"), h, kind, 0, bs, res))
    n = e.r32(bs + 0x28)
    print("  bytes (%d): %s" % (n, bytes(e.uc.mem_read(bsbuf, min(n, 64))).hex()))
for f in range(a.frames):
    frm = vbuf(pw * ph * 3 // 2)
    e.w32(bs + 0x28, 0)
    section("encode frame %d" % f, lambda: e.call(e.sym("h264_enc_encode"), h, 4, frm, bs, res))
if a.rc_out:
    rc_va = e.r32(h + 0x190 + 0x10)         # VAL_MEMORY_T at +0x190: register 0x8c's buffer
    open(a.rc_out, "wb").write(bytes(e.uc.mem_read(rc_va, e.r32(h + 0x198))))
    print("rate-control code: %#x bytes -> %s" % (e.r32(h + 0x198), a.rc_out))
print("allocations:")
for p_, s_, c_ in allocs:
    print("  %#x %#x  %s" % (p_, s_, c_))
if a.verbose:
    print("stubs:", ", ".join(sorted(e.stub_seen)))
