//! titan-gps-probe: drive the MT6631's GNSS receiver without libmnl, from a
//! replay script, and log what the chip answers (docs/gnss.md, "Stage 1").
//!
//!   titan-gps-probe SCRIPT [--seconds N] [--libmnl PATH] [--log FILE] [--quiet]
//!
//! SCRIPT comes from tools/gnss/stpgps-script.py: `<ms> <type> <payload hex>`
//! per host frame, and `<ms> PATCH` where the firmware patch goes (taken from
//! the phone's libmnl.so, see titan_gps::patch). Each frame is written at its
//! time; the chip's frames are decoded as they come, logged (`<ms> <type>
//! <seq> <payload hex>`, XOR undone) and counted. At the end: how many of each
//! type arrived, for comparing with a capture of libmnl's own session
//! (stpgps-decode.py).
//!
//! Location must be off (titan-location off): only one process may hold
//! /dev/stpgps, and opening it is what powers the receiver.

use std::collections::BTreeMap;
use std::fs::{File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::process::ExitCode;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use titan_gps::frame::{self, Decoder};
use titan_gps::patch::{self, Patch};

const DEVICE: &str = "/dev/stpgps";

enum Step {
    /// frames written together in one write(), as libmnl did
    Write(Vec<(u8, Vec<u8>)>),
    Patch,
}

fn parse_hex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

fn load_script(path: &str) -> io::Result<Vec<(u64, Step)>> {
    let bad = |n: usize| io::Error::new(io::ErrorKind::InvalidData, format!("{path}:{n}: bad line"));
    let mut steps = Vec::new();
    for (n, line) in BufReader::new(File::open(path)?).lines().enumerate() {
        let line = line?;
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut f = line.split_whitespace();
        let time = f.next().ok_or_else(|| bad(n + 1))?;
        let (more, time) = match time.strip_prefix('+') {
            Some(t) => (true, t),
            None => (false, time),
        };
        let ms: u64 = time.parse().map_err(|_| bad(n + 1))?;
        match f.next().ok_or_else(|| bad(n + 1))? {
            "PATCH" => steps.push((ms, Step::Patch)),
            kind => {
                let frame = (u8::from_str_radix(kind, 16).map_err(|_| bad(n + 1))?,
                             parse_hex(f.next().unwrap_or("")).ok_or_else(|| bad(n + 1))?);
                match steps.last_mut() {
                    Some((_, Step::Write(frames))) if more => frames.push(frame),
                    _ => steps.push((ms, Step::Write(vec![frame]))),
                }
            }
        }
    }
    Ok(steps)
}

/// one write(), as libmnl does (a frame, or a moment's frames). With no
/// window space on the STP
/// link the driver takes nothing and returns ENOSPC ("native process should
/// not call GPS_write with no delay at all", stp_chrdev_gps.c): wait a
/// little and try again (libmnl's firmware chunks go about 11 ms apart).
fn write_frame(dev: &mut File, f: &[u8]) -> io::Result<()> {
    let give_up = Instant::now() + Duration::from_secs(2);
    let mut off = 0;
    while off < f.len() {
        match dev.write(&f[off..]) {
            Ok(n) => off += n,
            Err(e) if e.raw_os_error() == Some(28) && Instant::now() < give_up => {
                thread::sleep(Duration::from_millis(3));
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// the chip's answer to the session header, and to each firmware chunk
const HEADER_ACK: u8 = 0x31;
const CHUNK_ACK: u8 = 0x32;
const ACK_WAIT: Duration = Duration::from_millis(500);

type Rx = mpsc::Receiver<(Duration, frame::Frame, usize, usize)>;

/// the chip's frames as they come: logged and counted
struct Seen {
    rx: Rx,
    quiet: bool,
    counts: BTreeMap<u8, usize>,
    stray: usize,
    bad: usize,
}

impl Seen {
    /// take frames until `until`, or (with `want`) until one of that type
    /// arrives: true if it did
    fn until(&mut self, log: &mut Box<dyn Write + Send>, until: Instant, want: Option<u8>) -> bool {
        loop {
            let now = Instant::now();
            if now >= until {
                return false;
            }
            match self.rx.recv_timeout(until - now) {
                Ok((t, f, stray, bad)) => {
                    *self.counts.entry(f.kind).or_default() += 1;
                    self.stray = stray;
                    self.bad = bad;
                    if !self.quiet {
                        let hex: String = f.payload.iter().map(|b| format!("{b:02x}")).collect();
                        let _ = writeln!(log, "{} {:02x} {:x} {}", t.as_millis(), f.kind, f.seq, hex);
                    }
                    if want == Some(f.kind) {
                        return true;
                    }
                }
                Err(mpsc::RecvTimeoutError::Timeout) => return false,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    thread::sleep(until.saturating_duration_since(Instant::now()));
                    return false;
                }
            }
        }
    }
}

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned()
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let Some(script_path) = args.get(1).filter(|a| !a.starts_with("--")) else {
        eprintln!("usage: titan-gps-probe SCRIPT [--seconds N] [--libmnl PATH] [--log FILE] [--quiet]");
        return ExitCode::from(2);
    };
    let seconds: f64 = arg(&args, "--seconds").and_then(|s| s.parse().ok()).unwrap_or(30.0);
    let libmnl = arg(&args, "--libmnl").unwrap_or_else(|| patch::LIBMNL.to_string());
    let quiet = args.iter().any(|a| a == "--quiet");

    let script = match load_script(script_path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("titan-gps-probe: {e}");
            return ExitCode::FAILURE;
        }
    };
    let patch = match Patch::load(&libmnl) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("titan-gps-probe: {e}");
            return ExitCode::FAILURE;
        }
    };
    let chunks = patch.chunks();
    eprintln!("titan-gps-probe: patch {} bytes in {} chunks, {} script steps",
              patch.image.len(), chunks.len(), script.len());

    let mut log: Box<dyn Write + Send> = match arg(&args, "--log") {
        Some(p) => match File::create(&p) {
            Ok(f) => Box::new(io::BufWriter::new(f)),
            Err(e) => {
                eprintln!("titan-gps-probe: {p}: {e}");
                return ExitCode::FAILURE;
            }
        },
        None => Box::new(io::stdout()),
    };

    let mut dev = match OpenOptions::new().read(true).write(true).open(DEVICE) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("titan-gps-probe: {DEVICE}: {e} (is Location off?)");
            return ExitCode::FAILURE;
        }
    };
    let start = Instant::now();

    // the chip's frames, read on a thread (reads block)
    let (tx, rx) = mpsc::channel();
    let mut reader = dev.try_clone().expect("dup /dev/stpgps");
    thread::spawn(move || {
        let mut dec = Decoder::new();
        let mut buf = [0u8; 4096];
        loop {
            match reader.read(&mut buf) {
                Ok(0) => thread::sleep(Duration::from_millis(5)),
                Ok(n) => {
                    for f in dec.push(&buf[..n], true) {
                        if tx.send((start.elapsed(), f, dec.stray, dec.bad)).is_err() {
                            return;
                        }
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(e) => {
                    eprintln!("titan-gps-probe: read: {e}");
                    return;
                }
            }
        }
    });

    let mut seen = Seen {
        rx,
        quiet,
        counts: BTreeMap::new(),
        stray: 0,
        bad: 0,
    };
    let mut written = 0usize;
    let end = start + Duration::from_secs_f64(seconds);
    for (ms, step) in &script {
        let at = start + Duration::from_millis(*ms);
        if at >= end {
            break;
        }
        seen.until(&mut log, at, None);
        match step {
            Step::Write(frames) => {
                let bytes: Vec<u8> = frames.iter().flat_map(|(k, p)| frame::encode(*k, p)).collect();
                if let Err(e) = write_frame(&mut dev, &bytes) {
                    eprintln!("titan-gps-probe: write: {e}");
                    return ExitCode::FAILURE;
                }
                written += frames.len();
            }
            Step::Patch => {
                // as libmnl: the header's answer (0x31) first, then each
                // chunk only once the chip has acknowledged the last (0x32);
                // a chunk sent on top of one still being taken in stalls
                // the link (its window fills, writes get ENOSPC)
                let deadline = Instant::now() + ACK_WAIT;
                if seen.counts.get(&HEADER_ACK).is_none()
                    && !seen.until(&mut log, deadline, Some(HEADER_ACK))
                {
                    eprintln!("titan-gps-probe: no answer to the session header");
                }
                for (i, c) in chunks.iter().enumerate() {
                    if let Err(e) = write_frame(&mut dev, &frame::encode(patch::FRAME, c)) {
                        eprintln!("titan-gps-probe: write: {e}");
                        return ExitCode::FAILURE;
                    }
                    written += 1;
                    if !seen.until(&mut log, Instant::now() + ACK_WAIT, Some(CHUNK_ACK)) {
                        eprintln!("titan-gps-probe: chunk {} of {} not acknowledged", i + 1,
                                  chunks.len());
                        break;
                    }
                }
            }
        }
    }
    seen.until(&mut log, end, None);
    let _ = log.flush();
    let Seen { counts, stray, bad, .. } = seen;

    eprintln!("titan-gps-probe: {:.1} s, {} frames written; from the chip:",
              start.elapsed().as_secs_f64(), written);
    for (kind, n) in &counts {
        eprintln!("  type 0x{kind:02x}  {n}");
    }
    eprintln!("  bytes outside frames {stray}, bad sums {bad}");
    // closing /dev/stpgps powers the receiver off
    ExitCode::SUCCESS
}
