//! The rear flash: two LEDs (white and warm) on the MT6370 PMIC, as LED class flash devices
//! on Linux 6.12. `brightness` is the torch (12.5 mA steps from 25 mA); `flash_brightness`
//! (µA), `flash_timeout` and `flash_strobe` the strobe, which the PMIC ends by itself. The
//! torch has no timeout, so a small shell holds it: it switches the LEDs off when this process
//! dies (its stdin closes) or the timeout runs out.
use std::io::Write;
use std::path::Path;
use std::process::{Child, Command, Stdio};

const WHITE: &str = "/sys/class/leds/white:flash";
const WARM: &str = "/sys/class/leds/yellow:flash";
/// a level is 25 mA x (level + 1) per LED
pub const TORCH_LEVEL: u32 = 6; // 175 mA
pub const TORCH_MAX: u32 = 15; // 400 mA, the torch maximum
#[allow(dead_code)]
pub const PHOTO_LEVEL: u32 = 39; // 1000 mA strobe
#[allow(dead_code)] // the strobe waits for the pre-flash metering port
pub const PULSE_MS: u32 = 1184; // the strobe's hardware timeout (device tree)

/// white and warm share of a level's current, for the tones Neutral, Warm, Cool
const TONES: [(f64, f64); 3] = [(1.0, 1.0), (0.25, 1.0), (1.0, 0.25)];

#[allow(dead_code)]
#[derive(PartialEq, Clone, Copy, Debug)]
enum Lit {
    Off,
    Torch,
    Strobe,
}

pub struct Flash {
    pub tone: usize,
    lit: Lit,
    guard: Option<Child>,
}

fn write(led: &str, attr: &str, v: impl ToString) -> std::io::Result<()> {
    std::fs::write(Path::new(led).join(attr), v.to_string())
}

impl Flash {
    pub fn new(tone: usize) -> Flash {
        Flash { tone, lit: Lit::Off, guard: None }
    }

    pub fn present() -> bool {
        Path::new(WHITE).exists() && Path::new(WARM).exists()
    }

    pub fn lit(&self) -> bool {
        self.lit != Lit::Off
    }

    fn currents(&self, level: u32) -> [(&'static str, f64); 2] {
        let (w, y) = TONES[self.tone.min(2)];
        let ma = 25.0 * (level + 1) as f64;
        [(WHITE, ma * w), (WARM, ma * y)]
    }

    /// the torch at `level`, off after `timeout_ms` (0: until off())
    pub fn torch(&mut self, level: u32, timeout_ms: u32) -> bool {
        self.off();
        let wait = if timeout_ms > 0 { format!("timeout {:.3} cat", timeout_ms as f64 / 1000.0) } else { "cat".into() };
        self.guard = Command::new("sh")
            .arg("-c")
            .arg(format!("{wait} > /dev/null; echo 0 > '{WHITE}/brightness'; echo 0 > '{WARM}/brightness'"))
            .stdin(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok();
        self.lit = Lit::Torch;
        for (led, ma) in self.currents(level.min(TORCH_MAX)) {
            let b = (((ma - 25.0) / 12.5).round() as i64 + 1).max(1);
            if let Err(e) = write(led, "brightness", b) {
                eprintln!("flash: {e}");
                self.off();
                return false;
            }
        }
        true
    }

    /// the flash proper (for the pre-flash metering, not ported yet); dim levels (under the strobe's 50 mA floor) stay on the torch
    #[allow(dead_code)]
    pub fn pulse(&mut self, level: u32) -> bool {
        let cur = self.currents(level);
        if cur.iter().any(|c| c.1 < 50.0) {
            return self.torch(level, PULSE_MS);
        }
        self.off();
        self.lit = Lit::Strobe;
        for (led, ma) in cur {
            if write(led, "flash_timeout", PULSE_MS * 1000).and(write(led, "flash_brightness", (ma * 1000.0) as u32)).is_err() {
                self.off();
                return false;
            }
        }
        for attempt in 0..10 {
            // the torch's off reaches the driver a moment later: EBUSY until then
            match cur.iter().try_for_each(|(led, _)| write(led, "flash_strobe", 1)) {
                Ok(()) => return true,
                Err(e) if e.raw_os_error() == Some(libc::EBUSY) && attempt < 9 => {
                    std::thread::sleep(std::time::Duration::from_millis(10))
                }
                Err(e) => {
                    eprintln!("flash: {e}");
                    self.off();
                    return false;
                }
            }
        }
        false
    }

    pub fn off(&mut self) {
        match self.lit {
            Lit::Strobe => {
                for led in [WHITE, WARM] {
                    let _ = write(led, "flash_strobe", 0);
                }
            }
            Lit::Torch => {
                for led in [WHITE, WARM] {
                    let _ = write(led, "brightness", 0);
                }
            }
            Lit::Off => {}
        }
        if let Some(mut g) = self.guard.take() {
            drop(g.stdin.take().map(|mut s| s.flush()));
            let _ = g.wait();
        }
        self.lit = Lit::Off;
    }
}

impl Drop for Flash {
    fn drop(&mut self) {
        self.off();
    }
}
