# Open GNSS: state and handoff

Where the work to run the Titan Pocket's GPS without MediaTek's closed
`libmnl.so` stands, what is known, how to work on it, and what not to do.
Written for whoever (person or AI) picks it up next.
[gnss.md](gnss.md) is
the full chronological record, with the evidence behind each finding; this
page is the current summary and takes precedence where they differ (a few
of gnss.md's early sections, like "Verdict: replacing libmnl with open
code", are history now).

Last updated 2026-11-18 (the sections "Offline: the almanac" and "Time from the sky").

---

## The daemon in Rust (2026-11-17)

`titan-gnss-open` is now a Rust program, `userspace/titan-gnss-open/`
(crate of the same name, std only, static aarch64 musl, `build.sh` →
`out/titan-gnss-open`, runs the tests first). It is a port of the Python
daemon (`userspace/titan-gnss-open/titan-gnss-open` and `tools/gnss/`
`gpseph.py`, `snapfix.py`, `own02.py`, `navsub.py`, `session.py`,
`meas-match.py`'s `record()`), formula for formula, with the same files
in `/var/lib/titan-gnss-open`, the same status file, wakelock, watchdog,
backoff, clock switching, orbit tiers and server failover, NMEA to gpsd,
and the same `TITAN_GNSS_*` environment settings (the experiments that are
off by default included: `TRACK`, `STEER`, `AIM_LEARN`, `CLOCK_GAIN`,
`B_STEP`, `RECORD`). `chip-gnss.json` stays readable by both.

| Module | Python it follows |
|---|---|
| `eph.rs` | `gpseph.py`: RINEX 3, GPS/BeiDou/GLONASS orbits, Doppler |
| `navsub.rs` | `navsub.py`: 2B/3B/4B navigation data, Klobuchar, RINEX out |
| `meas.rs` | `meas-match.py` `record()`: the `29` fields |
| `snapfix.rs` | `snapfix.py`: the solves, `fix_from`, `identify` |
| `session.rs` | `session.py`: the generated script, `CLOCKS` |
| `own02.rs` | `own02.py` `regenerate()`, `visible()` |
| `state.rs` | the daemon's files, the download, `status()` |
| `main.rs` | the daemon: sessions, the feed, NMEA, the main loop |

How it differs, on purpose:

- One thread runs the session: the script is written to the probe from
  the same loop that reads the chip's lines (the Python daemon had feed,
  refresh and watchdog threads). The orbit download during a session runs
  on a thread of its own, so the feed never waits for a server. SIGTERM is
  taken on a thread with `sigwait`: the probe is killed, the receiver
  marked closed, the wakelock released and the status file removed, as the
  Python daemon's `finally` did.
- The download is `curl` (with `-L --fail`, 20 s per server) and the file
  is checked through `gzip -dc`, as `titan-cellloc` uses curl: no TLS crate.
- GPS ephemerides always carry their carrier and system. In the Python
  daemon those decoded from the chip or read back from `chip.rnx` had
  neither, so a chip issue newer than the downloaded one reached
  `look_any`'s `e["freq"]` and raised `KeyError` (a script failure, or a
  failed session).
- A GLONASS candidate whose orbit is too far from the time (`GLO_MAX_SPAN`)
  is skipped in `identify` instead of raising: the Python daemon logged
  "steering: ValueError" for those records (eight times in the replay
  below) and could have failed the session in its second `identify` call.
- A script piece that can't be made is logged ("the session's script
  failed"); the Python feed ended silently on a `ValueError`.

**Tests** (`cargo test`, 15): `tests/gen_vectors.py` runs the Python
modules on an excerpt of a public IGS broadcast file
(`tests/data/brdc-excerpt.rnx`) for a made-up receiver in the open
Atlantic and writes `tests/vectors.json`; the Rust code must give the same
satellite positions (GPS and BeiDou to 1 µm, GLONASS to 1 cm, integrated
from another state), Doppler (0.1 mHz), elevations, the session script
line for line, the regenerated `02`/`03` (GLONASS Doppler within 70 units of
2^-16 Hz), `29` record fields, GPS subframes encoded from an ephemeris and
decoded back, the Klobuchar page, the RINEX record, the NMEA sentences and
DOPs byte for byte, and the snapshot fixes of seven synthetic scenarios
(GPS only, with ionosphere, moving at 13 m/s, a four-satellite quick fix,
GPS+BeiDou and GPS+GLONASS batches of two, a rejected time error) within
1 mm of the Python fix. No capture or phone position is in the repository.

On recorded data, `tests/replay_py.py DIR` runs the Python daemon's own
`session()` over a probe log (`TITAN_GNSS_RECORD`; the probe and the clock
replaced by the log), and `TITAN_GNSS_REPLAY_DIR=DIR cargo test --release
replay_against_python` runs the Rust session's record handling
(`titan-gnss-open replay LOG BRDC POSITION OUT`) over the same log. On the
2026-11-15 00:55 UTC live session (150 s, 13 satellites locked at most):
39 fixes each, at the same log times and of the same kind, the largest
distance between the two 0.000 m. On the phone the same replay takes 0.53 s
(0.46 s CPU) in Rust against 11.1 s (11.0 s CPU) for the Python daemon's
code, start-up and the 8 MB orbit file included: ~6 ms against ~150 ms a
2 s fix cycle. A solve is a few milliseconds, the feed writes 1.5 s ahead and
the orbit download has its own thread, so the session stays on one thread:
splitting the solves would gain nothing, wake more cores, and change the
order of sums the tests compare to the millimetre. DIR stays under `out/`.

**On the phone** (2026-11-17): installed by `install.sh` as
`/usr/local/bin/titan-gnss-open`, and `titan-gnss-open.service` runs it.
The Python daemon stays in `/usr/local/lib/titan-gnss-open/` (a drop-in
with `ExecStart=/usr/bin/python3 /usr/local/lib/titan-gnss-open/titan-gnss-open`
runs it; `install.sh` installs it again with `PYTHON=1`), and the old unit is
`/root/titan-gnss-open.service.python`. The Rust daemon uses ~3 MB
resident. The first sessions, indoors with no
sky in view, tracked nothing in 75 s and ended, exactly as the Python
daemon's sessions did there minutes later (same log lines; this morning
the Python daemon had had five satellites at another spot). Later that
morning, once satellites were in view, the Rust daemon's first fix came
50 s into a session (full, five GPS satellites locked); the Python daemon,
started right after in the same place, fixed 15 s into its session (44
fixes in 5 minutes, spread about their median p50 122 m, p90 279 m; 17.1 s
of CPU). The Rust daemon used 6.1 s of CPU in an 18-minute run, mostly
searching. The side-by-side comparison was cut short (the phone reset,
an AP watchdog after a resume like the one at 09:01 with Location off)
and then dropped: the replay above is the like-for-like check.
`tools/gnss/fix-ab.py` (on the phone as `/root/fix-ab.py`) records gpsd's
fixes per run and compares runs by distance only.


## Offline: the almanac (2026-11-18)

Without the network the daemon had orbits for a while only: the downloaded
file up to 4 h old (with the chip's own ephemeris) for fixes, then the file
up to 48 h old only to aim the search, then nothing ("no-orbits", waiting
for good). A phone offline for two days, or one that never downloaded,
could not get a fix.

Every GPS satellite broadcasts the almanac of all of them (subframe 5 pages
1-24, subframe 4 pages 2-5 and 7-10), coarse Keplerian orbits good for
weeks, the whole of it every 12.5 minutes. Now:

- **Kept from the chip.** `navsub::almanac` decodes those pages
  (IS-GPS-200 20.3.3.5.1.2) into an `Eph` with no corrections, `iode` −1
  marking it, its reference time resolved to the week nearest the session's
  start. The daemon keeps the newest entry of each healthy satellite in
  `STATE/almanac.rnx` (RINEX, as `chip.rnx`); a satellite whose page says
  unhealthy is dropped.
- **A third tier.** `State::ephemeris`: the file up to 4 h old with the
  chip's ephemeris (fixes), then the file up to 48 h old with the almanac
  added (aim only), then the almanac alone, entries up to 28 days from their
  reference time (`Tier::Almanac`, `"orbit": "almanac"` in the status, "aimed
  with the almanac" in the log). As in the search tier, fixes use only what
  the chip decodes in the session (`solve_own`): the first fix needs four or
  five satellites' subframes 1-3, at least 18-30 s each after lock.
- **Aiming with old orbits never worked.** `own02::visible` (and the GSV sky
  view) skipped a satellite whose nearest orbit was over 4 h from its
  reference time, so the 4-48 h search tier made no assignments at all; the
  first offline run with the almanac showed the same ("0 satellites to
  search"). They now take the nearest orbit within its own limit
  (`own02::aim_age`: GLONASS 3 h, almanac 28 days, other ephemeris 4 h). With
  a fresh file nothing changes (the tests compare the same assignments).

Measured:

- **Accuracy** (`tests::gps_almanac`): almanac pages encoded from the public
  broadcast excerpt's 32 GPS ephemerides, quantized as sent, decode within
  1.3 km of the ephemeris over its 4 h fit. Real pages from the 2026-10-30
  window recordings: 0.7-1.6 km from that day's broadcast orbits, 42 h
  before their reference time. A kilometre moves a satellite's Doppler by
  well under 1 Hz; the search windows are 100 Hz and more. Three days after
  the ephemeris, the made-up receiver of the tests sees no satellite to aim
  at from the ephemeris and six or more from the almanac.
- **Collecting it** (phone, 2026-11-18 06:11 UTC, sky in view): 28 GPS
  satellites' almanac in 15 minutes of a normal session (G04, G06, G27 and
  G31 not among them: no healthy page of theirs came in that time).
- **A fix offline:** not measured yet. `tools/gnss/offline-test.sh` (on the
  phone as `/root/offline-test.sh`) runs the daemon on a state directory
  holding only `position`, `clock`, `iono` and `almanac.rnx`, with a `curl`
  that fails and no IP but localhost, and compares its fix with the last
  online one by distance only. Its first run made no assignments (the bug
  above); then it ran alongside the normal service (Location switched on),
  and with two daemons opening `/dev/stpgps` neither got an answer. The
  script now yields to the service and leaves Location as it found it.

**The satellite list (2026-11-18).** The status file now carries `"sats"`:
every satellite above the horizon (from the orbits that aim the search, so
the almanac's too) and every one heard in the last 10 s, each with its
elevation and azimuth (whole degrees), C/N0, whether it is in full lock,
in the current fix, and whether the solver has an ephemeris for it. Since
2026-11-18 also its health, the broadcast orbit's accuracy and any special
message:

- **Health:** the newest usable orbit's own flag. A satellite with none
  is taken from the orbit file's newest record (`eph::unhealthy`; `parse`
  drops unhealthy records) or an almanac page. Satellites that aren't heard
  and have no orbit stay unknown.
- **Accuracy:** in metres. Orbits decoded from the chip this session carry
  the URA index, converted with IS-GPS-200's table (`navsub::ura_m`, the same
  for BeiDou). The orbit file's records give metres. `chip.rnx` keeps no
  accuracy (the RINEX writer writes 0, as the Python one does), so a GPS
  orbit reloaded from it shows none. GLONASS broadcasts no accuracy.
- **Messages:** GPS subframe 4 page 17 (`navsub::special_message`, 22
  characters), by the satellite that sent it, and logged.

Live (2026-11-18, 32 satellites): 30 healthy, one GLONASS satellite heard
but unhealthy by the file, one GPS satellite unknown. Accuracies were
2-2.8 m (GPS, BeiDou), and no message, as usual. A
satellite counts as heard from any record at 10 dB-Hz or more, of any layout
(GPS by its PRN, without needing an orbit), so the list can show more heard
than the `heard` count, which is fully tracked records only. The Location
app shows it. The sky view behind GSV is now made from the same orbits and
refreshed before anything is tracked; with a fresh file it is the same as
before.

Not covered offline: BeiDou and GLONASS (no almanac of theirs is decoded,
so they are aimed only while a file or the chip's ephemeris is under 4 h
old); a phone with no `position` (it waits); a phone moved far from the
last fix (the seed must be within some tens of km, and fixes over 200 km
from it are rejected).

## Time from the sky (2026-11-18)

Two clocks the daemon already had, made into a time source:

- **Subframe times.** Every GPS subframe's HOW carries the time of week at
  its end (`navsub::how_tow`). Against the line's time on the session's
  clock (`t_start + ms`), 47 subframes of the 2026-11-15 recording agreed
  within 3 ms. They reach the daemon a steady `L` after they end.
- **The fix's time error.** A full fix solves one time error for the
  session's clock. On an NTP-synced phone it reads +0.10 to +0.18 s (20
  first fixes logged over 10 days): a bias `B`, the session's start against
  the chip's time base, with ~0.03 s of noise. On the 2026-11-15 recording
  the fix said +2.04 s and the subframes +1.80 s on the same clock, so
  `L` ≈ 0.24 s.

The phone clock's error (true time less the phone's) is then `dt − B` from
a full fix, or `median(subframe) − B + L` from three subframes within 50 ms
of each other: one satellite strong enough to decode for about 20 s, no fix
needed. `B` and `L` are learnt while the clock is synced (each synced full
fix steps them a tenth of the way, saved in `STATE/time-cal` every 5
minutes; defaults 0.145 and 0.24 s). Measured live, synced (2026-11-18,
three readings 20 s apart): the fix put the phone 0.022, 0.028 and 0.008 s
behind network time, the subframes 0.058, 0.021 and 0.007 s ahead, closing
in as the calibration learnt.

While the kernel calls the clock unsynchronised (no network time):

- **More than 0.5 s off:** the daemon sets the system clock
  (`clock_settime`) once a session, unless the step would be over 3 days
  (the week comes from the phone's clock) or `TITAN_GNSS_SET_CLOCK=0`.
  Then it ends the session and starts again on the new time. It does not
  write the RTC.
- **Within 0.5 s:** it trusts the clock for four-satellite (quick) fixes,
  holding the time error at the estimate. Before, these needed network
  time.

The kernel's flag stays unsynced either way; timesyncd takes over when the
network returns. Subframe 4 page 18's GPS−UTC (`navsub::leap`) is decoded
and logged if it isn't the 18 s the build assumes. The status file's
`"time"` has the estimate (`ahead`: the phone's clock less true time; its
`source`; `subframes_ahead`; `synced`; `set`; `leap`), and the Location app
shows it as "Phone clock".

Not yet run: an unsynced clock on the air (the step and the quick fixes it
allows). A phone whose clock is before 2026 still waits ("no-clock").

## Broken since the modem runs (2026-11-09; fixed 2026-11-10)

**Summary.** The receiver must be told its clock's offset in the session's
`10` frame, and the daemon sent the value for the crystal trim the phone
powers on with (0x88: B = +27390 Hz). The modem sets its own trim (0xD4)
when it boots, which stays until a power-off; with it the right value is
near −200 Hz, and with the old one the chip tracks nothing. Measured with
the modem running, 120 s sessions at one window: old clock, 0 tracked
records; B = −90 Hz (what libmnl sends there, given the modem's
calibration), 752 records from 9 satellites; B = −300 Hz, 706 from 9.

The fix (`session.py` `CLOCKS`, `own02.offsets`, the daemon):

- Two clocks, `modem` (A 27594, B −200 Hz) and `preloader` (the old frame,
  byte for byte). The daemon starts with the one that last tracked
  (`STATE/clock`, the modem's by default); a session with no satellite
  tracked after 75 s ends and the next uses the other, with growing pauses
  while neither tracks. A record counts as tracked from 10 dB-Hz: the wrong
  clock gives a few records at −3.
- **Whole hertz only** in the `10` frame: with 218.5 Hz in the BeiDou
  group the chip tracked nothing at all.
- BeiDou's and GLONASS's `02` offsets (−494, +483 + 13.4 k Hz) were the
  old clock's 24.4 ppm on their carriers' distance from L1 (predicted −349
  and +648 against GPS's −150; measured −344 and +633): with the modem's
  clock all three take −150. Not yet confirmed on the air: no BeiDou or
  GLONASS satellite has been tracked with the new clock (BeiDou scanned
  over ±3 kHz in 200 Hz steps: none; GLONASS had no assignments then, next
  item).
- **Orbit file after midnight UTC**: for three hours the daemon used
  yesterday's file alone, which ends at midnight, so GLONASS (orbits good
  for an hour) had no assignments from 01:00 to 03:00 UTC every day. It now
  appends what today's file has so far (assignments 12350 → 17110 at
  02:45 UTC).

State on the phone (kernel #124, modem registered): the daemon acquires
5-10 GPS satellites within 12 s of a session's start; at the test window
(night; Android's YGPS averaged 23 dB-Hz there) all but 1-3 are lost again
within half a minute, so no fix there (five are needed). A fix with the
modem running came later that night (below). The `preloader`
clock is untested since the change (it needs a power-off with the modem
kept off). Why weak satellites are lost after acquisition is open:
libmnl switches a tracked satellite's `02` to track mode with its own
Doppler three times a second, and our replayed script never does.

**One session, fed as it goes, and a rough fix (2026-11-10, late).**

- `titan-gps-probe -` takes the script on standard input, and the daemon
  writes it 20 s at a time, 12 s ahead (`session.cycles`,
  `own02.regenerate` per piece). A session no longer ends every 600 s (it
  runs up to 6 h, with new orbits merged in as the file is renewed), so
  the fix is not dropped every ten minutes and newly risen satellites get
  assignments within 20 s. Measured on the phone: the script starts 0.8 s
  after the service (about 4 s before), the first satellite is tracked
  within 10 s, and one session ran 3 minutes across nine pieces. Not yet
  run: an hour or more, and the orbit renewal inside a session.
- A session that tracked satellites and then tracks none for 300 s ends,
  and the next tries the other clock (the modem may have started
  meanwhile). After a start the chip doesn't answer, the next try comes
  10 s later (it was 30 s).
- `snapfix.fix_from` is the fix rule, shared with `tools/gnss/replay.py`
  (the rule over a recorded log, against a position kept in a local file;
  it prints times and distances only). The rule was already quick with
  enough sky: on the 2026-10-30 window recordings the first strong
  satellite at 2-3 s, five at 4-5 s, the first fix at 7 s (131 m off, 11 m
  median from 20 s on), 13 s and 29 s. New: with four satellites, a rough
  fix with the height and the time error held (at the last full fix's
  value; only while the kernel calls the clock synchronised), reported
  with three times the DOP. Replayed with the sky cut to four satellites,
  where the old rule gives nothing: first fix at 7 s, 46 m median (p90
  81 m) on one recording, 13 s and 104 m (188 m) on another, none on the
  third. Not yet seen live.
- **Why there was still no fix at the test window that night**: every
  batch held one satellite. One channel was fully locked (status bytes 6-7
  `0261`, reporting every 500 ms with batch bytes `0010...`); the others
  stayed half acquired (`4011`, `c011`, `0111`, with bytes 12-17 zero) at
  15-17 dB-Hz and came and went. In October every tracked channel read
  `0261` and shared one batch value three times a second, at 20-39 dB-Hz.
  So at ~25 dB-Hz and below the chip does not bring satellites to the
  state our fix needs; whether libmnl's track-mode `02` frames would, is
  the open question. Bytes 6-7 look like the lock state: worth decoding.

**Track-form `02` frames (2026-11-10/11, no clear effect).** libmnl,
once the chip tracks a satellite, sends its `02` in a second form: bytes
12-19 `[~30][00][u16 window 4 Hz, GLONASS 3][01 03][tail]`, the tail's
bit 1 set while the chip's record is not in full lock (status bytes 6-7
not `0261`); its Doppler is libmnl's own estimate, which differs from the
record's by a steady per-satellite amount (-130 to +210 Hz in run6), so
it was not copied. The daemon now writes the session 1 s at a time, 1.5 s
ahead, and gives a satellite with a tracked record in the last 3 s that
form, with the predicted Doppler (`own02.TRACK_02`, `regenerate(...,
tracked=)`; `TITAN_GNSS_TRACK=0` turns it off). Alternating 3-minute runs
at the window with the modem running (one session each):

| Run (22:32-22:44) | Track form | Fixes | Mean satellites heard / strong | First fix |
|---|---|---|---|---|
| 1 | off | 75 | 7.0 / 6.3 | 17 s (rough) |
| 2 | on | 72 | 6.8 / 6.1 | 11 s (rough) |
| 3 | off | 0 | 3.2 / 2.4 | - |
| 4 | on | 0 | 5.5 / 1.6 | - |

The sky changed more between runs than the form did; no gain shown, none
lost. Kept on. An earlier pair (22:19-22:31) is void: the receiver was
reopened within ~1 s of the last close and two sessions in a row never
measured; the daemon now leaves it closed REST = 10 s
(`/run/titan-gnss-open.closed`, across daemon restarts too) and retries a
session that tracks nothing once before switching clocks.

**First fixes with the modem running (2026-11-10, 22:12 onward)**, at the
window: a full fix 32 s into a session (5 satellites, residual 71 m rms),
gpsd 3D with `eph` ~57 m and HDOP 3; rough four-satellite fixes from 6 s,
11 s and 17 s after a session started. So the clock fix holds and the
modem can stay on.

**A moving phone (2026-11-12; installed on the phone that morning, not yet run there).** The fix
averaged 60 s of batches as if the phone stood still: on a train it lagged
half a window behind and reported 0 km/h. `snapfix.solve_joint_tv` solves
the window for a position at the newest batch and a steady velocity (one
time error, a bias per batch eliminated as before, the height prior, and a
prior holding the velocity near zero, 50 m/s horizontal and 2 m/s
vertical); `fix_from(motion=True)` takes it over the static solve when its
horizontal speed passes 4 sigmas and 2 m/s, and the daemon then sends speed
and course in RMC. `tools/gnss/replay.py --move VE,VN` replays a recording
as if the phone moved at that velocity (each record's code phase shifted by
the geometric range change from the recorded place); `--motion` turns the
velocity solve on. On the three window recordings (2026-10-30), median
horizontal error:

| Recording | still: old / new | 8 m/s north: old / new | 28 m/s: old / new |
|---|---|---|---|
| regen | 11 / 11 m | 177 / 33 m | 745 / 46 m |
| win | 41 / 41 m | 220 / 76 m | 802 / 77 m |
| gnss | 79 / 79 m | 208 / 156 m | 809 / 212 m |

Speed came out within 0.5 m/s of the truth at 8 and 28 m/s; standing
still, no fix was taken as moving (at 3 sigmas and 1 m/s, 6 of 52 were,
and the median grew to 14 m). Walking pace (1.4 m/s) stays below the
threshold and is reported as still, with the old lag. The velocity solve
costs ~15 ms a fix on the Mac (the robust static solve ~63 ms). Not
covered: acceleration within the window (stations, bends), and real
motion: the shift is synthetic.

titan-mnld's default init still gives libmnl no crystal calibration, so
the libmnl engine starts 24.7 ppm off unless `TITAN_MNLD_CALIB` is set.

**Nothing tracked since 2026-11-12 (open).** On kernel #128, from about
06:19 on 2026-11-12, the chip tracks no satellite (no `29` record with
layout `A0`) at a place the user says has sky; at 06:06 the same boot had
heard 7. Ruled out so far: the clock frame (B swept from −6200 to +5800 Hz,
nothing), Wi-Fi on or off, `RG_SRCLKEN_IN3_EN`, and the phone's clock
(0.13 s from NTP on 2026-11-13). The crystal measured +2.94 ppm against
the Mac's clock on 2026-11-12 (−0.42 ppm on 2026-11-09).

A traced libmnl session on 2026-11-13 (cold start, the modem's
calibration passed in, 5 minutes, `tools/gnss/gps-capture.sh`) heard
nothing either: 1590 `29` records, none tracked, gpsd mode 1 in all 296
reports. With the calibration libmnl does not sweep: it held B at +702
Hz, sliding to +564 Hz with its crystal model ($PMTKTSX1 +0.71 ppm, then
about −0.04 ppm after 135 s), and sent no `02` assignments (no aiding
data). The AGC values moved as before (~3500-3900 and ~6450-6700). This
is weak evidence: the calibrated libmnl also tracked nothing on
2026-11-10, when our own session tracked 9 satellites.

Also ruled out (2026-11-13): suspend (the daemon held the phone awake
from 05:50 to 07:54 on 2026-11-12, across the change), USB and charging
(plugged in at the working and the failing times alike), and our own
install at 06:18 that morning (the clock loop: with its gain at 0 the
generated session is byte-identical to the one that worked at 06:06,
checked over 40 cycles). Nothing else on the phone's disk changed between
05:55 and 06:40. The chip's search is not swamped either: the untracked
detections in the failing libmnl run sit at the same levels as in a
working libmnl capture of 2026-10-29 (BeiDou median 26 against 25 dB-Hz,
GPS 15 against 13, GLONASS 18 against 17), and they land on satellites
below the horizon at scattered Dopplers, so they are the search's noise
floor, not signals. What is left: the sky at that spot, or something
outside the disk that changes over time, such as the modem's LTE band and
its antenna tuner settings. Under Android the modem is told when GNSS
runs; under Linux it is not. The airplane test of 2026-11-10 ran with the
wrong clock frame, so it says nothing about this.

**Not the modem (2026-11-13, 20:53-21:00, same spot).** With today's
orbits (the phone's file was from 2026-11-12 07:59: the daemon only
refreshes it while it runs, and `clock-sweep.py` reads the same file), a
session with the modem registered tracked one satellite in full lock from
23 s, G03 at ~16 dB-Hz, held it for 2 minutes and decoded its ephemeris
from the sky; no fix. A fresh session with `titan-net airplane on` (modem
`disabled`) tracked one satellite at 24 s and no more. The GPS LNA pin
(GPIO155) goes from low to high when GPS opens and back when it closes, as
on 2026-11-09. So the receiver works and hears the sky at this spot, but
weakly: the strongest satellite is ~16 dB-Hz, where the chip does not
bring others to full lock.

**Android at the same spot is ~13 dB stronger (2026-11-13, 21:01-21:12).**
Stock boot flashed for the test (then #128 back, checked by hash): a GPS
fix 62 s after YGPS started, 24 satellites, the strongest four averaging
29.3 dB-Hz (strongest 32, mean 21; YGPS averages GPS 22.8, GLONASS 24.0,
BeiDou 21.4). Linux ten minutes before: one satellite at ~16 dB-Hz. So the
sky is there and the loss is in how Linux runs the receiver. Same under
both: the LNA pin (GPIO155 `0111000100`, driven high), the GPS supplies
(VCN28 2.8 V, VCN18, VCN33 3.5 V on, VFE28 off), and every DCXO register
but the live calibration words, the debug read-out mux and `DCXO_CW23`.

Ruled out since, by A/B inside one session (same satellite, 90 s each):
- `DCXO_CW23` bit 0, `XO_BB_LPM_CEL` (the crystal's hardware low-power
  mode): Linux has 0x0003, Android 0x0002 (the vendor clock-buffer driver
  clears the bit at boot; nothing in our 6.12 tree touches it). G03 read
  14-20 dB-Hz at 0x0003 and 18-20 at 0x0002. Left at 0x0003.
- The compositor (the display and GPU on MMPLL, whose half
  frequency is 0.42 MHz from L1): G17 read 21-23 dB-Hz running and 21-23
  stopped.

A session's first minute shows up to 9 GPS satellites at 10-18 dB-Hz that
are acquired but never reach full lock, beside one at ~25.

Also ruled out: the kernel. The 2026-11-02 image (`boot-6.12-deep-ncs.img`,
#86, built before the last good fix, with the same connectivity modules:
`mtk_btif`, `wmt_drv` and `gps_drv` are byte-identical to that day's
builds) gave the same picture at the same spot: G17 20-25 and G3 14-21
dB-Hz, up to 9 more at 11-18 dB-Hz in the first minute, gone by 70 s.

**Android's libmnl, traced (2026-11-13, 21:52-21:58).** strace on stock
`mnld` (Debian's strace with its glibc, run through the loader on
Android; SELinux left enforcing) during a YGPS session that held YGPS
averages of 23-25 (GPS), 18-20 (GLONASS) and 21-23 (BeiDou) dB-Hz;
capture in `out/gnss-cap/android-1113/` (local). What it shows:
- **The clock frame moves.** B (first group) went from −444 Hz at 0.5 s
  to +52 Hz at 58 s, +200 Hz at 133 s and +294 Hz at 6 minutes: +738 Hz,
  in whole-Hz steps each second (largest 30 Hz). A moves with it (about
  1.36 per Hz of B), and the groups are 800 (A) and ~590 Hz (B) apart, not
  418 Hz as in our modem profile. Our daemon holds B at −200 Hz. A step
  test on our session (B +200 Hz at 70 s) lost every tracked satellite at
  once: B matters to within much less than 200 Hz. But copying libmnl's
  curve does not help (below): its B is relative to its own aiding.
- The daemon's own drift measure does not see this: with
  `TITAN_GNSS_CLOCK_GAIN=1` it read +20 to +40 Hz from one satellite and
  moved B only to about −160 Hz; the weak satellites still dropped at
  ~75 s.
- **Replays on our session** (a generated script straight to the probe,
  4 minutes each, tracked = `29` records with layout `A0`): our fixed
  clock tracked 13 GPS satellites from 20 to 80 s, then 12 of them were
  lost together and only G19 (25-29 dB-Hz) stayed; Android's frames as
  they are tracked nothing; our frames plus Android's change since its
  start tracked 2-6 and nothing after 60 s. So the receiver's clock under
  our session does not move as libmnl's frames do (no script change
  happens near 80 s either: our `01` millisecond field wraps at 65.5 s,
  but Android's does too).
- **Correction: layout `A0` is not full lock.** In a 150 s run of the same
  kind, only G17 and G19 (20-27 dB-Hz) reached the lock states (status
  bytes 6-7 `0251`, then `0261`) and held them. G3, G4, G6, G12 and G25
  (10-20 dB-Hz) cycled through detection states (`0100`, `0111`, `x011`,
  `x019`, under layouts `80`, `58`, `20` and also `A0`) for 40-80 s and
  never locked; then the chip stopped reporting them. The "13 tracked,
  then lost together" above counts those detection records.
- **What libmnl does with a detection.** G17 in the Android trace: the
  `02` search centre is the ephemeris prediction (−1619.5 Hz) with a
  512 Hz window until 2.0 s; at 2.5 s it moves to the chip's measured
  Doppler (−1703.1 Hz, 84 Hz away) with a 101 Hz window, then follows the
  measurement with windows of 96, 12, 7 and 4 Hz (7.5 s) and switches to
  the track form at 12.6 s. Our daemon keeps every satellite on the
  ephemeris prediction with a ±512 Hz window until the chip locks it on
  its own, which happens only above ~20 dB-Hz. Answering detections this
  way is the likely reason Android holds satellites at 15-20 dB-Hz.
- In the trace libmnl's first move (2.5 s) shifts **every** GPS search
  centre by the same −83.6 Hz, undetected satellites included, and moves B
  by about as much: a receiver clock correction from the first detection
  (G19 at 2.0 s). It narrows every window at once (46-139 Hz by
  satellite), then to 12, 7, 4 Hz; G3, G17, G12 and G6 lock 1-6 s later at
  20-28 dB-Hz. The record's Doppler and the `02` centre differ per
  satellite (−188 to +169 Hz), so the record cannot simply be copied into
  the search centre.
- Fixed narrow windows on our session (2026-11-13, 22:50, 120 s each,
  counting `0251`/`0261` only): centre geometric −150 Hz with 512 Hz
  windows (ours) locked G19 and G4; geometric −234 Hz (libmnl's after its
  correction) with 46 Hz locked up to three, G6, G19 and G12 at 18 dB-Hz;
  the 101 Hz run never started (no detections at all). Repeated with more
  points (22:58-23:08): −234 Hz / 101 Hz locked up to five (G4, G6, G17,
  G19 and G9 at 14-16 dB-Hz); −200 Hz / 46 Hz one; −270 Hz / 46 Hz two;
  −234 Hz staged down to 12 Hz (512, 101 at 2.5 s, 46 at 5 s, 12 at 8 s)
  one. Narrow windows need a centre known better than ours.
- **libmnl's centre moves through the session.** Its GPS `02` centre less
  the geometric Doppler (today's orbits): −86 Hz at the start, −36 at
  20 s, +39 at 60 s, +68 at 120 s, +81 to +89 from 200 s; about 0.21 Hz
  per hertz of its B. The locked satellites' record Doppler less geometry
  rose only ~+110 Hz over the same 6 minutes, with ~280 Hz jumps on single
  satellites, so the records give no simple measure of that centre.
- `own02.regenerate` takes `gps_offset` and `gps_window`, and the daemon
  `TITAN_GNSS_GPS_OFFSET`, `TITAN_GNSS_GPS_WINDOW` and
  `TITAN_GNSS_AIM_GAIN` (the centre moved by the measured drift times the
  gain); defaults unchanged (−150 Hz, the template's 512 Hz), checked
  byte-identical. A live daemon session with −234 Hz / 101 Hz (22:51)
  heard nine GPS satellites in the first 20 s and held only G25 (14-19
  dB-Hz) after that: no fix in 5.5 minutes. Runs minutes apart differ by
  a few satellites as the sky moves, so the comparison needs runs
  interleaved side by side; the phone runs the previous files.

**Fixes again with the modem on (2026-11-14, 00:07).** What it took, in
order (details in gnss-chip.md §6-8):
- **The GPS search term and window.** Interleaved scripted runs (70-120 s
  each, GPS satellites in full lock): −150 Hz / 512 Hz (ours until now)
  0-2; −234 Hz / 101 Hz 1-5; −300 Hz 3-5; **−370 Hz 4-6** (up to six
  locked in one burst, 119-171 bursts with four or more in two
  minutes); −450 Hz 2. Raising B by 100 Hz did about what moving the term
  by −100 Hz did.
- **The acquisition controller** in the daemon (v1): GPS satellites not
  yet locked are searched at geometric + a learned term with a 101 Hz
  window; the term is dithered ±80 Hz while nothing new locks; each new
  lock credits the term in use 3 s before, and the median becomes the
  term (`/var/lib/titan-gnss-open/aim-<profile>`, saved as it is learned).
  `TITAN_GNSS_AIM=0` gives the old behaviour.
- **The track form only on full lock.** Layout `A0` is not lock; the
  daemon had given detected satellites the 4 Hz track form. With
  `TITAN_GNSS_TRACK=0` (search form only) a live session locked seven
  GPS satellites and held four or five at once; with the track form on
  detections, one.
- **Fixes from locked records only, batched on bytes 20-24.** Detection
  records' code phases put solutions tens of kilometres off, and byte 25
  holds per-channel flag bits that split every satellite into a batch of
  its own. Grouped on bytes 20-24, solves came out at 83-94 m rms.
- **A synced clock.** The phone's clock was 167 ms ahead with no time
  sync running; the quick fix holds the time error at 0 ± 0.05 s. With
  `systemd-timesyncd` on (2026-11-14, over cellular data) the next
  session fixed: first fix 69 s in (quick: G6, G9, G11, G21), more than 60
  fixes over two minutes at 72-112 m residual rms, until G21 faded to 13
  dB-Hz. The term learned itself to −330 Hz.

Still to do: hold satellites past ~2 minutes (in every live run the
weaker ones fade then: the term should follow the crystal as it warms),
GLONASS and BeiDou with the same aim, and full (five-satellite) fixes.
The track form is now off by default (`TITAN_GNSS_TRACK=1` turns it
on): the live runs that held satellites and fixed ran without it.

**After the first fixes (2026-11-14, 00:20-03:05).**
- **The fade after ~2 minutes was our dither.** Scripted 3-minute runs
  with the term ramped at 0, −40, +40, +80 Hz a minute: 0 and −40 held
  four satellites together through 90-180 s (84 and 118 bursts with four
  or more, means of two rounds), +40 and +80 none. In the daemon, once
  nothing new locked the term swung ±80 Hz every 4 s, and with the search
  form on locked satellites too, each swing put their signal at the edge
  of the 101 Hz window. Locked satellites now keep the undithered term
  (`own02.regenerate(gps_hold=)`).
- **Learning by dither and credits is off by default**
  (`TITAN_GNSS_AIM_LEARN=1`): live, two locks credited the +80 Hz step and
  moved the term to −250 Hz, outside the region the side-by-side runs
  found best. The term is fixed at −350 Hz. The warm-up ramp
  (`TITAN_GNSS_AIM_RAMP`) and a drift gain (`TITAN_GNSS_AIM_GAIN`) exist,
  at 0.
- **Sky late at night:** live sessions at 00:50-01:05 locked four GPS
  satellites over the session but never four at once (one strong, the
  rest 14-17 dB-Hz); no fix. The fix needs four GPS satellites together
  because each constellation's batch had its own clock bias and only
  batches of four were used; the daemon now combines constellations
  ("Mixing constellations" below).
- **GLONASS was not searched at all:** our generator used a GLONASS orbit
  for aiming only within an hour, and the broadcast file's newest is ~82
  minutes old; now three hours (`own02.GLO_AGE`, deployed). Searched, it
  locked 0-3 satellites a run; BeiDou none, with every form, centre and
  time tried (gnss-chip.md §4).
- **The clock frame is a curve:** gnss-chip.md §7.
- **libmnl does not centre a satellite's search on the chip's Doppler**
  (checked 2026-11-14 on the Android trace): its centres stay at geometry
  plus the common term, within 2-7 Hz across 26 satellites, while the
  record Doppler less the centre differs by satellite (G4 −162, G6 −31,
  G9 −64, G17 +61 Hz, median over locked records, ±27 Hz record to
  record) and wanders over minutes. Centring on the record Doppler would
  put most windows ~100 Hz from where libmnl keeps them.
- libmnl narrows each satellite's `02` window once it has it (G17: 512 Hz,
  then 101, 96, 12, 7, 4 Hz by 7.5 s) and switches it to the track form
  (`0x0301`) at ~12 s. Its `01` frame carries two fields that change
  through the session where ours are fixed: the first u32 (0xffff, then
  −83 … +87 as a signed value; ours 0x60) and the second (0x118e falling
  to ~0x0f72, with flag bits 0x40000/0x50000 later; ours 0x111f).
- Frames we leave out: `07` (89, from 6.5 s; SBAS search rotation) and
  `0A` (9, from 7.6 s). `02` frames: 11,567 in 5.8 minutes.
- The `08` header differs from ours in bytes 30 (150 against 200), 34-37,
  46, 50, 54 and 56; ours was captured under titan-mnld.
- `mnld` reads `/mnt/vendor/nvdata/APCFG/APRDEB/GPS` (the device path,
  26 MHz, crystal type 0xff, zero LNA mode; created 2026-11-03), the
  modem's `ML4A_000` and its EPO files; titan-mnld and our daemon read
  none of the first.

The rest of this section is the investigation, in the order it happened.
An early reading in it ("Cause: the crystal settings...") is superseded by
the above: the settings are fine, our clock value was not.


**Cause: the crystal settings the modem leaves in the MT6358.** After a
power-off, with the modem kept off (`titan-net voice off`, `cell off`) and
the same kernel and parameters, the DCXO came up as on 2026-11-01 and
under 4.14 (`XO_CDAC_FPM` 0x88, `XO_COFST_FPM` 3, `XO_AAC_FPM_SWEN` 0,
`RG_SRCLKEN_IN3_EN` 1; only `DCXO_CW23` read 0 against 2), and the open
daemon heard satellites again at the same window: 10 (3 strong) 30 s into
the first session and 9 in the second, with two satellites tracked for
minutes and ephemerides decoded from the sky (G32, G28). Too few strong
ones there for a fix (five are needed). In the warm-rebooted boot before
it, modem never started but its DCXO settings still in place (`CDAC`
0xD4, `COFST` 0, `AAC` on), five minutes heard nothing, as in every
session since 2026-11-03. The MT6358 keeps the DCXO registers through a
warm reboot; only a power-off resets them.

**First fix since 2026-11-02 (2026-11-10, 13:51 UTC):** with the modem
still off after that power-off, on a moving train by a window, the open
daemon reported a 3D fix: 5 satellites used, 6 heard, 28 overhead, gpsd's
accuracy estimate ±74 m (seen in the Location app; not compared with a
reference). Four minutes later, `cgps` on the phone: 3D fix, HDOP 1.10, PDOP
2.80, gpsd's horizontal error estimate (`eph`) 20.9 m, GPS and GLONASS
satellites in the sky view. Its speed read 0.0 km/h: the snapshot fix gives no velocity,
and a moving phone is outside what its 60 s window assumes (see "When it
fails").

**Android, measured (2026-11-10, stock boot flashed for the test).**
With the modem running, Android's clock-buffer driver reports capid 0xD4
and the DCXO reads the modem's values: `CW05` 0x1d2c-0x1d34 (`COFST` 0),
`CW09` 0x408f (`AAC` on), `CW23` 0x0002, `TOP_SPI_CON1` 1 (CONNSYS's
clock request goes through the wrapper's DCXO channel, `DCXO_CONN_ADR`
0x44e); `XO_WCN` is under software control and on. YGPS at the same
window tracked satellites (average C/N0 GPS 23.1, GLONASS 28.5, BeiDou
21.4 dB-Hz). A watch of the DCXO words through `pmic_access`
(`out/modem/android-probe/dcxowatch`, every 0.4 ms for 30 s with the GPS
on) saw no change in any of them except `CW11` bit 13 (`XO_EXTBUF7_EN_M`),
which toggles a few times a second with the GPS off too. So the crystal
does not flip under Android, and the modem's settings alone do not deafen
the receiver: Android's GPS works with them. What differs is the engine's
side: Android's mnld passes the modem's crystal calibration and runs with
its own learned co-clock state, where ours start from the 0x88 era's clock
(the `10` clock frame replayed by `session.py`, libmnl's state learned
under 0x88). The `CW23` difference is not it: it read 0 in the cold boot
where the GPS worked. libmnl's `$PMTKTSX1` under Android is in its log
only truncated; the clock frame mnld sends there is not captured (no
strace on that system).

**Something keeps rewriting the DCXO.** `XO_32KDIV_NFRAC_FPM` (`CW05`
bits 0-13) read 0x1d40 at power-on and on 2026-11-01, then 0x1d44
(11-05), 0x1d4a (11-09) and 0x1d4c (11-10, warm boot, modem not
started); `XO_32KDIV_NFRAC_LPM` (`CW06`) moved too. Snapshots can't show
whether the trim or the oscillator mode also flip while the GPS runs;
`modules/debug/pmicwatch` (read-only: samples the DCXO words and
`TOP_SPI_CON1` every millisecond for a set time and logs each change)
is built for that, not yet run.

**One field at a time (2026-11-10, inconclusive).** With the modem off
and the crystal at its power-on values, `modules/debug/pmicpoke` (writes
one MT6358 register through the PMIC wrapper's regmap) applied each of the
modem's settings alone for a 3.5-minute session, restoring after each
(`/tmp/xotest.sh` on the phone): control (nothing changed) 0 satellites
heard; `COFST` 0: 0; `AAC` on: 8 heard, 1 tracked; `CDAC` 0xD4: 0; all
three: 0. The control hearing nothing, where two sessions 15 minutes
earlier with the same settings heard 9-10, means the window's sky is too
marginal for single sessions to separate the fields: repeat with controls
interleaved, or outdoors.

What follows is the earlier investigation.


**GPS has not acquired a satellite since the modem started running under
Linux.** The last fix (`STATE/position`) is from 2026-11-02 23:06, on a 6.12
boot, so 6.12 itself worked. Checked on kernel #122 (2026-11-09), the phone
by a window where earlier sessions tracked 10-19 satellites:

- **Everything up to the chip works.** `titan-gps-driver` loads `gps_drv`,
  `/dev/stpgps` appears, the chip answers the session header, takes all 49
  patch chunks, and sends its usual status frames. The LNA pin (GPIO 155)
  reads high while the device is open and low after (pinctrl debugfs). The
  `conn` power domain is on. The phone didn't suspend during the tests.
  `gps_drv.ko`, `wmt_drv.ko`, the probe and the patch are unchanged since
  before the last fix.
- **The chip searches but never tracks.** In 60 s the probe got 1052 `29`
  records and none with layout byte `A0` (tracked); in the working logs
  (`own02/own.log`, `window/*.log`) `A0` is most of them. So the daemon sees
  no satellites (`heard: 0`; `above: 0` too, as the sky view is only worked
  out after a first record).
- **libmnl fails the same way.** A 4-minute libmnl session (warm, its
  stored state) and a 4-minute cold start (its `mtkgps.dat` and `cache.dat`
  moved to `/root/mnl-state-backup/` on the phone) gave no `A0` record and
  no fix (gpsd mode 1 throughout). So it isn't our daemon.
- **The crystal's trim moved.** The MT6358 DCXO registers (regmap debugfs)
  differ from both the 4.14 dump and the 6.12 dump of 2026-11-01, which
  agree with each other: `XO_CDAC_FPM` 0x88 → 0xD4, `XO_COFST_FPM` 3 → 0,
  `XO_AAC_FPM_SWEN` 0 → 1. It was already 0xD4 in the dump of 2026-11-05.
  Android runs at 0xD4 too ([modem.md](modem.md), "Radio on with a valid
  SIM"), so it is the modem's calibrated cap ID, written when MD1 boots;
  before the modem ran under Linux (2026-11-03/04) the crystal stayed at the
  preloader's 0x88. Today's crystal: −0.42 ppm (the phone's
  `CLOCK_MONOTONIC_RAW`, which counts the same 26 MHz, against the Mac's NTP
  clock over 292 s, 0.06 ms residual). In the 0x88 era libmnl measured a
  receiver drift of ~38.4 kHz (24.4 ppm) on L1, and the per-channel GLONASS
  term showed the same 24 ppm. If that was crystal error, the receiver's clock moved by ~24 ppm, far
  outside libmnl's ±2 ppm uncertainty (titan-mnld's init: 2000 ppb); the
  cold-start result below argues against it being the cause.
- **`10` is the clock frame.** Read as three groups `[u32 A][i32 B, 2^-16
  Hz]`: on a cold start libmnl sends no `02` at all and steps B across
  ±30 kHz in ~8.6 kHz bins of ~15 s (−2.1, +16.8, −21.1, −29.7, +25.4,
  −10.8, +6.4 kHz, ...), the three groups 413 Hz apart; its first
  byte is 0 then (1 in warm sessions). `session.py` replays B = +27.4 kHz
  (`CLOCK_10`, the 0x88 era).
- **Not fixed by B.** Our aided sessions with B at −11 kHz (the old
  value less 38.4 kHz; also with the `02` window field at 4000), at
  libmnl's coarse bins, and at −4.1 to −0.6 kHz in 500 Hz steps (A at
  libmnl's cold 28696): no `A0` record in 60-90 s each. Either B is not
  simply the search centre for aided searches, A or another field has to
  agree with it, or something besides the clock changed when the modem
  started (it now transmits on LTE: a shared antenna or front end, or a
  co-existence signal, could desense the GPS).

**libmnl's cold start doesn't find the sky either (2026-11-10).** 12
minutes under strace by the same window, with libmnl's state from a cold
start (`mtkgps.dat` holding no learned drift): it sent no `02`, swept B
over −28 to +28 kHz about five times (+28.1, +0.2, −27.5 kHz, ... in
~70 s cycles), and got no `A0` record and no fix (gpsd mode 1 in all 726
reports). A search over every clock offset finding nothing means the
clock is not the (only) cause: the signal doesn't reach the correlators.
That also reads the 0x88-era numbers differently: B ≈ +27 kHz looks like
the chip's nominal centre (cold libmnl starts at +28.1 kHz), not 24 ppm of
crystal error.

Where libmnl's frames differ from `session.py` and from its own 0x88-era
run6 (the fix came at ~75 s there):

| Frame | run6 (warm, fixed) | now (cold) | `session.py` |
|---|---|---|---|
| `10` byte 0 | 1 | 0 | 1 |
| `10` A, B | 26636, +27.0 kHz, falling slowly | 33350, swept ±28 kHz | 27208, +27.4 kHz |
| `01` u32 #1 | 97, then 65388 (i16 −148: the clock residual in Hz, as libmnl's `02` sit −145…152 Hz off) at the fix | 95, 221 | 0x60 |
| `01` u32 #2 | ~3980, 69508 after the fix | 10000 | 0x111F |
| `01` u16 at 10 | 0x0420 | 0x0200 | 0x0425 |
| `05` | adds 0x0048=1, 0x000f=500, 0x0019=3, the `0c` stream | adds 0x0004 | run6's first ten |

These follow libmnl's state (warm or cold, fixed or not) and explain no
failure of both engines at once.

**Not the LTE transmitter either (2026-11-10).** With `airplane on`
(ModemManager: modem `disabled`, power state `low`; Wi-Fi and Bluetooth
off) the daemon ran a normal session (16177 search assignments, today's
orbit file) for 3 minutes: `heard: 0`, no record. Airplane mode off
afterwards: modem registered again, data and `titan-ims` up.

**A second fault, found on the way: offline, the daemon can aim at
nothing.** With airplane mode on (no network), the orbit file it had was
fetched at 04:05 UTC, early in the UTC day, so it held about four hours of
ephemeris; at 11:35 UTC the "search" tier made **0 assignments** and the
daemon ran an empty session, which can never succeed (status `searching`,
`orbits: 0`). The 48 h fallback only helps if the file covers the hours
before it; fetching the previous day's file as well, or keeping it, would.

**What libmnl expects from the modem side (2026-11-10, from the local
decompile in `out/gnss-re/`).** libmnl imports only libc and reaches the
hardware only through `/dev/stpgps`; it drives no antenna switch, LNA
supply or RF pin. Its modem-side inputs, all through mnld:

- the clock type (co-clock or TCXO) and the LNA pin, from `/dev/stpgps`
  ioctls (titan-mnld does both);
- the modem's crystal calibration: stock mnld reads 16 bytes at offset
  0x40 of the modem NVRAM file `CALIBRAT/ML4A_000` (on MT6771) into the
  init block's clock-calibration fields (C0, C1 and two more), for
  libmnl's co-clock temperature model. titan-mnld passes zeros (its
  template was captured under Debian, without that file); this only
  shapes libmnl's clock prediction;
- modem time-sync hooks (`mtk_MD_HW_SW_*`), unused here (the driver logs
  that this chip needs no GPS-modem sync);
- its own co-clock learning files (`CoClock_Learn.dat`,
  `CoClock_HeatDistin.dat`).

So nothing libmnl needs from the modem would stop the signal reaching the
correlators. Also checked: the MT6358's VFE28 ("front end") supply, which
the modem's boot moved to hardware control, is off in every dump including
4.14's with GPS working (its enable bit 0), so it isn't the GPS's; the
modem node claims no pins through pinctrl. libmnl also has factory tests
(`mtk_gps_set_jammer_test`, `…_cn0_test`, `…_chn_test`) that would measure
the RF front end directly.

**libmnl's own diagnostics (2026-11-10).** Logging the `$PMTK` sentences
in libmnl's NMEA output (titan-mnld `TITAN_MNLD_PMTK`, below) shows two
lines once a second: `$PMTKTSX1`, its crystal model, and `$PMTKAGC`, the
RF front end's gain values. With titan-mnld's zero clock calibration the
model put the crystal at **−24.68 ppm** (the measured value is −0.42
ppm). With the modem's calibration passed in as stock mnld does
(`TITAN_MNLD_CALIB`) it read **+0.96 ppm**, then refined to about −0.05
ppm: within libmnl's ±2 ppm. Even so, 6 minutes by the window gave no fix
and no satellite used (gpsd mode 1 in all 356 reports). The AGC values move
second to second (~2900-3200 and ~6500-6650 in pairs), so the front end is
powered and regulating. PMTK commands sent in through
`mtk_gps_nmea_input` (`PMTK605`, and `PMTK837,<band>,1,0,1`, the
single-frequency test: band 0 GPS L1, 1 GLONASS, 2 BeiDou B1) got no
answer in the output, so the factory test has not run yet.

**A boot without the modem (2026-11-10).** With `titan-net
voice off` and `cell off` (nothing starts `titan-modem`), after a warm
reboot on the same kernel and parameters: `titan_modem` not loaded, and
the crystal trim **still 0xD4**: the MT6358 stays powered through a warm
reboot and keeps its DCXO registers, so the modem's trim outlives it. Only
a power-off (or the preloader on a cold boot) returns it to 0x88. The
USB link dropped during the first run; over Wi-Fi, a 5-minute run in the
same boot (fresh orbits, ~12800 assignments a session) heard nothing:
`heard: 0` throughout. So the modem's running state is not the cause; its
trim, left at 0xD4, is the one modem-side change still in place.

**What else changed since the last fix (2026-11-10).** The device tree
in `boot-6.12-deep-ncs.img` (built 2026-11-02 21:36, the last image before
the fix at 23:06) against today's, phandles normalised: new are the NFC
controller (`st21nfc`), the SD card slot (`mmc1`; a card is in and mounted,
running UHS SDR50 at 96 MHz from MSDCPLL 384 MHz, VMC at 1.8 V), the flash
LEDs, SMI larbs 2-4, the video encoder, VPU and SVS nodes, and the modem
node. No change touches CONNSYS, BTIF, the GPS LNA pin or its supplies;
`WMT_SOC.cfg` is unchanged since 2026-10-11. PLLs and clock muxes
(APMIXED and TOPCKGEN read now, against the 2026-11-01 4.14 and 6.12
captures): MMPLL is 3150 MHz and on in all three (its half, 1575.000 MHz,
sits 0.42 MHz from L1, but it was so when GPS worked); the differences are
the CPU PLLs (cpufreq), the camera's MIPI 26 MHz gates (`AP_PLL_CON8` bits
6 and 11, off while the camera is idle), the SPM's PLL control bits and
mux settings. None of the SD card's or the new PLLs' harmonics falls
within a few MHz of L1, B1 or G1. Untested as desense sources: the SD
card's bus (edges, not just harmonics of its clock) and the NFC
controller.

Next, in order: (1) the modem's static setup:
its RF and antenna-switch pins (BPI) and front-end supplies, set when MD1
boots (a boot with the modem not started is the clean comparison: GPS
worked that way until 2026-11-02); (2) the crystal trim only after that.
Tools: `tools/gnss/clock-sweep.py` (generated sessions with the `02`
centres, the `10` clock or the window changed; counts `A0` records) and
`tools/gnss/libmnl-drift.py` (a libmnl capture's `02` against the
ephemeris, its `29` layouts, gpsd's modes); both run on the phone from
`/tmp` and print no position. libmnl's state from before these tests is in
`/root/mnl-state-backup/` (it holds a position: phone only).

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
titan-gnss-open (Rust since 2026-11-17; Python before)
  ├─ GPS broadcast ephemeris, fetched hourly (IGS merged file from BKG)
  ├─ our 02 search assignments (Doppler from ephemeris), rewritten into a replay script
  ├─ titan-gps-probe (Rust): firmware patch + script → /dev/stpgps → CONNSYS GNSS DSP
  ├─ reads the chip's 29 measurement records as they come
  └─ snapshot fix every 2 s over 30 s of batches → NMEA → UDP 10110 → gpsd
     (GGA, GSA, GSV, RMC; GSA and GSV between fixes too)
```

- **Sky view and DOP** (2026-10-30): every 2 s the daemon sends GSV, the
  satellites above the horizon (elevation and azimuth from the ephemeris
  at the last position, every 30 s) with the C/N0 of those the chip has
  reported in the last 10 s (weak ones too, below the fix's 15 dB-Hz), and
  GSA, the satellites in the fix (less those `solve_robust` dropped) with
  PDOP, HDOP and VDOP from their geometry, one clock unknown per
  constellation. GGA carries that HDOP (it was a fixed 1.0), so gpsd's
  error estimate follows the geometry: HDOP 0.8 gives `eph` 15.2 m. It is
  the geometry of one moment, though the fix spans the window's batches.
  gpsd 3.22 numbers BeiDou in NMEA only as 201-237 (under the `BD`
  talker; 33-64 it takes for SBAS), so BeiDou 38 and up are left out of GSV
  and GSA; they still count in the fix, GGA's satellite count and the DOP.
  Checked against a private gpsd (`-S 2950`, UDP 10111) with today's
  ephemeris at a public place: 31 satellites in view (GPS, GLONASS,
  BeiDou with their own numbers), the used ones marked.

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
- **libmnl's fraction is no measurement either** (2026-11-14, run6:
  libmnl's measurement log beside the chip's records, 2026-10-29, ~2
  minutes). For a pair of satellites, libmnl's time difference follows
  the integral of its reported Doppler difference less ~0.05 chip/s and
  jumps by ~2.2 chips every ~40 s (a sawtooth: Doppler carried forward,
  pulled back to the whole chips now and then). Against the geometry
  (broadcast orbits, libmnl's own median fix as the position, so if
  anything in libmnl's favour), the pair differences scatter about their
  mean by 94, 114 and 200 m rms for libmnl's times and 135-139 m for the
  records' whole chips, and libmnl's error is correlated over tens of
  seconds where the whole chips' rounding is fresh each second, so it
  averages down more slowly. The chip reports no sub-chip phase that
  libmnl reads (bit flips: only bytes 26-28 move its time); finer code
  phase would have to come from the firmware itself. For a static phone
  the joint fix's averaging over the window (~85 m / √epochs) is the
  method; Doppler could add to it only while moving, with each
  satellite's record offset estimated.
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

### The signal gap is a 6.12 problem (2026-11-14)

Locked satellites' C/N0 at the window, top four: Linux on the vendor 4.14
kernel 2026-10-30 32.2 and 29.5 dB-Hz (19 and 12 satellites locked), Android
2026-11-13 30.2 dB-Hz (31 locked), Linux on the 6.12 port since mid-November
~20 dB-Hz (4-5 locked). The earlier "not the kernel" test (build #86) compared
6.12 builds only. Ruled out on 6.12 since:

- **The clock frame's B:** sessions with B at -350, -275, -200, -125 and
  -50 Hz (search term moved with it), interleaved, two rounds of 120 s: the
  same satellites read the same C/N0 at every B (G32 18-20, G25 16-22).
- **The setup frames:** Android's are ours byte for byte but for the `08`
  header's two varying fields.
- **Vendor GPS hooks:** on MT6771 the vendor kernel does nothing for GPS:
  DRAM frequency hopping for GPS exists only for MT6765/MT6768,
  `mt_spm_for_gps_only` has no callers, `MTK_GPS_REGISTER_SETTING` is off,
  and `clk_buf_ctrl(CLK_BUF_CONN)` only records the request (for the
  buffers' low-power mode, whose CW23 bit was tested on 2026-11-13).

Left: what the 6.12 port does differently in general (no DRAM frequency
scaling: DRAM stays where the bootloader left it; CPU frequencies include
1586 MHz, the little cores idling at 793 MHz; RF and clock setup).

**What the two kernels leave in the PMIC** (the dumps of 2026-11-01 with
GPS running, `out/mainline/captures/pmic414-gps.txt` and
`pmic612-gps.txt`, names from the vendor `upmu_hw.h`): besides regulators
that 6.12 leaves under software control (on) where 4.14 hands them to
hardware low-power control, two settings in the GPS reference-clock path
differ, neither tested yet:

- `XO_EXTBUF7_EN_M` (`DCXO_CW11` bit 13, 0x7a2): the PMIC's seventh 26 MHz
  output buffer is forced on under 6.12 (0xa000), off under 4.14 (0x8000).
  The 2026-11-13 comparison with Android took `CW11` for the debug read-out
  mux; on Android this bit toggled during GPS (2026-11-10).
- `RG_VXO22_VOCAL` (`VXO22_ANA_CON0` bits 0-3, 0x1e24): the crystal
  oscillator supply's trim is 4 under 4.14 (0x0104), 0 under 6.12 (0x0100):
  our port does not apply that calibration. VXO22 was not among the
  supplies measured on 2026-11-13.

`tools/gnss/pmic-ab.py` (run on the phone, with `pmicpoke.ko` in /root)
switches them in 90 s phases inside one session (base, buffer 7 off, base,
VOCAL 4, base, both, base), prints each locked satellite's median C/N0 per
phase, and restores what it found.

**4.14 against 6.12 back to back (2026-11-14, 10:53-11:12, same spot).**
The 4.14 boot image of 2026-10-31 (`backups/boot_k35_20261031.img`) flashed
for the test, the same Debian root and the same daemon: the daemon locked 16
satellites at once (8 GPS, 5 GLONASS, 3 BeiDou) at a median 25-28 dB-Hz
(6.12 that morning: 1-5 at ~18). A 70 s probe session from the same
generator on each kernel, minutes apart: 4.14 locked 12, top four 30.2
dB-Hz (R4 29); 6.12 locked one (R4 20). `#128` flashed back and checked by
hash. On 4.14 the chip often would not answer a second session soon after
one (`no answer to the session header`) until the driver was restarted; and
no fix was reported with 16 satellites (the daemon at 87 % CPU; the
captures are kept to replay, `out/gnss-cap/k414-1114/`).

Hardware state during each session (`tools/gnss/hwdump.sh`: PMIC, clock
summary, regulators, the topckgen, infracfg, GPIO, SPM and apmixedsys
blocks, CPU and devfreq state; `out/mainline/captures/hw-1114/`):

- **The modem was not running on 4.14** (titan-modem fails there): its RF
  and SIM supplies (`VFE28`, `VRF18`, `VRF12`, `VSIM1/2`) are off there, on
  under 6.12. A confound to test first.
- `RG_VXO22_VOCAL` 4 against 0 again; `DCXO_CW11` is 0xa000 on both today.
- The CPU PLLs: under 6.12 the little cluster at 793 MHz runs its PLL at
  1586 MHz (con1 0x810f4000: 26 MHz x 61, /2), 10.6 MHz from L1; the 4.14
  dump has a PLL at 1586 MHz too, so registers alone don't separate them.

Tested on 6.12 since, each inside one GPS session in 90 s phases, the same
satellites compared (2026-11-14, 11:20-11:55): none changed C/N0.

- `RG_VXO22_VOCAL` 4 (as 4.14) against 0: no change (all records median
  17-20 dB-Hz, p90 30, in every phase).
- CPUs pinned to 1014 MHz (no PLL near 1586 MHz) against schedutil: no
  change.
- MMPLL (3150 MHz, twice L1): 6.12 clocks the multimedia bus (`mm_sel`,
  topckgen `CLK_CFG_0` bits 8-10) from MMPLL/7 at 450 MHz, so MMPLL runs
  all the time (the compositor's screen-off only turns the backlight off;
  the CRTC stays on); the vendor kernel had it on UNIVPLL/2/2 at 312 MHz.
  Switching to the vendor's source with MMPLL's enable bit cleared
  (`tools/gnss/mmpll-ab.py`) left every satellite where it was (G28 24/25,
  G31 20/21, R4 22/22).
- The modem: Android runs it (registered on LTE) at ~30 dB-Hz, and airplane
  mode on 6.12 gained nothing (2026-11-13).
- The panel supplies (GPIO175/177) and reset (GPIO45) differ only because
  the panel was off in the 4.14 dump.

So the 4.14 boot of 2026-11-14 differed in more than the kernel: panel,
GPU (MFGPLL), display PLLs and the modem all off. Still to separate: 6.12
with the display pipeline and GPU fully off, and CONNSYS's own registers
(the AFE the WMT driver programs), which the dumps leave out.

CONNSYS on 6.12, checked since (2026-11-14 afternoon): the GPS supplies
(VCN18, VCN28) are identical in all four PMIC dumps, mode and low-power
bits included; VCN33 differs in its voltage code only (3.5 V measured on
both, 2026-11-13). The connectivity drivers are the same source on both
kernels (our build); the 6.12-only branches are headers, debug output, the
CONN power domain through genpd, the regulators and the clock-buffer
request (a no-op, as the vendor's records only). `WMT_SOC.cfg` (shared
root): `co_clock_flag=1`, `wmt_gps_lna_enable=0`, `wmt_gps_lna_pin=0`.
The AFE block (0x180B6000), read during a GPS session, holds what the
driver writes (`WBG_PLL_03` 0x000c1df0, `WBG_GPS_02` 0x110a2000). The
MT6631's RF registers are set by the CONNSYS firmware over its own bus,
from the same firmware and configuration on both kernels.

**On 4.14 again (2026-11-14, 14:45-15:25):** DRAM is not it. The vendor
kernel's points are DRAM 3600, 3200, 3200 and 1600 Mbps (`/sys/power/vcorefs`);
it ran at 3600 for the first ~40 s of a session and 1600 after, and the
satellites were as strong in both (G16 29/30, G27 24/28). Nor is the display
being on: with the panel powered (`fb0` unblanked) a 10-minute session
held G16 29-33, G27 24-35, all records 22-28 dB-Hz.

**The display link rate.** The display PHY's PLL (`MIPITX`, 0x11E50000;
`tools/gnss/dsipll.py`): the vendor kernel drives the panel at 460.0 Mbps
(`CON0` 0x8d89d89d, /8; the link clock 230 MHz, its 7th harmonic 1610 MHz,
34.6 MHz from L1); our 6.12 driver computes 449.4 Mbps from the panel timing
(`CON0` 0x8a440e7e; clock 224.7 MHz, 7 x 224.7 = 1572.8 MHz, 2.6 MHz under
L1). Neither spreads it (`CON2` 0x80000001, `CON3` 0x80008000 on both: the
vendor's spread-spectrum is off for this panel). On 6.12 the link never
stops (the compositor's screen-off keeps the CRTC on). `tools/gnss/dsi-ab.py`
switches the rate live inside a GPS session, as the vendor's
`mipi_clk_change` does (`CON0`, then toggle `CON1` bit 0).
Result (2026-11-14 15:20-15:31, fresh orbits): no change, G16 17-19 at
both rates, G27 22/23, G8 20/18 (4.14 an hour earlier: G16 29-33, G27
24-35, G8 24-27). An earlier run of it was void: the orbit file was 4.5 h
old and the generator wrote no `02` frames (the A/B tools now refuse to run
without them).

**The audio PLLs.** Under 6.12 APLL1 (180.6336 MHz) and APLL2 (196.608 MHz,
from 3145.728 MHz) run though nothing uses them (no PCM open; the clock
framework counts them unused): `clk_ignore_unused` on our boot line keeps
whatever the bootloader left on. Under 4.14 they are off. APLL2 lands on
1572.864 MHz twice over (8 x 196.608, and its oscillator / 2), 2.56 MHz
under L1. `tools/gnss/apll-ab.py` clears their enable bits in 90 s phases.
Result: no change (all records 17-23 dB-Hz on and off). Also no change
(`tools/gnss/idle-ab.py`): the SPM cluster idle states (`mcdi-cpu`,
`mcdi-cluster`) disabled, WFI only: G16 16-17 and G27 25-28 in every phase.
The camera is off on 6.12 (clocks and supplies unused). The 4.14 GPS module
is not richer than ours (its size is debug data). The panel's link rate
differs because the vendor panel driver hard-codes `PLL_CLOCK = 230` (460
Mbps, `ssc_disable = 1`) while ours gives the timing (848 x 736 at 60 Hz)
and the DSI driver derives 449.3 Mbps.

Eleven direct tests have not moved C/N0 on 6.12. Next: whether the loss is
noise or gain. The chip's once-a-second status frames differ steadily
between the two 70 s captures (`out/gnss-cap/k414-1114/`): frame `42`
bytes 6 and 16 (u16: 4.14 24662 and 13089, 6.12 22653 and 11143), `30`
byte 10 (26716, 22600), `2c` byte 22 (25213, 44007), `4e` byte 12 (247,
12791), `37` bytes 28-34. libmnl prints a `$PMTKAGC` report; which field it
reads would say whether the receiver turned its gain down (a jammer) or up
(a weak front end).
Checked (2026-11-14): libmnl's `$PMTKAGC`, `$PMTKAG2` and `PMTKAGC,`
belong to its assisted-GPS messages (the output in `mtk_gps_agps_msg_init`),
not automatic gain control; no libmnl string names a gain or noise field.
(`tools/gnss/thumb-xref.py` finds the code that loads a string in a Thumb
PIC library: `ldr rX, [pc, #]` ... `add rX, pc`; libmnl's virtual addresses
are its file offsets + 0x2000, the decompile's are + 0x10000 more.)

**Display, GPU and audio drivers all off at once** (2026-11-14 16:08-16:15,
6.12, unbound live through sysfs: DRM, panel, DSI and its PHY, the display
blocks, panfrost, the sound card and the AFE; the compositor stopped; the
display link PLL, GPU PLL and panel off afterwards): no change (baseline
G27 20, G8 19; stripped G8 19, G27 18). The PMIC's bucks match on both
kernels (mode and frequency registers). Left in the dumps: infracfg
`0x10001f00` (`AP2CONN_OSC_EN`) bit 11 is set under 4.14 (0x01c01a24) and
clear under 6.12 (0x01c01224); our WMT code reads it as the BT CVSD
interrupt mask and logs "BT CVSD interrupt unmasked" at every power-on.
`tools/gnss/osc-ab.py` sets it in 90 s phases.
Result: no change (set: G2 21, G8 19; clear: G2 19/15, G8 17/15).

**Without `clk_ignore_unused`** (#128 with only that word removed from the
boot header, `out/mainline/boot-6.12-128-noclkignore.img`): the kernel
gates a clock the UFS needs, the root partition's superblock read fails
with an I/O error, and the ramdisk stays in rescue mode (telnet on
172.16.42.1). That is why our Debian images carry the option. #128 flashed
back and checked. The infra clock gates differ widely between the kernels
(infracfg `0x10001090`: 4.14 0x8baffb00, most gated; 6.12 0x00407800, most
running), so a test of them would have to gate unused clocks one by one.
Also: Wi-Fi was off in every 6.12 GPS run today and on in the strong 4.14
runs.

The infra gates one by one: `out/mainline/captures/hw-1114/infra-gates.txt`
lists all 97 (`clk-mt8183.c` `GATE_INFRA0-3`) with their state on 4.14 and
6.12 and the 6.12 enable counts: 47 run on both; 31 are off on 4.14 and on
under 6.12, all but `infra_disppwm` unused by any 6.12 driver. Gating those
30 live (the framework already counts them off) left the phone running and
storage readable. `tools/gnss/gate-ab.py` gates them in a GPS session:
group A (PWMs, IR, SPI 0-5, UART 2/3, idle I2C buses, ICUSB), then also
group B (the SD/eMMC self clocks, `gce_26m`, `cpum`, `uart1`).
Result (2026-11-14 16:44-16:55): at most a few dB, inconsistent: G8 (the
one satellite through all phases) base 24/25/20/18, A 28, A+B 22 and 23;
G27 base 19/17, A 20, A+B 16. Not the 10 dB. One STP transmit timeout
(retried) in the kernel log during the run, perhaps group B. All restored.

**How CONNSYS runs on each kernel** (the WMT driver's
`wmt_core_dump_func_state` lines; 4.14 from the journal of 2026-10-19):
under 4.14 Bluetooth powers the chip 18.9 s after boot and Wi-Fi joins
0.14 s later; the chip stays on (`b:2 w:2 wmt:2`) and GPS joins it
(`status(b:2 f:0 g:2 w:2 lpbk:2 … wmt:2)`). Under 6.12 `titan-bt` is
stopped about two minutes after boot and Wi-Fi does not come up, so the
chip is off between GPS sessions (`wmt:0`) and every session cold-starts
it with GPS alone (`b:0 g:2 w:0`). With Wi-Fi, or Bluetooth and Wi-Fi, on
under 6.12, our probe's session never starts ("no answer to the session
header", 4 of 4 tries), so GPS joining a running chip, the 4.14 and
Android case, is untested on 6.12. Not the cause: the "GPS LNA pin
number(-1)" line (6.12; 4.14 prints 155) is the number a vendor helper
reports; the LNA is switched through the device-tree states on both
(GPIO155 low with GPS off, high with it on); libmnl's ioctls 10 and 11
after opening the device are read-only queries (`RTC_FLAG`,
`CO_CLOCK_FLAG`).

**The session start on a running chip, fixed (2026-11-14):** libmnl opens
`/dev/stpgps`, runs six ioctls, starts its reader and writes the header
~60 ms after the open (Android trace); the probe wrote it the moment the
open returned. On a cold chip the open takes 1-2 s (the chip powers up)
and that worked; with Bluetooth or Wi-Fi running the open returns in ~30
ms and the header went unanswered. `titan-gps-probe` now waits 100 ms
after the open (`TITAN_GPS_OPEN_SETTLE_MS`); installed (the previous one as
`.prev-settle`). First session with Bluetooth and Wi-Fi on (`b:2 g:2 w:2`):
started at once; G8 28, G2 18, G1 16, all records median 18, p90 28 (cold
sessions: median 17-18, p90 25-26).

Also ruled out on 6.12 since (2026-11-14 evening):
- Bluetooth and Wi-Fi on (GPS joining a running chip, as 4.14 and Android
  run it) against off, alternating, two rounds: no difference (off G8
  26/22, G2 22/21; on G8 22/15, G2 17/17).
- Userspace: a session with the compositor, modem stack, sensor client,
  charge controller, keyboard and power-key daemons, Bluetooth, Wi-Fi, USB
  storage and the user session stopped: G1 20 before and after.
- The CONNSYS patch: both kernels load the same two (314092 and 229368
  bytes, built 2021-03-04, `PhVer 0x00f7`); the WMT config is the shared
  file; the CONNSYS interface pins match.
- The connectivity driver source: the 4.14 modules (built 2026-10-11/13)
  match the 2026-10-26 commit; what changed since for 6.12 is API updates
  (`ioremap`, time types, thermal and remove callbacks) and two STP fixes
  (acknowledge dropped packets, accept zero-checksum assert frames).

**CONNSYS's own registers (2026-11-14 evening).** The WMT driver's debug
file reads them through the firmware: after `echo DB9DB9 >
/proc/driver/wmt_dbg`, `b ADDR MASK` sends a register-read command over STP
and logs the answer (`c ADDR VALUE` writes). Addresses are the connectivity
MCU's: 0x80000000 is the chip configuration block (chip id 0x788 at +8).
`tools/gnss/connreg.py` wraps it (about 300 reads a second; a GPS session
running beside it is not disturbed). The RF chip (MT6631) is reached through
the SPI master at 0x81026000: a request (register | bit 12 to read | set
<< 13) written to the top slot's address register (+0x50), the answer at
+0x58. Set 5 is the chip's top registers (32 bits, every 4; 0x024 reads
0x66318b01), set 4 the GPS RF registers (16 bits in the upper half; 0x500
to 0x51b are in use, and 0x508 holds the 0x0210 the firmware's own last
write left in the GPS slot). Sets 1 and 2 hold values too (Bluetooth/Wi-Fi
and FM by the look of it); 0, 3, 6 and 7 read empty.

`tools/gnss/conndump.sh` runs one session and from 32 s in dumps the top and
GPS sets twice, the other sets, and nine CONNSYS pages (0x80000, 0x80021,
0x80024, 0x80026, 0x80030, 0x81020, 0x81021, 0x81024, 0x81026). On 6.12
(`out/mainline/captures/conn-1114/612-a/`): both passes identical in every
top and GPS register, so a difference from 4.14 will stand out; the session
was as weak as ever (median 18 dB-Hz, four locked at 16-19).

Two reads reboot the phone (watchdog, about a minute; noted under Hazards):
the firmware-side read of 0x80050000, and an AP-side read (debug command
`17`, or `/dev/mem`) of a CONNSYS address outside the blocks the drivers
map. Also seen: one STP transmit timeout (retried) when the debug file
powers GPS on (`7 2 1`).

### The 10 dB found: the camera clock pins (2026-11-14)

**The same dump on 4.14** (`backups/boot_k35_20261031.img` flashed, #128
back afterwards and checked by hash; `conn-1114/414-a/`; that session:
median 25 dB-Hz, 15 locked). Bluetooth and Wi-Fi run there, so the CONNSYS
pages differ widely and the firmware's own use of the SPI top slot spoils
a read now and then (0x00000007). In the RF chip's GPS set two registers
differ: 0x508 (4.14 0x028e, 6.12 0x0210) and 0x505 (0x5887, 0x5e07).

**0x508 is the receiver's gain** (bits 7-5 by the look of it, the low five
bits hover at 0x0c-0x10): the firmware sets it at the session's start and
moves it after. Watched once a second (`tools/gnss/agcmon.py`): on 4.14 it
stayed where it started through four sessions (0x028e, 0x02ce, 0x02ce,
0x02cf); on 6.12 it started alike (0x028e, 0x026f) and fell to 0x020f/0x0210
within 3 s, and a value written over it was replaced at once. With the LNA
switched off (GPIO155 low for 30 s) it rose to 0x02f0, so higher is more
gain: on 6.12 the receiver ran at its lowest gain because about 10 dB more
power came in through the LNA. Noise, not lost gain.

With that as a one-second indicator, no change from: storage traffic (a 3
GB direct read), all cores busy, cores 1-7 offline, airplane mode. Then the
pins whose function differs between the kernels' GPIO dumps
(`hw-1114/k414/gpio.txt` against `k612`), switched to plain GPIO inside a
session: **GPIO99, 100 and 108, the camera sensors' clock outputs (CMMCLK0,
1, 2).** With the three as GPIOs the satellites came up within seconds (12
locked, G1 28-30, C29 33) and fell back when the clocks returned (G1 20).
GPIO99 (the rear camera) alone gives most of it (G1 27); 100 or 108 alone
little. PWM_A (GPIO90), the I2S pins (170-174) and the audio pins (136-139)
made no difference.

The cause: the bootloader leaves the 24 MHz sensor clocks running,
`clk_ignore_unused` keeps them, and our device tree muxed GPIO99 and 100 as
clock outputs in the camera node's default state (GPIO108 came muxed from
the bootloader). All three pins toggle with no camera ever opened (sampled
through the GPIO block's input register: high in 40-50 % of reads), into
unpowered sensors beside the GPS antenna. The vendor kernel holds the pins
as GPIOs, low. Setting the gate bits I took for the `camtg` muxes
(topckgen 0x70 and 0x80) did not stop the pins.

A whole session with the pins as GPIOs (6.12 #128, 150 s): median 25-26
dB-Hz, p90 30-32, 15 satellites locked (G17 30-35, G1 28-33, C3 29-32), the
gain at 0x02ce throughout: the same as 4.14 that evening.

**The fix:** the camera node's `default` pin state is now the three pins as
GPIOs driven low, and a new `mclk` state has the two clocks; the camera
driver (`modules/camera/linux612/titan_camera.c`) selects `mclk` when the
first sensor is powered and `default` when the last is off (with a device
tree that has no `mclk` state it behaves as before).
`out/mainline/boot-6.12-128-mclk.img` is #128 with only the device tree
replaced (kernel, ramdisk and command line identical, checked by unpacking).

**Flashed and checked (2026-11-14, 19:25-19:35).** With
`boot-6.12-128-mclk.img` (partition hash checked) and the new
`titan_camera.ko` (the previous one beside it as `.prev-mclk`): after boot
the three pins are GPIOs, outputs, low in 500 of 500 reads. First session,
120 s, nothing switched by hand: median 25 dB-Hz, p90 29-31, 18 satellites
locked (G1 38-40, C29 28-31, R12 28-30), the gain starting at 0x02ae. Both
cameras through libcamera (`cam -c 1` and `-c 2`, YUYV): 20 frames each at
29-31 fps with picture content; during a 150-frame rear stream GPIO99 is
the clock again (mode 1, high in 260 of 500 reads), and all three are low
GPIOs once it stops. While a camera is open the GPS loses the 10 dB again.

### Mixing constellations (2026-11-14)

Each batch is one constellation's and has its own clock bias, so a batch
of n satellites gives n − 1 equations. Until 2026-11-14 the daemon used
only batches of four or more, so a fix needed four satellites of one
system locked together; at night only three GPS satellites held at once,
and the one or two GLONASS satellites that locked beside them were
wasted. Now:

- **Batches of two or more go in** (`FIX_MIN_BATCH`, env
  `TITAN_GNSS_MIN_BATCH`, default 2).
- **The rule counts what the batches constrain:** `snapfix.counted`
  counts satellites seen in two batches or more, less one for each
  constellation past the first, and `snapfix.epochs` counts the
  measurement times (batches within 0.3 s) with three equations or more.
  A full fix needs 5 and 3 epochs; a quick fix (height and clock held)
  needs 4 and 3 epochs. With one constellation and batches of four these
  are the old rules. Three GPS and two GLONASS satellites count as four.
- **GLONASS measurements weigh a quarter** (`snapfix.WEIGHT`, half the
  amplitude): its chip is twice as long. On the window recording four
  GLONASS satellites alone fixed at 105-136 m median, four GPS at 46 m.
- **One shared clock bias is not possible yet:** a GLONASS or BeiDou
  batch's bias less the nearest GPS batch's jumped by ~140 km between
  pairings. The systems' records are not measured at one instant, and the
  time tags (bytes 20-24) count per constellation; GLONASS channels carry
  two tag groups of their own. Decoding the tags would save one equation
  per constellation per epoch.
- **GLONASS and BeiDou are aimed with the GPS term** (`offset=` to
  `own02.regenerate`, the undithered term, −350 Hz under the modem's
  trim): libmnl uses one common term for all three. Before, they were
  aimed at −150 Hz with the template's 512 Hz window.

Replayed over the 2026-10-30 window recording (`replay.py --only-per`
keeps the first N satellites of each system), horizontal p50 / p90:

| Satellites kept | Batches of 4 (before) | Now |
|---|---|---|
| all (G, R, C) | full 12 / 23 m | full 11 / 24 m, first fix 60 m off instead of 131 m |
| G4 | quick 46 / 81 m | quick 48 / 81 m |
| G3 R2 | no fix | quick 58 / 109 m, from 17 s |
| G3 R3 | no fix | full 84 / 135 m |
| G3 C2 | no fix | quick 88 / 163 m |
| G2 R2 C2 | no fix | quick 78 / 142 m |
| G3 R2 C2 | no fix | full 22 / 35 m |
| R4 | quick 136 / 232 m | quick 104 / 235 m |

On the earlier recording of the same evening (all satellites) the first
full fix came at 18 s instead of 29 s, p90 120 m instead of 201 m.

Live at the window (2026-11-14 09:03, 5 minutes, deployed): two GPS
satellites locked (G10, G13 at 17-20 dB-Hz), no GLONASS or BeiDou, so no
fix; the new rule's report said "0 times with 3+ equations, 2 satellites
counted". Combining needs a third and fourth satellite to lock.

**More samples would not have helped there.** The chip reports each locked
satellite once a second (window recording: 0.93-1.01 records/s per
satellite) and the daemon uses every one. Each is whole chips (~85 m rms
rounding), and the rounding is fresh each second (the measurement instant
moves within the code period: G19's chips 888, 975, 488, 960...), so a
60 s window averages it to the ~11 m seen with many satellites. More
records a second would sharpen a fix that has enough satellites; with
two, every record repeats the same two lines of position and no rate
gives a third. No frame that raises the rate is known (libmnl's sessions
also run at 1/s).

## When it fails (2026-11-04, written and tested offline, not yet on the phone)

Reported in use: the service turning off, "searching" with no satellites, and satellites listed with no fix. From reading the code:

- A Python exception in a session killed the daemon, and during systemd's 10 s restart `is-active` said `activating`, so the switch showed Location off. A session's exception, a bad frame or a failed solve no longer ends the daemon (only the kind of error and the line are logged: a message could hold a position), and `titan-location status` counts `activating` as on. `Requires=` from `titan-gps-driver` and `titan-wmt` can still stop it, which the journal will show.
- "Searching" was shown whatever the daemon was doing: no position, no orbit data, the probe failing, or the chip silent, which it would have waited for without end. `/run/titan-gnss-open.status` now says which, with no position in it, and titan-location-gui shows it. Nothing from the probe for 90 s ends a session, its stderr is drained, and sessions that hear nothing back off up to 5 minutes (see Hazards).
- The GSV sentences list every satellite above the horizon, from the orbits, heard or not, so "satellites" could be none received. The app now says used, heard and overhead.
- The unit did not start gpsd, as titan-gnss's does; it does now.

(Since 2026-11-10 a session runs on instead of restarting every 600 s.) Not changed, to be measured: and a fix over 200 km from the last position is rejected as an ambiguity with nothing to update that position, so a long trip with the GPS off would never get a fix.

**Orbit data failover.** In order: the first of `BRDC_URLS` that serves a valid file (a login page also answers 200; only BKG's three were seen to serve it, as IGN timed out from here); the file up to 4 h old with the chip's own ephemeris; then the file up to 48 h old, GPS and BeiDou only, with the almanac, only to aim the search, with fixes from what the chip decodes from the sky; then the almanac alone (2026-11-18, "Offline: the almanac" above). Until 2026-11-18 the 48 h tier made no assignments at all (the 4 h aiming limit). For 3 h after midnight UTC it fetches the previous day's file, as the new one has little in it. With nothing at all the daemon waits and the app says so. Tested with fake probes and the real file: validation, each tier by file age, a corrupt file, a silent chip, garbage frames, 300 kB on stderr, a failed probe.

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
| `userspace/titan-gnss-open/` | the open daemon: the Rust crate (`src/`, `build.sh` → `out/titan-gnss-open`, tests in `src/tests.rs` with `tests/gen_vectors.py` and `tests/replay_py.py`), the Python daemon it was ported from, its unit, `install.sh` |
| `tools/gnss/fix-ab.py` | two daemons' fixes in the same place, compared by distance only (`record LABEL SECONDS`, `compare`) |
| `userspace/titan-gps/` | Rust crate: `frame` (codec), `patch` (finds/cuts the patch), `titan-gps-probe` (runs a script, from a file or as a stream on standard input with `-`, logs chip frames to stdout or `--log`; `--quiet` disables the log entirely). `build.sh` → `out/titan-gps-probe` (static aarch64 musl), tests included |
| `userspace/titan-mnld/` | our libmnl host (C, 32-bit bionic); env switches `TITAN_MNLD_MEAS`, `_DBG`(`_TYPE`), `_MEMDUMP`, `_RAW`, `_DEBUG`, `_DROP`(`_AFTER`,`_B2`,`_ID`), `_FLIP`(`_SATS`,`_B2`,`_ID`,`_PERIOD`), `_WFLIP`(`_SV`,`_CON`,`_PERIOD`), `_PMTK` (`SEC:PMTK...;...`: send PMTK sentences, log libmnl's `$PMTK` output), `_CALIB` (a file whose bytes 0x40-0x4f become the init block's clock calibration, as stock mnld reads the modem's `CALIBRAT/ML4A_000`); all off unless set. `build.sh` → `out/titan-mnld` |
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
  `/usr/local/bin/titan-gnss-open` (the daemon),
  `/usr/local/lib/titan-gnss-open/` (the Python daemon, not run), `/var/lib/titan-gnss-open/`
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
- **Two CONNSYS register reads reboot the phone** (2026-11-14, twice): the
  firmware-side read of 0x80050000 (`wmt_dbg` command `b`; the driver's own
  comment says this chip cannot access 0x80050078), and AP-side reads of
  CONNSYS addresses the drivers don't map (0x18021010 or 0x18070008, through
  command `17`). `connreg.py` syncs the address to its output before each
  read, so the last line names the culprit. Pages not yet tried: 0x80070,
  0x81027, 0x81030, 0x81040, 0x81050, 0x81060, 0x81080.
- **Don't suspend with the GPS running.** A resume while the probe runs
  can crash the connectivity chip (BTIF write fails, `tx_timeout`, whole-
  chip reset, Wi-Fi gone until it ends): twice in about 315 suspend/resume
  cycles on 2026-10-30. `titan-gnss-open` holds the wakelock
  `titan-gnss-open` while it runs and titan-powerkey doesn't suspend while
  one is held ([power.md](power.md#power-button-titan-powerkey)). The old
  libmnl stack (`titan-mnld`, `titan-gnss`) holds none.
- strace: a blocking `read()` is time-stamped at its start, so replies
  appear before the writes they answer. strace often attaches after
  `/dev/stpgps` is opened: use `--fd`.
- The kernel log's timestamps don't follow the wall clock (no suspend
  time), and floods wrap the ring buffer quickly.
- **titan-mnld aborts once process ids pass 65535.** 32-bit bionic libc
  refuses to run (SIGABRT, "pthread_mutex_t ... accepts pid <= 65535")
  and Debian sets `kernel.pid_max` to 4194304, so after some hours of
  uptime the libmnl engine can't start. For a test, a runtime drop-in
  that runs it in its own PID namespace works: `RootDirectory=` and
  `BindPaths=` cleared, `BindPaths=/dev:/opt/android-gps/dev` (and
  `/proc`, `/sys`), `ExecStart=/usr/bin/unshare --pid --fork --kill-child
  /bin/sh -c "/usr/sbin/chroot /opt/android-gps /vendor/bin/titan-mnld;
  exit $$?"` (2026-11-13; not in the unit file yet).
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
   (an ephemeris must repeat in two 30 s frames; longer sessions). Still from the network: BeiDou and
   GLONASS aiming after 4 h offline (GPS has the almanac since 2026-11-18),
   and BeiDou's geostationary satellites (`4D`, not visible here).
6. **Reliability**: restart sessions that never measure (the daemon does
   on the next cycle; it could check sooner).
7. **Rust**: done (2026-11-17, "The daemon in Rust" above). The Python
   modules in `tools/gnss/` stay the analysis tools and the reference the
   tests are made from: a change to the method goes into both, and
   `tests/gen_vectors.py` is run again.
