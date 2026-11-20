// SPDX-License-Identifier: GPL-2.0-or-later
//! titan-headphones: wired headphones on Linux 6.12 (docs/audio.md, "Headphones on Linux 6.12").
//!
//! The jack is our `mt6358-accdet` (input device "titanpocket Headset Jack": SW_HEADPHONE_INSERT,
//! SW_MICROPHONE_INSERT, SW_LINEOUT_INSERT). The loudspeaker plays DL1 (PipeWire's
//! titan-speaker, behind the limiter titan-speaker-limited) through I2S3. While something is in
//! the jack the headphones get a sink of their own, so each app can play on either:
//!
//! - DL2 (hw:0,1, otherwise unused) is routed to the codec's DAC (ADDA DL) and the headphone
//!   muxes; DL1 keeps only I2S3. Pulled out, DL2's routes and the muxes go off again.
//! - The sink, "titan-headphones" ("Headphones"), is an ALSA adapter node created through a
//!   pw-cli this daemon keeps running: it lives as long as that process (pulled out, PipeWire
//!   restarted). pipewire-pulse can't load ALSA sinks, and a one-shot `pw-cli create-node`
//!   exits before the node exists.
//! - Plugged in, it becomes PipeWire's default and the apps playing on the speaker move to it,
//!   as a phone does. Afterwards the Sound app moves apps or the default between the two (and
//!   Bluetooth) like any outputs. Pulled out, the sink goes; its apps fall back to the speaker.
//!
//! Each output keeps its own level, as titan-speakerd does on Linux 4.14, and the shared level
//! file (volume) belongs to the default: the headphones' while their sink is the default, else
//! the speaker's. On a change it is saved to volume.speaker or volume.headphones and loaded with
//! the other one's, and volume.output says whose it is, so the volume keys, the Sound app and
//! the top bar work on it. For the headphones the level is applied here whenever the file
//! changes: full scale is -8 dB, 1.5 dB a key step below; the codec's "Headphone Volume" has
//! 1 dB steps from -10 dB up, so it takes the step at or above the target and the headphones'
//! sink the rest. Level 0 silences. The speaker's level is the amp's gain, as always.
//!
//! Calls switch routes of their own (titan-ims, titan-call-audio: the UCM profile's devices) and
//! put the speaker back when they end. Changes to these routes are watched (ALSA control
//! events); outside a call ours are put back. During a call nothing is switched: a plug or
//! unplug waits for its end.
use std::fs;
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::time::{Duration, Instant};

const CARD: &str = "titanpocket";
const JACK_NAME: &str = "titanpocket Headset Jack";
const SINK: &str = "titan-headphones";
const SINK_DEVICE: &str = "hw:CARD=titanpocket,DEV=1"; // DL2
const LEVEL_DIR: &str = "/var/lib/titan-audio";
const VOL_FILE: &str = "volume"; // in LEVEL_DIR, and the rest
const VOL_SPEAKER: &str = "volume.speaker";
const VOL_HEADPHONES: &str = "volume.headphones";
const VOL_OWNER: &str = "volume.output";
const VOL_MAX: i32 = 224;
const VOL_DEFAULT: i32 = 160; // the speaker's, as titan-powerkey
const HP_DEFAULT: i32 = 107; // about -30 dB
const HP_MAX_DB: f64 = -8.0; // level VOL_MAX
const HP_RANGE_DB: f64 = 42.0; // 1.5 dB per key step of 8
const HP_PGA_MIN: i32 = -10; // "Headphone Volume" 0; 1 dB a step up to +8 dB (18)
const HP_CONTROL: &str = "Headphone Volume";
const AMP_CONTROL: &str = "Speaker Amp Volume";
const AMP_RANGE_DB: f64 = 42.0; // as titan-powerkey and titan-mixer
const AMP_STEP_DB: f64 = 0.5;
// where apps play on the speaker: the limiter's input, or the speaker without it
const SPEAKER_SINKS: [&str; 2] = ["titan-speaker-limited", "titan-speaker"];
const PIPEWIRE_RUNTIME: &str = "/run/user/0";
// the routes: DL1 to the speaker always, DL2 to the headphones while plugged in; DL1 to the
// codec only in a call (the UCM profile's devices)
const ROUTES_IN: [(&str, &str); 8] = [
    ("I2S3_CH1 DL1_CH1", "on"),
    ("I2S3_CH2 DL1_CH2", "on"),
    ("ADDA_DL_CH1 DL1_CH1", "off"),
    ("ADDA_DL_CH2 DL1_CH2", "off"),
    ("ADDA_DL_CH1 DL2_CH1", "on"),
    ("ADDA_DL_CH2 DL2_CH2", "on"),
    ("HPL Mux", "Audio Playback"),
    ("HPR Mux", "Audio Playback"),
];
const ROUTES_OUT: [(&str, &str); 8] = [
    ("HPL Mux", "Open"),
    ("HPR Mux", "Open"),
    ("ADDA_DL_CH1 DL2_CH1", "off"),
    ("ADDA_DL_CH2 DL2_CH2", "off"),
    ("ADDA_DL_CH1 DL1_CH1", "off"),
    ("ADDA_DL_CH2 DL1_CH2", "off"),
    ("I2S3_CH1 DL1_CH1", "on"),
    ("I2S3_CH2 DL1_CH2", "on"),
];
const JACK_SETTLE: Duration = Duration::from_millis(300); // a plug's ring passes the mic contact
const ROUTE_SETTLE: Duration = Duration::from_millis(1500);
const IN_CALL_RECHECK: Duration = Duration::from_secs(2);
const OWN_CHANGES: Duration = Duration::from_secs(1); // control events from our own switching
const SINK_WAIT: Duration = Duration::from_secs(3); // for the new sink to show
const RESTART: Duration = Duration::from_secs(3); // pw-cli or pactl gone: PipeWire restarting
const DEFAULT_SETTLE: Duration = Duration::from_millis(200);

// ---- C library ----

#[repr(C)]
struct PollFd {
    fd: i32,
    events: i16,
    revents: i16,
}
const POLLIN: i16 = 1;

extern "C" {
    fn poll(fds: *mut PollFd, nfds: u64, timeout: i32) -> i32;
    fn ioctl(fd: i32, request: u64, arg: i64) -> i32;
    fn inotify_init1(flags: i32) -> i32;
    fn inotify_add_watch(fd: i32, path: *const std::ffi::c_char, mask: u32) -> i32;
    fn fcntl(fd: i32, cmd: i32, arg: i32) -> i32;
}
// EVIOCGSW(8) = _IOR('E', 0x1b, 8): the switch states
const EVIOCGSW8: u64 = 0x8008_451b;
const SW_HEADPHONE_INSERT: u32 = 2;
const SW_MICROPHONE_INSERT: u32 = 4;
const SW_LINEOUT_INSERT: u32 = 6;
// SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS = _IOWR('U', 0x16, int)
const CTL_SUBSCRIBE: u64 = 0xc004_5516;
const CTL_EVENT_SIZE: usize = 64; // struct snd_ctl_event; the element's numid at byte 8
const O_NONBLOCK: i32 = 0o4000;
const F_SETFL: i32 = 4;
const IN_CLOSE_WRITE: u32 = 0x8;
const IN_MOVED_TO: u32 = 0x80;

fn log(msg: &str) {
    eprintln!("titan-headphones: {msg}");
}

fn nonblock(fd: i32) {
    unsafe { fcntl(fd, F_SETFL, O_NONBLOCK) };
}

// ---- levels ----

fn level_path(name: &str) -> String {
    format!("{LEVEL_DIR}/{name}")
}

fn read_level(name: &str) -> Option<i32> {
    fs::read_to_string(level_path(name)).ok()?.trim().parse::<i32>().ok().map(|v| v.clamp(0, VOL_MAX))
}

fn write_text(path: &str, text: &str) {
    let tmp = format!("{path}.tmp");
    if fs::write(&tmp, text).and_then(|_| fs::rename(&tmp, path)).is_err() {
        log(&format!("{path}: can't write"));
    }
}

/// The shared level file follows the default output: saved for the old one, loaded with the
/// new one's
fn switch_levels(to_headphones: bool) {
    let _ = fs::create_dir_all(LEVEL_DIR);
    let owner = fs::read_to_string(level_path(VOL_OWNER)).unwrap_or_default();
    // no owner yet: the level file always belonged to the speaker
    let from_headphones = owner.trim() == "headphones";
    if from_headphones == to_headphones {
        if owner.trim().is_empty() {
            write_text(&level_path(VOL_OWNER), "speaker\n");
        }
        return;
    }
    let cur = read_level(VOL_FILE);
    if let Some(c) = cur {
        write_text(&level_path(if from_headphones { VOL_HEADPHONES } else { VOL_SPEAKER }), &format!("{c}\n"));
    }
    let new = if to_headphones {
        read_level(VOL_HEADPHONES).unwrap_or(HP_DEFAULT)
    } else {
        read_level(VOL_SPEAKER).or(cur).unwrap_or(VOL_DEFAULT)
    };
    write_text(&level_path(VOL_FILE), &format!("{new}\n"));
    write_text(&level_path(VOL_OWNER), if to_headphones { "headphones\n" } else { "speaker\n" });
    log(&format!("the volume keys on the {}, level {new}", if to_headphones { "headphones" } else { "speaker" }));
}

/// A headphone level as the codec's control value and the sink's dB (None: silent)
fn headphone_gain(level: i32) -> (i32, Option<f64>) {
    if level <= 0 {
        return (0, None);
    }
    let target = HP_MAX_DB - HP_RANGE_DB * (VOL_MAX - level) as f64 / VOL_MAX as f64;
    let pga = ((target - 1e-9).ceil() as i32).max(HP_PGA_MIN);
    (pga - HP_PGA_MIN, Some(target - pga as f64))
}

/// The sink volume for a gain: a linear factor, since pactl takes a value with a sign ("-28dB")
/// as a step from where the volume is
fn sink_value(db: Option<f64>) -> String {
    db.map_or("0".to_string(), |db| format!("{:.6}", 10f64.powf(db / 20.0)))
}

/// The amp's volume for a speaker level (0 mutes), as titan-powerkey's
fn amp_value(level: i32) -> i32 {
    if level <= 0 {
        return 0;
    }
    (VOL_MAX as f64 - AMP_RANGE_DB * (1.0 - level as f64 / VOL_MAX as f64) / AMP_STEP_DB).round_ties_even() as i32
}

// ---- mixer and PipeWire ----

fn cset(name: &str, value: &str) {
    match Command::new("amixer").args(["-q", "-c", CARD, "cset", &format!("name={name}"), value]).stdout(Stdio::null()).output() {
        Ok(o) if !o.status.success() => log(&format!("{name} = {value}: {}", String::from_utf8_lossy(&o.stderr).trim())),
        Err(e) => log(&format!("amixer: {e}")),
        _ => {}
    }
}

/// The control numids of `names` (`amixer controls`)
fn numids(listing: &str, names: &[&str]) -> Vec<u32> {
    listing
        .lines()
        .filter_map(|l| {
            let id = l.strip_prefix("numid=")?.split(',').next()?.parse::<u32>().ok()?;
            let name = l.split("name='").nth(1)?.trim_end_matches('\'');
            names.contains(&name).then_some(id)
        })
        .collect()
}

fn pactl_cmd(args: &[&str]) -> Command {
    let mut c = Command::new("pactl");
    c.args(args).env("XDG_RUNTIME_DIR", PIPEWIRE_RUNTIME).stdin(Stdio::null()).stderr(Stdio::null());
    c
}

fn pactl(args: &[&str]) -> bool {
    pactl_cmd(args).stdout(Stdio::null()).status().is_ok_and(|s| s.success())
}

fn pactl_out(args: &[&str]) -> Option<String> {
    let o = pactl_cmd(args).output().ok()?;
    o.status.success().then(|| String::from_utf8_lossy(&o.stdout).to_string())
}

/// `pactl list short sinks`: (index, name)
fn short_sinks(text: &str) -> Vec<(String, String)> {
    text.lines().filter_map(|l| {
        let mut f = l.split('\t');
        Some((f.next()?.to_string(), f.next()?.to_string()))
    }).collect()
}

/// The streams to move to the headphones: those on the speaker's app sink (the limiter's
/// input, not the limiter's own stream into the speaker), from `pactl list short sink-inputs`
fn speaker_streams(sinks: &[(String, String)], inputs: &str) -> Vec<String> {
    let Some(speaker) = SPEAKER_SINKS.iter().find_map(|n| sinks.iter().find(|s| s.1 == *n)) else { return vec![] };
    inputs
        .lines()
        .filter_map(|l| {
            let mut f = l.split('\t');
            let (idx, sink) = (f.next()?, f.next()?);
            (sink == speaker.0).then(|| idx.to_string())
        })
        .collect()
}

// ---- calls ----

/// A call has the routes: a circuit-switched one runs titan-call-audio, a VoLTE one titan-ims's
/// player (pw-cat with the Phone role)
fn in_call() -> bool {
    let Ok(dir) = fs::read_dir("/proc") else { return false };
    dir.flatten().filter(|e| e.file_name().to_string_lossy().bytes().all(|b| b.is_ascii_digit())).any(|e| {
        let comm = fs::read_to_string(e.path().join("comm")).unwrap_or_default();
        if comm.trim() == "titan-call-audi" {
            return fs::read(e.path().join("cmdline")).is_ok_and(|c| c.split(|&b| b == 0).any(|a| a == b"run"));
        }
        if comm.trim() == "pw-cat" {
            let c = fs::read(e.path().join("cmdline")).unwrap_or_default();
            return c.split(|&b| b == 0).any(|a| a == b"Phone");
        }
        false
    })
}

// ---- the jack ----

fn open_jack() -> Option<fs::File> {
    for e in fs::read_dir("/sys/class/input").ok()?.flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        if !n.starts_with("event") {
            continue;
        }
        if fs::read_to_string(e.path().join("device/name")).is_ok_and(|s| s.trim() == JACK_NAME) {
            let f = fs::File::open(format!("/dev/input/{n}")).ok()?;
            nonblock(f.as_raw_fd());
            return Some(f);
        }
    }
    None
}

fn plugged(bits: u64) -> bool {
    [SW_HEADPHONE_INSERT, SW_MICROPHONE_INSERT, SW_LINEOUT_INSERT].iter().any(|&b| bits >> b & 1 != 0)
}

fn jack_in(jack: &fs::File) -> bool {
    let mut bits = 0u64;
    unsafe { ioctl(jack.as_raw_fd(), EVIOCGSW8, &mut bits as *mut u64 as i64) };
    plugged(bits)
}

fn drain(f: &mut impl Read) -> (Vec<u8>, bool) {
    let mut out = vec![];
    let mut buf = [0u8; 4096];
    loop {
        match f.read(&mut buf) {
            Ok(0) => return (out, true), // the other end is gone
            Ok(n) => out.extend_from_slice(&buf[..n]),
            Err(_) => return (out, false),
        }
    }
}

/// The file names in a batch of inotify events
fn inotify_names(buf: &[u8]) -> Vec<String> {
    let mut out = vec![];
    let mut i = 0;
    while i + 16 <= buf.len() {
        let len = u32::from_ne_bytes(buf[i + 12..i + 16].try_into().unwrap()) as usize;
        let name = buf.get(i + 16..i + 16 + len).unwrap_or(&[]);
        out.push(String::from_utf8_lossy(name.split(|&b| b == 0).next().unwrap_or(&[])).to_string());
        i += 16 + len;
    }
    out
}

// ---- the headphones' sink ----

/// The values quoted: SPA JSON reads ':' as a separator, so an unquoted hw:CARD=... path came
/// out as "hw", which is DL1
fn sink_command() -> String {
    format!(
        "create-node adapter {{ factory.name=api.alsa.pcm.sink node.name={SINK} node.description=\"Headphones\" \
         media.class=Audio/Sink api.alsa.path=\"{SINK_DEVICE}\" api.alsa.disable-mmap=true api.alsa.period-size=1024 \
         api.alsa.period-num=4 audio.format=S16LE audio.rate=48000 audio.channels=2 audio.position=[FL,FR] \
         priority.driver=1500 priority.session=1500 device.icon-name=\"audio-headphones\" }}\n"
    )
}

/// A pw-cli holding the sink: its stdin open keeps it running, its stdout tells when it ends
struct Holder {
    child: Child,
    _stdin: ChildStdin,
}

fn start_sink() -> Option<Holder> {
    // the same settings as the speaker (50-titan-speaker.conf): no mmap, 16 KB out of SRAM
    let mut child = Command::new("pw-cli")
        .env("XDG_RUNTIME_DIR", PIPEWIRE_RUNTIME)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| log(&format!("pw-cli: {e}")))
        .ok()?;
    let mut stdin = child.stdin.take()?;
    nonblock(child.stdout.as_ref()?.as_raw_fd());
    if stdin.write_all(sink_command().as_bytes()).is_err() {
        let _ = child.kill();
        let _ = child.wait();
        return None;
    }
    Some(Holder { child, _stdin: stdin })
}

impl Drop for Holder {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn wait_for_sink() -> Option<Vec<(String, String)>> {
    let until = Instant::now() + SINK_WAIT;
    while Instant::now() < until {
        let sinks = short_sinks(&pactl_out(&["list", "short", "sinks"]).unwrap_or_default());
        if sinks.iter().any(|s| s.1 == SINK) {
            return Some(sinks);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    None
}

/// `pactl subscribe`: its lines say when PipeWire's default may have changed
fn subscribe() -> Option<Child> {
    let c = pactl_cmd(&["subscribe"]).stdout(Stdio::piped()).spawn().ok()?;
    nonblock(c.stdout.as_ref()?.as_raw_fd());
    Some(c)
}

// ---- the switch ----

struct Daemon {
    numids: Vec<u32>,
    plugged: bool,       // as switched (the jack may differ while a call holds it)
    holder: Option<Holder>,
    on_headphones: Option<bool>, // whose level the file holds, as applied here
    sink_set: Option<String>,
    own_until: Instant,
}

impl Daemon {
    fn routes(&mut self) {
        for (n, v) in if self.plugged { ROUTES_IN } else { ROUTES_OUT } {
            cset(n, v);
        }
        self.own_until = Instant::now() + OWN_CHANGES;
    }

    /// The headphones' level from the shared file
    fn headphone_level(&mut self) {
        let (pga, db) = headphone_gain(read_level(VOL_FILE).unwrap_or(HP_DEFAULT));
        cset(HP_CONTROL, &format!("{pga},{pga}"));
        let value = sink_value(db);
        if self.sink_set.as_deref() != Some(&value) && pactl(&["set-sink-volume", SINK, &value]) {
            self.sink_set = Some(value);
        }
    }

    /// The level file to the default output's
    fn follow_default(&mut self) {
        let default = pactl_out(&["get-default-sink"]).map(|s| s.trim().to_string());
        let hp = self.plugged && default.as_deref() == Some(SINK);
        if self.on_headphones == Some(hp) {
            return;
        }
        switch_levels(hp);
        if hp {
            self.headphone_level();
        } else {
            cset(AMP_CONTROL, &amp_value(read_level(VOL_FILE).unwrap_or(VOL_DEFAULT)).to_string());
        }
        self.on_headphones = Some(hp);
    }

    /// The sink up (again); `take_over`: the default, and the speaker's apps moved to it
    fn sink_up(&mut self, take_over: bool) {
        self.holder = start_sink();
        self.sink_set = None;
        let Some(sinks) = wait_for_sink() else {
            log("the headphones' sink didn't appear");
            return;
        };
        let (pga, db) = headphone_gain(read_level(if self.on_headphones == Some(true) { VOL_FILE } else { VOL_HEADPHONES }).unwrap_or(HP_DEFAULT));
        let value = sink_value(db);
        if pactl(&["set-sink-volume", SINK, &value]) {
            self.sink_set = Some(value);
        }
        if take_over {
            cset(HP_CONTROL, &format!("{pga},{pga}"));
            pactl(&["set-default-sink", SINK]);
            let inputs = pactl_out(&["list", "short", "sink-inputs"]).unwrap_or_default();
            for idx in speaker_streams(&sinks, &inputs) {
                pactl(&["move-sink-input", &idx, SINK]);
            }
        }
    }

    fn plug(&mut self, plugged: bool) {
        self.plugged = plugged;
        if plugged {
            log("plugged in");
            // the codec's gain before the route: never louder than the level for a moment
            let (pga, _) = headphone_gain(read_level(VOL_HEADPHONES).unwrap_or(HP_DEFAULT));
            cset(HP_CONTROL, &format!("{pga},{pga}"));
            // DL2 to the headphones first, then the apps over, then DL1 off the codec: the
            // sound stays where it was until it is on the headphones' sink
            for (n, v) in ROUTES_IN.iter().filter(|r| r.1 != "off") {
                cset(n, v);
            }
            self.sink_up(true);
            self.routes();
        } else {
            log("pulled out");
            let was_default = pactl_out(&["get-default-sink"]).is_some_and(|s| s.trim() == SINK);
            self.holder = None; // the sink goes, its apps fall back
            self.routes();
            if was_default {
                if let Some(s) = SPEAKER_SINKS.iter().find(|s| pactl(&["get-sink-volume", s])) {
                    pactl(&["set-default-sink", s]);
                }
            }
        }
        self.follow_default();
    }
}

fn main() {
    let Some(mut jack) = open_jack() else {
        log(&format!("no \"{JACK_NAME}\" input device"));
        std::process::exit(1);
    };
    // the card's control device, its route events
    let ctl_path = fs::read_link(format!("/proc/asound/{CARD}"))
        .ok()
        .and_then(|p| p.to_string_lossy().strip_prefix("card").map(|n| format!("/dev/snd/controlC{n}")));
    let mut ctl = ctl_path.and_then(|p| fs::File::open(p).ok());
    let names: Vec<&str> = ROUTES_IN.iter().map(|r| r.0).collect();
    let watched: Vec<u32> =
        Command::new("amixer").args(["-c", CARD, "controls"]).output().map(|o| numids(&String::from_utf8_lossy(&o.stdout), &names)).unwrap_or_default();
    if let Some(c) = &ctl {
        nonblock(c.as_raw_fd());
        let mut on: i32 = 1;
        unsafe { ioctl(c.as_raw_fd(), CTL_SUBSCRIBE, &mut on as *mut i32 as i64) };
    } else {
        log("no control device: routes changed by calls aren't put back");
    }
    let _ = fs::create_dir_all(LEVEL_DIR);
    let ino_fd = unsafe { inotify_init1(O_NONBLOCK) };
    let dir = std::ffi::CString::new(LEVEL_DIR).unwrap();
    unsafe { inotify_add_watch(ino_fd, dir.as_ptr(), IN_CLOSE_WRITE | IN_MOVED_TO) };
    // SAFETY: inotify_init1 returned a new descriptor that nothing else owns
    let mut ino = unsafe { fs::File::from_raw_fd(ino_fd) };

    let mut d = Daemon { numids: watched, plugged: false, holder: None, on_headphones: None, sink_set: None, own_until: Instant::now() };
    let mut is_in = jack_in(&jack);
    log(&format!("started, jack {}", if is_in { "in" } else { "out" }));
    let mut sub = subscribe();
    let mut jack_at: Option<Instant> = None;
    let mut check_at: Option<Instant> = Some(Instant::now()); // the first one sets everything
    let mut default_at: Option<Instant> = None;
    let mut restart_at: Option<Instant> = None;
    let mut first = true;

    loop {
        let now = Instant::now();
        let next = [jack_at, check_at, default_at, restart_at].into_iter().flatten().min();
        let ms = next.map_or(-1, |t| t.saturating_duration_since(now).as_millis().min(60_000) as i32);
        let mut fds = vec![
            PollFd { fd: jack.as_raw_fd(), events: POLLIN, revents: 0 },
            PollFd { fd: ino_fd, events: POLLIN, revents: 0 },
            PollFd { fd: ctl.as_ref().map_or(-1, |c| c.as_raw_fd()), events: POLLIN, revents: 0 },
            PollFd { fd: sub.as_ref().and_then(|s| s.stdout.as_ref()).map_or(-1, |o| o.as_raw_fd()), events: POLLIN, revents: 0 },
            PollFd {
                fd: d.holder.as_ref().and_then(|h| h.child.stdout.as_ref()).map_or(-1, |o| o.as_raw_fd()),
                events: POLLIN,
                revents: 0,
            },
        ];
        unsafe { poll(fds.as_mut_ptr(), fds.len() as u64, ms) };
        let now = Instant::now();

        if fds[0].revents != 0 {
            drain(&mut jack);
            jack_at = Some(now + JACK_SETTLE);
        }
        if fds[1].revents != 0 && inotify_names(&drain(&mut ino).0).iter().any(|n| n == VOL_FILE) && d.on_headphones == Some(true) {
            d.headphone_level();
        }
        if fds[2].revents != 0 {
            let ev = ctl.as_mut().map(|c| drain(c).0).unwrap_or_default();
            let route = ev.chunks(CTL_EVENT_SIZE).any(|e| e.len() >= 12 && d.numids.contains(&u32::from_ne_bytes(e[8..12].try_into().unwrap())));
            if route && now >= d.own_until {
                check_at = Some(now + ROUTE_SETTLE);
            }
        }
        if fds[3].revents != 0 {
            let (lines, gone) = sub.as_mut().and_then(|s| s.stdout.as_mut()).map(drain).unwrap_or_default();
            if String::from_utf8_lossy(&lines).contains("on server") {
                default_at = Some(now + DEFAULT_SETTLE);
            }
            if gone {
                sub = None;
                restart_at = Some(now + RESTART);
            }
        }
        if fds[4].revents != 0 {
            let gone = d.holder.as_mut().and_then(|h| h.child.stdout.as_mut()).map(drain).is_some_and(|r| r.1);
            if gone {
                log("the headphones' sink went (PipeWire restarted?)");
                d.holder = None;
                restart_at = Some(now + RESTART);
            }
        }
        if jack_at.is_some_and(|t| now >= t) {
            jack_at = None;
            let was = is_in;
            is_in = jack_in(&jack);
            if is_in != was {
                check_at = Some(now);
            }
        }
        if restart_at.is_some_and(|t| now >= t) {
            restart_at = None;
            if sub.is_none() {
                sub = subscribe();
            }
            if d.plugged && d.holder.is_none() {
                d.sink_up(false); // WirePlumber keeps it the default if it was
            }
            default_at = Some(now);
        }
        if default_at.is_some_and(|t| now >= t) {
            default_at = None;
            d.follow_default();
        }
        if check_at.is_some_and(|t| now >= t) {
            check_at = None;
            if in_call() {
                check_at = Some(now + IN_CALL_RECHECK);
                continue;
            }
            if first {
                first = false;
                d.plug(is_in);
                // the speaker's sinks at 100 %: its level is the amp's. An earlier version of
                // this daemon kept the headphones' fine steps there; only now, with nothing on
                // them reaching the headphones, is that safe to undo.
                for s in SPEAKER_SINKS {
                    pactl(&["set-sink-volume", s, "100%"]);
                }
            } else if is_in != d.plugged {
                d.plug(is_in);
            } else {
                d.routes(); // a call or someone else changed them: put them back
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gains() {
        // full scale -8 dB: all codec
        assert_eq!(headphone_gain(VOL_MAX), (2, Some(0.0)));
        // the default, about -30 dB: codec -10, the sink the rest
        let (pga, db) = headphone_gain(HP_DEFAULT);
        assert_eq!(pga, 0);
        assert!((db.unwrap() - (-8.0 - 42.0 * (224 - 107) as f64 / 224.0 + 10.0)).abs() < 1e-9);
        // -8.75 dB: the codec at -8, the sink -0.75
        let (pga, db) = headphone_gain(220);
        assert_eq!(pga, 2);
        assert!((db.unwrap() + 0.75).abs() < 1e-9);
        assert_eq!(headphone_gain(0), (0, None));
        assert_eq!(sink_value(None), "0");
        assert_eq!(sink_value(Some(-20.0)), "0.100000");
        assert!(!sink_value(Some(-31.0)).starts_with('-'));
    }

    #[test]
    fn amp_matches_powerkey() {
        assert_eq!(amp_value(VOL_MAX), 224);
        assert_eq!(amp_value(0), 0);
        assert_eq!(amp_value(160), 200);
    }

    #[test]
    fn controls() {
        let l = "numid=54,iface=MIXER,name='I2S3_CH1 DL1_CH1'\nnumid=112,iface=MIXER,name='HPL Mux'\nnumid=92,iface=MIXER,name='Headphone Volume'\nnumid=73,iface=MIXER,name='ADDA_DL_CH1 DL2_CH1'\n";
        let names: Vec<&str> = ROUTES_IN.iter().map(|r| r.0).collect();
        assert_eq!(numids(l, &names), vec![54, 112, 73]);
        // in and out set the same controls
        let mut a: Vec<&str> = ROUTES_IN.iter().map(|r| r.0).collect();
        let mut b: Vec<&str> = ROUTES_OUT.iter().map(|r| r.0).collect();
        a.sort();
        b.sort();
        assert_eq!(a, b);
    }

    #[test]
    fn streams() {
        let sinks = short_sinks("31\ttitan-speaker\tPipeWire\ts16le 2ch 48000Hz\tRUNNING\n37\ttitan-speaker-limited\tPipeWire\tfloat32le 2ch 48000Hz\tRUNNING\n50\ttitan-headphones\tPipeWire\ts16le\tIDLE\n");
        assert_eq!(sinks[2], ("50".into(), "titan-headphones".into()));
        // the limiter's own stream (on titan-speaker) stays
        let inputs = "38\t31\t-\tPipeWire\tfloat32le 2ch 48000Hz\n709\t37\t708\tPipeWire\tfloat32le 2ch 48000Hz\n800\t50\t799\tPipeWire\ts16le\n";
        assert_eq!(speaker_streams(&sinks, inputs), vec!["709".to_string()]);
        // no limiter: the speaker itself
        let sinks = short_sinks("31\ttitan-speaker\tPipeWire\n");
        assert_eq!(speaker_streams(&sinks, "5\t31\t4\n"), vec!["5".to_string()]);
        assert!(sink_command().contains("node.name=titan-headphones") && sink_command().ends_with("}\n"));
        assert!(sink_command().contains("api.alsa.path=\"hw:CARD=titanpocket,DEV=1\""));
    }

    #[test]
    fn jack() {
        assert!(plugged(1 << 2) && plugged(1 << 4) && plugged(1 << 6) && !plugged(1 << 3));
        let mut b = vec![];
        for name in ["volume.tmp", "volume"] {
            let mut n = name.as_bytes().to_vec();
            n.resize(16, 0);
            b.extend_from_slice(&1i32.to_ne_bytes());
            b.extend_from_slice(&IN_MOVED_TO.to_ne_bytes());
            b.extend_from_slice(&0u32.to_ne_bytes());
            b.extend_from_slice(&(n.len() as u32).to_ne_bytes());
            b.extend_from_slice(&n);
        }
        assert_eq!(inotify_names(&b), vec!["volume.tmp".to_string(), "volume".into()]);
    }
}
