# Open GNSS: state and handoff

Where the work to run the Titan Pocket's GPS without MediaTek's closed
`libmnl.so` stands, what is known, how to work on it, and what not to do.
Written for whoever (person or AI) picks it up next.
[gnss.md](gnss.md) is
the full chronological record, with the evidence behind each finding; this
page is the current summary and takes precedence where they differ (a few
of gnss.md's early sections, like "Verdict: replacing libmnl with open
code", are history now).

Last updated 2026-10-30 (evening).

---

## Goal and why

The project's goal is a clean Linux system on this phone with no Android
userspace (see the [README](../README.md)). GPS is the last piece that
still runs Android code: MediaTek's closed engine `libmnl.so`, in a small
bionic chroot, hosted by our `titan-mnld`. The aim is to replace it with
our own code, keeping only the chip's firmware patch, which stays
MediaTek's (read from the phone's own libmnl.so at run time, never stored
in the repository), as with Wi-Fi and Bluetooth firmware.

The agreed plan, in stages with a go/no-go at each:

1. **Chip bring-up without libmnl.** Done (2026-10-29).
2. **Decode the chip's measurements.** Done for what a fix needs.
3. **Aiding and our own position.** Working: GPS fixes of ~100 m into
   gpsd, with parts of the session still replayed from libmnl.

## Where it stands (2026-10-30)

Two GNSS stacks exist side by side on the phone.

**The everyday stack (libmnl), unchanged**, used by the Location switch:

```
titan-location on → titan-gnss (Rust) → titan-mnld (C, bionic) ──hosts──▶ libmnl.so
                                              │                              │
                                              └──────── /dev/stpgps ◀────────┘  gps_drv (from source)
titan-gnss → NMEA → UDP 127.0.0.1:10110 → gpsd -n → apps
```

**The open stack (no libmnl)**, `titan-gnss-open`, opt-in:

```
titan-gnss-open (Python)
  ├─ GPS broadcast ephemeris, fetched hourly (IGS merged file from BKG)
  ├─ our 02 search assignments (Doppler from ephemeris), rewritten into a replay script
  ├─ titan-gps-probe (Rust): firmware patch + script → /dev/stpgps → MT6631
  ├─ reads the chip's 29 measurement records as they come
  └─ snapshot fix every 2 s over 30 s of batches → NMEA → UDP 10110 → gpsd
```

- First run, indoors: 3D fixes 45-136 m from libmnl's own fix of the same
  spot (height ~+200 m), the first after ~100 s, then every 2 s; 5-6
  satellites; ~5 s CPU in 4 minutes.
- **The Location switch uses it** (2026-10-30): `titan-location on` (the
  Location app, the Compass app's GPS button, the keyboard shortcut)
  starts `titan-gnss-open`. `titan-location engine libmnl` switches to the
  libmnl stack (`ENGINE=` in `/etc/titan-location.conf`, default `open`);
  `off` stops either. The unit is not enabled at boot.
- **Assistance data:** the IGS merged broadcast file (GPS, GLONASS,
  BeiDou ephemeris), fetched hourly. From it the daemon computes the
  satellites above the horizon now and makes every search assignment
  (`02`) and the satellite list (`03`) for them (`own02.regenerate`); the
  base capture's own list was hours stale and left out four of the ten GPS
  satellites at a window. GPS, GLONASS and BeiDou all go into the fix.
- **Atmosphere corrections** (2026-10-30; the single-frequency use of
  L1/L2 data: the chip receives L1, G1 and B1I only, no L2 or L5).
  `snapfix.py` adds to each predicted range the ionospheric delay from the
  broadcast Klobuchar model, scaled by (f_L1/f)² for GLONASS's channels
  and BeiDou B1I, and the tropospheric delay (Saastamoinen dry delay from
  a standard atmosphere at the receiver's height, 0.1 m wet, mapping
  1.001/√(0.002001 + sin²E)). Satellite group delays were already in
  (GPS TGD, BeiDou TGD1; GLONASS has none). The Klobuchar coefficients
  come from the chip: GPS subframe 4 page 18 (`navsub.iono`), cached in
  `STATE/iono`; the IGS merged file carries none. Delays at a window run
  (night): troposphere 2.4 m at zenith to 17 m at 7.5°, ionosphere 1.5 to
  4.4 m. Effect on the window recording: horizontal within 1 m (whole-chip
  rounding dominates), height ~15 m lower. Against the test spot's known height
  the uncorrected height was closer (GPS −6.5 m,
  all three systems −1 m; corrected −21.5 / −18 m). The corrections do what
  the physics says; something else biases this one-sided, through-glass
  view downward by a similar amount. To settle outdoors at the same spot.
- **Ephemeris from the chip** (2026-10-30): during a session the daemon
  decodes the `2B` subframes (`navsub.py`), adds any GPS issue it lacked
  to the running solution, and keeps every decoded issue of the last 4 h
  in `STATE/chip.rnx`. `ephemeris()` merges that file with the download
  and uses it alone when offline (the download fails and `brdc.rnx` is
  over 4 h old), so GPS keeps working without the network once a session
  has tracked. First live run: 6 satellites' ephemerides within the first
  minutes, 9 cached after two sessions; the chip's issues arrive before
  the hourly download has them.
- **Accuracy (2026-10-30, window, against an iPhone's 5 m fix):** the
  regenerated session replayed through the daemon's loop: GPS alone 20 m
  median (p90 35 m) with a 30 s window, 11 m (26 m) with 60 s; with
  GLONASS and BeiDou 18 m and 10 m. With GPS cut to four satellites, GPS
  alone never fixes; with GLONASS and BeiDou it fixes after 88 s (45 m
  median, p90 313 m). Live, the daemon: GPS only with the regenerated list
  266 fixes in 5 minutes, 10 m median (p90 24 m); with GLONASS and BeiDou
  260 fixes on up to 19 satellites, 10 m (p90 19 m), but 25 m high (GPS
  alone 8 m): a constellation biases the height, to check.
- **No libmnl at all** (2026-10-30): the session is generated from scratch
  (`tools/gnss/session.py`: libmnl's constant setup frames, generated `00`/
  `01` counters and `02` time fields, a fixed `10`; `own02.regenerate`
  fills `02`/`03` from ephemeris), and the DSP firmware patch is loaded
  from `/lib/firmware/titan/mt6631-gnss-patch.bin`, extracted once from
  the phone's own libmnl.so (`titan-gps-probe --extract-patch`, run by
  `install.sh`; never in the repository). Live test with `/opt/android-gps`
  (libmnl and its chroot) made inaccessible to the daemon
  (`InaccessiblePaths`): continuous fixes on 10-14 satellites, ephemeris
  from the chip, gpsd in 3D mode. Left out of the generated session: the
  SBAS rotation (`07`), `0A` and `05` 0x000c/0x010c; not needed so far.
  `base.script` is no longer used.
- `len`: 12-bit body length; the chip puts a sequence number in the top
  4 bits. It is escaped like the body.
- Escapes inside len and body: `DE DF` = `AA`, `DE E0` = `DE`.
- `sum` = 16-bit sum of the two length bytes (as sent) and every body
  byte before it, little-endian.
- libmnl writes each moment's frames in one `write()` of at most 1424
  bytes; one frame per write fills the STP link's window (ENOSPC).
- Chip → host payload bytes are scrambled: libmnl decodes each as
  `rotr8(b ^ 0xCA, 3)` (`FUN_0001d5e0`, `rotl(b,5) ^ 0x59`). The probe's
  log and `stpgps-decode.py` undo only the 0xCA, so the field maps here
  (made on those bytes) see each field rotated by 3 bits: that is why the
  code-phase counts look shuffled. `32`'s chunk number is raw (the decoder's
  XOR is undone again).
- Code: `tools/gnss/stpgps-decode.py` (Python), `userspace/titan-gps/src/frame.rs` (Rust).

### Session start and the firmware patch

- The patch: 61440 bytes in libmnl.so at file offset 0x3E0106 after an
  8-byte header, CRC-32 0x5A7874A9 (`userspace/titan-gps/src/patch.rs`
  finds it by CRC). Sent as 49 `0C` frames `[u16 number from 1][data]`
  (1400-byte blocks, split in two when escaping would pass 1412).
- Host sends a `08` session header (110 bytes); the chip answers `31`.
- **`32 [u16 n]` acknowledges chunk n**, about 10 ms after it arrives. In
  strace it can look like a request for the next chunk because a blocking
  `read()` is stamped with the time it *started*.
- Now and then an acknowledgement doesn't come: resend the chunk (libmnl
  does; its captures show one chunk sent twice).
- About one session in ten, the header gets no answer at all (the answer,
  when it comes, is within 2 ms) though the driver turns GPS on fine. About
  as often, the patch completes but the receiver never measures. Both:
  close, wait ~10 s, start again.

### Chip → host

| Type | Rate | What |
|---|---|---|
| `29` | ~7-11/s | **per-channel measurement records**, 57-byte payload (below) |
| `2B` | ~every 6 s per satellite | **GPS navigation subframes, decoded** (2026-10-30; `tools/gnss/navsub.py`). After the full descramble (logged byte → `rotr8(b, 3)`): bytes 0-1 status (bit i = word i failed the chip's check), byte 2 PRN, byte 4 subframe id, bytes 6-45 ten words of 4 bytes `[d9-16][parity][d17-24][d1-8]` (libmnl `FUN_00015ee4`); data bits are source data (D30 inversion undone). Ephemeris from subframes 1-3 matches the IGS broadcast file exactly (0.000 m, same clock terms, 5 satellites) and arrives before the hourly download has it |
| `3B` | 2 s per satellite | **GLONASS navigation strings, decoded** (`navsub.decode_glo`): after `rotr8(b, 3)`, byte 0 the frequency channel k + 8 (as the `29` records name GLONASS satellites), byte 2 the string number (1-15), bytes 4-13 the string's bits 80..1 as five 16-bit little-endian words, MSB first (the idle bit and number not repeated; the Hamming bits 1-8 are there but miss bits they cover, so an ephemeris is accepted once strings 1-4 repeat identically in a second 30 s frame). Checked: t_k matches the time (14 of 17 string-1 frames), and a decoded ephemeris agrees with the broadcast file's neighbouring issue to 2.9 m |
| `4B` | 6 s per satellite | **BeiDou D1 subframes, decoded** (`navsub.decode_bds`): after `rotr8(b, 3)`, bytes 0-1 status, byte 3 PRN, byte 5 subframe (FraID), bytes 7-46 ten 30-bit words packed like `2B`'s (`q1 q0 q3 q2`), already decoded: word 1 26 information bits + 4 parity, words 2-10 22 + 8 (BCH codewords de-interleaved, parity at the end). Checked: preamble in all 51 frames, FraID = byte 5, SOW steps of 6 s; ephemerides agree with the broadcast file to 0.04-0.11 m (previous hourly issue) with identical clock terms. `4D` (same size) is presumably D2, for the geostationary satellites (not visible here) |
| `3A` | 2 s | companion to `3B` |
| `31`, `32` | session start | header answer, chunk acknowledgements |
| `30`, `37`, `2C`, `4E`, `42`, `28` | 1/s | counters and status |

**The `29` record** (after XOR; bit n = bit n%8 of byte n/8):

| Bits / bytes | Field |
|---|---|
| 0-2 | flags |
| 3-7 | channel 0-31 |
| 11-15 (16) | satellite: GPS PRN; GLONASS k+8; BeiDou PRN mod 32 + 32 (6 bits) |
| byte 2 | constellation: `08` GPS, `80` GLONASS, `01` BeiDou |
| byte 3 | C/N0 = ((b3 >> 3) \| (b3 & 1) << 5) − 3 dB-Hz |
| byte 5 | layout: `A0` normal (use these), `50`/`80` other kinds |
| bytes 20-26 (bits 160-215) | **batch value**: identical in every record of one constellation's batch (~3 batches/s); not a time stamp. Defines which records were measured together |
| bits 213-231 | **code phase, whole chips**: from counts A and B (below). GPS (A + B) mod 1023; GLONASS (A + 2B) mod 511; BeiDou B1I (A + B + 1023 × bit 229) mod 2046 |
| bits 240-263 | Doppler, signed, 2^-11 Hz, *without* the receiver clock's drift (~38.4 kHz). **Not the precise carrier Doppler**: off from the geometric Doppler by a per-satellite amount that holds through a session (−24 to −355 Hz in a libmnl session, +57 to −306 Hz in a probe session); libmnl reports it as it is |

Code phase bit weights in chips (from bit flips against libmnl, exact to
~10 ns):

| chips | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|---|---|
| count A, bit | 219 | 220 | 221 | 222 | 223 | 216 | 217 | 218 | 227 | 228 |
| count B, bit | 229 | 230 | 231 | 224 | 225 | 226 | 213 | 214 | 215 | ? |

- `tools/gnss/meas-match.py`: `record()` and `code_phase(p, constellation)`.
- GLONASS and BeiDou (2026-10-30): tested against geometry (a probe
  session at a window, the position from an iPhone's 5 m fix), each batch's
  residuals less their mean: GPS (A + B) mod 1023 gives 92 m rms, GLONASS
  (A + 2B) mod 511 GLONASS chips 148 m, BeiDou (A + B + 1023 × bit 229) mod
  2046 B1I chips 44 m: the rounding of each signal's chip (293, 587, 147
  m). For BeiDou, B's lowest bit (229) is also A's eleventh, 1024-chip bit
  (found by a greedy search for bit weights from a partial fit).
- The value is the satellite's transmit time modulo 1 ms, plus an offset
  common to the batch (a position solution absorbs it in the clock bias).
- **No sub-chip fraction in the record.** No byte of 4-54 other than 26-28
  moves libmnl's time. libmnl's time advances each second by exactly its
  own Doppler (median 2.3 ns): it carries the fraction itself by
  integrating Doppler, anchored by these whole chips.
- The record's Doppler can't do that for us (2026-10-30): carried over
  30 s, counts less integrated record Doppler drift ±500-650 m between
  satellites (~±100 Hz error), though strong signals' counts are a clean
  rounding (a pair stays within ±1 chip).
- Checked again without libmnl (2026-10-30): no bit field of the record,
  added to the whole chips as a fraction, makes two satellites'
  difference smoother over a minute (0.41 chip rms either way, which is
  rounding alone: 0.29 × √2). The whole chips follow the geometry: their
  drift between two satellites matches the ephemeris Doppler difference to
  0.005 chip/s, and not the records' Doppler (off by up to 0.23 chip/s).
  So integrating the records' Doppler can't supply the fraction; the
  geometry (the joint fix itself) is the better smoother.
- Byte 26's low bits (208-212) also count (464 chips together) but are
  always 0 in practice.

### Host → chip

| Type | Rate | What |
|---|---|---|
| `08` | session start | header (110 B, carries the patch header and chunk count) and short controls |
| `0C` | session start | the firmware patch |
| `05` | setup, periodic | register-style settings `[u16 id][value]` |
| `02` | 3/s per satellite | **search assignments** (below) |
| `00` | 2/s | `00 00 [u16 ms]`, +500 each frame |
| `01` | 2/s | `[u32 ~0x60][u32 ~0x0f93, slowly rising][u16 ms = 00's previous][u16 0x0420][00][u16 seconds, as in 02][00 00 00]` |
| `10` | 2/s | `01` + three groups `[u16 a][0000][ffff][u16 b]`; a and b follow the clock drift at a run-dependent scale (TCXO/temperature setting?). Changing them changes nothing in tracking |
| `03` | 1/s | satellite list `00 [count]` + `[sv][constellation]` pairs |
| `1C` | 1/s | empty control |

**`02` payload (20 bytes):**

| Offset | Field |
|---|---|
| 0 | satellite (GPS/BeiDou PRN; GLONASS k+8) |
| 1 | constellation: `01` GPS, `10` GLONASS, `20` BeiDou |
| 2-3 | `0400` |
| 4-5 | u16 seconds (same count as `01`'s) |
| 6-7 | u16 sub-step in 1/24 s (three a second, 8 apart) |
| 8-11 | **i32 Doppler, 2^-16 Hz, without receiver drift** |
| 12 | u8, nearly always a multiple of 3 |
| 13 | u8 |
| 14-15 | u16: 4-46 while tracked, 156-475 searching (a Doppler window in Hz, it seems; libmnl's search frames use 512) |
| 16-17 | u16 mode: `0x0001` search, `0x0301` track |
| 18-19 | u16 `0x0041` / `0x0043` / ... |

- **The Doppler is the acquisition search centre; it is ignored while
  tracking.** Offsetting it by +200 Hz on a tracked satellite changed
  nothing. Offsetting every GPS satellite by +3 kHz from the start: no GPS
  satellite acquired in 90 s.
- libmnl's value = geometric Doppler (from ephemeris) − 145…152 Hz,
  common to all satellites (part of the drift it folds in). There is no
  code-phase prediction: the chip searches code itself.
- BeiDou (B1I carrier): libmnl's = ours − 494 Hz, every satellite within
  ±1 Hz. GLONASS (each satellite on its own carrier): libmnl's = ours + 483
  + 13.4 k Hz for frequency channel k, ±7 Hz; 13.4 Hz per 0.5625 MHz
  channel step is 24 ppm, the receiver clock's drift, scaled to each
  channel. The frame names the channel (k + 8), which two antipodal
  satellites share: use the one above the horizon. `gpseph.py` computes
  BeiDou orbits (geostationary ones rotated as the ICD says) and GLONASS
  (the broadcast state integrated by Runge-Kutta, the ICD's equations);
  `own02.py` rewrites all three (0.3 s for a 90 s script on the phone,
  with Doppler cached per 2 s and GLONASS integration carried forward).

## How a fix is made (`tools/gnss/snapfix.py`)

A snapshot ("coarse-time") fix, as AGPS receivers make:

1. Records of one batch (bytes 20-26 alike, received within 400 ms) are
   measured together. Each gives the satellite's transmit time mod 1 ms
   (whole chips × 293 m).
2. From a rough position (within some tens of km) and time (the phone's
   NTP clock), resolve each satellite's whole milliseconds against a
   reference satellite.
3. Least squares for position, receiver clock bias and the time stamp's
   error (which moves the satellites). Satellite orbit and clock
   (including relativity and TGD) from broadcast ephemeris (`gpseph.py`).
4. All batches together (`solve_joint`): one position, a bias and time
   error per batch, eliminated per batch (Schur complement) so only a 3×3
   system is solved; batches with residual rms > 150 m are dropped.

Each batch's whole chips scatter by rounding only (~85 m rms per
satellite); over 30 s of batches that averages down.

`solve_joint_t` (2026-10-30) keeps one time error for all batches instead
of one per batch: the batches' times relative to each other come from the
probe log and are good to a few ms, so only the start time is unknown.
Each batch then keeps only its clock bias, and batches of four satellites
count. `solve_robust` adds a check: each satellite's mean residual over
the batches (`sat_bias`) should be near zero; one that stays off by more
than 120 m (a reflected signal, say) is left out and the fix redone. The
daemon uses `solve_robust`.

**How good are the fixes?** The earlier figures (45-136 m) were measured
against libmnl's fix in run6, and that reference is poor: libmnl's own
indoor fixes in three sessions at the same spot are 106-169 m apart
horizontally and up to 253 m in height, each claiming ~30 m. Against
run3's libmnl fix, the robust joint fix of the live run is 7 m off
horizontally (63 m low), of the own02 run 32 m (44 m high); replaying the
daemon's loop over the live run gives fixes at a median 68 m from it in
3D, the first after 20 s (24 s and 94 m with the older solver). An outdoor
test against a known position is the next measure of accuracy.

**At a window (2026-10-30, evening).** libmnl's fix there (fresh session,
20 fixes after ~2 minutes) agrees with its run3 fix to 20 m, so it serves
as the reference. The daemon, live with a 30 s window and ≥20 dB-Hz: 118
fixes in 4 minutes, median 131 m from it. A logged probe session at the
same spot, replayed: 88 of its 133 batches had exactly four satellites at
≥20 dB-Hz, so the fix is short of measurements, not wrong:

| Settings | Horizontal p50 / p90 |
|---|---|
| 30 s, ≥20 dB-Hz, height 0 ± 300 m (as the daemon was) | 63 / 141 m |
| same, no height prior | 88 / 142 m |
| same, height prior at libmnl's height ± 30 m | 45 / 113 m |
| 60 s window | 28 / 118 m |
| 60 s, ≥15 dB-Hz (twice the fixes) | 35 / 108 m |
| 90 s, ≥15 dB-Hz | 23 / 76 m |

The daemon now uses 60 s and ≥15 dB-Hz; live at the same window: 222 fixes in 5 minutes, median 45 m (p90 113 m) from libmnl's fix. A longer window averages over the
phone's movement, so it lags while walking: the window should follow
whether the phone moves. A first live run with the 4-satellite rule but
without guards gave fixes kilometres off (position and time error can't
be separated when the same four satellites fill every batch); the daemon
now needs five satellites in two batches or more, and `solve_joint_t`
takes a height prior.

## Methods that work

- **Bit flips on chip data** (`TITAN_MNLD_FLIP`, `TITAN_MNLD_FLIP_SATS`):
  titan-mnld exports `read()`, which libmnl binds to; it XORs chosen bits
  of `29` records (fixing the checksum) in alternate windows and logs each
  change. The step in libmnl's measured satellite time is that bit's
  meaning. `tools/gnss/flip-run.sh` (on the phone as `/root/flip-run.sh`),
  `tools/gnss/flip-steps.py`. Per-satellite mode tests ~11 bits per 100 s.
- **Changes to host frames** (`TITAN_MNLD_WFLIP=type:offset:width:delta`,
  `_SV`, `_CON`, `_PERIOD` with 0 = always): titan-mnld also exports
  `write()`, rebuilds each of libmnl's writes frame by frame, adds `delta`
  to a field and fixes sum and escaping. `tools/gnss/wflip-run.sh` adds an
  strace capture.
- **Pairing records with libmnl's measurements**: `TITAN_MNLD_MEAS` logs
  libmnl's raw measurements; `tools/gnss/meas-match.py` pairs them with
  `29` records from an strace capture. Pair satellites from the same
  batch so the batch's time cancels.
- Dead ends (don't repeat): reading libmnl's decompile for the `29` parser
  (`out/gnss-re/libmnl.c`; libmnl has no XOR code and keeps no copy of
  records), memory dumps (no copies either), libmnl's debug log (status
  only), randomized fuzzing of `29` (whole-byte screening already showed
  nothing else matters), reading the DSP patch (unknown 16-bit ISA, ROM
  hooks; weeks of work).

## Tools and code

| Path | What |
|---|---|
| `userspace/titan-gnss-open/` | the open daemon, its unit, `install.sh` |
| `userspace/titan-gps/` | Rust crate: `frame` (codec), `patch` (finds/cuts the patch), `titan-gps-probe` (runs a script, logs chip frames to stdout or `--log`; `--quiet` disables the log entirely). `build.sh` → `out/titan-gps-probe` (static aarch64 musl), tests included |
| `userspace/titan-mnld/` | our libmnl host (C, 32-bit bionic); env switches `TITAN_MNLD_MEAS`, `_DBG`(`_TYPE`), `_MEMDUMP`, `_RAW`, `_DEBUG`, `_DROP`(`_AFTER`,`_B2`,`_ID`), `_FLIP`(`_SATS`,`_B2`,`_ID`,`_PERIOD`), `_WFLIP`(`_SV`,`_CON`,`_PERIOD`); all off unless set. `build.sh` → `out/titan-mnld` |
| `tools/gnss/stpgps-decode.py` | decode an strace capture (`--dump`, `--fd N` when strace attached after the open) |
| `tools/gnss/stpgps-script.py` | capture → probe replay script (`--fd`, `--seconds`) |
| `tools/gnss/gps-capture.sh` | on the phone (`/root/gps-capture.sh`): libmnl session with strace |
| `tools/gnss/meas-match.py` | `29` record decoding, pairing with libmnl's measurements |
| `tools/gnss/flip-run.sh`, `flip-steps.py` | chip-side bit flips |
| `tools/gnss/wflip-run.sh` | host-side field changes |
| `tools/gnss/gpseph.py` | RINEX 3 GPS ephemeris → satellite position, Doppler |
| `tools/gnss/own02.py`, `own02-run.sh` | rewrite a script's GPS `02` with our Doppler; run it on the phone |
| `tools/gnss/probe-sats.py` | what a probe run tracked |
| `tools/gnss/navsub.py` | GPS subframes, ephemeris (RINEX with `--rinex`) and Klobuchar coefficients (`iono`) from a probe log's `2B` frames |
| `tools/gnss/snapfix.py` | snapshot/joint position fix from a probe log |
| `tools/gnss/mnl-dbglog.py` | decode libmnl's debug log |

Ephemeris source: `https://igs.bkg.bund.de/root_ftp/IGS/BRDC/<yyyy>/<ddd>/BRDC00WRD_S_<yyyy><ddd>0000_01D_MN.rnx.gz`
(IGS merged broadcast navigation, updated every 15 min, public).

## Working with the phone

- `ssh root@172.16.42.1` over USB (key-only). Python 3 is on the phone.
- Only one process can own `/dev/stpgps`. For probe or experiment runs:
  Location off (`titan-location off`), then `systemctl start
  titan-gps-driver`; stop it afterwards.
- On the phone now: `/usr/local/bin/titan-gps-probe`,
  `/usr/local/lib/titan-gnss-open/`, `/var/lib/titan-gnss-open/`
  (`base.script`, `brdc.rnx`, `position`; root only),
  `/opt/android-gps/vendor/bin/titan-mnld` (the build with experiment
  switches; the previous one is `titan-mnld.orig` beside it), `/root/`
  `gps-capture.sh`, `flip-run.sh`, `wflip-run.sh`. Files in `/tmp` go at
  reboot.
- Run long experiments in the background; one probe session is 25-100 s.

## Hazards and gotchas

- **Never push the firmware patch into a receiver that isn't answering.**
  Sending chunks over and over without acknowledgements (~150-190 writes)
  hung the whole connectivity chip three times (STP `tx_timeout`,
  `consys_hang_debug`, then the kernel's "whole chip reset"). Wi-Fi and
  Bluetooth came back by themselves after a minute; GPS opens failed until
  then. The probe now stops a session when the header isn't answered or a
  chunk fails 3 tries.
- strace: a blocking `read()` is time-stamped at its start, so replies
  appear before the writes they answer. strace often attaches after
  `/dev/stpgps` is opened: use `--fd`.
- The kernel log's timestamps don't follow the wall clock (no suspend
  time), and floods wrap the ring buffer quickly.
- The Mac's shell is zsh: `$var` with spaces is not word-split in loops.
- `pgrep -f name` inside an `ssh 'command'` matches the ssh command
  itself.
- A probe log only exists without `--quiet`.

## Privacy and what never goes in the repository

- Captures, logs, scripts, `position` and `base.script` say where the
  phone was: keep them in `out/` (local) or on the phone. **Never print
  or write coordinates** in docs or chat; compare positions by distance
  (`snapfix.py --truth` prints only distances). The reference position for
  tests is libmnl's own fix, in the local capture `out/gnss-cap/run6/gps2.json`.
- The firmware patch and libmnl.so are MediaTek's: read at run time from
  the phone's own copy, never committed. The decompile in `out/gnss-re/`
  stays local.
- Per-phone NVRAM data never leaves the phone.

## Local data (`out/`, not in the repository)

`out/gnss-cap/`: `run3`, `run5`, `run6` (libmnl sessions with strace,
meas logs, memory dumps), `flip/` (bit-flip logs), `wflip/` (host-side
experiments), `own02/` (base capture and probe runs with our `02`), `fix/`
(the live fix run), `open/` (the daemon's gpsd output). `out/gnss-re/`:
the Ghidra project and decompile of libmnl.

## Next steps (roughly in order)

1. **An outdoor test** with a clear sky and a known position: the only
   way left to measure accuracy (indoor references disagree by 100-250 m).
   Sub-chip phase from the records' Doppler is ruled out (above); with
   more satellites outdoors the joint fix should tighten.
2. **Location switch choice**: done (`titan-location engine open|libmnl`,
   open by default).
3. **Generate the replayed frames**: done (session.py; no libmnl capture
   or library needed). Open: what `10`'s values and `01`'s two constants
   mean (fixed values work), and whether `07`/`0A`/`05` 0x000c help.
4. **Faster first fix**: fixes need three batches of ≥4 satellites at ≥20
   dB-Hz (≥5 before `solve_robust`); a height constraint may help further.
5. **Ephemeris from the chip**: done for all three systems (GPS `2B`,
   BeiDou `4B`, GLONASS `3B`; the daemon caches them, `chip.rnx` and
   `chip-gnss.json`). Live (10-30, 6 minutes): GPS G06, G11, G12, G13,
   G17, G19 and BeiDou C14, C23, C27, C28 from the chip; no GLONASS yet
   (an ephemeris must repeat in two 30 s frames; longer sessions). Still from the network: the first ephemeris on a
   phone with no cache, and BeiDou's geostationary satellites (`4D`, not
   visible here).
6. **Reliability**: restart sessions that never measure (the daemon does
   on the next cycle; it could check sooner).
7. **Rust**: the project prefers Rust for tools that ship on the phone;
   titan-gnss-open is Python (it grew out of the analysis scripts) and
   could move into the `titan-gps` crate once the method settles.
