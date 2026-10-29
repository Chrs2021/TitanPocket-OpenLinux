# GNSS (MT6631 CONNSYS GPS)

Status (2026-10-29): the receiver runs on MediaTek's closed navigation
library, **libmnl.so**, hosted by **titan-mnld**, our own replacement for
the stock daemon (mnld). **3D fix verified** with stock mnld outdoors (12 of
20 satellites used: GPS, GLONASS, BeiDou, SBAS; HDOP 0.74, about ±14 m,
after a few minutes from cold with no assistance data) and **with
titan-mnld indoors** (2026-10-29: 32 satellites seen across GPS, GLONASS,
BeiDou and Galileo, 5 used). The **Location** app (app drawer) and
`titan-location on|off` switch it. It is off by default because it drains
the battery.

**Replacing libmnl, stage 1 (2026-10-29): GO.** Our own code
(`userspace/titan-gps`, Rust) brings the receiver up with no libmnl: it
loads the firmware patch (from the phone's own libmnl.so), runs a session
and gets every report type the chip sends libmnl. See "Replacing libmnl"
below.

## Stack

```
gps_drv.ko  (/dev/stpgps; built from source on the WMT core)
   │  framed binary protocol (see "The /dev/stpgps protocol" below)
libmnl.so  (MediaTek, closed: firmware patch, tracking, fixes, NMEA)
   │  C API: mtk_gps_mnl_run() + 17 callbacks (see "The libmnl interface")
titan-mnld  (ours, C, 32-bit bionic; chroot /opt/android-gps)
   │  "\0mtk_mnl2hal" / "\0mtk_hal2mnl" datagrams (mnld's GNSS HAL protocol)
titan-gnss  (ours, Rust; the HAL's side)
   │  NMEA over UDP 127.0.0.1:10110
gpsd -n  ──>  cgps, gpspipe, the Location app, any gpsd client
```

| Unit | Role |
|---|---|
| `titan-gps-driver.service` | `modprobe gps_drv`, waits for `/dev/stpgps` (needs `titan-wmt`) |
| `titan-mnld.service` | titan-mnld in `RootDirectory=/opt/android-gps` (`/dev`, `/proc`, `/sys` bound); `PartOf=titan-gnss` |
| `titan-gnss.service` | `titan-gnss daemon`; **not enabled at boot**: `titan-location on` starts it |

- **The driver:** `modules/connectivity/gps/` (MediaTek's open
  `vendor-mediatek-kernel_modules-connectivity-gps`, android-11 branch),
  built by `modules/connectivity/build.sh`.
- **The chroot (8 files, 7 MB):** the stock bionic linker,
  `ld-android.so`, `libc.so`, `libc++.so`, `libdl.so`, `libm.so`,
  `libmnl.so` and `MNL_Config.xml`, plus `titan-mnld`.
  - `tools/gnss/stage-mnld.sh` copies them from the phone's own system
    image and runtime APEX into `out/android-gps/root`.
  - **Never redistribute the chroot.**
  - `/data/vendor/gps/` in it holds libmnl's saved state: `mtkgps.dat`
    (almanac, ephemeris, last position) speeds up the next start.
- **titan-mnld:** `userspace/titan-mnld/build.sh` builds it with clang-11
  (`armv7a-linux-androideabi`, no NDK). It uses its own `_start`
  (`start.c`) in place of the NDK's crtbegin and links straight against
  the staged libraries.
  - It can't run in a plain `chroot`: bionic crashes at startup without
    `/dev/null`. Use the unit, or `systemd-run` with the same
    `RootDirectory` and `BindPaths`.
  - `TITAN_MNLD_DEBUG=1` logs every property read.
  - `TITAN_MNLD_MEAS=/tmp/meas.log` logs libmnl's raw measurements once a
    second (`mtk_gnss_get_measurement`, the call behind Android's
    GnssMeasurement, and `mtk_gnss_get_clock`) as hex, timestamped like
    strace: the reference for decoding the chip's records ("Replacing
    libmnl"). Set it with a runtime drop-in for the unit. The call takes 12
    arguments: six arrays of 0xA4-byte entries (GPS 0x18FC, GLONASS 0xF60,
    BeiDou 0x285C, Galileo 0x1710 bytes, then 0xC2C and 0x8F8) and six small
    per-satellite byte arrays. The decompiler showed only the first 4 (the
    rest go on the stack); called with 4 it crashes. libmnl stores doubles
    into them with VFP stores, so they must be word-aligned (unaligned:
    SIGBUS).
  - More reverse-engineering switches, all off unless set:
    - `TITAN_MNLD_DBG=/path` (with `TITAN_MNLD_DBG_TYPE`, default `0x11`,
      stock mnld's "debug to file"): libmnl's debug log, through callback
      12. Most lines are obscured; `tools/gnss/mnl-dbglog.py` decodes them
      (each printable byte is ((previous plain byte + byte) mod 0x60) +
      0x20, from 0x4D at each line). Other lines are binary.
    - `TITAN_MNLD_MEMDUMP=/dir`: the process's writable memory every 20 s.
      libmnl keeps no copy of its reported values (it computes them on each
      call), so this was a dead end.
    - `TITAN_MNLD_DROP=29,3b` (`TITAN_MNLD_DROP_AFTER=s`,
      `TITAN_MNLD_DROP_B2=hex`, `TITAN_MNLD_DROP_ID=n`): titan-mnld exports
      its own `read()`, which libmnl binds to; on `/dev/stpgps` it breaks
      the chip's frames of those types (only `29` records with that byte 2
      and satellite field, if given) after that many seconds, so libmnl
      drops them. Which measurements disappear shows which reports carry
      them.
- **No assistance data.** libmnl asks for AGPS (EPO and QEPO orbit
  predictions, NTP time, network location) through
  `mtk_gps_sys_agps_disaptcher_callback`. titan-mnld logs each request
  type once and declines it, so the engine runs standalone and cold
  starts take longer. Fetching QEPO and EPO over Debian's network is the
  obvious next feature.
- **Stock mnld** (from before titan-mnld) needs 32 Android libraries
  (HIDL, sensors, curl, OpenSSL, nvram), Android init's socket and a
  property shim. Only the phone has a copy, at `/opt/android-gps.stock`.

### HAL ↔ mnld protocol (titan-gnss ↔ titan-mnld)

Recovered from `gps.default.so`; titan-mnld speaks it, as stock mnld does.
All values are little-endian. The abstract socket names are NUL-padded to
the full 108-byte `sun_path`.

| Direction | Message |
|---|---|
| to mnld | `[i32 1][i32 cmd]`: 101 init, 102 start, 103 stop, 108 set_position_mode `[mode][recurrence][interval ms][accuracy][time][u8 low power]` |
| from mnld | `[i32 1][i32 104]` NMEA: `[i64 timestamp][u8 present][i32 len incl. NUL][text][i32 len]` |

If the engine goes quiet for 30 s (for example after titan-mnld restarts),
titan-gnss starts the session again.

### The libmnl interface (titan-mnld ↔ libmnl)

Recovered by decompiling stock mnld with Ghidra (it keeps its symbols) and
checked with `tools/gnss/mnlspy.c`. mnlspy is an `LD_PRELOAD` library that
logs every libmnl call stock mnld makes, every callback and the config
bytes. Run it in the stock chroot with a `/run` drop-in on the old
`titan-mnld.service`:
`Environment="LD_PRELOAD=libpropshim.so libmnlspy.so"`, log in the chroot's
`/tmp/mnlspy.log`.

1. `mtk_gps_sys_function_register(table)` registers 17 callbacks
   (`titan-mnld.c` lists them):
   - `nmea_output_to_mnld(buf, len)` carries each second's NMEA.
   - `gps_mnl_callback(type)` signals events. 13 = NEED_RESTART: stock mnld
     exits, and so does titan-mnld (systemd restarts it).
   - Entries 8 and 9 are a byte cipher: rotate right by 2, then XOR 0x63,
     and the reverse.
   - The rest are logging, offload, suspend and sensor hooks that do
     nothing here.
2. Open `/dev/stpgps`. Its ioctls supply two values for the init struct:
   - `11`: clock type (0x01 = co-clock);
   - `16`: eLNA pin (155).

   `10` is the RTC flag; mnld calls it too and uses no value from it.
3. `mtk_gps_mnl_run(init_cfg, driver_cfg)` returns `0x12` on success and
   runs its own threads; libmnl reads and writes the device itself.
   - `init_cfg` is 0x60 bytes: clock, chip key `0x170`, chip id
     `0xffff6771`, 1 Hz.
   - `driver_cfg` is 0x434 bytes: data paths, the DSP fd at 0x280, and an
     extension block with magic `0xAA55`.
   - Both are in `userspace/titan-mnld/config.h`, from the spy dump.
4. Then `mtk_gps_set_param` with keys 0x3B=0, 0x49=0, 0x3C=1 and 0x4A=0.
5. To stop: `mtk_gps_mnl_stop()`, `mtk_gps_set_param(0, NULL)`, close the
   fd.

libmnl imports two functions from its host executable:
`mtk_gps_sys_agps_disaptcher_callback` and libcutils' `property_get`. It
reads only `ro.build.type`, and titan-mnld returns the default.

## The /dev/stpgps protocol (libmnl ↔ chip)

From strace captures of libmnl's sessions (`tools/gnss/gps-capture.sh` on
the phone, attached before titan-mnld opens the device; 2026-10-13 and
2026-10-29, the second with satellites tracked and a fix). The decoder,
`tools/gnss/stpgps-decode.py`, reads them.

**Framing (both directions):**

```
AA F0 | len (2 bytes) | body (len bytes) | AA 0F
body  = [type] FE [fields...] [u16 checksum]
len   = 12 bits; the chip puts a sequence number in the top 4 bits
checksum = 16-bit sum of the two length bytes as sent and the body bytes
           before it
escapes in the length and the body: DE DF → AA,  DE E0 → DE
```

- The checksum holds for every frame, both ways (3987 written, 1264 read
  in the 2026-10-29 capture). It was first noted as "the body's sum plus
  len"; that only agrees for lengths under 256, and the firmware chunks
  (1406 bytes) looked like they had their own trailer.
- The length is escaped like the body: the last firmware chunk (0x4DE
  bytes) starts `AA F0 DE E0 04`.
- Chip→host payloads after `[type] FE` are XORed with **0xCA**.
- The read side is a byte stream: frames are split across `read()` calls.
- Every byte the chip sends is inside a frame (the pieces that looked
  unframed were reads the decoder failed to join, below).
- strace splits a call that another thread interrupts into
  `<unfinished ...>` and `<... resumed>` lines; the decoder joins them.
  Missing them first made 3 firmware chunks look skipped. A split read is
  written `read(6,  <unfinished ...>` with two spaces, which the decoder
  also missed until 2026-10-29: a quarter of the chip's output was lost,
  and with it every record of the satellites in steady tracking, which
  looked like a separate "tracking report" that doesn't exist (and broke
  the once-a-second `42` and `28` reports into unframed pieces).

**Host → chip:**

| Type | When | What it looks like |
|---|---|---|
| `08` | session start | a 110-byte header (it carries the patch's header, below, and the chunk count, 49), then short download controls |
| `0C` | session start | **the firmware patch**, `[u16 number from 1][data]` (below) |
| `04` | after the setup | 30 bytes of FF |
| `02` | 1 Hz | **satellite search assignments**, 24 bytes each, ~30 a second: `[satellite][constellation: 01 GPS, 10 GLONASS, 20 BeiDou][u16][u16 seconds, +1 a second][u16]` and predicted values (Doppler and code phase, it seems). The satellite is the PRN for GPS and BeiDou, and the frequency channel k + 8 for GLONASS (libmnl's slot 11 at k = 0 is 8). libmnl aids the search from its saved almanac and ephemeris. |
| `07`, `0A` | occasional | short controls |
| `05` | setup, then periodic | register-style settings `[u16 id][value]` |
| `00` | 1 Hz | a millisecond counter rising by 1000 each second (the low 16 bits of `01`'s, a second later) |
| `10` | 1 Hz | clock and frequency values that drift slowly |
| `01` | 1 Hz | constant fields (contains 10000 = `0x2710`) plus the chip's millisecond tick (u32 at byte 8, +1000 a second) |
| `03` | 1 Hz | the satellite list: `00 [count]` then `[satellite][constellation]` pairs, numbered as in `02`, roughly strongest first (33 or 34); plus a short `03 0100` |
| `1C` | 1 Hz | an empty control frame |

**The firmware patch.** 61440 bytes (0xF000), in libmnl.so at file offset
0x3E0106 after an 8-byte header: `[u16 0x7800, its size in 16-bit words]
[u16 0x8000] [u16 0x8004] [u16 0x007B]`. libmnl holds other images with the
same header (another follows at 0x3EF106); the MT6631's is recognised by
its CRC-32, 0x5A7874A9. It goes in 1400-byte blocks, a block whose escaped
data would pass 1412 bytes as two halves (10 frames of 700), 49 frames in
all. **Flow control:** the chip answers the header with `31`
(`0000 4615 ...`) and each chunk with a `32` carrying its number; libmnl
sends the next chunk only then (about 10 ms each).

**Writes are batched.** libmnl writes each moment's frames in one
`write()` of at most 1424 bytes: the 12 setup frames together, the ~30
search assignments together, the 1 Hz frames together. One frame per
write fills the STP link's window (the driver then answers ENOSPC: "native
process should not call GPS_write with no delay at all").

**Chip → host:**

- `31`: the answer to the session header.
- `32`: 6-byte acknowledgements, one per firmware chunk.
- `29` (61 B): about 7-11 a second, with satellites in view: per-channel
  measurement records (structured, varying with the sky).
- `3A`, `3B`, `4A`, `4B`, `14`: seen with satellites tracked.
- `4E` (20 B), `37` (65 B), `2C`, `30` and `29` (61 B): about one per second.
  These are the measurement and status reports, with mostly zeros indoors.
- `42`, `28`, `2A`, `2B`: occasional.

**Periodic restart:** about every 20 s, mnld sends `05 FE 04 00` three
times and then downloads the whole patch again. This is libmnl's own
recovery while it has no signal; the NMEA output carries on through it.

### Verdict: replacing libmnl with open code

1. **The chip cannot run without a closed blob.**
   - Every session starts by loading a 60 KB DSP firmware patch that exists
     only inside libmnl.so.
   - The best possible outcome keeps it, extracted from the phone's own
     vendor image into a firmware file at build time (as the Wi-Fi and BT
     firmware are handled now).
   - "No Android blobs" can therefore mean only "no Android *userspace*",
     never "no blobs".
2. **libmnl is not a thin protocol layer; it is the whole receiver back
   end.**
   - Each second the host sends channel and search configuration and timing
     and clock corrections to the chip.
   - The chip returns scrambled, undocumented measurement records.
   - Replacing libmnl means writing:
     - acquisition and channel scheduling against an unknown register set;
     - the measurement decoder (field layout and scaling unknown);
     - navigation-message decoding;
     - a PVT solver (RTKLIB could provide this part).
   - It could only be validated outdoors, one capture at a time.
   - The scale is months of reverse engineering with a real risk of never
     reaching a fix.
3. **The smaller step is done: titan-mnld** (above) hosts libmnl in place
   of stock mnld. Notes from planning it:
   - libmnl.so itself needs only bionic's libc, libm, libdl and libc++.
   - mnld needs 20 more: HIDL, sensors, curl, OpenSSL, nvram and others.
   - A small open replacement for the mnld daemon, linked against libmnl,
     would shrink the chroot from 32 Android libraries to libmnl plus 4
     bionic ones and the linker.
     - Through a libhybris-style loader it could even run without the
       chroot.
     - It would also let our own code handle EPO assistance over Debian's
       network stack.
   - The engine would stay closed.

## Replacing libmnl

The plan agreed on 2026-10-13: open code step by step, with a go/no-go at
each stage. The DSP firmware patch stays MediaTek's: it is read from the
phone's own libmnl.so at run time (`titan_gps::patch`) and never stored in
the repository.

1. **Chip bring-up (2026-10-29): GO.** `userspace/titan-gps` (Rust, no
   dependencies; `build.sh` cross-builds for the phone and runs the tests):
   - `frame`: the framing above (encode, a streaming decoder for the chip's
     side); the tests check a frame byte for byte against libmnl's;
   - `patch`: finds the image in libmnl.so by its CRC and cuts it into
     chunks as libmnl does (all 49 frames byte-identical to libmnl's);
   - `titan-gps-probe SCRIPT`: opens `/dev/stpgps` (Location off: one owner
     at a time), runs a script from `tools/gnss/stpgps-script.py` (libmnl's
     session frames from a capture, with timing and write batching; the
     patch as one `PATCH` line), waits for `31` and each `32` during the
     download, retries ENOSPC, and logs and counts the chip's frames.

   Result: a 40 s session driven entirely by our code, every report type
   libmnl gets (`42`, `4E`, `37`, `30`, `2C`, `28` once a second; `29`
   about 11 a second; `3A`, `3B`, `2A`, `2B`, `4A`, `4B`), no ENOSPC, no bad
   sums. Two things it took: waiting for each chunk's acknowledgement (a
   chunk sent on top of the last stalls the link) and batching writes.
   The session frames are still libmnl's, replayed from a capture
   (including its satellite search assignments, an hour stale).
2. **Decoding the measurements** (in progress). The reference is libmnl's
   own measurements (titan-mnld's `TITAN_MNLD_MEAS` log) beside an strace
   capture of the same session: each record is matched to the satellite
   and second it describes, and bit fields are searched for ones that
   follow libmnl's values. Fields are bit-packed, little-endian (bit n is
   bit n % 8 of byte n / 8, after the 0xCA XOR). The `29` record so far
   (57-byte payload):

   | Bits | Field |
   |---|---|
   | 0-2 | flags |
   | 3-7 | channel, 0-31 (the chip assigns them; a channel moves between satellites) |
   | 11-15 (-16) | satellite: GPS PRN; GLONASS k + 8; BeiDou PRN modulo 32, + 32 (6 bits: C1 and C33 alike; the search assignments tell them apart) |
   | byte 2 | constellation: `08` GPS, `80` GLONASS, `01` BeiDou (bit 16 doubles as the BeiDou id's top bit), `10` rarely (Galileo?) |
   | 24, 27-31 | C/N0: ((byte 3 >> 3) \| (byte 3 & 1) << 5) − 3 = libmnl's dB-Hz (GLONASS and BeiDou agree every time) |
   | 40-47 | `A0` in most records; `50` marks another layout (GLONASS searches, it seems) |
   | 160-191 | the time the record was latched: 2^-27 s units, modulo 32 s (bit 187 is 1 s). Records come in groups sharing one time |
   | 240-263 | Doppler, signed, 2^-11 Hz, without the receiver clock's drift (about 38.5 kHz here, which libmnl adds to every satellite); r = 0.999 against libmnl's, the rest in libmnl's 16 Hz steps |

   Every satellite libmnl measures has its `29` records: with the decoder
   fixed (above), each of libmnl's measurements in three captures pairs
   with a record of its satellite from the second before. Dropping frames
   (`TITAN_MNLD_DROP`) agrees: without the `29` records of one constellation
   (byte 2), or of one satellite (its id), exactly those measurements stop;
   without `3A`, `3B` or `2B` nothing changes. Satellites in steady
   tracking get about two records a second, of two kinds, and only one
   kind's C/N0 agrees with libmnl's.

   The other reports, so far:
   - `2B` (46 bytes): GPS navigation subframes, 300 bits, about every 6 s
     per satellite; its "channel" byte (byte 2 >> 3) is the PRN. The
     ephemeris for our own solution can come from here.
   - `3B` (14 bytes, every 2 s per channel): the navigation bit stream,
     nearly the same bytes on every GPS channel at once (the satellites
     send their subframes in step). `3A` (5 bytes, every 2 s) goes with
     it.
   - `30`, `37`, `2C`, `4E` (once a second): counters and status, no
     per-satellite values.

   Still open:
   - The code phase (the pseudorange). It isn't a fixed field compared
     across satellites at libmnl's epochs: the records are latched at
     their own times, so it needs the per-record time above and the
     Doppler to bring them to one instant.
   - The two record kinds of steady tracking.
   - Then the 1 Hz host frames (`10`, `01`'s constant part) and `02`'s
     predicted values, so the probe can make its own instead of replaying.
3. **Aiding and the solution**: search assignments from ephemeris (first
   from broadcast ephemeris downloaded over the network, later decoded from
   the satellites), pseudoranges from the measurements, and a PVT solver
   (RTKLIB's is the obvious start).

