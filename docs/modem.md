# Modem (MD1, LTE)

Status (2026-11-03, Linux 6.12): **the modem boots to handshake 2 and
ModemManager drives it** over a WWAN AT port (`/dev/wwan0at0`), as the
`titan-modem` service, on a RAM copy of the phone's own NVRAM served by
`tools/modem/mdd.py` (see "File service" and "AT port and
ModemManager"). It reports `MOLY.LR12A.R3.MP.V162` and is kept in flight
mode (`+CFUN: 4`: the SIM up, the radio off; nothing has been
transmitted). ModemManager sees it `disabled`, power state `low`.

The driver is a proper platform driver (kernel #94+): it binds to a DT
node `modem@10209000` (`mediatek,mt6771-modem`) and takes every register
window, interrupt, clock and syscon from it, with devm/probe-remove
teardown. The node is in `mt6771.dtsi` (disabled) and enabled in the
board dts. It is still loaded by hand for bring-up (`insmod`; `rmmod`
powers the modem off), not started at boot.

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
3. **Power** (`md_cd_power_on`): clear TOPCKGEN CLK_MODE bits 8-9 (the
   modem's clock gating), power the MD1 domain (MD1_PWR_CON 0x320 with the
   extra SPM registers MD_SRAM_ISO_CON 0x394 and MD_EXTRA_PWR_CON 0x398,
   then the bus protection: INFRA PROTECTEN bits 3, 4, 7 and PROTECTEN_1
   bit 6), close MD-to-AP, set the modem's 26 MHz request
   (INFRA 0xf0c = 0x21), initialise the modem PLLs (MDTOP_PLLMIXED
   0x20140000, MDTOP_CLKSW 0x20150000; the PLL block reads version
   0x20160627) and stop the modem watchdog. This stays open-coded rather
   than a genpd: the MT8183 power-domain table has no MD1, and MD1's
   sequence touches MD_SRAM_ISO_CON (0x394) and MD_EXTRA_PWR_CON (0x398),
   separate registers the generic scpsys genpd data model can't express,
   so the driver drives the scpsys and infracfg syscons itself (the
   phandles come from DT).
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

The MD1 domain powers off again on unload (measured: MD1_PWR_CON back to
off, PWR_STATUS bit 0 clear), also after a modem exception.

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
  (`IPC_RPC_GET_EINT_ATTR_OP`, 0x4005). The board's vendor DT has no
  hot-plug nodes, so the vendor kernel answers
  `ERR_SIM_HOT_PLUG_QUERY_STRING` (-15); `mdd.py` does the same (the
  reply: op | 0xffff0000, its parameters, on channel 33 with the request's
  buffer index kept in the reserved word), and the modem goes on.
- **File service.** Then the modem's file requests start (channel 14,
  the vendor `ccci_fs` port served by Android's `ccci_fsd`): same
  framing as RPC, UTF-16 paths on drive `Z:`. A reply with the wrong
  number of parameters makes the modem assert in `ccci_fs.c:1259` with
  (expected count, sent count, op), one with a data parameter of the
  wrong length in `ccci_fs.c:1321` with (op, sent length, expected
  length). See "File service" for the operations.

## File service

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

## AT port and ModemManager

When the daemon asks (`TITAN_MD_AT_PORT`, `_IO('t', 1)` on
`/dev/titan-modem`, after handshake 2) the driver registers the AT
channel with the kernel's WWAN framework (`wwan_create_port`,
`WWAN_PORT_AT`, parent the modem's platform device): `/dev/wwan0at0`.
Until then the AT channel goes to the daemon like any other. Writes go out as CCCI packets on
UART2_TX (12), at most 1 KB of text each; UART2_RX (10) packets lose
their header and go to the port, or are dropped while it is closed.
That channel no longer reaches `/dev/titan-modem`. The WWAN core is
`wwan.ko` from the same kernel build (`CONFIG_WWAN=m`; `build612.sh`
copies it).

`tools/modem/install.sh` puts the driver in the module tree (`updates/`,
blacklisted so the DT node does not load it at boot: the modem must start
with its file server), `mdd.py` and `nvcopy.sh` in
`/usr/local/lib/titan-modem`, and `titan-modem.service` (not enabled):
it takes a wake lock, copies the NVRAM to `/run/titan-modem`, loads the
driver (which boots the modem) and runs `mdd.py` until stopped; stopping
unloads the driver, which powers the modem off, and drops the copy. With
`AT_PORT=1 AT_INIT=AT+CFUN=4` (the unit's), `mdd.py` waits for the
modem's AT layer (`+EIND: 128`, about 1 s after handshake 2; a command
sent at handshake 2 is ignored), sends `AT+CFUN=4`, waits for its `OK`,
then asks for the WWAN port (after 10 s at the latest).

Debian's ModemManager 1.20.4 (`apt install --no-install-recommends
modemmanager`; no NetworkManager, so nothing enables the modem) takes the
port with its generic plugin: manufacturer `MTK1`, model `MTK2`,
firmware `MOLY.LR12A.R3.MP.V162, 2021/05/11`, GSM/UMTS + LTE, 2G/3G/4G,
IPv4/IPv6, power state off. Stopping and starting the service removes
and re-adds the modem (udev hot-plug); a port that existed before
ModemManager was installed needed `udevadm trigger` once. Measured over
the port by hand first: `ATE0`, `ATI`, `AT+CFUN?` (0), `AT+CSQ` (99,99),
`AT+GCAP` (`+CGSM`).

At `CFUN=0` the SIM stays down: `AT+CPIN?` gives `+CME ERROR: 14` (SIM
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

1. Enabling the modem in ModemManager (`CFUN=1`: the radio on, network
   registration): outward facing, ask first.
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
4. Data (CCMNI on CLDMA, as a WWAN net link for ModemManager), SMS and
   voice. Each is outward facing: ask first.
5. Later: a modem logger on the CCB (what `mdlogger` sends to start
   logging) and decoding the DHL log, the driver in tree with MD1
   as a power domain, the modem's exception handshake, its cost in
   suspend.

## Testing it

The module is loaded by hand (`insmod`, or `systemctl start titan-modem`),
never at boot, and unloading powers the modem off. The service holds a
wake lock while it runs: the modem across suspend is untested. Each experiment ran with a watchdog pinger on
`/dev/watchdog` (systemd's runtime watchdog is off), stopped by its stop
file with the magic close, so a hang resets the phone into the unchanged
kernel. `titan-powerkey` was paused (SIGSTOP) during the tests: this
kernel has no `/sys/power/wake_lock`, and the phone suspended between
commands.
