# Modem (MD1, LTE)

Status (2026-11-04, Linux 6.12): **the modem registers on LTE and mobile data works.** ModemManager drives it
over a 27.010 mux on WWAN AT ports (`wwan0at0` = `cmd1`), and it is `registered`, `home` on 310260 within 2-3 s of
enabling (see "Radio on with a valid SIM"). `tools/modem/ltedata.py` brings up IPv6 data on `ccmni0` over CLDMA (see "Mobile
data"). Calls and SMS work over IMS (see "IMS"); USSD works over IMS (one-shot code measured; menus offline only, see "USSD"). It runs as the `titan-modem` service, on a RAM copy of the phone's own NVRAM
served by `tools/modem/mdd.py` (see "File service" and "AT port and ModemManager"). It reports `MOLY.LR12A.R3.MP.V162`.
ModemManager sees it `disabled`, with the SIM up and the radio off, until it is enabled.

The driver is a platform driver in the kernel tree (2026-11-10:
`drivers/net/wwan/mt6771/titan_modem.c`, `CONFIG_MTK_MT6771_MODEM`, module
`titan_modem`; until then an out-of-tree module loaded by hand, kept in
`out/modem/out-of-tree-driver/`). It binds to the DT node `modem@10209000`
(`mediatek,mt6771-modem`) and takes every register window, interrupt,
clock, syscon and its power domain from it. The node is in `mt6771.dtsi`
(disabled) and enabled in the board dts. udev loads the module at boot; the
modem itself runs only while `/dev/titan-modem` is open: `titan-mdd`'s open
powers it on and boots it, its close stops it (see "In tree, MD1 as a power
domain, crashes"). Runs on the phone since kernel #124 (2026-11-10): the
modem boots, registers on LTE and IMS registers, as before.

## What LK does

LK loads `md1img`, checks it, programs the modem's memory protection
(11 EMI MPU regions) and its bank remap, and describes all of it in a tag
block. `/chosen` `ccci,modem_info_v2` (48 bytes, LK's struct written as
is, little endian, not DT cells) gives the block's address and size, an
error number and a load flag. The tags (64-byte names, then data offset,
size, next tag; the vendor's `ccci_util_lib_fo.c` parses the same):

| Tag | Value on this phone |
|---|---|
| `hdr_tbl_inf` | image region 0x8c000000 + 0x7e00000, modem type 12 (LWCTG) |
| `md1img` | image size 0x15fe8c0 |
| `smem_layout` | non-cached shared memory 0x88000000 + 1 MB |
| `ccb_info` | cached shared memory 0x86000000 + 22 MB |
| `md1_smem_cahce_offset` | 0x0e000000 (the cached region's place in the modem's bank 4) |
| `md_mpu_inf`, `md_mpu_num` | the 11 MPU regions LK set |
| `retrieve0`..`4` | parts of the image region the kernel may take back |
| `md1_phy_cap` | 0 (no PHY capture buffer) |

The structs keep their natural padding (`ccb_info` is 16 bytes,
`smem_layout` 40). LK's `/reserved-memory` entries for these regions are
not no-map, so they are in the kernel's linear map (cacheable). The
modem's view of the non-cached region is 0x40000000 (bank 4, 32 MB
aligned), of the cached one 0x4e000000.

The image, the memory protection and security are LK's; the kernel's
`ccci_init_security` is a stub on this platform, and the vendor kernel
never set any MPU itself (`SET_EMI_STEP_BY_STAGE` is not defined for the
MT6771).

## What the kernel does

As the vendor eccci driver (`mt6771/md_sys1_platform.c`, `modem_sys1.c`,
`hif/ccci_hif_ccif.c`, `ccci_modem.c`, `fsm/ccci_fsm.c`):

1. **Shared memory.** Clear the non-cached region and lay out the CCIF
   ring buffers in it: eight queues at 160 KB (each a guard, read, write
   and length words per direction, the rx then tx buffers, a guard) and
   eight exception queues at 881 KB. The region is used through a
   write-combined `vmap` of its pages, as the vendor driver does: it
   cannot be ioremapped (it is RAM in the linear map), a cacheable mapping
   does not work (the modem does not snoop, and each ring's control words
   for both directions share a cache line), and a static no-map entry in
   our DT over it would stop LK (it halts on overlapping reservations).
   The linear alias's lines are dropped once first (a DMA unmap from
   device). This mismatched-attribute alias is the vendor's solution too;
   a cleaner one needs LK's region marked no-map before the linear map is
   built, which 6.12's reserved-memory hooks (run from
   `unflatten_device_tree`, after `paging_init`) cannot do.
2. **CCIF.** Enable the CLDMA and CCIF0-2 infra clocks, reset CCIF0
   (INFRA_RST0/1 bit 8), clear both CCIF SRAMs and write the debug area's
   address into the SRAM's last 12 bytes.
3. **Power** (`md_cd_power_on`): the MD1 power domain through runtime PM
   (since 2026-11-10 the scpsys driver's: clear TOPCKGEN CLK_MODE bits 8-9,
   the modem's clock gating, then MD1_PWR_CON 0x320 with the extra SPM
   registers MD_SRAM_ISO_CON 0x394 and MD_EXTRA_PWR_CON 0x398, then the bus
   protection: INFRA PROTECTEN bits 3, 4, 7 and PROTECTEN_1 bit 6; see "In
   tree, MD1 as a power domain, crashes"), close MD-to-AP, set the modem's
   26 MHz request (INFRA 0xf0c = 0x21), initialise the modem PLLs
   (MDTOP_PLLMIXED 0x20140000, MDTOP_CLKSW 0x20150000; the PLL block reads
   version 0x20160627) and stop the modem watchdog.
4. **Release** (`md_cd_let_md_go`): MD_BOOT_VECTOR_EN (0x20000024) = 1,
   then AP-to-MD bus protection on and MD-to-AP off.
5. **Handshake 1.** The modem writes a header and its feature query (64
   support:version bytes between two 0x49434343 patterns) at the start of
   the AP CCIF SRAM and raises channel 15. Measured: 55 ms after the
   release. Its query: feature 0 (boot info) required, 1-34 optional. The
   AP writes its feature list (one entry per feature, with data for the
   supported ones: where each shared region is, the time, a random seed,
   ...) into the runtime data area (non-cached + 58 KB), a block saying
   where everything is after the header in the SRAM, and raises channel
   15 back.
6. **Packets.** From then on the modem talks in CCCI packets (16-byte
   header: two data words, 16-bit channel, 15-bit sequence number and an
   assert bit, a reserved word) on the eight rings, each announced on CCIF
   channel 0-7. The modem's exceptions use channels 16-19, on CCIF0's
   second interrupt (SPI 152; data is on SPI 151, both active low).

The MD1 domain powered off again on unload (measured with the out-of-tree
driver: MD1_PWR_CON back to off, PWR_STATUS bit 0 clear), also after a modem
exception. The in-tree driver powers it off when `/dev/titan-modem` closes.

VMODEM stays at LK's 0.55 V (MT6358 ELR0 0x08): the vendor MT6358 code
only re-applies the value LK left, and the modem sets its own voltage.

## Findings

- **The CCIF SRAM takes 32-bit accesses only.** `memcpy_toio` ends an
  odd-sized copy with byte stores, which are lost: the AP block's tail
  pattern read back 0x43000000 instead of 0x43434349, and the modem
  asserted in its handshake code (`ccci_hs.c`, an ASSERT_FAIL record in
  the debug area) 21 ms after the runtime data. With 32-bit `writel` the
  runtime data is accepted. Bisecting the features first (declining all
  optional ones still asserted) pointed away from the feature data.
- **Exception records.** On an exception the modem fills an overview in
  the non-cached region at 2 KB (MDSS_DBG, the vendor's
  `mdee_dumper_v3.h` layout): a per-core reason array (8 cores, an
  11-char name and an is-offender byte each), then a brief-main-info
  block whose type says assert (0x50) or fatal, then the details. For an
  assert: the source path (256 bytes), line and three parameters; for a
  fatal: codes, offender, PC and LR. The driver decodes this into one
  `dev_err` line (`decode_exception`) whenever an exception CCIF channel
  (16-19) fires, and still exposes the raw region through debugfs
  `titan-modem/smem` (and the CCIF SRAM at `titan-modem/sram`). This
  record is how each step below was found.
- **RPC.** The modem's first packet is an RPC (channel 32): the SIM
  hot-plug interrupt of each of four SIM slots, three attributes each
  (`IPC_RPC_GET_EINT_ATTR_OP`, 0x4005). Android's kernel answers SIMs 1 and 2 from its
  device tree and `ERR_SIM_HOT_PLUG_QUERY_STRING` (-15) for 3 and 4; `mdd.py` does
  the same with the values measured there (the reply: op | 0xffff0000, its
  parameters, on channel 33 with the request's buffer index kept in the
  reserved word). The modem also went on booting when all four got the error.
- **File service.** Then the modem's file requests start (channel 14,
  the vendor `ccci_fs` port served by Android's `ccci_fsd`): same
  framing as RPC, UTF-16 paths on drive `Z:`. A reply with the wrong
  number of parameters makes the modem assert in `ccci_fs.c:1259` with
  (expected count, sent count, op), one with a data parameter of the
  wrong length in `ccci_fs.c:1321` with (op, sent length, expected
  length). See "File service" for the operations.

## File service

**The service runs `titan-mdd` (Rust, `userspace/titan-phone`) since 2026-11-06**, with `titan-radioinit` and
`titan-ltedata` in place of `radioinit.py` and `ltedata.py`; the Python ones below stay for bring-up work (the
Rust daemon leaves out `mdd.py`'s `SPH_TEST` loopback and `AT_CMDS`). Checked by replay: the modem's
requests from one boot (1438 packets: 1072 compact reads, 64 writes, 61 opens, RPC queries, speech and AT
traffic) were recorded under `mdd.py`, then sent to each daemon over a SOCK_SEQPACKET pair (`MDD_FD`) against
two fresh copies of the same NVRAM snapshot. 1437 of the 1438 packets got byte-identical replies, the
exception being `FS_GetDiskInfo`'s two free-space counts (the other copy took room in `/run`), and all 370
resulting files were identical. On the phone it then booted the modem (registered in 6 s), handed the AT
channel over, wrote the speech parameters and got the modem's acks for speech on and off; 0.5 MB resident
against `mdd.py`'s 11.7 MB. The recording held NVRAM contents and was deleted.

The modem's files (CCCI_FS, channels 14/15, served on Android by
`ccci_fsd`) come from `tools/modem/mdd.py`, from a scratch directory in
RAM (`FS_ROOT`, default `/run/titan-modem-mock`) holding the modem's
drives as directories. `tools/modem/mdmock.sh` runs it: with `NV=real`
it first copies the phone's NVRAM into it (`tools/modem/nvcopy.sh`:
`nvdata`, `protect1` and `protect2` mounted read-only without journal
replay, their `md/` trees copied, mode 700); with nothing set it starts
empty; `KEEP=1` keeps the last run's. The modem's writes stay in RAM and
are gone at the next reboot; the partitions are never written. With
`NV=real` the log holds paths and sizes only (`NV_QUIET`), never file
contents: the NVRAM is per-phone data (IMEI, calibration) and stays on
the phone.

| Drive | Android directory | Here |
|---|---|---|
| `Z:` | `/mnt/vendor/nvdata/md` | `nvdata` (`sdc10`) `md/`: `NVRAM/`, 357 files |
| `X:` | `/mnt/vendor/protect_f/md` | `protect1` (`sdc13`) `md/`, 7 files |
| `Y:` | `/mnt/vendor/protect_s/md` | `protect2` (`sdc14`) `md/`, 5 files |
| `W:`, `V:`, `U:`, `T:`, `S:` | DSP firmware, `md_cmn`, `mdlpm`, `mdota`, `nvcfg` | not served; never asked for |

The operations, structures and reply shapes are those of FirmWire's
MediaTek FSD emulation (`firmwire/vendor/mtk/hw/FSD.py`, Team FirmWire,
BSD-3-Clause), each checked against this modem. A reply starts with the
result except for `CMPT_READ`:

| Op | Name | Request | Reply |
|---|---|---|---|
| 0x1001 | OPEN | path, flags | handle or error |
| 0x1002 | SEEK | handle, offset, whence | position |
| 0x1003 | READ | handle, size | result, bytes read, data |
| 0x1004 | WRITE | handle, data, size | result, bytes written |
| 0x1005 | CLOSE | handle | result |
| 0x1007 | CREATEDIR | path | result |
| 0x1009 | GETFILESIZE | handle | result, size |
| 0x100e | GETDISKINFO | drive, flags | result, `FS_DiskInfo` (84 bytes) |
| 0x100f | DELETE | path | result |
| 0x1010 | GETATTRIBUTES | path | attributes (read-only 0x01, dir 0x10) or error |
| 0x1012/13/14 | FINDFIRST/NEXT/CLOSE | pattern, attribute, mask, name length | handle or error, `FS_DOSDirEntry` (52 bytes), name |
| 0x101c | SETDISKFLAG | none | result (0) |
| 0x1021 | RESTORE | path | result: from the raw `nvram` partition's backup on Android; success here when the file exists |
| 0x1022 | CMPT_READ | path, `CMPTRead` | (steps done, error), file size, bytes read, data |

Also defined but not seen: CLOSEALL 0x1006, REMOVEDIR 0x1008, RENAME
0x100b, MOVE 0x100c, COUNT 0x100d, OPENHINT 0x1011, LOCKFAT 0x1015 ...
GETCLUSTERSIZE 0x101b, OTP 0x101d-0x1020,
BIN_REGION_ACCESS 0x1023, CMPT_WRITE 0x1024, GETFILEDETAIL 0x1025.
Flags and errors are MediaTek's FS API (`FS_READ_ONLY` 0x100,
`FS_OPEN_SHARED` 0x200, `FS_OPEN_NO_DIR` 0x400, `FS_OPEN_DIR` 0x800,
`FS_CREATE` 0x10000, `FS_CREATE_ALWAYS` 0x20000, `FS_COMMITTED`
0x1000000, `FS_PROTECTION_MODE` 0x20000000; `FS_FILE_NOT_FOUND` -9,
...).

`CMPT_READ` is open, size, seek, read and close in one request.
`CMPTRead` (40 bytes): Operation (steps: open 1, size 2, seek 4, read 8,
close 0x10; the modem sends 0x1f), Return0, Return1, OpenFlag (0x700:
read-only, shared, no directory), FileSize, Offset, From, Ptr (the
modem's buffer), Length, BytesRead. The modem reads one record after a
file's 64-byte header: offset 0x40 or 0x40 plus a record, length one
record (`IM56_001`: 0x28 at 0x68). The reply's first pack is the steps
done and the error as two words.

Requests and replies larger than one packet (3476 bytes) go in
fragments: CCCI header word 0 bit 31 on all but the last, each fragment's
payload starting with the operation word; `mdd.py` reassembles and
fragments.

Measured (2026-11-03, 30-45-s runs):
- **On the phone's NVRAM:** the modem reads its index (`INFO_FILE`),
  then 998 records through `CMPT_READ`, writes 15 times, sets its disk
  flag, and is ready (handshake 2) 0.31 s after the runtime data, with
  no assert.
- **On an empty root:** it builds a new NVRAM of its own defaults (324
  files, about 580 KB, 1,800 requests in 1.5 s). That run predates the
  FirmWire op table and stopped afterwards; not retried since.

How the NVRAM stop was found: with `CMPT_READ` answered in a guessed
shape (a 4-byte result first), both boots stopped 2.97 s after the
modem's last file request (an address error whose bad address was the
text `iidb_idb` from a context block on the first, `nvram_main.c:3176`
on later ones), whatever else the AP changed (seven reply shapes, the
memory-region features, META boot, the RAT bitmap; the rings showed
every reply consumed). FirmWire's table showed the reply starts with an
8-byte pack (steps, error); with it, the NVRAM start passed at once.

## After handshake 2

**The CCB** (cross-core buffers: the DHL log, the MD monitor and META,
in the first 1.5 MB of the cached region; their control block, an
array of 64-byte `buffer_header`s, at non-cached + 96 KB). The driver
sends the vendor's four words (control block address and 4 KB; the CCB
data address and the users' total, 0x178000 = DHL 0x70000 + MDM 0x8000
+ META 0x100000 from the MT6771 `ccb_configs`, each rounded to 4 KB).
The control block needs one header per buffer before the modem starts.
Android's `ccci_mdinit` writes them (`ccci_ccb_init_users` in
`libccci_util.so`, read from the stock vendor image's disassembly; the
vendor kernel does nothing for it): for each of the 20 `ccb_configs`
entries, in order, a 64-byte header of two 32-byte halves, DL (modem to
AP) then UL. Each half is zero except its last three words: page size
(+0x14), buffer size (+0x18) and the pattern `0xeeff0011` (+0x1c).

| Entries | Buffer | DL page / size | UL page / size |
|---|---|---|---|
| 0 | DHL control | 1 KB / 32 KB | 1 KB / 32 KB |
| 1 | DHL exception | 20 KB / 120 KB | 128 / 128 |
| 2 | DHL PS | 512 / 36 KB | 1 KB / 32 KB |
| 3-5 | DHL hardware loggers | 512 / 32, 32, 128 KB | 128 / 128 |
| 6-16 | DHL padding | 128 / 128 (entry 16: 640) | 128 / 128 |
| 17 | MD monitor | 512 / 16 KB | 1 KB / 16 KB |
| 18 | META | 65 KB / 520 KB | 63 KB / 504 KB |

The DHL entries add up to 0x70000, the users' total to 0x178000.
Without the headers the modem stamped `0xaabbccdd` on the first word,
waited about 3 s and its shared-memory buffer manager asserted
(`ccci_shm_bm.c:584`, params 0, 0, 0), right after reading `NA77_005`
and before handshake 2. With 19 headers (one padding entry short) it
stamped every half and the empty 20th slot, then asserted the same way:
it knows the count itself. With all 20 it stamps each half, then
rewrites the stamp as `0xddbbccaa`, and boots to handshake 2 (0.35-0.4 s
after the runtime data; 60 s run, no exception; AT as below). The CCB
buffers stay empty until a logger starts logging (Android's `mdlogger`
over `/dev/ccci_ccb_*`; not done here). The DHL raw buffer fills with
about 2 MB of nonzero data in a minute (not decoded).

The driver writes the headers (`init_ccb_headers`, after clearing both
regions) and offers the CCB by default; `ccb=0` leaves it out,
`ccb_users=` sets the users' total. It also offers DHL raw (the cached
region after the first 2 MB) and CONSYS (empty, at the end of the
cached region, as the vendor gives it when LK has no cached layout
table: an earlier version overlapped it with the CCB users), and clears
the cached region before each boot, as the vendor's first boot does (it
held untouched DRAM, mostly 0xff).

Then the modem talks on (`inc/ccci_core.h` names):

| Channel | Name | Seen |
|---|---|---|
| 0 | CONTROL_RX | handshake 2 (boot ready) |
| 4 | PCM_RX | header-only packets |
| 10 | UART2_RX | AT responses and URCs: `+EIND: 64`, `+EIND: 128` |
| 34 | IPC_RX | one IPC message |
| 57, 59 | IMSA_DL, IMSDC_DL | IMS |
| 169 | IMSM_RX | IMS manager |
| 171 | WOA_RX | Wi-Fi offload: `wostatusget=0` |

**AT:** commands go out on channel 12 (UART2_TX) as plain text with a
CR (`AT_CMDS="AT;AT+CFUN?"` in `mdd.py` sends them 3 s after handshake
2). The modem echoes each character in its own packet, then answers:
`ATI` gives `MTK2` / `MOLY.LR12A.R3.MP.V162`, `AT+CGMR` the version and
build date (2021/05/11), `AT+CFUN?` **0** (minimum functionality: radio
off), `AT+CPIN?` `+CME ERROR: 14` (SIM busy, with the radio off),
`AT+COPS?` 0, `AT+CSQ` 99,99. Only read-only queries were sent, none
that print an identity (IMEI, IMSI, ICCID).

Driver changes for this: the CCB headers (see above); the
`MISC_INFO_SBP_ID` feature now carries the image's RATs, as the vendor's
`get_wm_bitmap_for_ubin()` does (0x3f for this type 12); a `boot_id`
parameter (the boot mode, for experiments); debugfs `ccb` (the cached
region) and a log line for CCIF channels 8-14. `mdmock.sh` saves the
exception record, all of the non-cached region, the CCIF SRAM and every
ring's pointers (`tools/modem/rings.py`) after each run.

## In tree, MD1 as a power domain, crashes

**2026-11-10: runs on the phone (kernel #124, `boot-6.12-124.img`);
stop/start cycles, suspend with the modem on and the crash path are not
yet tested.** It needs a kernel and DT built from the same tree (the power
domain is built in). Measured on the first boot of #124:

- the probe (6.5 s), with no power-domain or bus-protection error from the
  platform core's switch of MD1 at probe;
- the service's open at 11.2 s: MD PLL version 0x20160627, released;
- handshake 1 at 11.3 s and handshake 2 at 11.55 s, all 24 mux channels;
- `pm_genpd_summary`: `md1 on` while the service runs;
- ModemManager registered on LTE, and `titan-ims` registered.

The old out-of-tree module on #124 fails as expected: no `mediatek,scpsys`
syscon, `-ENODEV`.

**The driver in the tree.** `drivers/net/wwan/mt6771/titan_modem.c`
(`CONFIG_MTK_MT6771_MODEM=m`, in `titanpocket.config`; Kconfig selects
`WANT_DEV_COREDUMP`). `modules/modem/build612.sh` builds it and `wwan.ko`
from the tree for `tools/modem/install.sh`. The out-of-tree copy is kept in
`out/modem/out-of-tree-driver/`. The module carries the DT node's alias, so
udev loads it at boot. Probing only maps and allocates: the CCIF and
watchdog interrupts stay disabled, nothing is powered, and
`/dev/titan-modem` appears last.

**A session per open.** The first open of `/dev/titan-modem` (one user at
a time) runs `md_start`. It clears what the last session left (flags, the
handshake state, the AT mux, the packet queue), lays out the rings, resets
CCIF and CLDMA, and puts CLDMA's descriptors back in place. Then it powers
the modem on, releases it and starts CLDMA. The close runs `md_stop`, which
does the same in reverse: the WWAN AT ports go, MD1 powers off and the 26
MHz request drops. The `ccmniN` interfaces stay registered across sessions.
So restarting the service restarts the modem without unloading anything,
and a crash ends with a fresh boot from the same image in memory. The
out-of-tree driver already rebooted that image on each `insmod`.

**MD1 as a power domain.** The scpsys power-domain driver
(`drivers/pmdomain/mediatek`) gained `mediatek,mt6771-power-controller`:
MT8183's table, with the same indices and registers and the CONN
`ACTIVE_WAKEUP` our tree had added, plus `MT6771_POWER_DOMAIN_MD1` (15,
`dt-bindings/power/mt6771-power.h`). The modem's sequence does not fit the
generic one: its clocks are gated in topckgen, and its SRAM powers up with
no ack before the isolation is released. It has an extra SRAM isolation
(0x394) and a buck isolation (0x398), and the vendor releases its bus
protection without waiting. So it has its own cap, `MTK_SCPD_MD_PWRSEQ`,
running the sequence the driver used to open-code (`scpsys_md_power_on` and
`_off`):

- **The registers come from data:** `md_sram_iso_offs`,
  `md_clk_mode_mask` and the existing `ext_buck_iso_*` fields. The domain
  node's `mediatek,topckgen` phandle reaches the clock gates.
- **Bus protection** is three `BUS_PROT_WR_IGN` entries, AP to MD (bit 7),
  MD to AP (`_1` bit 6) and step 2 (bits 3-4). Power-on releases them in
  the driver's old order. Power-off sets them in a slightly different order
  (MD to AP before step 2), each waiting for its ack.
- **The caps:** the domain is `KEEP_DEFAULT_OFF` and `ACTIVE_WAKEUP`.

The driver switches the domain with `pm_runtime_resume_and_get` and
`pm_runtime_put_sync`. On suspend it calls `device_set_wakeup_path`, so
genpd keeps MD1 on while the modem runs. The modem node lost its
`mediatek,scpsys` and `mediatek,topckgen` phandles and gained
`power-domains`. The binding documents the new compatible and property.

The gates live in the domain rather than the driver because the platform
core switches a device's domain on before probe and off after it. With the
gates in the driver, that cycle would run with the modem's clocks gated.
Inside the domain, every switch keeps the vendor's order: ungate, power on;
power off, gate. Whether that probe-time cycle is harmless to an unbooted
modem is one of the things the phone test must show.

**The exception handshake** (vendor `modem_sys1.c` `md_cd_ee_handshake`,
`fsm/ccci_fsm.c` `EXCEPTION_EE`, `fsm/ccci_fsm_ee.c`). The steps:

1. The modem raises CCIF channel 16 (`D2H_EXCEPTION_INIT`) on the second
   CCIF interrupt.
2. The driver stops CLDMA and acks on channel 16 (`H2D_EXCEPTION_ACK`).
3. It waits for channel 17 (init done) and 18 (queues cleared), then 2 s
   for the modem's log to flush, as the vendor does.
4. It resets CLDMA and moves the CCIF queues onto the exception rings (881
   KB into the shared memory), which it empties, with sequence numbers back
   to 0.
5. It acks on 17 (`H2D_EXCEPTION_CLEARQ_ACK`) and waits for 19 (all queues
   reset). Each step waits up to 5 s; the modem's own limit is 10 s.
6. On the control channel, the modem sends `MD_EX` (id `0x45584350`),
   which the driver answers with the same message. Then `MD_EX_REC_OK` (id
   `0x45524543`), whose payload is kept, and `MD_EX_PASS`. The driver
   waits up to 10 s for each.

**The modem's watchdog.** Its interrupt (SPI 283, the vendor's
`IRQ_MDWDT`, now `md-wdt` in the node) is armed after the release, as the
vendor does. It is ignored once an exception is being handled. A watchdog
has no handshake.

**After either:** the driver decodes the exception record into one log
line, as before, and saves a core dump. The session is then dead: reads
fail with `-EIO`, and poll reports `POLLERR` and `POLLHUP`. Writes, the
ioctl and the AT ports fail with `-EIO`.

**Recovery.** `titan-mdd` exits with status 3 on `-EIO`. Its close powers
the modem off, and `titan-modem.service` (`Restart=on-failure`, at most 3
starts in 10 minutes) boots it again 10 s later. Whether ModemManager,
`titan-net` and `titan-ims` pick the modem up again by themselves after
such a restart has not been tested.

## Crashes

The crash logger is a devcoredump (`dev_coredumpv`): the kernel's standard
way to hand a device's crash state to userspace. A dump is `TMDCORE1`, a
version, then sections:

- `reason`: watchdog or exception, and which handshake steps and messages
  arrived;
- `rec_ok`: the modem's `MD_EX_REC_OK` payload, up to 16 KB;
- `ccif_sram`: the CCIF SRAM;
- `mdccci_dbg`: the shared memory's first 2 KB;
- `mdss_dbg`: the 10 KB exception record.

`82-titan-modem-core.rules` runs `save-core.sh` for each new devcoredump of
the `titan-modem` driver. The script copies it to
`/var/lib/titan-modem/crash/<time>.core` (mode 600, the newest 10 kept),
frees it and logs the path. `tools/modem/mdcore.py CORE [--dbginfo
DbgInfo_<build>] [--hex SECTION]` prints the reason and the decoded record
(file, line and parameters of an assert; codes, pc and lr of a fatal error,
named from the symbol file when it is given). It was checked on a made-up
dump. A dump holds whatever the modem had in those areas: it stays on the
phone (or in `out/`), never committed.

**Testing it.** `echo 1 > /sys/kernel/debug/titan-modem/force_assert`
makes a ready modem assert the way the vendor's `force_assert` does
(`CCIF_INTERRUPT`: CCIF channel 21, `AP_MD_SEQ_ERROR`, raised from the AP
side). It drops calls and data until the service has restarted the modem,
and has not been tried.

**Not a trace logger.** The modem's running log (DHL, on the CCB) can't be
decoded: its trace database is encrypted (see "Modem debug data"). So the
logger keeps what can be read: the exception record and the debug areas
around each crash.

## AT port and ModemManager

When the daemon asks (`TITAN_MD_AT_PORT`, `_IO('t', 1)` on
`/dev/titan-modem`, after handshake 2) the driver starts the mux on the AT
channel and registers each DLCI with the kernel's WWAN framework
(`wwan_create_port`, `WWAN_PORT_AT`, parent the modem's platform device):
`/dev/wwan0at0` to `wwan0at23` (see "The AT channels on Linux"). Until then the AT
channel goes to the daemon like any other. Frames go out as CCCI packets on UART2_TX
(12); UART2_RX (10) packets lose their header and go through the mux to the ports,
or are dropped while a port is closed.
That channel no longer reaches `/dev/titan-modem`. The WWAN core is
`wwan.ko` from the same kernel build (`CONFIG_WWAN=m`; `build612.sh`
copies it).

`tools/modem/install.sh` puts the driver in the module tree (`updates/`;
until 2026-11-10 it was blacklisted, because loading it booted the modem;
now the open of `/dev/titan-modem` does, and install.sh removes the
blacklist), `mdd.py` and `nvcopy.sh` in `/usr/local/lib/titan-modem`, and
`titan-modem.service` (not enabled): it takes a wake lock, copies the NVRAM
to `/run/titan-modem` and runs the file service, whose open boots the
modem, until stopped; its close powers the modem off, and the copy is
dropped. With
`AT_PORT=1` (the unit's), `mdd.py` waits for the modem's AT layer (`+EIND: 128`, about
1 s after handshake 2; a command sent at handshake 2 is ignored), then asks for the ports
(after 10 s at the latest), and `radioinit.py` brings the SIM up with the radio off.

Debian's ModemManager 1.20.4 (`apt install --no-install-recommends
modemmanager`; no NetworkManager, so nothing enables the modem) takes the
port with its generic plugin: manufacturer `MTK1`, model `MTK2`,
firmware `MOLY.LR12A.R3.MP.V162, 2021/05/11`, GSM/UMTS + LTE, 2G/3G/4G,
IPv4/IPv6, power state off. Stopping and starting the service removes
and re-adds the modem (udev hot-plug); a port that existed before
ModemManager was installed needed `udevadm trigger` once. Measured over
the port by hand first: `ATE0`, `ATI`, `AT+CFUN?` (0), `AT+CSQ` (99,99),
`AT+GCAP` (`+CGSM`).

The measurements in the next paragraphs are from before the mux, when `AT+CFUN=4` was the first
command. At `CFUN=0` the SIM stays down: `AT+CPIN?` gives `+CME ERROR: 14` (SIM
busy) and ModemManager gave up after six tries (`failed`, reason
`sim-missing`). `AT+CFUN=4` (flight mode) brings it up within the
command: the modem answers with a burst of SIM URCs (`+EUSIM`,
`+ESIMIND`, `+ESIMAPP`, ...), then `+CPIN: READY`, `+ESIMS: 1` (SIM
inserted), `+CSQ: 99,99` (no signal: the radio stays off). ModemManager
then initialises the modem to `disabled`, power state `low`, every time
the service starts.

**Identities.** The SIM URCs carry the IMSI and the IMS identities, and
ModemManager reads the IMEI. They stay on the phone: `mdd.py` masks
quoted strings and long digit runs in the AT text it logs and, with
`NV_QUIET` (the service), logs no packet payloads; ModemManager at its
default log level does not print them (at `DEBUG` it does). `mmcli -m`
prints the IMEI and `mmcli -i` the SIM's: filter them.

## Speech (voice audio)

The modem's voice runs through the AFE's internal modem PCM interface
(PCM 2) and is controlled with MediaTek's speech messages on CCCI
channels 5 (AP to modem) and 4 (modem to AP), Android's `/dev/ccci_aud`.
The protocol was read from the stock audio HAL (`audio.primary.mt6771.so`,
`SpeechDriverNormal` / `SpeechMessengerNormal`, disassembled; vendor
userspace, not the modem firmware):

- **Mailbox:** a bare CCCI header, `d0` 0xffffffff, `d1` = id << 16 |
  16-bit parameter, reserved = 32-bit parameter.
- **Payload:** `d0` 0, `d1` the packet length (the vendor kernel fills it
  in), reserved = id << 16 | (size + 6), then sync (0xa2a2 from the AP,
  0x1234 from the modem), data type, size (the modem adds page index and
  count), data.
- Ids: AP to modem 0x2fxx, modem to AP 0xafxx. The modem acks an AP
  command with id | 0x8000; the AP acks the modem's 0xaf78, 0xaf90,
  0xafa0 and 0xafb5 with id & 0x7fff (the HAL's `getSyncType` table).
- At boot the modem sends 0xafa0 (alive; answered with 0x2fa0) and 0xaf70
  (a notification).
- 0x2f20 speech on, payload type 0x19: `sph_info_t`, 128 bytes:
  application (+0; 0 normal, 2 loopback), Bluetooth (+1), sample rate
  enum (+2; 0-3 = 8, 16, 32, 48 kHz, the HAL's default 2), parameter
  location in the speech shared memory (+4..+9, +0x14..; "valid" 0 = none
  sent), loopback flag (+0xc; the HAL uses 3) and delay, echo and mic
  delays, mute info, enhancement mask, smart PA count (+0x54).
  0x2f21 speech off. 0x2f00/0x2f01 downlink/uplink gain, 0x2f02/0x2f03
  uplink/downlink mute, 0x2f08 uplink source mute, 0x2f09 downlink codec
  mute (the value in the 16-bit parameter); 0x2f13 sets the modem's
  loopback point.
- **The speech shared memory** (`AUDIO_RAW_SHARE_MEMORY`, 52 KB) is
  formatted once per boot by the AP (`SpeechExtMemCCCI`): 0x0a guards
  (32 bytes) at both ends, AP flag (+0x20, bit 0 = formatted), modem flag
  (+0x24), three rings (offset, size, read, write): speech parameters
  (0x80, 12 KB), AP data (0x3080, 8 KB), modem data (0x5080, 0x7f60).
  The driver now does this before each boot (`init_audio_shm`); the
  modem sets its flag to 1.

`mdd.py` answers the modem's requests and, with `SPH_TEST=loopback`,
turns speech on 8 s after handshake 2 for `SPH_SECS` (`SPH_APP`,
`SPH_RATE`, `SPH_LOOPBACK_FLAG`, and `SPH_AFTER_ON="2f03:0,2f13:1"` for
messages after the ack). The modem acks every one of these, with no
exception.

**Measured** with `tools/modem/sphloop.sh`, which makes no sound: a 1 kHz
tone (-20 dBFS) from `Playback_2` (DL2) into the modem's uplink
(`PCM_2_PB_CH1 DL2_CH1`), and `Capture_2` (UL2) recording its downlink
(`UL2_CH1 PCM_2_CAP_CH1`, a route added to the AFE driver), nothing to
the codec or the amp:

| Loop | 8 kHz | 32 kHz |
|---|---|---|
| speech on (app 2, flags 0-3, or app 0) | digital zeros | zeros |
| + loopback point 1 (`2f13:1`) | tone back at -20.0 dBFS, peak 3277 (the input's) | the same |
| + loopback point 2 | zeros | zeros |

Point 1 loops at the modem's PCM interface, so the AFE and the modem
exchange audio both ways.

### Speech parameters

The HAL's parser for this modem generation is `SpeechParserGen93`, inside
`audio.primary.mt6771.so` (the separate `libspeechparser_vendor.so` is for
other generations and rejects this phone's XML). It builds one blob from
the XML audio parameters in `/vendor/etc/audio_param` (`Speech`,
`SpeechDMNR`, `SpeechGeneral`, `SpeechMagiClarity`, `SpeechNetwork`;
`AudioParamOptions.xml`: `SPH_PARAM_VERSION` 2.0, so network support on
and seven speech parameters). `tools/modem/sphparam.py` does the same:

- **Units** in this order, each a 16-byte header then data:
  Speech, DMNR, General, MagiClarity (Dereverb when supported, not here).
  The header's words: 1, 2 (General/MagiClarity: 1, 1, 1, 1), counts
  (low nibble networks or profiles, high nibble bands), a kind (0x1f
  Speech, 3 DMNR), the band mask (1, 3, 7, 15 for 1-4 bands), the XML
  version (major << 4 | minor, 0x20), 0, a magic (0xaa01 Speech, 0xaa02
  General, 0xaa03 DMNR, 0xaa04 MagiClarity).
- **Speech:** then volume index << 4 | mode (0 handset, 1 headset,
  2 speaker, 3 BT), then for each network and band a data header (band
  0x1000/0x2000/0x3000 | the network's `speech_network_support` bits; this
  phone has one network, `GSM`, 0xfff) and the raw values of
  `speech_mode_para`, `sph_in_fir`, `sph_out_fir`, `sph_in_iir_mic1_dsp`,
  `sph_in_iir_mic2_dsp`, `sph_in_iir_enh_dsp`, `sph_out_iir_enh_dsp` for
  the category path `Default,<band>,<profile>,<volume>,<network>` (the
  most specific `ParamTree` path that matches).
- **DMNR:** for each band i and profile j (`Handset`, `MagiConference`),
  (i + 1) << 4 | (j + 1), then `dmnr_para`. **General:** 0xf, then
  `speech_common_para` and `debug_info`. **MagiClarity:** 0xf, then
  `shape_rx_fir_para`.

On this phone's XML the blob is 3280 bytes (Speech 2136, DMNR 988,
General 74, MagiClarity 82). It goes into the shared memory's parameter
ring (the AP flag's bit 1 set while writing) at the write index, which
moves on by its size; speech on then carries parameter path 1, valid 1,
the size (+6) and that index (+8). The driver maps the speech shared
memory for this (`mmap` of `/dev/titan-modem`, as Android's
`/dev/ccci_raw_audio`); `mdd.py` writes `SPH_PARAM=file` before speech on.
The blob is derived from vendor tuning data: generated where needed, not
committed.

**The modem takes it:** its read index moves to 0xcd0 (= the write index,
3280) and its speech-on ack carries 0x0cd0. **The DSP loops stay
silent:** with the parameters (the loopback profile `Lpbk_Handset`), at 8,
16 and 32 kHz, loopback flags 2 and 3, all mutes off (0x2f02, 0x2f03,
0x2f08, 0x2f09, 0x2f0a), gains 0 (the HAL's gains are attenuations,
"degradeDb"), enhancement on or off (0x2f23), both PCM channels, and with
the PCM running before speech on: digital zeros on both downlink
channels. Of the loopback points 0-6 only 1 returns the tone. The DSP
image is where the md1rom check header says (+0x7000000, 14 MB; its
`MMM` header is there, debugfs `dsp`), and the modem writes about 2 MB
of DSP log a minute into DHL raw. Not yet known what the speech DSP
waits for.

## Radio on (first try)

2026-11-03, with the user's go-ahead for a test call: `mmcli -m 0 -e`
(`CFUN=1`). The power state went to on and registration to searching;
right after, the modem wrote `NA77_005` (X: and Y:) and `D2A0_000`
through the file service, then asked the AP for nothing more, and 43 s
later asserted in its LTE RRC connection handling
(`lte_sec/errc/chm/src/errc_chm_main.c:8825`, params 0, 1, 0; `chm` is
the module that builds the RRC connection's configuration, so it had
found a cell). ModemManager's commands then timed out. The SIM in the
phone is not a valid subscription (the user, after the test), so it
could not have registered; whether the assert comes from that or from
something the AP lacks is open. The RF clock buffer was checked: the
cellular buffer (MT6358 XO buffer 4) is enabled and not in low-power mode
(`DCXO_CW00` 0x4e1d, `DCXO_CW23` 0), as the vendor's clock-buffer driver
leaves it outside flight mode. The radio was turned off again by
restarting the service (flight mode). For a call, `mdd.py` turns speech
on and off on SIGUSR1/SIGUSR2 (application 0, parameters from
`/var/lib/titan-modem/sph-params.bin`); untested.

## Radio on with a valid SIM

It registers now; the end of this section says why. Most of the section is the search, in order, and
its earlier theories are kept as they were measured.

2026-11-03, kernel #101, an activated Tello SIM (T-Mobile's network,
`310260`), measured with ModemManager stopped and AT commands by hand on
`/dev/wwan0at0`, cell IDs masked:

- **Receive works:** camped at RSRP index 55-58 (about -86 to -83 dBm), RSRQ
  index 16-25; `AT+COPS=?` lists four LTE networks, this one included.
- **It does not attach:** `AT+COPS=0` or a manual `AT+COPS=1,2,"310260"`
  ends after 48-70 s in `+CME ERROR: 100` with `+CEER: 0,NONE`, `CGATT` 0.
- **`AT+EDALLOW=1` changes it.** Without it registration goes 2 to 0; with it
  (the vendor RIL's string: "Single SIM project, send EDALLOW=1 at boot
  time") the modem reports `+CEREG: 3` (denied) with `cause_type` and
  `reject_cause` 0, `nwk_existence` 0 in `+EREG`. A network reject would
  carry a cause. ModemManager's generic plugin sends no MediaTek commands.
- **No effect:** the deep-sleep image against `regulator_ignore_unused` with
  no low-power table; a profile (`AT+CGDCONT=1,"IPV4V6"[,"wholesale"]`,
  `AT+EIAAPN="wholesale",3`); `AT+EGTYPE=4`; `AT+CEMODE=2`; `AT+EOPS=0`.
- **Ruled out:** network and SIM locks (`AT+CLCK` 0), the SIM's forbidden list
  (only `310830`), the file service: over one modem boot and attach, about
  1300 requests of which 1050 are compact reads, the only failures are a stale
  `.log` attribute check and one missing `NVD_DATA/SP81_046`, asked for by a
  delete and a compact read. `CALIBRAT` holds its 173 files. `mdd.py` now logs a
  failed compact read by name (its reply is hidden in the quiet log).
- **Supplies:** `VFE28`, `VRF18` and `VRF12` switch under the modem's control
  (status c000/0000 with `SRCLKEN1`, software enable off). **`VPA` reads 0 in
  `CON0`, `DBG0` and `DBG1` in all 164221 sweeps (0.55 ms each) of a camped
  attempt.** Either nothing was transmitted or this board's PA does not use it:
  not known which.

- **No RRC connection:** with `AT+CSCON=1` only `+CSCON: 0` ever arrives,
  several at the end of an attempt. RRC connected needs a round trip with the
  cell, so the network's reject is not what ends it. The denial comes about 40 s
  after the attempt starts, every time (the first radio-on's assert was at 43
  s): the standard's 5 attach attempts 10 s apart (T3411) would take that long,
  each failing below EMM. That timing is an inference.
- **The modem is meant to drive `VPA`:** its symbols include
  `EL1D_RF_VPA_ENABLE`, `MML1_RF_VPA_Enable`, `MML1_RF_VPA_VOSEL_CWGen` and
  `MML1_RF_PMIC_VPA_Status_Check` (names only, from `DbgInfo`). The vendor kernel
  leaves the rail off (`pmic_buck_vpa_lp(SW, 1, SW_OFF)`, no consumer).
- **Also checked, no fault:** RF pins (`BPI_ANT*`, `BPI_BUS3-10`, `MIPI0/1`,
  `RFIC0_BSI`) in mode 1 with nothing claiming them (mode is the low 3 bits of
  each nibble at `GPIO_BASE + 0x300`); bands 2, 4, 12, 66 enabled (`AT+EPBSE?`);
  the vendor driver's `mdcldma_pinctrl` is never selected; channel 34
  (`res 80000003`, `AP_IPC_WMT`, probably one-way coexistence info) and 38 come
  at radio on and off, not at the 40 s mark. The wrapper's `HPRIO_ARB_EN` reads
  0x3fa75 (the vendor kernel's own init would write 0x3fa65; not explained,
  and our driver does not write it unless it initialises the wrapper).

- **`VPA` forced on makes no difference:** `modules/modem/vpatest` (a one-off,
  75 s limit) held it at 3.4 V (status 3, `DA_VOSEL` 0x3a) through an attempt:
  denied at 40.4 s, no RRC connection. The voltage select never moved in
  96802 sweeps, so the modem wrote nothing to it. It is not the missing
  supply; the modem stops before driving the PA, or its PMIC writes do not arrive.
- **Also no difference:** `AT+EBOOT=1`, `AT+EMODCFG=1`, `AT+EFUN=1`; the SPM
  firmware running with all rails on (`deep=1` with `regulator_ignore_unused`,
  image `boot-6.12-radio-spm.img`), with `EDALLOW`: `SPM_DVS_LEVEL` and
  `SPM_SW_DEBUG` unchanged, so no DVFS event served.
- **The same as the vendor:** the power-up (`md_cd_power_on`, the MD PLLs, the
  26 MHz request); the RPC `IPC_RPC_DTSI_QUERY_OP`: LK's `md_attr_node` holds
  `0f0f0f0f`, which the vendor returns too, and this firmware asked only for
  `md_product_name_model_id`. Whether LK writes the same under Android is not
  measured.

**Under stock Android (same phone, same SIM, 2026-11-03, the phone's Magisk boot
image, `userdata` formatted): it registers.** So the hardware, the SIM and the
carrier work, and the gap is on the Linux side. From the radio log (filtered on
the phone: commands only, identities masked):

- Android never sends `AT+CFUN=1`. Radio on is `AT+EGTP=1`, `EMPPCH=1`,
  `EDSDA=1`, `ESADM=3`, `EBOOT=0`, `EDSBP=1`, `EPOC`, `EFUN=0`, `ESLOTSINFO=1`,
  `EFUN=1`, `ERAT=6,4` on one channel, 0.4 s in all.
- 31 s later: nine `AT+EAPNSET`, then `AT+EIAAPN` (APN `fast.t-mobile.com`, id 0,
  for this MVNO SIM too); `+EIAREG: ME ATTACH` follows 1.3 s after it.
  `EDALLOW` comes after the attach.
- Android has two sets of AT channels, one per protocol stack, and its URCs on
  their own channels; Linux has one port.
- It gets the same `CME ERROR 100` as Linux for `CSCON=3` and `ECAMPUI=1`. It
  gets `OK` for `ESADM=3`, where the Linux port gets `ERROR`.

**Android's AT channels are a 3GPP 27.010 mux** on CCCI UART2, the channel the
WWAN port carries: `gsm0710muxd -s /dev/ttyC0` (`ttyC0` is `CCCI_UART2`,
`port_cfg.c`), `AT+CMUX=%d,%d,%d,%d`. The daemon's table entries are
`{dlci, mtu, name}`, and the DLCIs are not sequential:

| Channel | SIM 1 | SIM 2 |
|---|---|---|
| `cmd4`, `noti`, `cmd1`-`cmd3` | 1-5 | 6-10 |
| `atci` | 26 | 27 |
| `cmd7` (RIL `RT_7`), `cmd8` (`NRT_8`) | 61, 60 | 57, 56 |
| `cmd9` (`RADIO_9`), `cmd10` (`DATA2_10`), `cmd11` | 59, 58, 43 | 55, 54, 42 |
| `nwcmd`, `nwurc` | 45, 44 | shared |

Measured: `/proc/ccci_dump` under Android shows `AT+ESBP` going out in a frame with
address `0xED`, DLCI 59. Under Linux `tools/modem/muxtest.py` opens all 24, and the
stacks are as the table says: `AT+CPIN?` is `READY` on DLCI 3 and `CME ERROR 10`
on 8. DLCI 11 and up are the third SIM's (refused, as this phone has two).

**The kernel side matches Android's where compared.** Android's `ccci_dump` (root,
filtered on the phone) has `sbp=0x0, wmid[63]`, `c2k_flags 0x4`, and the runtime data at
`0x4000e800` (AP) and `0x4000f000` (MD): what `titan_modem` sends. `+ESBPID: 0,8`
under Android is the modem's own choice after `EDSBP=1`, not boot data.
`ccci_mdinit` sends the modem only this boot data; its other strings are start,
stop and supervision.

**A real gap, closed, not the cause.** The modem asks for the SIM hot-plug interrupts
(`IPC_RPC_GET_EINT_ATTR_OP`, about 0.3 s after release). `mdd.py` answered all of them
with an error; Android's kernel returns values from its device tree. `mdd.py` now returns
those measured under Android: SIM 1 source pin 1, interrupt 0, dedicated 0, debounce
100 ms, polarity 0, sensitivity 0, socket type 0; SIM 2 source pin 2, interrupt 1. The
modem then makes the same follow-up queries as under Android (types 0, 0, 1, 2, 3, 4,
about 1.2 s later), which the errors had stopped.

**Attach under Android does not depend on the APN commands.** On later boots
`+EIAREG: ME ATTACH` comes 1.3 s and 1.7 s after `EFUN=1`, before any `EDALLOW`,
`EAPNSET` or `EIAAPN`. (The 31 s above was the first boot on a formatted `userdata`.)

**`AT+CFUN=4` as the modem's first command was the wrong state.** `titan-modem` sent
it as soon as the modem's AT layer came up, so every test above started there. Without it,
in Android's order on the right channels, the differences go: `AT+ESIMS` is `OK` with
`+ESIMS: 1`, `AT+ESADM=3` is `OK`, and `+ESIMS: 1,29`, `+EUSIM`, `+ESBPID: 0,8` and
`+EIND: 32, 2, 1` arrive. `tools/modem/radioinit.py` now does this start-up on `cmd9`: `ESIMS=1`,
`ESIMS`, `EGTP=1`, `EMPPCH=1`, `ESADM=3`, `EBOOT=0`, `EDSBP=1`, `EPOC`, `EFUN=0`.

**The modem now makes an RRC connection (2026-11-03).** `+CSCON: 1` within a second of
`AT+EFUN=1`, and within 3 s of ModemManager's enable (`AT+CFUN=1`). `VPA` switches on from about
0.7 s after `EFUN=1`, as under Android (`VPA_CON0` bit 0, on in about 4% of samples for the
15 s connected). Under `CFUN=4`-first, in the same replay, it never switched on in 62 s.

**It still does not attach.** The connection lasts 15 s, the length of the attach timer T3410,
then `+CEREG: 3` (denied), cause 0, `+ECCAUSE: 0,1,0`, `+CSCON: 0`. The reports Android gets
just before its attach (`+CEN1`, `+CEN2`, `+CIREPI`, `+CNEMS1`, `+EPDN`, `+EIAREG: ME ATTACH`) never
arrive. So the attach request gets no acceptance; whether the network answers at all is not
known. Tested, with the same result: `AT+EDALLOW=1` (no effect), `AT+EIAAPN` before radio-on (still
connects, still 15 s).

**`AT+EIAAPN` with no APN table causes the older symptom:** sent after `EFUN=1` with the table unset, in
the short or the seven-argument form, there is no RRC connection, and `+CEREG: 3` comes about 40 s after
radio-on.

**The APN table needs a lock.** From the RIL's `requestSyncApnTable` (`libmtk-ril.so`, read as
reference): `AT+EAPNLOCK=1`; `AT+EAPNSET` (clears the table); for each APN `AT+EAPNSET="apn",1,"user","pwd"`
and `AT+EAPNSET="apn",2,"type=a,b;protocol=P;roaming_protocol=P;authtype=N;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;bearer_bitmask=N;inactive_timer=0"`;
`AT+EAPNLOCK=0`. Then `AT+EIAAPN`. Without the lock `EAPNSET` gets `CME ERROR 5841`. Measured, with
`fast.t-mobile.com` and `ims`: all `OK`, and `+CSCON: 1` within 0.5 s, sent before or just after `EFUN=1`, so
with the table set `EIAAPN` no longer stops the connection. **The attach still times out at 15 s, cause 0.** The
values used for the type list and `bearer_bitmask` are not Android's (masked in its log). On Android's first boot it
did not attach for 31 s, until it set the table; on later boots it attaches 1.3-1.7 s after `EFUN=1`
without it. Why is not known.

Under Android, compared during an attach, with `mt_gpio` and `pmic_access` (root):

- Pin modes differ on 14 pins: camera clocks and audio or I2S lines that Linux has claimed.
- DCXO registers match except two live status registers and one bit each in two others. The
  capacitor trim (`0xd4`) is the same.
- `VMODEM` is 0x18 under Android, 0x20 under Linux. Not tested as a cause.
- LK's `md_attr_node` gives `0x0f0f0f0f` for `md_drdi_rf_set_idx` under Android too.
- `VPA_DBG0` reaches higher values under Android during its attach (`090e`, `111e`, `1d16`, `3c28`).
  In about 15 s under Linux: mostly `0506`, then `0f0a`, a few `090e` and `1d16`.

Android's calls to `AT+EGMR` before its attach are all reads (`0,4`, `0,5`, `0,9`, `0,13`, from the raw
frames in `/proc/ccci_dump`), and `ccci_mdinit` sends only the boot data compared above.

Not yet compared: the modem's NVRAM files under the two systems. `nvcopy.sh` mounts the partitions
read-only with `noload`, so a journal left by Android's unclean reboot is not replayed into the
copy, and `NVD_DATA/SP81_046` is missing in the copy. Android's first boot
may have written files the attach depends on. The NVRAM partitions from before that boot are in
`/root/android-test-backup` on the phone, with checksums: per-phone data, never copied off.

### The AT channels on Linux

`titan_modem` runs the mux itself (`mux=0` gives the one raw port, for testing). When `mdd.py` asks
(`TITAN_MD_AT_PORT`) it sends `AT+CMUX=0,0,5,512`, opens all DLCIs and makes a WWAN AT port for each:
measured 24 of 24. It answers the modem's MSC; the modem's flow control bit is not obeyed.
`81-titan-modem.rules` names them `/dev/radio/<channel>` and leaves ModemManager two: `cmd1`, the first
port (`wwan0at0`, its primary), and `noti` for URCs. With `noti` as primary, the modem's disable timed
out. `tools/modem/atreplay.py` replays a command list over the ports.

**Without CLDMA running, the modem asserts about 50 s after radio-on, when the attach has failed:**
`errc_chm_main.c:8825`, params 0, 1, 0, the same assert as on the earlier SIM. All channels then go
silent. It needs no disable: measured with the modem left enabled (ModemManager still says
`searching`). So enable, disable, enable after that point cannot work, and a restart of
`titan-modem` is needed. In one run the phone also stopped answering over USB and had rebooted when it
came back; the journal is not kept across boots, so why is not known.

Also ruled out as the attach's cause: Android's boot id `0xf0000` (a logger wait time, tested with
`boot_id=`), the journal (`nvdata`, `protect1` and `protect2` are clean, and a replayed copy matches
`nvcopy.sh`'s file for file), and the file service during the attach (all requests answered; the only
failure since boot is `SP81_046`). Android sends no AT command between `EFUN=1` and its attach.
The modem sends packets on channels 34 and 38 as it connects, and nothing on Linux reads them. Under
Android, 34 goes to `AP_IPC_WMT`, the kernel's bridge to Wi-Fi/BT coexistence (`mtk_conn_md_bridge_send_msg`), and
38 is `ttyC2`, held by `mtk_agpsd`. Neither looks necessary for an attach; not tested.

**Starting CLDMA fixed the attach (2026-11-04).** The vendor resets it before the modem's power-on and
starts it after the release (`md_cldma_hw_reset`; `cldma_reset` and `cldma_start`, from `ccci_md_hif_start`);
`titan_modem` had only enabled its clock. The driver now does the same: four idle uplink queues, one downlink queue of
64 buffers, no interrupts. With that alone, `+CGATT: 1` about 2 s after enabling, and ModemManager reports `registered`,
`home`, LTE. No APN commands were sent. Measured: five enables, four of them after a disable, registered in 2-3 s each
where timed, disables all clean, registered after 90 s, and no modem assert or exception in the boot. The modem wrote nothing to
the downlink buffers (0 of 64). Why it needs CLDMA running for an attach is not known. So the earlier "attach timeout" and
the `errc_chm_main.c:8825` assert were this, and the second enable's failure was that assert. The `VPA`, DCXO, SBP, journal,
`EDALLOW` and channel 34/38 comparisons above were not the cause; `AT+CFUN=4` as the first command and the mux are
separate: with `CFUN=4` first it did not reach an RRC connection at all.

ModemManager's signal quality read 60 on the first enable and 0 after later ones; not looked at.

The driver used to report CCIF channel 20 (the vendor's `AP_MD_PEER_WAKEUP`, for which it only takes a
wake lock) as a modem exception: the modem raises it about every 2 ms from a second after radio-on until
the assert (22403 times in 45 s). Exceptions are channels 16-19.

## Mobile data

Measured 2026-11-04 with the Tello SIM on 310260: 9 of 9 pings to a public IPv6 address (36-156 ms) and a TCP
connection in 53 ms, through a host route, with the phone's Wi-Fi routes untouched. No CLDMA errors. Soak, same day, with
`AT+CEMODE=2` (below): 780 of 780 pings at one a second, over 8 minutes of traffic, 2 more after 5 quiet minutes (RRC idle, then
connected again) and 3 more after a radio off and on (registered within about a second); no modem assert, and
USB up throughout. Not measured: long runs beyond that, and data across suspend.

**LTE stopped 60 s after radio on (fixed).** With the modem's defaults, every run lost data about 60 s after `mmcli -e`
(between T+50 and T+71 s, the last good and first bad 10 s samples), whatever the traffic: uplink packets still drained through CLDMA, downlink stopped,
`AT+CSCON?` went to 0 and `AT+CESQ` to 255/255, while `+CEREG` stayed 1 and the PDN stayed listed. Minutes later it read not registered,
and LTE signal returned. The modem reported nothing on `nwurc` or `nwcmd`, and asserted nothing. Measured with the same result: MCDI idle states
off, ModemManager stopped, and the SPM firmware not started (`mt6771_spm.deep=0`), with the SPM's request and mask registers unchanged across
the failure. The modem starts as `+CEMODE: 1` (voice-centric), `+CEVDP: 3`, `+CIREG: 0,0,0`, and two changes each held for the whole
two-minute run: `AT+CEMODE=2` (data-centric) and `AT+CEVDP=1` (CS voice only). This fits the standard's rule that a voice-centric
device with no voice service on LTE disables LTE, though only the settings and the result were measured; the modem's reason was not. `radioinit.py` sends
`AT+CEMODE=2`, the choice that does not rule out voice over IMS; a modem restart forgets it.

**The driver.** `titan_modem` runs CLDMA as the vendor's order has it: reset before the modem's power-on (the infra reset
registers), then after its release the MTU, the receive DMA enable (`SO_CFG` bit 0), the queues' start addresses, the start
command, and the interrupts (`GIC_SPI 173`, in the device tree with the `cldma-ao` and `cldma-pdn` registers). Uplink queue 0
has a ring of 128 descriptors and the other three an idle one. The downlink queue has 256 buffers of 3584 bytes. NAPI does
receive and transmit completion. An uplink descriptor names the interface (`netif`), and a downlink buffer starts
with a 4-byte `lhif_header` that does (bits 0-4 of its last byte); the payload is raw IP. `ccmni0` to `ccmni7` are plain
network devices (`ARPHRD_NONE`). The modem's request to move an interface to another uplink queue
(`IPC_RPC_CCCI_LHIF_MAPPING`, 0x400d) gets success from `mdd.py`, and every interface stays on queue 0. The vendor gives IMS
queue 3; not done here. debugfs `titan-modem/cldma` shows the registers and counters.

**The uplink ring can jam (2026-11-14 and 15, open).** Twice in 13 hours queue 0 stopped being served: all 128
descriptors owned by the hardware, none completing (`tx in flight 128` in debugfs), queue 0 active in `UL_STATUS`, the
current pointer on the oldest descriptor, `CLDMA_IP_BUSY` 1, no error bit. The second time was within 10 minutes of a
modem start, with no sleep in between. Data and VoLTE are dead from then on (titan-ims: "no connection"), the interfaces'
queues stop, and the modem holds `MD1_SRCCLKENA` and `MD1_APSRC_REQ` to the SPM without a break, so the phone's deep sleep
stops at DRAM self-refresh (power-rails.md). `systemctl restart titan-modem` clears it. Nothing detects
it yet. The modem had asked for interface 0 on queue 3 and interface 1 on queue 2 both times; whether keeping them on
queue 0 is the cause is not known.

**The commands (`ltedata.py`), from the RIL (`libmtk-ril.so`, read as reference) and Android's log:**

1. `AT+EDALLOW=1`: without it `AT+EAPNACT` gets `CME ERROR 5745`.
2. The APN table (see "Radio on with a valid SIM").
3. `AT+EAPNACT=1,"apn","default",0`: `+CGEV: ME PDN ACT 0`, then `AT+CGACT?` shows context 0 active.
4. On `nwurc`, `+EIF: 100, ifup, 2, <link-local address>/64, 0`: interface 100 is `ccmni0`, type 2 is IPv6. The AP brings the
   interface up with that address and answers on `nwcmd`: `AT+EIF=100, "ifst", "up", 2`. Then `+EIF: 100, mtu, 1428`.
5. The network's router advertisement gives the global address (`accept_ra=2`, `autoconf=1`).

Before step 4's answer no packet came back (13 sent, 0 received). `AT+CGCONTRDP=0` gives the APN (`wholesale...`) and
two IPv6 DNS servers; it gives no address on this network, which is IPv6 only. The carrier's DNS server does not answer ping.

IPv4 is under "Network switches". `ltedata.py` alone leaves DNS and routing alone unless given `--default`. Not done:
ModemManager knowing about the bearer (it stays `registered`, not `connected`), and a clean way down.
`AT+CGACT=0,0` on the only PDN takes the modem off the network (`searching`, then `CME ERROR 5873`), so `ltedata.py` has no
"down": restart `titan-modem`. An already active PDN gets no second `ifup`: restart it too. The APN is `fast.t-mobile.com` by default.
NetAgent's other `+EIF` reports (`ipupdate`, `ipchg`, `ra`, `ifdown`) are not handled, and `ltedata.py` exits once data is up.

## Network switches

`titan-net` (`/usr/local/bin/titan-net`, with `titan-net.service` as its daemon) holds the state in
`/var/lib/titan-net/state.json`. The Wi-Fi app has the Wi-Fi switch and Phone Settings has Airplane mode and Cellular data.

- `wifi off` disables and stops `wpa_supplicant@wlan0` and takes `wlan0` down, so it stays off across boots. Measured: on again,
  a route in about 6 s.
- `airplane on` turns off Wi-Fi, Bluetooth (`titan-bt`, `bluetooth`) and the modem's radio; off restores what was on. `wifi on`
  during airplane mode stays off until airplane mode ends. Measured for Wi-Fi and Bluetooth; the modem's part only on a modem that was off.
- `cell on` is the fallback, off by default: after Wi-Fi has had no default route for 15 s the daemon starts `titan-modem`, enables the
  modem and runs `ltedata.py --default`. `ccmni0`'s route has metric 1024 and `wlan0`'s 600, so traffic returns to Wi-Fi by itself.
  Measured with Wi-Fi switched off: DNS set about 25 s later, and `example.com` and `github.com` resolved through the carrier's DNS and
  completed TLS over IPv6; with Wi-Fi back, routes returned to `wlan0`.
- IPv4 over LTE is 464XLAT, in the daemon while LTE is up and the network gave no IPv4 of its own (it has given IPv6 alone on a fresh attach).
  It asks LTE's DNS for `ipv4only.arpa` (RFC 7050) for the carrier's NAT64 /96, then runs `tayga` (Debian package, `tayga.service` disabled)
  on a tun device `clat`, `192.0.0.1` here, with a random IPv6 address in LTE's /64. `clat`'s default route has metric 2048, behind `wlan0`.
  Delivering that address to the tun device needs IPv6 forwarding, a system-wide switch, so it is on only while `clat` exists; `wlan0` is
  unaffected (networkd handles its router advertisements). Measured with Wi-Fi off: 8 of 8 pings to `1.1.1.1`, TCP and UDP DNS to
  IPv4 addresses, and 3 MB by HTTPS over IPv4 at about 3 MB/s (24 Mbit/s). A name with only IPv4 addresses resolves to the
  carrier's IPv6 translation, which skips `clat`. Wi-Fi on: both families moved to `wlan0` within 4 s; off: back to `clat` and `ccmni0` in 3 s.
  `cell off` removes the device and tayga and restores forwarding; `cell on` had IPv4 back in 33 s. IPv4 packets marked don't-fragment are limited to
  1380 bytes, as tayga applies `clat`'s 1400 to the IPv6 side too (it reports the limit by ICMP, so TCP is unaffected). `titan-net status` shows `cell_ipv4`.
- If the PDN goes down while LTE is meant to be up (`AT+CGACT?` inactive at two checks in a row), the daemon takes down `clat` and the address and starts over.
- `cell off` stops `titan-modem` if the daemon started it, otherwise restarts it: taking down the only PDN detaches the modem.

The modem service holds a wake lock, so the phone does not deep sleep once fallback has started. The fallback has not been tested with Wi-Fi losing
its network while switched on, only with the switch.

## IMS

Voice on this network is IMS, and the SIP stack is the AP's job: on Android `volte_stack` and `volte_ua` run on the CPU, with
`volte_imcb` and `volte_imsm_93` between them and the modem (`ccci_imsc`, `ccci_imsm`), read as reference only and not shipped. Replaying
Android's AT commands with nothing on those channels gets the IMS PDN, and no registration: the modem does not register by itself.
`titan-ims` (`/usr/local/sbin/titan-ims`) is our own client, on TS 24.229 with AKA (RFC 3310) and sec-agree (RFC 3329, TS 33.203). It exists in two forms with the same behaviour and control protocol: the original Python (`userspace/debian/overlay/...`) and a Rust rewrite (`userspace/titan-ims-rs`, see "Rust port" below).
It logs status lines and a short list of safe headers: never identities, keys, addresses or SIP bodies.

**The modem's part.** With IMS enabled (`AT+EIMSPDN= "onoff", 1`, `AT+EWFCP=2`, `AT+EIMSRCS=0,0`, `AT+EIMSCFG=1,0,1,1,1,1`) and an `ims` entry
in the APN table, `AT+EAPNACT=1,"ims","ims",0` brings the PDN up beside data's: without the IMS commands it gets `CME ERROR 5873`, and when it is
already up `5847`. `+EIF` then gives it an interface of its own (`ccmni1` when data has `ccmni0`), MTU 1500, and `AT+CGCONTRDP=<cid>` the P-CSCF addresses (fields
7 and 8; 3 here). It has a default route of metric 4096, for sockets bound to that interface; `titan-net` tells it from data's by that metric.

**Registration.** A first `REGISTER` over UDP to port 5060 gets a 401 with an AKAv1-MD5 challenge and `Security-Server`. The SIM's ISIM
(`AT+CCHO`, `AT+CGLA`, `AUTHENTICATE` in 3G context) answers it and gives the keys. The client sets up four ESP SAs in transport mode with `ip xfrm`
(the network chose `hmac-sha-1-96` and `aes-cbc`) and sends the second `REGISTER` through them. **Over UDP that got no answer, and over TCP a 200**, so
all protected signalling is TCP. Measured 2026-11-04: registered on the first try each run, about 3590 s granted, and `SUBSCRIBE` to `reg` accepted, with IPv6 and
IPv4 data still working. The refresh, at 60 % of that, has not been observed. A `reg` NOTIFY ends the registration
(and `titan-ims` starts over) only when its `<registration>` is terminated, or the contact at our own address and port:
after a restart the network's first NOTIFY still lists the previous run's contact as terminated, which once made it
start over in a loop (fixed 2026-11-19, two restarts in a row then stayed registered). The kernel needs `CONFIG_XFRM_USER`, `CONFIG_INET_ESP`, `CONFIG_INET6_ESP` and friends
(`titanpocket.config`). The registration's P-Associated-URI gave one identity here and no `tel:` URI, so calls present the SIM's public identity.

**Calls.** First incoming call 2026-11-04: it rang, answered and the other side heard the phone's mic (2048 RTP packets sent), so the network carries RTP from the AP. The phone's own sound failed: PipeWire still held the output after the ringtone, so `aplay` failed with "Device or resource busy" and the receive loop stopped after 1 packet. `titan-ims` now suspends PipeWire's sinks for the call and retries the player. On the second call the other side was heard, through the loudspeaker: DL1 feeds it as well as the earpiece, so `titan-ims` now turns that route off for the call and back on after (the earpiece not yet confirmed by ear). The speaker mode bypasses PipeWire's limiter, so it plays 6 dB down; calls set `Handset` to -6 dB, as the card's default is about -22 dB. Since 2026-11-06 the call's sound goes through PipeWire (`pw-cat` with the Phone role) instead of `aplay`, so a ringtone or other stream can't hold the device; after changing routes `titan-ims` suspends and resumes the `titan-speaker` sink so PipeWire opens DL1 again with the new route. The earpiece was then confirmed by ear, but quiet, for two reasons. `amixer sset Handset -6dB` never worked, because amixer reads `-6dB` as an option (`titan-ims` now sets the raw `Handset Volume` value, dB + 10). And the MT6358 driver drops the handset gain to -40 dB when the earpiece powers down and never restores it on power-up, so the sink reopen left it at -40 dB (the control then reads -13, the inverted 0x1f). Kernel #111 restores the gain at power-up (`mt_rcv_event`, with -10 dB until the control is set); `titan-ims` also sets it after every reopen.

**Jitter buffer (2026-11-06).** Calls crackled with no packet lost: each decoded frame went into the player's pipe as its packet
arrived, so late packets and the other side's silence (AMR DTX: one comfort-noise frame in 160 ms) left the player short, and
every short period is a click. Now the receive thread only queues frames (at most 10, 200 ms; a burst drops the oldest), and a
playout thread starts once 3 are in and keeps 60 ms of sound in the pipe (`FIONREAD` on its end), so the speaker paces it and
no clock drifts against another. A slot with no frame gets the decoder's own fill (`NO_DATA`: concealment, or comfort noise
after a SID frame), as AMR's receiver is meant to run every 20 ms. `pw-cat`'s latency went from 20 to 40 ms. Tested without a
call against a fake speaker draining the pipe in real time, fed 5 s of AMR-WB with 0-60 ms of jitter and 1 s of DTX: playing
each frame on arrival left 64 of 270 periods short, the jitter buffer none (2 at the very end, after the feed stopped). The
call log now counts filled and dropped frames.

**Call volume.** In a call on the earpiece the volume keys step the handset gain by 2 dB, -10 to +8 dB, starting at -6 dB: `titan-powerkey` sees `call=active` in `/run/titan-lockinfo` and sends `{"cmd": "volume", "up": ...}` to `titan-ims`, which keeps the level in `/var/lib/titan-ims/earpiece-db` for later calls. On the loudspeaker or headphones `titan-ims` declines and the keys change PipeWire's volume as usual, which the call's stream follows. Since 2026-11-15 the mixer sets the same level: its "Earpiece" slider (-10 to +8 dB, 1 dB steps) sends `{"cmd": "volume", "db": n}`, which `titan-ims` (Rust and Python) clamps, keeps in `earpiece-db` and, in a call on the earpiece, writes to `Handset Volume` at once; the answer has `"live": true` then. Outside a call it is only kept, and the next call starts at it. The slider follows the file, so key presses in a call move it.

Outgoing calls took three tries. An offer with `a=des:qos mandatory remote sendrecv` got `580 Precondition Failure`; `optional` for the remote side passed it. Then the carrier's server answered `200 OK` within about 2 s, with no ringing stage and `Reason: SIP;text="Invalid Identity Header";code=438`, and the other phone did not ring, with either form of the number as the identity. Adding `P-Early-Media: supported`, `Session-Expires: 1800`, `Min-SE: 90`, `Privacy: none`, `timer` in `Supported` and `application/3gpp-ims+xml` in `Accept` together made the other phone ring (one call, 2026-11-04); which of them mattered was not tested. A call should present `sip:+1...`: the `reg` event lists it, while P-Associated-URI gave the number without the `+`. Signalling: Outgoing and incoming INVITE with preconditions (183, PRACK, UPDATE), CANCEL, BYE, session timers. A call hung up here stays in the call table, ended, until its `BYE` is answered or 32 s pass, so the answer is logged (`call N: BYE <status>`, or "no answer to the BYE"); before 2026-11-21 the record was dropped on the next loop pass and the answer was never seen. In an answered call the dialog's local URI is the `To` URI of their INVITE (RFC 3261 12.1.1), not our own identity. The sound is on the CPU, not
the modem: RTP on the IMS interface, AMR-WB (12.65 kbit/s) or AMR (12.2 kbit/s) through `libvo-amrwbenc` and `libopencore-amr*`, in either RFC 4867 packing,
`telephone-event` for DTMF, and `pw-cat` (the Phone media role) for playback with `arecord` for the mic (`plughw:...,DEV=2`), on the HiFi routes from the UCM profile (earpiece, speaker, headphones; main mic with the preamplifier at 24 dB). Offline, encoding and decoding a tone in both codecs and packings kept its level (AMR-WB RMS 4151 of 4242, AMR 4053), and the devices open. Echo cancellation and the jitter buffer came later (below, "Voicemail, hold, call waiting, echo cancellation and MMS" and "Jitter buffer").

**Rust port (2026-11-06).** `titan-ims` is also written in Rust, in its own crate `userspace/titan-ims-rs` (build `userspace/titan-ims-rs/build.sh` -> `out/titan-ims/titan-ims`). Unlike the `titan-phone` services (static musl) it is a glibc dynamic binary, 0.7 MB: it `dlopen`s the AMR codec libraries (`libvo-amrwbenc`, `libopencore-amr{wb,nb}`) and `libsqlite3` at runtime, as the Python did with `ctypes`, so it links nothing but glibc and `libdl` and the build needs no codec libraries. Everything else is the standard library plus small C calls (`poll`, `ioctl FIONREAD`, sockets with `SO_BINDTODEVICE`/`IPV6_TCLASS` set before `bind`, `dlopen`), shelling out to `mmcli`/`gdbus`/`amixer`/`ip`/`pactl` as before. SIP parsing, SDP, the IPsec SAs (`ip xfrm`), the MD5 digest, base64, JSON, AMR RFC 4867 packing and GSM 7-bit/TPDU/RP are all by hand, with 18 unit tests. Modules: `util` (log, programs, MD5, base64, JSON, SIP text), `net` (IPv6 sockets and `poll`), `modem` (AT channels, the SIM's ISIM, the PDN, identities), `codec` (AMR via `dlopen`), `sms` (GSM-7/TPDU/RP and the SQLite store), `sip` (message parser, SDP, `Xfrm`, the `Call` record), `media` (RTP, the jitter buffer, routing, earpiece), `ua` (the user agent and event loop). It keeps `/run/titan-ims.sock` and the same JSON, so the dialer and `titan-call-watch` do not change. The SIM runs its own authentication, as in the Python: the code only relays the `AUTHENTICATE` APDU and hands the CK/IK the SIM returns to the kernel's IPsec. The port was built one module at a time and compared against the Python's bit-exact outputs where possible (AMR pack/unpack, GSM-7, the digest). Registration was verified on the phone 2026-11-06 (a 60 s run with the Python stopped): it brought up the bearer, ran the AKA challenge, negotiated IPsec (hmac-sha-1-96/aes-cbc), registered for 3600 s and answered the control socket, with identities masked in the log exactly as the Python masks them. It was then swapped in as the service and carried a real incoming call (answered, earpiece audio, the jitter buffer reporting 0 dropped and 0 player restarts) and SMS both ways on 2026-11-06. `cp /usr/local/lib/titan-py/titan-ims /usr/local/sbin/titan-ims && systemctl restart titan-ims` rolls back to the Python one. `build-rootfs.sh` keeps installing the Python version until the Rust one has carried calls for a while.

**Using it.** `titan-net voice on` (Phone Settings, "Voice calls"; off by default, as it keeps the modem on and so the phone awake) keeps the modem on
the network and `titan-ims.service` running, with or without data. `titan_mm` connects to `/run/titan-ims.sock` and offers its calls with the same signals as ModemManager's, so the dialer and
`titan-call-watch` take them without knowing the difference; the header shows "VoLTE" when registered.

**The IMC link is off by default (2026-11-16).** Since the modem asserts after our
"Registered" report, `titan-ims` opens the IMC link only with `TITAN_IMS_IMC=1`; the phone had
been running a hand-edited build for that. `cell_info()` (the cell for
`P-Access-Network-Info`) needs `+CEREG` mode 2 and now restores the mode it found, since
titan-powerkey lowers it to 1 with the screen off ([power.md](power.md)).

**What arrives, counted (2026-11-16).** `titan_modem`'s debugfs file `rx` counts every packet
the modem sends by CCCI channel (2 system, 10 the AT mux, 14 the file service, 32 RPC, 34 IPC,
57/59 IMS, ...), every mux frame by DLCI, and every AT line by its name: the text before `:`
(`+CEREG`, `+ECSQ`, `OK`), never what follows, so cell IDs and identities aren't kept. The
echo, which arrives in pieces, isn't counted. `titan-wakelog` diffs it at each wake
(power-testing.md). The first screen-off half hour after a module reload
(2026-11-16 19:34-20:04) had no modem wake at all: one 1784 s sleep, ended by the cable.

**The modem told of the registration (2026-11-10: built and tested offline; not yet run on the phone).** `+CIREG` is the modem's
own view of IMS, and the modem only has one if the AP's IMS stack tells it: the RIL just reads `AT+CIREG?`, and no AT command
sets it. On Android `volte_imcb` does, over `/dev/ccci_imsc`, CCCI channels 54 (up) and 55 (down), to the modem's IMS controller
(IMC). Read from the stock `volte_imcb` (Ghidra; the decompile stays in `out/modem/imcb-re/`, not committed): every message is one
CCCI packet of the full MTU (3584 - 128 = 0xd80 bytes), a 64-byte header and the body padded with zeros. The header: message id
(32 bits), a sequence number (16), the destination (8; 2 for the controller), a flag (8; 0), the SIM (32), the body's length at
offset 16 (32); the rest is 0. Message ids start at 100000 (`MSG_ID_IMC_IN_BEGIN`). At start `volte_imcb` sends 100012 with its
header file version (0x79) for each SIM; the modem answers 100008 with its own (0x79) and a message-id version (1); on a match
`volte_imcb` reports both of the SIM's accounts (account = SIM × 2, + 1 for the emergency one) unregistered, and on a mismatch
sends 100013 and stops. Registration state is 100026, a 32-byte body: account, state (0 unregistered, 1 registering, 2
registered), a 16-bit cause, the SIP status (200 when registered), retry-after, then at offset 12 the radio access type in the
modem's own numbering, which the modem sends in 100034 (account, type); the remaining fields (an emergency flag, the
registration event, the SIP URI type) are left 0 here. The same channel carries the rest of that design, by the function names
in `volte_imcb` (`imcb_send_imc_cc_mo_call_rsp`, `..._send_sms_rsp`, `..._send_ussd_rsp` and many more): calls, SMS and USSD that
arrive at the modem as AT commands go to the AP's stack as messages, presumably only once the IMC sees a registration. Nothing here
answers those yet; `titan-ims` logs their ids.

Here: the driver puts the IMS channels on CCIF queue 6 as the vendor's `port_cfg.c` does; `titan-mdd` carries channels 54 and
55 on `/run/titan-imc.sock` (`IMC_SOCK` in `titan-modem.service`), one client at a time, each packet as a 32-bit length and its
bytes; `titan-ims` (`userspace/titan-ims-rs/src/imc.rs`) connects, exchanges versions, and reports state 2 for account 0 while
registered and 0 otherwise (and 0 when it stops), with the radio access type once the modem has sent one; after each report it
logs the modem's `+CIREG`. The control socket's `status` has `"imc"`: `off`, `connecting` or `linked`. Offline tests: the
frame layout, a version exchange and the reports through a socket, and a version mismatch (100013, link not used again).
Only message ids and lengths are logged. **On the phone (2026-11-15, the first run): it crashes the modem.** A build with this code linked
(`imc: the modem's version matches (0x79, 1): linked`), reported Unregistered then Registered, and about 13 s after the
Registered report the modem asserted at `imc_reg_msghdlr.c:2427` (params 0) and stopped; `titan-modem` restarted it and the
next registration did the same (twice in 4 minutes). The binary that had run since 2026-11-09 predates this code and never
connects. The phone went back to that binary; a phone build of `titan-ims` should use `Imc::new(None)` until the report is
understood. The phone now runs such a build (the mixer's volume command, `out/ims-noimc/`): registered, `imc` off, no
assert.

**Call settings (supplementary services), first look (2026-11-20, T-Mobile, read-only).** Three ways
to read or change forwarding, call waiting and caller-ID: the modem's own `AT+CCFC`/`AT+CCWA` answer
`+CME ERROR: 30` (no network service: on LTE these go through IMS, not the modem), and `AT+CLIR?`
`ERROR`; codes over USSD (USSI): `*#21#` came back "Request could not be completed. Please try
again." and `*#43#` got a 200 and no answer in 30 s, so T-Mobile's USSD server doesn't do them. That
leaves XCAP over Ut with GBA (TS 24.623, TS 33.220/33.222), as Android does. Not found yet: T-Mobile's
XCAP and BSF hosts. The standard names (`xcap.ims.mnc260.mcc310.pub.3gppnetwork.org`,
`bsf.mnc260.mcc310.pub.3gppnetwork.org`, the same with mnc240) don't resolve in the internet
connection's DNS; the IMS connection's own DNS servers (four ULA addresses from `AT+CGCONTRDP`) can't
be asked from outside titan-ims, since `ccmni2` has no address of its own. 
From the stock firmware (`Firmware/super_ext/`, read only): MediaTek's design splits it. An
AP-side XCAP client (`libutinterface_md.so`, with its own libcurl) does the HTTP and the XML; the
modem does the GBA bootstrap with the SIM (`AT+EGBA=<4 args>`, answering `+EGBA:` with the NAF key,
B-TID and lifetime); a CCCI port `/dev/ccci_ss_xcap` lets the modem's own SS commands run as XCAP
through the AP. The default root is `xcap.ims.mnc…mcc….pub.3gppnetwork.org`; per-operator values
come from `libutinterface_custom_md.so`, whose table gives T-Mobile (310260, 310240) the hosts
`xcap.msg.lab.t-mobile.com` and `bsf.msg.lab.t-mobile.com`. The stock APN list carries XCAP on the
internet APN (`fast.t-mobile.com`, types `default,mms,supl,rcs,xcap`), not on `ims`. Those hosts, and
`xcap.msg.pc.t-mobile.com` / `bsf.msg.pc.t-mobile.com`, resolve in public DNS to IPv4 only and not
in T-Mobile's LTE DNS, so they'd be reached through `clat`. `AT+EXCAPCFGGET` (the modem's XCAP
settings) answers `+CME ERROR: 100` on our AT channel, even for `"version"`. The RIL's `AT+EGBA=<NAF>,<Ua id>,<0|1>,<netid>`
is only for GBA_U (the UICC derives the keys; our tries: one `OK` after 23 s with no `+EGBA:`, then
error 100); in GBA_ME the AP does the bootstrap, the SIM only answering AKA. So `titan-ims` does GBA_ME
itself (`src/xcap.rs`: SHA-256, HMAC, the TS 33.220 KDF, HTTP Digest incl. AKAv1-MD5, 55 tests with
published vectors) behind a read-only control command `{"cmd": "xcap-get", "naf": host, "bsf":
host[/path]}` that writes the simservs document to `/run/titan-xcap.xml` (mode 600). On the network:
the `msg.lab` hosts don't answer at all; `xcap.msg.pc.t-mobile.com` and `bsf.msg.pc.t-mobile.com`
answer HTTPS on the public internet (plain HTTP: 308 to HTTPS; XCAP: TLS 1.2
`TLS_RSA_WITH_AES_256_CBC_SHA256`, so the Ua id is `01 00 01 00 3D` and curl is held to that suite;
XCAP alone: 401; BSF alone: 412). The bootstrap's first request (the IMPI in an empty digest) gets
**403** from the BSF over Wi-Fi, at `/` and at `/bsf-auth/` (a path in the operator table): most
likely it serves T-Mobile's own network only. Over LTE (data brought up by hand: `titan-ltedata --default`,
which came up on `ccmni4` with IPv4 as well) the same 403, so not the network: the request. The
operator table also gives T-Mobile an `X-TMUS-IMEI` header and a user-agent feature token
(`T-Mobile VoLTE-RCS-ePDG-IR94-RTT-ussd`). With both (the IMEI rebuilt from the GSMA instance id with
its Luhn digit; the user agent `Titan-Pocket-Linux/1.0 T-Mobile VoLTE-RCS-ePDG-IR94-RTT-ussd
3gpp-gba`) **it works, over LTE and over Wi-Fi alike (2026-11-20, 22:06)**: BSF 401 with the AKA
challenge, the SIM's RES accepted (200, B-TID and lifetime, 205 bytes), XCAP 401, then 200 with the
user's simservs document (1947 bytes) in about 3 s. It holds: originating identity presentation on
and its restriction's default "presentation-not-restricted"; communication diversion active with the
four rules (`cfu`, `cfb`, `cfnry`, `cfnrc`) each `rule-deactivated` and no target; communication
waiting on; incoming and outgoing barring off; the diversion capabilities (busy, no answer, not
reachable, deactivated; telephony targets). Since then `titan-ims` keeps the sign-in for 30 minutes (a
second read: 1.2 s, no bootstrap; a 401 on a kept one signs in again once) and has two control
commands: `{"cmd": "xcap-get"}` and `{"cmd": "xcap-set", "what": "waiting"|"id"|"forward", "on": …,
"rule": "cfu"|"cfb"|"cfnry"|"cfnrc", "number": …}`, both answering `{"ev": "xcap", "ok": …,
"settings": {"id_shown", "waiting", "forward": {rule: {"on", "number"}}, "barring_in",
"barring_out"}}` (the hosts come from the SIM's MCC/MNC). A change is one XCAP PUT, then a read:
call waiting is the attribute `communication-waiting/@active` (`application/xcap-att+xml`); caller
ID is the element `originating-identity-presentation-restriction/default-behaviour`
(`presentation-restricted` or `-not-restricted`); a forwarding rule is the whole `cp:rule[@id=…]`
element, written with `rule-deactivated` or not, its condition (`busy`, `no-answer`,
`not-reachable`) and a `tel:` target (`application/xcap-el+xml`). Tests: the document's parsing and
the change bodies on a document of T-Mobile's shape. The phone settings app shows them on a "Call
settings" page. Reads verified on the phone; no change has been sent to the network yet. On the way, a crash:
titan-ims panicked when a control command dropped the client that sent it (an event written to a
socket that had hung up) and the read loop then unwrapped the missing client (fixed: the loop stops).

**Conference calls (2026-11-20, built; not run on the network).** `{"cmd": "merge"}` with two
calls up (one held) merges them as 3GPP TS 24.147 and RFC 4579 have it, which is also what the
vendor's `volte_ua` does: a call to the conference factory, `sip:mmtel@conf-factory.<home domain>`
(the IR.92 default; the stock firmware builds the same, with nothing T-Mobile-specific), then, once
it's answered (the 200's Contact carries `isfocus`), a REFER to the focus for each call, one after
the other as each is accepted: `Refer-To: <sip:+1…@<domain>;user=phone?Replaces=<call-id>%3Bto-tag%3D
<their tag>%3Bfrom-tag%3D<ours>>` (a tel: party as a SIP URI, which can carry headers), with
`Referred-By` and `Accept: message/sipfrag`. The network then invites each party into the
conference with Replaces and ends our call with them; the REFER's NOTIFYs (event `refer`, a sipfrag)
are answered and logged. A merge with a conference already up refers the other calls into it.
Test: two answered calls, merge, the INVITE to the factory, the 200 with isfocus, the ACK to the
focus, two REFERs in turn with the right Replaces tags. The dialer has "Add" (a second call from the
keypad) and "Merge" (private/phone-apps.md).

**Caller verification (2026-11-20, built and installed; not yet seen on a call).** An incoming
INVITE's STIR/SHAKEN result as the US networks pass it on (ATIS-1000074): the `verstat` parameter of
the P-Asserted-Identity's or From's URI (`TN-Validation-Passed`, `-Failed`, `No-TN-Validation`; in a
tel: URI's parameters or a SIP URI's user part), an `Attestation-Info` header (A, B, C), and whether
a signed `Identity` header came. `sip::verification()`; the call keeps it and its `call` events carry
`verstat` ("passed", "failed", "none") and `attest`; the log line "caller verification: verstat …,
attestation …, Identity header …" records what the network sent (no number). Tests: the four forms.
The dialer shows it under the number of an incoming call (private/phone-apps.md). A call from another T-Mobile
line (2026-11-20, 23:46) carried none of it: no `verstat`, no `Attestation-Info`, no `Identity`, and
no display name in P-Asserted-Identity or From (`<sip:+1…@msg.pc.t-mobile.com;user=phone>`); its
headers were the usual ones plus T-Mobile's Ericsson SBG's `eric-sbg-mediasite` and `eric-sbg-thid`,
`p-ai-subscription`, `p-user-database`, `feature-caps`, `request-disposition`. So on-net calls come
unverified (as expected: the STIR/SHAKEN verdict is for calls from other carriers), and the badge
stays off; a call from another network is still to be seen. Each incoming INVITE now logs its header
names and its identity headers with digits masked and display names by length only.

**A registration outage (2026-11-20, 23:56 to about 00:01).** After about fifteen restarts of
`titan-ims` that evening (installs), every REGISTER got `480 Temporarily Unavailable` with
`reason: SIP;cause=480;text="CC_IMS_DNS_FAILURE_KEEP_CODE"` (T-Mobile's core failing a DNS lookup on
our registration's way), the IMS PDN itself up (`ccmni0`, 3 P-CSCF); the service exited and retried
every 2 minutes. A reboot (a new attach) cleared it: 401, then 200. Restarts in quick succession
are best avoided.

**Call statistics (2026-11-20, installed; not yet seen in a call).** Once a second, for each call
whose sound runs, a `media` event: the codec as `Codec::describe()` puts it, packets sent and
received, those expected from the RTP sequence numbers (RFC 3550 A.1: loss = expected −
received), the interarrival jitter (A.8) in ms, frames in the jitter buffer, frames concealed and
silent (no data), dropped and bad, and whether the echo canceller runs. The dialer's call card pulls
down to show them (private/phone-apps.md).

**Not done:** USSD menus on the network (see "USSD" below), emergency calls tested on a network (built since 2026-11-19, see "Emergency calls"), RCS,
conference calls on the network, video, handover to Wi-Fi calling, and on the modem's side, calls, SMS and USSD sent as AT commands (the IMC's messages for them go unanswered).

**Voicemail, hold, call waiting, echo cancellation and MMS (2026-11-19, tested offline; installed on the
phone the same day, where registration works as before; no call, voicemail or MMS has run against the
network yet).**

- *Voicemail waiting* (RFC 3842): after registering, `titan-ims` subscribes to `message-summary` at its
  own address and refreshes it before it expires (a refused subscription is asked again after 6 h). A
  NOTIFY's counts go to `/run/titan-voicemail.json` (`waiting`, `new`, `old`) and as a `voicemail` event;
  the account address (`Message-Account`) stays in memory and is never logged. `{"cmd": "voicemail"}`
  calls it (else our own number, which most networks' voicemail answers). On the test phone's network
  the SUBSCRIBE is refused (500 or 503), so a count can come only from a NOTIFY the network sends unasked;
  the dialer shows the count and
  holding 1 calls it.
- *Voicemail waiting by SMS* (2026-11-20): that network says "voicemail waiting" with an SMS from a
  short code instead, which had been filed as a blank text (four of them in the inbox, all empty). An
  SMS-DELIVER's message-waiting indication is now read in all three forms of TS 23.040/23.038: the DCS
  message waiting groups (`0xC0`-`0xEF`: discard or store, active bit, type), the Special SMS Message
  Indication header element (`01`: store bit, type, count) and the Enhanced Voice Mail Information
  element (`23`: the count after the mailbox address). A voicemail one sets the same state as a NOTIFY
  (the count if it gives one, else at least one while active; cleared sets none new). It is filed as a
  text only when it asks to be stored and has text. A type 0 SMS (PID `0x40`) is acknowledged and
  dropped, and so is any other SMS with no text. Those log their form (`PID .. DCS .. UDH ..`, no
  content). Offline tests cover each form. Installed on the phone the same day; a test voicemail's alert
  came about 2 min after the call in the DCS form with no count ("waiting", so 1 new), set
  `/run/titan-voicemail.json` and the dialer's count, and was not filed as a text.
- *Codecs offered* (2026-11-20): each call logs the codecs the other side's SDP lists (names and rates
  only). A test call offered EVS/16000 first, then AMR-WB, AMR and
  PCMU; `titan-ims` had no EVS then, so it took AMR-WB.
- *EVS* (2026-11-20): the 3GPP floating-point reference codec (TS 26.443, from 3GPP's
  `26443-j00.zip`, unpacked in `out/evs/src/`) behind a frame-at-a-time C interface of our own
  (`userspace/titan-ims-rs/evs/titan_evs.c`, built by `evs/build.sh` into `libtitan-evs.so`,
  installed in `/usr/local/lib`). EVS is patent-encumbered: the codec source and the library stay
  local and are never committed or published. Checked against 3GPP's own `EVS_cod` and `EVS_dec`
  on a 6 s test signal: identical encoded bits and identical decoded samples at 7.2, 9.6, 13.2,
  16.4, 24.4 and 32 kbit/s WB, and with DTX at 13.2. On the phone, encoding plus decoding a
  20 ms frame takes 1.9 ms on a little core (2.2 ms at 24.4 kbit/s) and 1.0-1.2 ms on a big one.
  `titan-ims` (`src/evs.rs`) offers EVS first when the library is there
  (`br=5.9-24.4;bw=nb-swb;max-red=0`) and picks it from an offer whose parameters leave a
  primary mode: the widest bandwidth up to SWB (FB would need 48 kHz audio) at the highest rate
  within their range up to 24.4 kbit/s (it fits `b=AS:49`). The answer repeats their parameters
  with the bandwidth held to SWB. SWB runs the audio (capture, playback, echo canceller) at
  32 kHz while the RTP clock stays at 16 kHz. Packets are compact unless they ask for
  header-full (`hf-only=1`, or `cmr=1`, which adds a CMR byte asking nothing); either format is
  read, and AMR-WB IO frames, 5.9 kbit/s SC-VBR sending and channel-aware mode aren't supported.
  An offer that starts in AMR-WB IO mode (`evs-mode-switch=1`) gets AMR-WB instead.
  First call on the network (2026-11-20, 23:50, from another T-Mobile phone; its offer: EVS
  `br=5.9-13.2;bw=nb-swb;ch-aw-recv=2;max-red=0` first, then AMR-WB, AMR, PCMU): EVS at 13.2 kbit/s
  SWB, 32 kHz audio and echo canceller, compact packets; about 20 s answered, 981 packets sent, 106
  received (the far end was silent: EVS's DTX sends a SID about every 8th frame), 984 played (879
  filled by the decoder's concealment/comfort noise, 83 empty), 0 bad, 0 dropped, no error. Speech
  both ways not yet checked.
- *Hold* (RFC 3264 section 8.4): a re-INVITE whose SDP says `sendonly`; the sound stops at once (the
  media keeps its RTP socket, sequence and SSRC) and comes back when the resume's `sendrecv` is
  accepted. A refused re-INVITE leaves the call as it was. Their hold (`sendonly` or `inactive` in their
  offer) is answered `recvonly` and shows as `remote-held`. An established call's SDP says both
  preconditions are met. Control: `{"cmd": "hold", "id": N, "on": true|false}`, `{"cmd": "swap"}`.
- *Call waiting*: an INVITE while one call is up rings as `waiting` instead of getting 486 (a third
  call, or one while a call is being set up, still does); a tone (440 Hz, two 200 ms beeps every 4 s,
  about -20 dBFS) is mixed into the call's sound until it is answered or ends. Answering it holds the
  call that was up; dialling during a call does the same. The dialer shows the second call under the
  first, with Answer, Decline or Swap, and a Hold button in calls over IMS.
- *Echo cancellation*: the WebRTC audio processing through PipeWire's own plugin,
  `libspa-aec-webrtc.so` (PipeWire 0.3.65, `libwebrtc-audio-processing1` 0.3), loaded with `dlopen` as
  the codecs are (`src/aec.rs`): echo canceller with the extended filter and delay-agnostic mode, noise
  suppression, high-pass filter, no gain control. The far end's sound is handed in as it goes to the
  player (after the loudspeaker's gain) and every mic frame is cleaned before it is encoded; the queue
  between them holds at most 500 ms. Tested in the phone's Debian tree with synthetic signals: an echo
  of noise 40 ms late at half level came out silent once converged, and a tone with the far end silent
  came through at -0.7 dB. `TITAN_IMS_AEC=0` turns it off. Not yet heard in a call.
- *MMS* (`src/mms.rs`, OMA MMS encapsulation, WSP): a WAP push (assembled from parts if needed) with
  an `m-notification-ind` starts a download in a thread: HTTP GET of its Content-Location with curl,
  bound to the cellular interface that has the default route, then the `m-retrieve-conf`'s parts are
  saved in `/var/lib/titan-ims/mms/<time>-<id>/` and an `m-notifyresp-ind` (retrieved) is posted back.
  The message is stored as its text with one `[image: path]` or `[file: path]` line a part; a group (more
  than one other person on it) is the thread `+1…,+1…`. Sending: `{"cmd": "mms", "to": [...], "text":
  ..., "files": [...]}` posts an `m-send-req` (multipart/mixed, UTF-8 text, the MMSC fills in From) and
  reads the `m-send-conf`'s status. The MMSC comes from `/etc/titan-ims/mms.conf` (`mmsc=`, `proxy=`) or
  from mobile-broadband-provider-info for the SIM's MCC/MNC. The messages app shows the pictures, picks
  one of the newest in `/root/Pictures` to attach, and takes several numbers for a group.
- *First MMS from the network* (2026-11-20, T-Mobile, Wi-Fi up): the notification came and was read,
  but the download timed out (curl 28). Two faults. With Wi-Fi up, the system resolver asks Wi-Fi's
  DNS, which gives the MMSC only an A record; curl, bound to the IPv6-only LTE interface, could not
  use it. And there is no source routing on this kernel (`ip -6 rule`: not supported), so packets
  for the carrier left by Wi-Fi's IPv6 default route: T-Mobile's DNS servers (unique-local `fd…`
  addresses) and the NAT64 prefix that 464XLAT's IPv4 goes to. Through `clat` with the prefix pinned
  by hand the MMSC answered 403; asked over LTE, T-Mobile's DNS gave the MMSC an AAAA, and the GET
  over LTE's IPv6 to it returned the message (200, 558 kB, `m-retrieve-conf`) in 2.4 s.
  Now `titan-net` routes the carrier's DNS servers (each a /128) and the NAT64 prefix (/96) over LTE
  while the translator runs, and `titan-ims` looks the MMSC up in the LTE link's DNS
  (`resolvectl -i`) and hands curl the IPv6 address (`--resolve`), falling back to `clat` for a host
  with only IPv4. A failed download is kept (`/var/lib/titan-ims/mms/pending.tsv`, mode 600: the
  message's address) and retried every 5 minutes, up to 12 tries, and on `{"cmd": "mms-retry"}`; a
  success replaces the `[MMS - not downloaded]` placeholder. The first message came in that way, on
  the first retry: one picture (557 kB JPEG). Sending, the same evening: a 40 kB PNG and a line of
  text to one number, accepted by the MMSC in 3 s (`m-send-conf`, status 0x80, OK) and received
  on the other phone (confirmed by the user).

**SMS.** First run 2026-11-04, on the first try: one text received and stored, its report accepted (202), and two sent from the messages app, each accepted and acknowledged by the network within a second (RP-ACK), and both received by the other phone. On Android `volte_ua` has SMS handling and the `+g.3gpp.smsip` tag, and the RIL sends `AT+CMGS`; our capture holds only the set-up, with no message sent. `titan-ims` does it in SIP (TS 24.341): the registration's Contact carries `+g.3gpp.smsip`, and a `MESSAGE` of type `application/vnd.3gpp.sms` carries an RP-DATA (TS 24.011) with an SMS-SUBMIT or SMS-DELIVER (TS 23.040), to `tel:` the service centre from `AT+CSCA?`. A message is stored before it is acknowledged, in `/var/lib/titan-ims/messages.sqlite3` (mode 600); text and numbers never reach a log. Long messages go in parts and come back whole, in any order. `{"cmd": "sms", "to": ..., "text": ..., "class": 0-3 (optional)}` on the control socket sends one (the class sets the TP-DCS message class: 0 flash, 1 ME, 2 SIM, 3 TE; omitted means no class); the state follows as `sms-state`, and `sms` announces a new one. An incoming WAP push (8-bit data or a port-addressing header, which is how an MMS notification arrives) started an MMS download since 2026-11-19 (below); before, it was stored as an `[MMS - not downloaded]` placeholder, as a failed download still is. (The Rust client earlier mis-stored such a message because `CString` cannot hold the NUL bytes a WAP-push PDU carries; it now binds the body's raw bytes, and the messages app reads bodies lossily, so a bad row cannot blank the list.) Long messages, and non-GSM text, have only been tested offline: the published GSM 7-bit and SMS-DELIVER examples, and our encoders against our decoder (GSM, escapes, UCS2, emoji, up to 7 parts). The app is `userspace/titan-messages` (docs/desktop.md).

**USSD (2026-11-09: a code answered on the network; menus tested offline only).** The path is USSD over IMS ("USSI", TS 24.390), in `titan-ims` (the Rust
one: `userspace/titan-ims-rs/src/ussd.rs` and the session in `ua.rs`). Why that path, from what was read rather than measured: on
Android the RIL chooses a domain per code (`RmcSuppServUssdDomainSelector`, `isImsRegOn`, `isVopsOn` in `libmtk-ril.so`) and, with IMS
registered, sends `AT+EIUSD`, which the modem hands to `volte_ua` on the AP, whose `ussd` module (`volte_ua/src/ussd/ussd.c`, strings
only) speaks SIP; the circuit-switched path is `AT+ECUSD`/`AT+CUSD`, which needs a 2G or 3G network to fall back to. Here the AP runs
IMS and the modem is not told of the registration (`+CIREG` stays 0), so `AT+EIUSD` has nothing behind it, and this network has no
3G. Measured: `AT+CREG?` gives stat 6, registered for SMS only (the combined attach gives SMS over SGs and nothing else in CS),
with `AT+CEREG?` and `AT+CGREG?` at 1 and `AT+CIREG?` at 0, so there is no CS domain for `AT+CUSD` (it reports `+CUSD: 1`, `(0-2)`,
and was not sent a code).

How it goes (TS 24.390 4.5.4.1 and Annex A, release 20, read from the 3GPP PDF): an `INVITE` to the code as a dialstring in the
home domain (`sip:*%23100%23;phone-context=<domain>@<domain>;user=dialstring`), with `Recv-Info: g.3gpp.ussd`, `Accept: application/sdp,
application/vnd.3gpp.ussd+xml, multipart/mixed` and a `multipart/mixed` body: an SDP offer whose audio line has port 0 (no media) and
a `<ussd-data>` document (`<language>en`, `<ussd-string>` the code; `Content-Disposition: render;handling=optional`). The network
answers 200 (ACKed). When it wants input it sends `INFO` (`Info-Package: g.3gpp.ussd`) with the question in `<ussd-string>`; the
answer goes back in an `INFO` of the same form. It ends the session with a `BYE` carrying the last text, or an `<error-code>`
(1 unspecified, 2 language, 3 unexpected data, 4 busy). A 404 to the `INVITE` means the network has no USSD over IMS. Our side
ends a session with `BYE` once answered, `CANCEL` before; a new code ends the one before it. Timeouts: 60 s for the network,
180 s for the user's answer. Codes and replies are never logged (a reply can hold a number or a balance), only states and lengths.

The control socket takes `{"cmd": "ussd", "code": "*#100#"}`, `{"cmd": "ussd-reply", "text": ...}` and `{"cmd": "ussd-cancel"}`
and sends `{"ev": "ussd", "state": ..., "text": ...}`: `sent`, `request` (an answer is wanted), `notify`, `done`, `failed` or
`ended`; `status` gains `"ussd"` (`idle`, `sent`, `request`), which tells a client that this `titan-ims` takes codes (the Python one
doesn't). From a shell: `titan-ims ussd '*#100#'` prints the replies and asks for an answer when the network wants one (an empty
line ends the session; `-m` masks digits, for logs that leave the phone). Both dialers send a code to `titan-ims` while it is
registered and says it takes them, and to ModemManager's `Ussd` otherwise.

Tested offline only (`cargo test`): the spec's examples, the multipart body round trip, XML entities and CDATA, and two sessions
driven through a real `Ua` with a loopback TCP connection as the P-CSCF and a fake AT channel: a menu (`INVITE`, 200, `ACK` to the
Contact through the Record-Route, the network's `INFO` answered 200 and shown as a question, our `INFO` answer with CSeq 2, the
network's `BYE` with the final text), and a 404 (ACKed, "no USSD over IMS"), a cancel before the answer and a code replacing
another. The CLI against a fake socket: question, answer, final text.

**On the network** (2026-11-09, Tello SIM on T-Mobile 310260, `titan-ims ussd -m '#225#'`, sent twice with the user's go-ahead): the
`INVITE` as above was answered 200 within about a second, ACKed, and the network's `BYE` followed with a 80-character
`<ussd-string>` ("Thank you, your request is being processed. A message will be sent to your phone") **and `<error-code>0`**. 0 is
not a defined code; with that text it means success, so the parser drops it (the first build showed "error, unspecified" instead
of the text). No text message had arrived 5 minutes later. Then T-Mobile's other free query codes from its "Self-service & short
codes" page, one session each: `#646#` (minutes), `#674#` (texts) and `#932#` (data) got the same 80-character "being processed"
answer, `#686#` (the line's number) a 34-character answer with the number; every one 200 then `BYE`, shown as `done`. The promised
text messages had not arrived 15 minutes after the first (none received in that time), so on this MVNO those codes are answered
but apparently not served. Registration and calls were unaffected; each install restarted
`titan-ims`, which registered again within seconds. Not yet run on the network: a menu (the `INFO` exchange), a cancel, and
network-initiated USSD (an incoming `INVITE` with a USSD body; we don't advertise `+g.3gpp.nw-init-ussi`, so the network should not
send one). The dialers' USSD page has not been tried by touch. Sending a code reaches the carrier: ask first.

## Status bar signal

`titan-signal` (a service) follows ModemManager and writes 0 to 4 to `/run/titan-signal`; the panel reads it every 10 s
and draws the bars left of Wi-Fi. No file means the modem is off, and no bars show. Bars come from LTE RSRP in ModemManager's
`Modem.Signal` interface (`Setup` with 15 s): -110, -100 and -90 dBm are the thresholds. ModemManager's plain signal quality read 0 on LTE
after a re-enable. Not registered shows 0 bars. Measured: -83 dBm gives 4.

Enabling did not always register. With `titan-signal` running, 4 enables after a run that had failed gave: not registered in 25 s
(`idle`, `CEREG: 2,0`, `CESQ` all empty), 16 s, 1 s, 2 s; with it stopped, 4 more gave 1-2 s. ModemManager logged `network forced PS
detach` on the disable before the first failure. The sample is too small to blame the service, and earlier cycles without it were all
2-3 s. Not understood.

**Carrier name.** The modem names no network: `AT+COPS?` answers `310260` in all three formats (`AT+COPS=3,0` long, `3,1` short,
`3,2` numeric), its own table (`AT+COPN`) has 35 entries without this code, the SIM has no service provider name, and
ModemManager passes the number on as the operator name. The name comes from Debian's `mobile-broadband-provider-info`
database instead: the first provider that lists the code, since virtual operators repeat their host's codes after it (310260 is
listed by T-Mobile, then SpeedTalk, Mint and US Mobile). `titan-signal` writes it to `/run/titan-carrier` while registered
(ModemManager's name when it is a name, else the database's, else the code); Phone Settings shows it for the network
("T-Mobile (310260)"), the SIM's operator and the network scan.

## Cell location

**2026-11-10: built and tested offline. 2026-11-16: installed on the phone (`/usr/local/sbin`); its `AT+ECELL` query works there, no estimate yet: there is no tower database on the phone.** `titan-cellloc` (Rust, in `titan-phone`)
estimates a coarse position from the cells the modem hears, with no GPS. It has its own AT channel (`cmd11`, which
nothing else uses) and asks `AT+ECELL`. The answer's layout comes from the vendor RIL's parser
(`RmcNetworkHandler::getCellInfoListV12` in `libmtk-ril.so`, decompiled locally with Ghidra into
`out/modem/ril-re/`, not committed). `+ECELL: <n>` is followed by n cells of 13 values, or 15 when the modem adds
BSIC and ARFCN, in this order:

1. access technology: 0 GSM, 2 UMTS, 7 LTE (11, NR, has another layout)
2. cell id, hex
3. LAC or TAC, hex
4. MCC
5. MNC, a string
6. PSC or PCI
7. two signal values
8. RSRP in quarter dBm
9. RSRQ in quarter dB
10. timing advance (0-1282)
11. RSSNR
12. CQI

The first cell is the one the phone is camped on; the RIL marks it registered. `0x7fffffff`, or all ones, means
unknown, and so is an empty field. With `AT+ECELL=4` first the modem adds BSIC and the EARFCN (15 values); on
LTE the two signal values are RSRP and RSRQ as 27.007 indices (dBm + 140, 2 × (dB + 20)). The modem has not listed
neighbours yet. The parser expects neighbours to carry a PCI without a cell id, the usual case for LTE.

How the estimate is made:

- **Tower sites** come from an OpenCelliD export (CC BY-SA 4.0). It is imported on the phone into
  `/var/lib/titan-cellloc/cells.db`, a sorted binary file read with `pread`, plus an index by PCI.
  The import command is
  `zcat cell_towers.csv.gz | titan-cellloc import --mcc 310,311,312,313,316`.
  Downloading the export needs an OpenCelliD account.
- **The serving cell** is looked up by its full identity. If that cell is missing, the locator uses another sector
  of the same LTE site (eNB = cell id >> 8, same TAC) and adds 1 km of uncertainty.
- **Each neighbour** is placed at the nearest site with its PCI within 15 km of the serving site.
- **The position** is the sites' centroid. Each site is weighted by 10^((RSRP + 140)/30), roughly 1/distance for a
  path-loss exponent of 3.
- **Timing advance.** When the serving cell gives one, the estimate is moved onto the ring at TA × 78.12 m from the
  serving site, in the direction of that centroid.
- **The accuracy** is the sites' weighted spread about the estimate, capped at the TA ring and kept within 200 m to
  20 km.
- **With only the serving cell,** the estimate is its site, with the TA ring or the database's range for that cell
  as the accuracy.

These weights and limits are not calibrated against a reference yet.

**BeaconDB.** When the serving cell is in neither the database nor the learned file, the cells can go to BeaconDB
(`api.beacondb.net`, the MLS-style geolocate API, `considerIp` false). This is only done when
`titan-cellloc online on` has created `/var/lib/titan-cellloc/online`; it is off by default. It goes out through
`curl` with a generic user agent. Each answer is kept in `learned.tsv` (mode 0600) under the serving cell, so a
cell is sent once. A request tells BeaconDB roughly where the phone is.

**Output and privacy.**

- `/run/titan-cellloc.json` holds time, position, accuracy, cells used and seen, and the source (db, learned or
  beacondb).
- The command prints how the estimate was made, never where: cells as technology, the last two hex digits of the id,
  PCI, RSRP and TA.
- When the GPS daemon's last position (`/var/lib/titan-gnss-open/position`) exists, the command prints the distance
  to it.
- The GPS daemon needs a rough position within tens of km before its first fix. When that file is missing, or more
  than 50 km from an estimate whose accuracy is within 20 km, the locator writes its estimate there.
- The database, the learned file and the output stay on the phone.

**The service.** `titan-cellloc watch 300` estimates every 5 minutes. Its unit, `titan-cellloc.service`, is
installed but not enabled, because each query wakes the modem.

**Its users** (2026-11-20). titan-gnss-open runs `titan-cellloc` itself when a session starts with no fix, or with
one over an hour old and an estimate over 30 minutes old, and seeds its search from the estimate when the phone has
moved far (gnss-open.md, "On demand, and the cells' seed"). The Location app and Maps show the estimate, marked as
approximate with its accuracy in km, while GPS has no fix. An emergency call without a GPS fix in time sends it as
its position (below).

Tests (7) cover these cases, all with made-up cells placed over open ocean:

- the `+ECELL` layouts and malformed input;
- the CSV rows;
- exact, sibling and PCI lookups;
- a fix from three cells with TA, landing on the ring towards the neighbours;
- the serving cell alone;
- the BeaconDB request and answer shapes;
- the log masks.

## Modem debug data

MediaTek ships three debug files with the modem build (in the firmware
package and in `md1img`). They are vendor data: read in place, never
committed. Nothing in the driver, `mdd.py` or `sphparam.py` needs them;
they are for diagnosis only.

| File | What | Usable |
|---|---|---|
| `DbgInfo_<build>` | the modem MCU's symbols: "CATICTNR" and "CATI" headers with the build strings, then 95441 entries of name, NUL, start and end address (u32 LE), then the source paths | yes, plain: `tools/modem/dbginfo.py` (address to function, `-g` to search names) |
| `MDDB_InfoCustomAppSrcP_*.EDB`, md1img `md1_mddb` (same) | the trace database that turns DHL log records into messages: a "CATD" container (version 10, five sections) | **no:** the section data is encrypted (7.997 bits/byte, no compression signature); only MediaTek's ELT tool holds the key. Not attempted |
| md1img `md1_mdmlayout` | the layout of over-the-air message traces | plain text, not needed so far |

**The DHL log** (the raw buffer in the cached region after 2 MB; the
modem fills about 2 MB a minute with the DSP and MCU traces) starts with
a "CCCw" header and carries repeated markers (`7259,xhTThx,9527`, "PR3",
"2IDL"); its records are trace ids that only the encrypted database
names, so it is not decoded. **The symbols** are enough to name
exception addresses and to see what the firmware has, e.g. for speech:
`Spc_SpeechOn`, `Spc_AcousticLoopback` and `spc_isAcLoopback` (the
acoustic loopback behind speech on with application 2),
`AcousticLoopback_PCM2WAY_UL_HisrHdl` / `_DL_HisrHdl` (that loopback
runs through the modem's PCM2Way path, uplink to downlink),
`spc_dev_LoopbackPoint_DVT` (0x2f13), and codec loopbacks
(`sp4g_AMR_NB_loopback`, `sp4g_fake_loopback`).

**An open alternative.** Diagnosis does not have to rely on these: the
modem's exception record (decoded by the driver: file, line and
parameters), its message acks, and probes such as the loopback points
(0x2f13) already locate faults without vendor data, as they did for the
file service and the CCB. The symbol file is the only piece used, and
only to name addresses; a future open toolchain can drop it at no cost to
function. Decoding the DHL traces has no open route: the ids are
meaningless without the database, and building a map of our own would
need the firmware disassembled, which this project does not do.

## Next steps

1. Registration (see "Radio on with a valid SIM"), then the apps on a real
   network. T-Mobile has no 3G, so voice needs IMS, whose channels are not
   served yet. Outward facing: ask first.
   Voice: the speech parameters are in (see "Speech"), but the modem's
   speech DSP still returns silence in loopback, and its log cannot be
   decoded (see "Modem debug data"); a real call would show whether the
   DSP runs outside loopback. Needs a valid SIM; the first radio-on ended
   in an LTE RRC assert (see "Radio on").
2. The file service in Rust, as a daemon on `/dev/titan-modem`, on the
   same RAM copy (or a persistent copy outside the partitions, if the
   modem's writes should survive a reboot: the user's call).
3. The modem's other channels: IPC (34), IMS (57/59/169), Wi-Fi offload
   (171), the status channel, and what they need answered.
4. SMS and voice (the apps are there, see "Phone apps"), and data through
   ModemManager rather than `ltedata.py`. Each is outward facing: ask first.
5. Later: the DHL log on the CCB (what `mdlogger` sends to start
   logging; not decodable, see "Modem debug data"). The driver in tree
   with MD1 as a power domain runs (kernel #124, 2026-11-10); the
   exception handshake and the crash logger are untested on a real
   crash.

## Testing it

The modem starts with `systemctl start titan-modem` (the file service's
open of `/dev/titan-modem`; before 2026-11-10, loading the module), never at
boot, and stopping the service powers it off. The service holds a
wake lock while it runs unless `/var/lib/titan-modem/allow-sleep` exists;
with that file the phone deep-sleeps with the modem on, and a call wakes
it (2026-11-05: see power.md, "Sleep with the modem on"). Risky experiments
ran with a watchdog pinger on `/dev/watchdog` (systemd's runtime watchdog
is off), stopped by its stop file with the magic close, so a hang resets
the phone into the unchanged kernel. A test that should keep the phone
awake takes a wake lock (`echo name > /sys/power/wake_lock`, kernel #94
and later); a USB cable also keeps it awake.

## Emergency calls

**Built, never tested on a network (2026-11-19).** Don't rely on this phone to reach emergency
services. Nothing below has placed a real emergency call; it is checked offline only, and it
cannot be tested by calling (see "Testing").

Calls go over VoLTE through `titan-ims`, our IMS client on the AP, so the emergency procedure is
ours to do (TS 23.167, TS 24.229 5.1.6); the modem's firmware can't fill the gap, as on phones
whose modem runs IMS. MediaTek's own stack (read as reference) brings up an APN of type
`emergency` with `AT+EAPNACT` and registers over it; ours does the same in this order:

1. **The emergency instance.** A dialled emergency number (911, 112, or one from the network's
   list below) makes `titan-ims` start `titan-ims --emergency NUMBER`, a second process with its
   own AT channel (`cmd11`) and SIM channel, so the normal registration is not touched. It
   activates the `sos` APN (in the APN table since this change: `type=emergency`, authtype 2, as
   MediaTek's RIL writes it) with `AT+EAPNACT=1,"sos","emergency",0`, waits for its interface
   (`+EIF` on `nwurc`) and reads its P-CSCF (`+CGCONTRDP`). Its IPsec cleanup touches only
   policies to or from its own address (before this change `titan-ims` flushed every policy).
2. **Emergency registration** over that PDN: the usual AKA and IPsec, the Contact with the `sos`
   URI parameter, no SMS feature tag. If it fails, or there is no usable SIM (an anonymous
   identity, the IMEI as the instance), the call goes **unregistered**: an unprotected INVITE to
   the P-CSCF, From `anonymous@anonymous.invalid`, no `P-Preferred-Identity`, no sec-agree.
3. **The INVITE**: Request-URI and To `urn:service:sos` (with `.police`, `.ambulance`, ... when the
   network's category names one service), no `P-Preferred-Service`, the cell in
   `P-Access-Network-Info`, and the position by value when there is one: `Geolocation:
   <cid:...>`, `Geolocation-Routing: no`, and a PIDF-LO (a circle of gpsd's 95 % error) next to
   the SDP in a multipart body. Location is turned on for the call if it was off (every phone
   does this for an emergency call) and off again after; the call waits at most 3 s for a fix and
   otherwise goes without one (the network still has the cell). Positions are never logged.
4. **Callback**: after the call the emergency registration stays up 5 minutes (`CALLBACK_S`) so
   the dispatcher can call back, the phone kept awake; then the PDN goes down
   (`AT+CGACT=0,<cid>`). The emergency instance's events reach the dialer through the service
   (its call ids + 1000), and the dialer's answer, hang-up, mute and route reach it the same way.
5. **Fallbacks**: no emergency PDN, or no call getting through, and the service tries an
   emergency INVITE over its normal registration; that failing, it tells the dialer
   (`"fallback": "modem"`), which places the call through ModemManager (the modem's own `ATD`,
   circuit-switched where a network still has 2G/3G). Without the service (no SIM, not
   registered) the dialer starts the emergency instance itself and falls back to the modem the
   same way. An emergency number is never blocked by the dialer's SIM-locked or flight-mode
   checks.
   Without a GPS fix within 3 s, the position is titan-cellloc's estimate when it is under 30
   minutes old: the same PIDF-LO with the estimate's accuracy as the radius and `gp:method`
   `Cell` (2026-11-20). For the whole emergency session the GPS receiver is held on
   (`/run/titan-gnss-open.hold`), since titan-gnss-open otherwise runs only while an app
   asks; it is released when the session ends. Tested offline only.

**Not done:** the radio isn't switched on from flight mode for the call (the PDN then fails and
so does the modem's call); the position isn't updated during the call (only the INVITE carries
it); MediaTek's emergency-mode commands (`AT+EMCS`, `AT+EMCI`, `AT+ECCTRL`) aren't sent, as
their arguments are unknown; the modem isn't kept running without a SIM; and the lock screen
has no emergency button. It has no PIN either (popping the bubble unlocks it), so nothing
stands between anyone and the dialer today; a PIN must come with an emergency button.

**Testing:** never by calling an emergency number. That is illegal in many places and takes a
dispatcher away from a real call. Activating the `sos` PDN or an emergency registration is
emergency signalling to the network too, so it isn't done casually either. Checked offline:
the dialer recognises the numbers; `titan-ims`'s tests build the unregistered emergency INVITE
(URN, no sec-agree, no service or identity asked for, the `sos` Contact, Geolocation and the
multipart body with the PIDF-LO) and read gpsd reports.

**The network's numbers (2026-11-19).** The modem reports the emergency
numbers the network gives it (`+CEN1: <reporting>,<mcc>,<mnc>`, then one
`+CEN2: <category>,<number>` a number; TS 27.007 `AT+CEN`). Those reports
go to ModemManager's port, so `titan-ims` asks for the list itself
(`AT+CEN?`) at each registration and keeps it in
`/var/lib/titan-modem/emergency-numbers` ("number category" a line; it stays
on the phone). 911 and 112 count whatever the list says. The dialer marks a
listed number as an emergency number, and `titan-ims` sends it as
`urn:service:sos`, with the sub-service when the category names one service
(1 police, 2 ambulance, 4 fire, 8 marine, 16 mountain; RFC 5031). With the
screen off `titan-powerkey` turns the reports off (`AT+CEN=0`) and back on at
screen on: they come with each new cell and woke the phone. Calling any of
these numbers is still untested, as above.

**To do before a release** (roadmap.md, "Community release"): the modem kept
running without a SIM (a postmarketOS device lost emergency calls by
disabling it), and an emergency call button reachable from the lock screen
without unlocking.

