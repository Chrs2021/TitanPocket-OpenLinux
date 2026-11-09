//! Contrast-detect autofocus on the preview frames (rear camera, AK7371 lens), as the Python
//! app had it: a coarse sweep until sharpness has clearly passed its peak, a fine pass around
//! the best position, then tracking that refocuses when sharpness stays well below the focused
//! level. Only frames whose capture started after the lens settled count.
use crate::camera::FOCUS_MAX;

const COARSE: i32 = 64;
const FINE: i32 = 16;
const SETTLE_NS: u64 = 15_000_000; // lens settling after a move
const FRAME_NS: u64 = 40_000_000; // longest exposure + readout we wait out
pub const REGION: f64 = 0.3; // focus region: this share of width and height
const FRAMES_PER_POS: usize = 2;
const FINE_FRAMES: usize = 4;
const MIN_RATIO: f64 = 1.25; // best/worst below this: nothing to focus on
const BASELINE_FRAMES: usize = 8;
const TRACK_FRAMES: usize = 15;
const DROP: f64 = 0.6;
const DROP_FRAMES: u32 = 30;
const MIN_GAP_S: f64 = 3.0;

/// gradient energy of the focus region of a YUYV frame (luma, binned 2x2 against noise),
/// normalised by brightness so exposure changes don't look like focus changes
pub fn sharpness(data: &[u8], w: usize, h: usize, stride: usize, region: (f64, f64)) -> f64 {
    let rw = ((w as f64 * REGION) as usize) & !3;
    let rh = ((h as f64 * REGION) as usize) & !1;
    if rw < 8 || rh < 8 || data.len() < stride * h {
        return 0.0;
    }
    let x0 = (((region.0 * w as f64) as isize - rw as isize / 2).clamp(0, (w - rw) as isize) as usize) & !1;
    let y0 = (((region.1 * h as f64) as isize - rh as isize / 2).clamp(0, (h - rh) as isize) as usize) & !1;
    let (bw, bh) = (rw / 2, rh / 2);
    let mut g = vec![0i32; bw * bh];
    for by in 0..bh {
        let r0 = &data[(y0 + 2 * by) * stride..];
        let r1 = &data[(y0 + 2 * by + 1) * stride..];
        for bx in 0..bw {
            let x = 2 * (x0 + 2 * bx); // Y at byte 2x
            g[by * bw + bx] = r0[x] as i32 + r0[x + 2] as i32 + r1[x] as i32 + r1[x + 2] as i32;
        }
    }
    let (mut ex, mut ey, mut sum) = (0f64, 0f64, 0f64);
    for y in 0..bh {
        for x in 0..bw {
            let v = g[y * bw + x];
            sum += v as f64;
            if x + 1 < bw {
                let d = (g[y * bw + x + 1] - v) as f64;
                ex += d * d;
            }
            if y + 1 < bh {
                let d = (g[(y + 1) * bw + x] - v) as f64;
                ey += d * d;
            }
        }
    }
    let n = (bw * bh) as f64;
    let energy = ex / ((bw - 1) * bh) as f64 + ey / (bw * (bh - 1)) as f64;
    let mean = sum / n;
    energy / (mean * mean + 1024.0)
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum State {
    Off,
    Manual,
    Hold,
    Sweep,
    Fine,
    Settle,
    Track,
}

#[derive(PartialEq, Debug)]
pub enum Action {
    Move(i32),
    /// focus found (Some) or nothing to focus on (None)
    Done(Option<i32>),
    Refocus,
}

pub struct AutoFocus {
    pub state: State,
    pub region: (f64, f64),
    pub pos: i32,
    start_pos: i32,
    moved_at: u64,
    frames: Vec<f64>,
    samples: Vec<(i32, f64)>,
    queue: Vec<i32>,
    baseline: f64,
    recent: Vec<f64>,
    low: u32,
    focused_at: f64,
}

fn median(v: &[f64]) -> f64 {
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    s[s.len() / 2]
}

impl AutoFocus {
    pub fn new(pos: i32) -> AutoFocus {
        AutoFocus {
            state: State::Off,
            region: (0.5, 0.5),
            pos,
            start_pos: pos,
            moved_at: 0,
            frames: vec![],
            samples: vec![],
            queue: vec![],
            baseline: 0.0,
            recent: vec![],
            low: 0,
            focused_at: 0.0,
        }
    }

    fn best(&self) -> (i32, f64) {
        self.samples.iter().copied().fold((0, f64::MIN), |b, s| if s.1 > b.1 { s } else { b })
    }

    fn value(&self, pos: i32) -> Option<f64> {
        self.samples.iter().find(|s| s.0 == pos).map(|s| s.1)
    }

    /// `now`: the pipeline's running time (ns), for gating frames after the move
    pub fn start(&mut self, region: Option<(f64, f64)>, now: u64) -> Action {
        if let Some(r) = region {
            self.region = r;
        }
        self.start_pos = self.pos;
        self.samples.clear();
        self.queue = (0..=FOCUS_MAX).step_by(COARSE as usize).collect();
        self.queue.push(FOCUS_MAX);
        self.state = State::Sweep;
        let p = self.queue.remove(0);
        self.moved(p, now)
    }

    fn moved(&mut self, pos: i32, now: u64) -> Action {
        self.pos = pos;
        self.frames.clear();
        self.moved_at = now;
        Action::Move(pos)
    }

    /// keep the lens where it is and watch sharpness from now on
    pub fn resume(&mut self, now: u64) {
        self.frames.clear();
        self.state = State::Settle;
        self.moved_at = now;
    }

    /// one frame's sharpness, captured at running time `pts`; `clock_s` for the refocus gap
    pub fn sample(&mut self, pts: u64, value: f64, clock_s: f64) -> Option<Action> {
        match self.state {
            State::Off | State::Manual | State::Hold => return None,
            State::Track => return self.track(value, clock_s),
            _ => {}
        }
        if pts < self.moved_at + SETTLE_NS + FRAME_NS {
            return None;
        }
        self.frames.push(value);
        if self.state == State::Settle {
            if self.frames.len() >= BASELINE_FRAMES {
                self.baseline = median(&self.frames);
                self.recent.clear();
                self.low = 0;
                self.state = State::Track;
            }
            return None;
        }
        let need = if self.state == State::Fine { FINE_FRAMES } else { FRAMES_PER_POS };
        if self.frames.len() < need {
            return None;
        }
        let v = self.frames.iter().sum::<f64>() / self.frames.len() as f64;
        self.samples.push((self.pos, v));
        if self.state == State::Sweep {
            let (best, bv) = self.best();
            let past_peak = self.pos - best >= 3 * COARSE && v < 0.6 * bv;
            if !self.queue.is_empty() && !past_peak {
                let p = self.queue.remove(0);
                return Some(self.moved(p, pts));
            }
            self.state = State::Fine;
            self.queue = ((best - 3 * FINE)..=(best + 3 * FINE))
                .step_by(FINE as usize)
                .filter(|p| (0..=FOCUS_MAX).contains(p) && self.value(*p).is_none())
                .collect();
        }
        if !self.queue.is_empty() {
            let p = self.queue.remove(0);
            return Some(self.moved(p, pts));
        }
        let (best, bv) = self.best();
        let worst = self.samples.iter().map(|s| s.1).fold(f64::MAX, f64::min);
        if bv < MIN_RATIO * worst {
            let p = self.start_pos;
            self.moved(p, pts);
            self.state = State::Hold;
            return Some(Action::Done(None));
        }
        self.moved(best, pts);
        self.state = State::Settle;
        self.focused_at = clock_s;
        Some(Action::Done(Some(best)))
    }

    fn track(&mut self, value: f64, clock_s: f64) -> Option<Action> {
        self.recent.push(value);
        if self.recent.len() > TRACK_FRAMES {
            self.recent.remove(0);
        }
        if self.recent.len() < TRACK_FRAMES {
            return None;
        }
        let level = median(&self.recent);
        if level > self.baseline {
            self.baseline = 0.95 * self.baseline + 0.05 * level;
        }
        if level < DROP * self.baseline {
            self.low += 1;
            if self.low >= DROP_FRAMES && clock_s - self.focused_at > MIN_GAP_S {
                self.low = 0;
                return Some(Action::Refocus);
            }
        } else {
            self.low = 0;
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(w: usize, h: usize, sharp: bool) -> Vec<u8> {
        let mut f = vec![128u8; w * h * 2];
        for y in 0..h {
            for x in 0..w {
                let v = if sharp { if (x / 4 + y / 4) % 2 == 0 { 40 } else { 220 } } else { 130 };
                f[(y * w + x) * 2] = v;
            }
        }
        f
    }

    #[test]
    fn sharp_beats_flat() {
        let (w, h) = (320, 240);
        let s = sharpness(&frame(w, h, true), w, h, w * 2, (0.5, 0.5));
        let f = sharpness(&frame(w, h, false), w, h, w * 2, (0.5, 0.5));
        assert!(s > 10.0 * f.max(1e-9), "{s} {f}");
    }

    /// a lens whose sharpness peaks at 500: the sweep and fine pass land within a fine step
    #[test]
    fn finds_the_peak() {
        let curve = |p: i32| 1.0 / (1.0 + ((p - 500) as f64 / 120.0).powi(2));
        let mut af = AutoFocus::new(256);
        let mut t = 0u64;
        let mut act = af.start(None, t);
        for _ in 0..2000 {
            let pos = match act {
                Action::Move(p) => p,
                Action::Done(Some(p)) => {
                    assert!((p - 500).abs() <= FINE, "focused at {p}");
                    return;
                }
                other => panic!("{other:?}"),
            };
            loop {
                t += 33_000_000;
                if let Some(a) = af.sample(t, curve(pos), t as f64 / 1e9) {
                    act = a;
                    break;
                }
            }
        }
        panic!("no focus");
    }
}
