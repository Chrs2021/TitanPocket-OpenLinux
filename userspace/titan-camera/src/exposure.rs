//! Manual exposure on the preview and in video. With both shutter and ISO auto, the driver's
//! own auto exposure runs (in the kernel, per frame). Fixing either takes it over: the sensor
//! gets the fixed value, and this loop moves the other one to keep the picture's brightness
//! (shutter priority, ISO priority); with both fixed it only sets them.
pub const TARGET_LUMA: f64 = 105.0; // centre-weighted mean Y the loop aims for
const EVERY: u32 = 3; // measurements between steps (a new exposure takes ~2 frames)
const MIN_US: f64 = 40.0;

#[derive(Clone, Debug)]
pub struct Exposure {
    pub us: f64,
    pub gain: f64,
    n: u32,
}

impl Exposure {
    pub fn new(us: f64, gain: f64) -> Exposure {
        Exposure { us, gain, n: 0 }
    }

    /// one brightness measurement; `shutter_us`/`gain` are the fixed values (None: free),
    /// `max_us` the longest exposure the stream allows. Returns a new (µs, gain) to set.
    pub fn step(&mut self, luma: f64, shutter_us: Option<f64>, gain: Option<f64>, max_us: f64, gain_max: f64) -> Option<(f64, f64)> {
        let (us0, g0) = (self.us, self.gain);
        if let Some(s) = shutter_us {
            self.us = s.min(max_us);
        }
        if let Some(g) = gain {
            self.gain = g.clamp(1.0, gain_max);
        }
        self.n += 1;
        if self.n % EVERY == 0 && (shutter_us.is_none() || gain.is_none()) {
            // Y is gamma encoded (about linear^(1/2.2)); half the step, against overshoot
            let f = ((TARGET_LUMA / luma.max(1.0)).powf(2.2)).sqrt().clamp(0.5, 2.0);
            if (f - 1.0).abs() > 0.06 {
                if gain.is_none() {
                    self.gain = (self.gain * f).clamp(1.0, gain_max);
                } else {
                    self.us = (self.us * f).clamp(MIN_US, max_us);
                }
            }
        }
        ((self.us - us0).abs() > 1.0 || (self.gain - g0).abs() > 0.01).then_some((self.us, self.gain))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a scene where Y = 255 * (exposure * gain / 40000) ^ (1/2.2): shutter priority settles
    #[test]
    fn shutter_priority_converges() {
        let y = |us: f64, g: f64| 255.0 * (us * g / 40000.0).min(1.0).powf(1.0 / 2.2);
        let mut e = Exposure::new(10000.0, 1.0);
        for _ in 0..200 {
            if let Some((us, g)) = e.step(y(e.us, e.gain), Some(1000.0), None, 33333.0, 15.5) {
                assert_eq!(us, 1000.0);
                assert!((1.0..=15.5).contains(&g));
            }
        }
        assert!((y(e.us, e.gain) - TARGET_LUMA).abs() < 12.0, "Y {}", y(e.us, e.gain));
    }

    #[test]
    fn iso_priority_moves_shutter_within_limit() {
        let mut e = Exposure::new(1000.0, 1.0);
        for _ in 0..100 {
            e.step(5.0, None, Some(2.0), 33333.0, 15.5); // very dark
        }
        assert_eq!(e.gain, 2.0);
        assert_eq!(e.us, 33333.0);
    }

    #[test]
    fn both_fixed_only_sets() {
        let mut e = Exposure::new(1000.0, 1.0);
        assert_eq!(e.step(50.0, Some(2000.0), Some(4.0), 33333.0, 15.5), Some((2000.0, 4.0)));
        assert_eq!(e.step(50.0, Some(2000.0), Some(4.0), 33333.0, 15.5), None);
    }
}
