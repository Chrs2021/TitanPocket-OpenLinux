//! Flash exposure: pre-flash metering on the hardware AF statistics (rear camera), as the
//! Python app did it (docs/camera.md, "Flash exposure").
//!
//! The preview keeps running at a short manual exposure (gain 1x) with the flash on at the
//! torch maximum; the AF statistics' block pixel sums (word 3, before any processing) are
//! linear, raw / 8 a pixel, black about 8, clipping at about 119. While the brightest blocks
//! clip the exposure drops 8x, while the scene is too dark it rises 8x; at the shortest
//! exposure the pre-flash itself dims. From the lit levels and the ambient ones read just
//! before (at the preview's exposure x gain), the flash's share is separated from the
//! ambient light, and the photo gets the exposure x gain (and, for a close subject, the
//! flash level) that puts the centre-weighted mean at METER_TARGET with the 97th-percentile
//! block at most METER_HIGHLIGHT.
use std::path::Path;

/// pixels a statistics block sums (the 2336x1752 mode: 16 x 12 blocks)
const STAT_NPIX: f64 = 146.0 * 146.0;
const STAT_BLACK: f64 = 8.0;
/// a block above this is (nearly) clipped
const STAT_CLIP: f64 = 110.0;
/// centre-weighted mean above black (a photo mean of about 80)...
const METER_TARGET: f64 = 32.0;
/// ...with the 97th-percentile block at most this (about 240 in the photo)
const METER_HIGHLIGHT: f64 = 66.0;
pub const START_US: f64 = 400.0;
const MIN_US: f64 = 40.0;
const MAX_US: f64 = 25600.0;
/// frames after the sensor has the new exposure
const SETTLE: u32 = 3;
pub const TIMEOUT_MS: u128 = 2500;
/// the sensor's shortest exposure, in lines
pub const MIN_LINES: f64 = 4.0;
/// the pre-flash: the torch maximum
pub const PRE_LEVEL: u32 = crate::flash::TORCH_MAX;

/// one read of the driver's af_stats: the ISP's frame counter and each block's signal level
/// above black (rows of `nx`)
pub struct Stats {
    pub frame: u32,
    pub nx: usize,
    pub levels: Vec<f64>,
}

/// /sys/.../rear/af_stats: 8 u32 of header (magic "TAF1", frame, running, nx, ny, block
/// bytes 40, row stride, SGG1 gain), then the AFO rows; word 3 of a block is its pixel sum
pub fn read_stats(path: &Path) -> Option<Stats> {
    let d = std::fs::read(path).ok()?;
    let w = |i: usize| -> Option<u32> { d.get(4 * i..4 * i + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]])) };
    let (magic, frame, running, nx, ny, blk, stride) = (w(0)?, w(1)?, w(2)?, w(3)? as usize, w(4)? as usize, w(5)?, w(6)? as usize);
    if magic != 0x3146_4154 || blk != 40 || running == 0 || nx == 0 || ny == 0 || d.len() < 32 + stride * ny {
        return None;
    }
    let mut levels = Vec::with_capacity(nx * ny);
    for y in 0..ny {
        for x in 0..nx {
            let o = 32 + y * stride + x * 40 + 12;
            let sum = u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as f64;
            levels.push((sum / STAT_NPIX - STAT_BLACK).max(0.0));
        }
    }
    Some(Stats { frame, nx, levels })
}

fn percentile(v: &[f64], p: f64) -> f64 {
    if v.is_empty() {
        return 0.0;
    }
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    // linear interpolation, as numpy's default
    let r = p / 100.0 * (s.len() - 1) as f64;
    let (i, f) = (r.floor() as usize, r.fract());
    if i + 1 < s.len() { s[i] * (1.0 - f) + s[i + 1] * f } else { s[i] }
}

/// what the metering wants next
#[derive(Debug, PartialEq)]
pub enum Step {
    /// keep waiting for frames
    Wait,
    /// set this exposure (µs, gain 1x) on the preview
    Expose(f64),
    /// light the pre-flash at this level (the same exposure)
    Dim(u32),
    /// done: the photo's flash level and exposure (lines, gain), or None (no measurement:
    /// the full flash and auto exposure)
    Done(u32, Option<(f64, f64)>),
}

pub struct Meter {
    /// the ambient levels per line x gain (1x), from just before the pre-flash
    ambient: Vec<f64>,
    weights: Vec<f64>,
    pub us: f64,
    pub level: u32,
    /// the frame counter when the sensor had the exposure, None while waiting for it
    frame: Option<u32>,
    t0: std::time::Instant,
    photo_level: u32,
    line_us: f64,
    max_lines: f64,
    max_gain: f64,
    pub log: String,
}

impl Meter {
    /// `amb`: the ambient statistics at the preview's `lines` x `gain` (1.0 = 1x)
    pub fn new(amb: &Stats, lines: f64, gain: f64, line_us: f64, photo_level: u32, max_lines: f64, max_gain: f64) -> Meter {
        let (nx, ny) = (amb.nx, amb.levels.len() / amb.nx);
        let weights = (0..ny)
            .flat_map(|y| (0..nx).map(move |x| (x, y)))
            .map(|(x, y)| {
                let c = (x as f64 - (nx as f64 - 1.0) / 2.0).abs() <= nx as f64 / 4.0
                    && (y as f64 - (ny as f64 - 1.0) / 2.0).abs() <= ny as f64 / 4.0;
                if c { 3.0 } else { 1.0 }
            })
            .collect();
        let k = (lines * gain).max(1.0);
        Meter {
            ambient: amb.levels.iter().map(|v| v / k).collect(),
            weights,
            us: START_US,
            level: PRE_LEVEL,
            frame: None,
            t0: std::time::Instant::now(),
            photo_level,
            line_us,
            max_lines,
            max_gain,
            log: "no measurement".into(),
        }
    }

    pub fn lines(&self) -> f64 {
        (self.us / self.line_us).round()
    }

    /// the exposure was just asked for: wait until the sensor has it
    pub fn exposing(&mut self, us: f64) {
        self.us = us;
        self.frame = None;
    }

    /// a tick: the latest statistics, and whether the sensor shows the asked exposure
    pub fn tick(&mut self, st: Option<&Stats>, sensor_has_it: bool) -> Step {
        let timed_out = self.t0.elapsed().as_millis() > TIMEOUT_MS;
        let st = match (st, self.frame) {
            (None, _) if !timed_out => return Step::Wait,
            (Some(s), None) if !timed_out => {
                if sensor_has_it {
                    self.frame = Some(s.frame);
                }
                return Step::Wait;
            }
            (Some(s), Some(f)) if !timed_out && (s.frame.wrapping_sub(f) & 0xff) < SETTLE => return Step::Wait,
            (Some(s), Some(_)) => Some(s),
            _ => None, // timed out without a valid measurement
        };
        if let Some(s) = st {
            let top = percentile(&s.levels, 97.0);
            if !timed_out && top > STAT_CLIP - STAT_BLACK {
                if self.us > MIN_US {
                    return Step::Expose((self.us / 8.0).max(MIN_US));
                }
                if self.level > 0 {
                    // clipping at the shortest exposure (a very close subject): pre-flash
                    // about 4x dimmer (15 -> 3 -> 0), the same exposure
                    self.level = ((self.level + 1) / 4).saturating_sub(1);
                    self.frame = Some(s.frame);
                    return Step::Dim(self.level);
                }
            }
            if !timed_out && top < 4.0 && self.us < MAX_US {
                return Step::Expose((self.us * 8.0).min(MAX_US));
            }
        }
        let (level, exposure) = self.result(st.map(|s| s.levels.as_slice()));
        Step::Done(level, exposure)
    }

    /// the photo's flash level and (lines, gain) from the pre-flash levels
    fn result(&mut self, lv: Option<&[f64]>) -> (u32, Option<(f64, f64)>) {
        let Some(lv) = lv else { return (self.photo_level, None) };
        let lines_now = (self.us / self.line_us).max(1.0);
        let full = (self.photo_level + 1) as f64 / (self.level + 1) as f64;
        // per line x gain: the flash's share at full power, and the ambient
        let (amb, flash): (Vec<f64>, Vec<f64>) = lv
            .iter()
            .zip(&self.ambient)
            .map(|(l, a)| {
                let lit = l / lines_now;
                let a = a.min(lit);
                (a, (lit - a) * full)
            })
            .unzip();
        let wsum: f64 = self.weights.iter().sum();
        let solve = |k: f64| -> f64 {
            let t: Vec<f64> = amb.iter().zip(&flash).map(|(a, f)| a + k * f).collect();
            let mean = t.iter().zip(&self.weights).map(|(v, w)| v * w).sum::<f64>() / wsum;
            let top = percentile(&t, 97.0);
            let a = if mean > 0.0 { METER_TARGET / mean } else { 1e9 };
            let b = if top > 0.0 { METER_HIGHLIGHT / top } else { 1e9 };
            a.min(b)
        };
        let mut level = self.photo_level;
        let mut x = solve(1.0);
        if x < MIN_LINES {
            // too bright even at the shortest exposure: dim the flash (about 25 mA x (level + 1))
            let (mut lo, mut hi) = (0.0, 1.0);
            for _ in 0..20 {
                let k = (lo + hi) / 2.0;
                if solve(k) < MIN_LINES {
                    hi = k;
                } else {
                    lo = k;
                }
            }
            level = ((lo * (self.photo_level + 1) as f64) as i64 - 1).clamp(0, self.photo_level as i64) as u32;
            x = solve((level + 1) as f64 / (self.photo_level + 1) as f64).max(MIN_LINES);
        }
        let lines = x.clamp(MIN_LINES, self.max_lines);
        let gain = (x / lines).clamp(1.0, self.max_gain);
        self.log = format!(
            "pre-flash {:.0} us at level {}, top {:.0}: flash level {}, {:.0} lines x {:.2}",
            self.us,
            self.level,
            percentile(lv, 97.0),
            level,
            lines,
            gain
        );
        (level, Some((lines.floor(), gain)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stats(frame: u32, v: f64) -> Stats {
        Stats { frame, nx: 16, levels: vec![v; 16 * 12] }
    }

    #[test]
    fn percentiles() {
        assert_eq!(percentile(&[1.0, 2.0, 3.0, 4.0, 5.0], 50.0), 3.0);
        assert!((percentile(&[0.0, 10.0], 97.0) - 9.7).abs() < 1e-9);
    }

    #[test]
    fn steps_down_while_clipped_then_solves() {
        let line = 8.72;
        let mut m = Meter::new(&stats(0, 2.0), 1000.0, 4.0, line, 39, 1146.0, 15.5);
        // waits for the sensor, then SETTLE frames
        assert_eq!(m.tick(Some(&stats(10, 115.0)), false), Step::Wait);
        assert_eq!(m.tick(Some(&stats(11, 115.0)), true), Step::Wait);
        assert_eq!(m.tick(Some(&stats(12, 115.0)), true), Step::Wait);
        // clipped: 8x shorter
        assert_eq!(m.tick(Some(&stats(14, 115.0)), true), Step::Expose(50.0));
        m.exposing(50.0);
        assert_eq!(m.tick(Some(&stats(15, 40.0)), true), Step::Wait);
        match m.tick(Some(&stats(18, 40.0)), true) {
            Step::Done(level, Some((lines, gain))) => {
                // bright at 50 us with the pre-flash: even the shortest exposure would be
                // too much at full power, so the flash dims and the exposure is the shortest
                assert!(level < 39 && level > 0, "{level}");
                assert_eq!(lines, MIN_LINES);
                assert_eq!(gain, 1.0);
            }
            s => panic!("{s:?}"),
        }
    }

    #[test]
    fn dims_the_preflash_for_a_close_subject() {
        let mut m = Meter::new(&stats(0, 1.0), 1000.0, 1.0, 8.72, 39, 1146.0, 15.5);
        m.exposing(MIN_US);
        assert_eq!(m.tick(Some(&stats(1, 115.0)), true), Step::Wait);
        assert_eq!(m.tick(Some(&stats(4, 115.0)), true), Step::Dim(3));
        assert_eq!(m.tick(Some(&stats(7, 115.0)), true), Step::Dim(0));
    }

    #[test]
    fn no_statistics_means_full_flash() {
        let mut m = Meter::new(&stats(0, 1.0), 1000.0, 1.0, 8.72, 39, 1146.0, 15.5);
        m.t0 -= std::time::Duration::from_millis(3000);
        assert_eq!(m.tick(None, false), Step::Done(39, None));
    }
}
