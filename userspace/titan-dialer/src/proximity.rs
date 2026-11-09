//! The proximity sensor and the screen during a call.
//!
//! Proximity sits behind the SCP sensor hub; on Linux 6.12 it's the IIO device `scp-proximity`
//! of our driver (docs/hardware.md, docs/mainline.md): 0 near, 1 far, reported only on change.
//! Each reader claims its own buffer (IIO_BUFFER_GET_FD_IOCTL); records are an s32 value and an
//! s64 timestamp. The state before the first change comes from `in_proximity_raw` (a direct
//! read, only possible while no buffer runs, so it's read before ours starts).
//!
//! Near during a call on the earpiece, the panel goes dark the way titan-powerkey's screen off
//! does on 6.12 (backlight level 0, the MT6370 boost off) but without locking or suspending, and
//! the app swallows touches. Far restores it (the boost's level, then titan-brightness apply).
//! A guard shell restores the screen if this process dies while it's dark.
use gtk::glib;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

const IIO: &str = "/sys/bus/iio/devices";
const IIO_BUFFER_GET_FD_IOCTL: libc::c_ulong = 0xc004_6991; // _IOWR('i', 0x91, int)
const BACKLIGHT: &str = "/sys/class/backlight/backlight/brightness";
const BOOST: &str = "/sys/class/backlight/mt6370-backlight/brightness";
const BRIGHTNESS: &str = "/usr/local/bin/titan-brightness";
/// exists while the call has the screen dark (for other programs to see)
const FLAG: &str = "/run/titan-proximity.off";
/// titan-powerkey's screen off: then far doesn't light the panel (the power key has it)
const POWERKEY_OFF: &str = "/run/titan-screen.off";

fn device() -> Option<PathBuf> {
    for e in fs::read_dir(IIO).ok()?.flatten() {
        if fs::read_to_string(e.path().join("name")).map(|n| n.trim() == "scp-proximity").unwrap_or(false) {
            return Some(e.path());
        }
    }
    None
}

pub struct Proximity {
    buf: Option<PathBuf>,
    file: Option<File>,
    source: Option<glib::SourceId>,
}

impl Proximity {
    /// start reading; `on_change(near)` on the main loop, first with the current state
    pub fn start(on_change: impl Fn(bool) + 'static) -> Option<Proximity> {
        let dev = device()?;
        let near0 = fs::read_to_string(dev.join("in_proximity_raw")).ok().and_then(|v| v.trim().parse::<i32>().ok()).map(|v| v == 0).unwrap_or(false);
        let node = format!("/dev/{}", dev.file_name()?.to_string_lossy());
        let devf = OpenOptions::new().read(true).open(&node).ok()?;
        let (mut file, mut buf) = (None, None);
        for i in 0..8 {
            let mut arg: libc::c_int = i;
            if unsafe { libc::ioctl(devf.as_raw_fd(), IIO_BUFFER_GET_FD_IOCTL, &mut arg) } == 0 {
                file = Some(unsafe { File::from_raw_fd(arg) });
                buf = Some(dev.join(format!("buffer{i}")));
                break;
            }
        }
        // the device node is exclusive while open: close it so others can claim their buffer
        drop(devf);
        let (file, buf) = (file?, buf?);
        unsafe {
            let fl = libc::fcntl(file.as_raw_fd(), libc::F_GETFL);
            libc::fcntl(file.as_raw_fd(), libc::F_SETFL, fl | libc::O_NONBLOCK);
        }
        if let Ok(rd) = fs::read_dir(&buf) {
            for e in rd.flatten() {
                if e.file_name().to_string_lossy().ends_with("_en") {
                    let _ = fs::write(e.path(), "1");
                }
            }
        }
        let _ = fs::write(buf.join("length"), "64");
        if fs::write(buf.join("enable"), "1").is_err() {
            return None;
        }
        on_change(near0);
        let mut reader = file.try_clone().ok()?;
        let source = glib::source::unix_fd_add_local(file.as_raw_fd(), glib::IOCondition::IN, move |_, _| {
            let mut b = [0u8; 16 * 16];
            let mut last = None;
            while let Ok(n) = reader.read(&mut b) {
                if n < 16 {
                    break;
                }
                for rec in b[..n].chunks_exact(16) {
                    last = Some(i32::from_ne_bytes([rec[0], rec[1], rec[2], rec[3]]));
                }
            }
            if let Some(v) = last {
                on_change(v == 0);
            }
            glib::ControlFlow::Continue
        });
        Some(Proximity { buf: Some(buf), file: Some(file), source: Some(source) })
    }
}

impl Drop for Proximity {
    fn drop(&mut self) {
        if let Some(s) = self.source.take() {
            s.remove();
        }
        if let Some(b) = self.buf.take() {
            let _ = fs::write(b.join("enable"), "0");
        }
        self.file = None;
    }
}

/// the panel dark while the phone is at the ear
pub struct Screen {
    boost: Option<String>,
    guard: Option<Child>,
}

impl Screen {
    pub fn new() -> Screen {
        Screen { boost: None, guard: None }
    }

    pub fn dark(&self) -> bool {
        self.guard.is_some()
    }

    pub fn off(&mut self) {
        if self.dark() || !Path::new(BACKLIGHT).exists() {
            return;
        }
        let level = fs::read_to_string(BOOST).map(|l| l.trim().to_string()).unwrap_or_default();
        let level = if level.is_empty() || level == "0" { "1023".to_string() } else { level };
        // restores the screen when its stdin closes: on(), or this process dying
        self.guard = Command::new("sh")
            .arg("-c")
            .arg(format!(
                "cat > /dev/null; if [ -e {FLAG} ] && [ ! -e {POWERKEY_OFF} ]; then echo {level} > {BOOST}; {BRIGHTNESS} apply; fi; rm -f {FLAG}"
            ))
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .ok();
        if self.guard.is_none() {
            return;
        }
        let _ = File::create(FLAG);
        self.boost = Some(level);
        let _ = fs::write(BACKLIGHT, "0");
        let _ = fs::write(BOOST, "0");
    }

    pub fn on(&mut self) {
        if let Some(mut g) = self.guard.take() {
            // the guard restores the boost level and the brightness (it holds the flag check)
            if let Some(mut s) = g.stdin.take() {
                let _ = s.flush();
            }
            let _ = g.wait();
        }
        self.boost = None;
    }
}

impl Drop for Screen {
    fn drop(&mut self) {
        self.on();
    }
}
