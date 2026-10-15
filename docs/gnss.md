# GNSS (MT6631 CONNSYS GPS)

Status (2026-10-13): the receiver runs on MediaTek's closed navigation
library, **libmnl.so**, hosted by **titan-mnld**, our own replacement for
the stock daemon (mnld). **3D fix verified outdoors** with stock mnld: 12 of
20 satellites used (GPS, GLONASS, BeiDou, SBAS), HDOP 0.74, about ±14 m,
after a few minutes from cold with no assistance data. titan-mnld tracks the
same satellites indoors; **its outdoor fix is not verified yet**. The
**Location** app (app drawer) and `titan-location on|off` switch it. It is
off by default because it drains the battery.

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

This is the result of a feasibility probe into replacing libmnl with open
code. The data is an strace capture of 2 minutes of mnld traffic: 540
transfers, 210 KB written and 16 KB read. The decoder,
`tools/gnss/stpgps-decode.py`, holds the capture command in its header.

**Framing (both directions):**

```
AA F0 | u16 len | body (len bytes after unescaping) | AA 0F
body  = [type] FE [fields...] [u16 checksum]
checksum = 16-bit sum of the body bytes before it, plus len
escapes in the body: DE DF → AA,  DE E0 → DE
```

- The checksum holds for every command and status frame.
- Chip→host payloads after `[type] FE` are XORed with **0xCA**.
- The read side is a byte stream: frames are split across `read()` calls.
- Some chip output is not in `AA F0` frames: blocks XORed with 0xCA that end
  in `AA 0F`. The decoder folds them into the preceding type-0x2C frame.
  They are not understood yet.

**Host → chip:**

| Type | When | What it looks like |
|---|---|---|
| `08` | session start | a 110-byte header, then download control |
| `0C` | session start | **the firmware patch**: about 48 chunks of 1400 bytes, numbered from 1. Each chunk is the next 1400 bytes of a ~66 KB image stored **inside libmnl.so at file offset 0x3E0106**, plus a 2-byte trailer (not the frame checksum). |
| `04` | end of the download | the last partial chunk |
| `05` | setup, then periodic | register-style settings `[u16 id][value]` |
| `00` | 1 Hz | a millisecond counter rising by 1000 each second |
| `10` | 1 Hz | clock and frequency values that drift slowly |
| `01` | 1 Hz | constant fields (contains 10000 = `0x2710`) plus a counter |
| `03`, `1C` | 1 Hz | 174-byte channel and search configuration plus short control frames |

**Chip → host:**

- `32`: 6-byte acknowledgements.
- `4E` (20 B), `37` (65 B), `2C`, `30` and `29` (61 B): about one per second.
  These are the measurement and status reports, with mostly zeros indoors.
- `42`, `28`, `2A`, `2B`: occasional.

**Periodic restart:** about every 20 s, mnld sends `05 FE 04 00` three
times and then downloads the whole patch again. This is libmnl's own
recovery while it has no signal; the NMEA output carries on through it.

### Verdict: replacing libmnl with open code

1. **The chip cannot run without a closed blob.**
   - Every session starts by loading a ~66 KB DSP firmware patch that exists
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
