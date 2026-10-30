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

**Replacing libmnl (2026-10-30): GPS fixes without it.** Our own code
brings the receiver up, gives it search assignments from broadcast
ephemeris and makes ~100 m position fixes from its measurements, fed to
gpsd by `titan-gnss-open`, which the Location switch starts by default since 2026-10-30 (`titan-location engine libmnl` for MediaTek's engine). **[gnss-open.md](gnss-open.md) is the
current summary and handoff for that work**; this page is the full record
behind it (see "Replacing libmnl" below), and some of its early sections
are history now.

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
    - `TITAN_MNLD_FLIP=byte:mask` (`TITAN_MNLD_FLIP_B2=hex`,
      `TITAN_MNLD_FLIP_ID=n`, `TITAN_MNLD_FLIP_PERIOD=s`, default 10): in
      the same `read()`, XORs payload byte `byte` of the chosen `29`
      records with `mask` during alternate windows of that many seconds,
      and fixes the checksum. Each change is logged to the measurement log
      as `F <ms> <byte 1> <byte 2> <old> <new>`, so the step in libmnl's
      measurement gives that bit's meaning. On the phone,
      `/root/flip-run.sh byte:mask seconds [b2] [id]` runs one experiment
      with Location on and leaves `/tmp/flip-byte:mask.log`.
    - `TITAN_MNLD_NAV=/path`: libmnl's navigation events
      (`mtk_gnss_get_navigation_event(event, svid, 1)` for GPS PRNs 1-32,
      once a second) as `N <ms> <svid> <return> <0x60 bytes hex>`; the data
      (offset 0x14) are ten words `[6 parity bits][24 data bits]`: plain,
      decoded GPS subframes, the reference for `2B`.
    - `TITAN_MNLD_FLIP2B=prn=byte[-byte]:mask,...`: the flip experiment
      for `2B` navigation subframes, by the PRN in their header (byte 2 >>
      3), during alternate `TITAN_MNLD_FLIP_PERIOD` windows (use 30, the
      subframe cycle); logged as `F <ms> 2b <prn> <old> <new> <byte>`.
      Pair with `TITAN_MNLD_NAV`.
    - `TITAN_MNLD_WFLIP=type:offset:width:delta` (`TITAN_MNLD_WFLIP_SV`,
      `_CON` hex, `_PERIOD` s, default 10, 0 = always): the host side.
      libmnl imports `write()` as well, and writes whole frames, so
      titan-mnld rebuilds each write: in host frames of that type it adds
      `delta` to the little-endian field of `width` bytes at that payload
      offset, during alternate windows, and fixes the sum and the escaping
      (frames it doesn't change come out byte for byte the same). Each
      change is logged as `W <ms> <type> <byte 0> <byte 1> <old> <new>`.
      `/root/wflip-run.sh type:offset:width:delta seconds [sv] [con]`
      (tools/gnss/wflip-run.sh) also records the traffic with strace,
      which can attach after `/dev/stpgps` is opened:
      `stpgps-decode.py --fd 6` reads such a trace.
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
| `02` | 3 a second | **per-satellite channel assignments**, 20-byte payload, ~30 a second: `[satellite][constellation: 01 GPS, 10 GLONASS, 20 BeiDou][0400][u16 seconds, as in 01][u16 sub-step, 1/24 s: three a second, 8 apart][i32 Doppler, 2^-16 Hz][u8, nearly always a multiple of 3][u8][u16 small while tracked (4, 15, 46), large while searching (156-475): a Doppler window in Hz, it seems][u16 mode: 0100 search, 0301 track][u16 41/43/01/03]`. The Doppler excludes the receiver clock's drift: against libmnl's measured Doppler minus a per-epoch common value (~38.4 kHz, the drift), it agrees to 1.3 Hz (median, three captures). While a satellite is searched it is libmnl's prediction (almanac/ephemeris); once tracked, libmnl's own estimate, updated three times a second. There is no code-phase prediction: the chip searches code phase itself. **The Doppler is the acquisition search centre and is ignored while tracking** (`TITAN_MNLD_WFLIP`): +200 Hz on a tracked satellite changed neither the chip's record Doppler nor libmnl's; +3 kHz on every GPS satellite from the start, and no GPS satellite was acquired in 90 s (750 measurements normally; GLONASS and BeiDou dropped too, from 338/408 to 2/75, libmnl's clock estimate relying on GPS, it seems). The satellite is the PRN for GPS and BeiDou, and the frequency channel k + 8 for GLONASS (libmnl's slot 11 at k = 0 is 8). |
| `07`, `0A` | occasional | short controls |
| `05` | setup, then periodic | register-style settings `[u16 id][value]` |
| `00` | 2 a second | `00 00 [u16 milliseconds]`, +500 each frame |
| `10` | 2 a second | `01` then three groups `[u16 a][0000][ffff][u16 b]` (one per constellation, it seems; a steps by 800 and b by 448 from group to group). a and b fall slowly in step in all three groups and follow the receiver clock's drift (r 0.89-0.98), but at 0.56-0.76 Hz per unit, different in each capture: a clock or temperature compensation setting rather than the drift itself. Adding 20 to the GPS group's a (`TITAN_MNLD_WFLIP=10:1:2:20`) changed nothing in tracking: neither the records' Doppler (0 ± 6 Hz) nor libmnl's clock drift. The stage 1 probe tracked with an hour-old replay of these |
| `01` | 2 a second | `[u32, ~0x60][u32, ~0x0f93, rising slowly][u16 milliseconds: 00's value from the frame before][u16 0x0420][00][u16 seconds, as in 02][00 00 00]` |
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
   | 160-215 | a batch value, identical in every record of one constellation in one batch (GPS `…dbf930`, GLONASS `…b21a38`, BeiDou `…51fae0`). It is not a latch time: byte 23 steps +8, +8, 0 and byte 25's top bits cycle with period 3 (about three batches a second) |
   | 213-231 | integer code phase in C/A chips: the sum of two 10-bit counts with their bits shuffled (see below) |
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

   **Code phase (2026-10-29).** Mapped bit by bit with `TITAN_MNLD_FLIP`
   (above): flipping a bit of the `29` records moves libmnl's
   `received_sv_time` for that satellite by exactly that bit's weight
   (repeatable to about 10 ns), so `tools/gnss/flip-steps.py` reads each
   bit's meaning straight off the measurement log. Weights in C/A chips
   (1 ms = 1023 chips; libmnl takes the sum modulo 1023):

   | chips | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
   |---|---|---|---|---|---|---|---|---|---|---|
   | count A, bit | 219 | 220 | 221 | 222 | 223 | 216 | 217 | 218 | 227 | 228 |
   | count B, bit | 229 | 230 | 231 | 224 | 225 | 226 | 213 | 214 | 215 | ? |

   Count A is byte 27 rotated right by 3, plus byte 28's bits 3-4; count B
   is byte 28 rotated right by 5 plus byte 26's top bits (always 0 in the
   captures so far; bits 208-212 together are worth 464 chips, so byte 26
   holds more of the counts, unmapped bit by bit).

   Checked against libmnl in three captures, in pairs of satellites from
   one batch: A + B matches the difference of libmnl's times to a median of
   1.2 chips, 0.6 once each pair's Doppler difference × the time between
   the record and libmnl's epoch is allowed for (it was 80 chips with the
   older reading). For one satellite over time, A + B steps exactly as
   libmnl's does (72 chips per 3 s); what is left:
   - an offset that depends on the batch value (bytes 20-26) and is the
     same for every satellite of the batch. A position solution absorbs it
     into the receiver clock bias;
   - the fraction of a chip, which the record doesn't carry. XORing whole
     bytes (one byte per satellite, `TITAN_MNLD_FLIP_SATS`) showed that no
     byte of 4-54 other than 26-28 moves libmnl's time, nor a 2048 Hz bit
     of the Doppler. Instead, libmnl's time for a satellite advances from
     one second to the next by exactly what its own pseudorange rate
     predicts (median 2.3 ns; the rest are whole-second rollovers and the
     jump when the time of week is decoded), with no whole-chip
     corrections. So libmnl carries each satellite's phase itself,
     integrating Doppler, and the chip's whole-chip count anchors it (which
     is why a flipped bit moves the time by exact whole chips). The
     fraction's starting point is host state from setting up the channel
     (`02` and the 1 Hz frames). An open receiver keeps it the same way:
     it issues those commands itself, or rebuilds the fraction from the
     whole-chip count and the integrated Doppler.

   Still open:
   - The batch offset (above), and how the `02` and 1 Hz host frames set a
     channel's code phase (the fraction's starting point).
   - The two record kinds of steady tracking.
   - The host frames, so the probe can make its own instead of replaying
     (table above): `02`'s Doppler (the search centre) and timing are
     decoded; left are its window/level/mode fields, which only matter to
     search, and what `10` is for (not tracking). `TITAN_MNLD_WFLIP` tests
     them.
3. **Aiding and the solution**: search assignments from ephemeris (first
   from broadcast ephemeris downloaded over the network, later decoded from
   the satellites), pseudoranges from the measurements, and a PVT solver
   (RTKLIB's is the obvious start).

   **Our own GPS search assignments work (2026-10-29).** The chip acquires
   GPS satellites from `02` frames computed with no libmnl:
   - `tools/gnss/gpseph.py`: GPS orbits from a RINEX 3 broadcast
     navigation file (IS-GPS-200), and the Doppler seen from a position,
     without the receiver's drift. Against libmnl's own `02` frames it
     agrees for every satellite to within a common offset of -145 to -152
     Hz (about 2 Hz scatter) in four captures: libmnl folds that much of
     the drift into `02`. The file came from IGS's merged broadcast
     ephemeris (BKG: `root_ftp/IGS/BRDC/<year>/<day>/BRDC00WRD_S_...MN.rnx.gz`,
     updated every 15 min).
   - `tools/gnss/own02.py`: rewrites every GPS `02` frame of a replay
     script with that Doppler (minus 150 Hz) for the moment the probe will
     send it, holding the other fields at the satellite's first
     (search-mode) frame; `tools/gnss/own02-run.sh` does this on the phone
     seconds before starting titan-gps-probe;
     `tools/gnss/probe-sats.py` says what the chip tracked.
   - Result, indoors, 90 s each from the same base capture (records at
     C/N0 >= 20 dB-Hz): our `02`, GPS 522 and 492 in two runs, 13 and 8
     satellites; libmnl's own values ten minutes stale, GPS 3; ours offset
     by 3 kHz, GPS 3 (brief threshold crossings). GLONASS and BeiDou,
     replayed the same in every run, varied between 20 and 100 at random.
     libmnl in its own session tracked 10 GPS satellites.

   Still replayed from libmnl: the setup, the clock frames (`00`, `01`,
   `10`), the satellite list (`03`) and GLONASS/BeiDou's `02`. The probe's
   firmware download used to stall in about half of back-to-back
   sessions; fixed, see "The probe's firmware download" below.
   `titan-gps-probe --quiet` writes no log at all.

   **First position fix without libmnl (2026-10-30).**
   `tools/gnss/snapfix.py` makes a GPS fix from titan-gps-probe's log
   alone: the chip's whole-chip code phases (`29`), broadcast ephemeris,
   the probe's start time (the phone's NTP clock) and a rough position.
   It is a snapshot ("coarse-time") fix, as AGPS receivers make: each
   batch of records (bytes 20-26 alike) gives each satellite's transmit
   time modulo 1 ms; whole milliseconds are resolved against one
   satellite from the rough position and time, and least squares finds
   position, clock bias and the error of the time stamp. All batches are
   then solved together (one position, a bias and time error per batch).
   - Live: rough position 20 km off (the true one rounded to 0.5°, height
     0), fresh ephemeris, our own `02` for GPS, the probe for 60 s (the
     first try was the one session in ten that never measures; the second
     tracked 10 GPS satellites): joint fix **67 m** from libmnl's own
     indoor fix of the same spot. Two earlier probe runs: 45 m and 78 m,
     the same from starting guesses 0 to 67 km off.
   - Residuals (~65 m rms) are what whole chips predict (±146 m, 85 m rms
     per satellite). Better needs the sub-chip phase, which the record
     doesn't carry: carry it in software from the integrated Doppler, as
     libmnl does.

   **titan-gnss-open: open fixes into gpsd (2026-10-30).**
   `userspace/titan-gnss-open`: a Python daemon (and an unenabled unit
   that conflicts with titan-gnss and titan-mnld) that feeds gpsd with no
   libmnl. Each session it refreshes the GPS broadcast ephemeris (about
   hourly; an older file is used offline, up to 4 h), rewrites the GPS
   `02` assignments of a replay script for that moment from the last
   position, runs titan-gps-probe for 10 minutes (the receiver keeps
   tracking after the script's ~90 s), reads the chip's records as they
   come and, every 2 s, solves a joint snapshot fix over the last 30 s,
   sending NMEA GGA and RMC to gpsd's UDP input (127.0.0.1:10110, as
   titan-gnss does). Install: `userspace/titan-gnss-open/install.sh`;
   then, on the phone, in `/var/lib/titan-gnss-open` (root only, never in
   the repository): `base.script` (a libmnl session captured on the phone:
   gps-capture.sh, stpgps-script.py) and, before the first fix,
   `position` ("lat lon" within some tens of km; later the last fix).
   With Location off: `systemctl start titan-gnss-open`.
   - First run, indoors: the first fix about 100 s into the session, then
     one every 2 s; gpsd reported 3D fixes 45-136 m from libmnl's own fix
     of the spot (height ~+200 m); 31 fixes in four minutes on 5-6
     satellites; the daemon used 5 s of CPU in that time.
   - Still from libmnl: the replayed setup and clock frames, the
     satellite list and GLONASS/BeiDou's assignments (from base.script),
     and the firmware patch (read from the phone's libmnl.so). GPS only.

   **The probe's firmware download (2026-10-29).** `32 [u16 n]` (raw, not
   XORed) acknowledges chunk n, about 10 ms after it (in strace it can look
   like a request for the next chunk: a blocking `read()` is stamped when
   it starts). Two things made sessions fail:
   - now and then an acknowledgement doesn't come; libmnl then sends the
     chunk again (its captures show one chunk sent twice), and the probe
     used to give up. It now sends a chunk up to 3 times, then gives up
     on the session.
   - now and then (about one session in ten, at 10-20 s gaps) the session
     header isn't answered at all, though the driver powers GPS on fine
     (when it is answered, it's within 2 ms). The probe now stops such a
     session (exit 1) instead of sending the patch; the next one, after
     a short wait, works. About as often, a session completes the patch
     (with a resend) but the receiver never starts measuring: a caller
     should check for `29` records and start again.
   **Never push the patch into a receiver that isn't answering**: sending
   chunks over and over with no acknowledgements (about 150-190 writes)
   hung the whole connectivity chip three times (STP `tx_timeout`,
   `consys_hang_debug`, then a kernel "whole chip reset"; Wi-Fi and
   Bluetooth recovered by themselves after a minute or so). With the
   checks above, ten back-to-back sessions at 10 s gaps caused no chip
   reset (8 measured, 1 stopped at the header, 1 never measured).


   **The Doppler field, the fraction and the reference (2026-10-30).**
   - The plan was to carry the sub-chip code phase by integrating each
     satellite's Doppler between records, as libmnl does. It doesn't
     work with the records' Doppler: against the Doppler predicted from
     broadcast ephemeris at libmnl's fix, it is off by a per-satellite
     amount that holds through a session (probe session: +57 Hz for G8,
     −306 Hz for G1, −231 Hz for G17; libmnl session: −24 to −355 Hz). The
     whole chips follow the geometry instead: their drift between two
     satellites matched the ephemeris Doppler difference (G1−G8 2.575
     chip/s measured, 2.580 predicted; G17−G8 3.359 and 3.363; G7−G8 0.305
     and 0.31), not the records' (2.344, 3.176, 0.19).
   - No field of the record holds a fraction: adding any bit field (3-16
     bits, outside the known ones) to the whole chips leaves two
     satellites' difference as rough as before over a minute (0.41 chip
     rms, best 0.40; rounding alone gives 0.41).
   - Against libmnl's measured time in run6, the whole chips differ by a
     per-satellite amount (G31 −2.9 chips, G16 −2.1, G8 +1.2, the rest
     within ±0.3) that wanders by about a chip during the session. libmnl's
     fixes at the same indoor spot also disagree between sessions: run3
     and run5 are 169 m and 106 m from run6 horizontally (run3 253 m
     lower), each with a claimed 30 m. Measured against run3's fix instead
     of run6's, snapfix's joint fix of the live run is 7 m off
     horizontally, the own02 run's 32 m.
   - `snapfix.py` gained `solve_joint_t` (one time error for all batches,
     so four satellites a batch are enough) and `solve_robust` (leaves out
     a satellite whose mean residual stays over 120 m). Replaying the
     daemon's loop over the live run: first fix after 20 s (24 s before),
     median 68 m in 3D from run3's fix (94 m). The daemon uses it.
