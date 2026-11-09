//! What the user picks (camera, rotation, flash, resolutions, shutter, ISO) and where it is
//! kept: ~/.config/titan-camera/settings, one `key=value` a line.
use std::collections::HashMap;
use std::path::PathBuf;

/// shutter speeds offered, in seconds; `None` is auto. Preview and video run at 30 fps, so
/// anything longer than 1/30 s is for photos only (the frame stretches for it).
pub const SHUTTERS: &[(&str, Option<f64>)] = &[
    ("Auto", None),
    ("1/2000", Some(1.0 / 2000.0)),
    ("1/1000", Some(1.0 / 1000.0)),
    ("1/500", Some(1.0 / 500.0)),
    ("1/250", Some(1.0 / 250.0)),
    ("1/125", Some(1.0 / 125.0)),
    ("1/60", Some(1.0 / 60.0)),
    ("1/30", Some(1.0 / 30.0)),
    ("1/15", Some(1.0 / 15.0)),
    ("1/8", Some(1.0 / 8.0)),
    ("1/4", Some(1.0 / 4.0)),
    ("1/2", Some(0.5)),
    ("1 s", Some(1.0)),
];
/// the longest exposure a 30 fps stream (preview, video) can take
pub const STREAM_MAX_S: f64 = 1.0 / 30.0;

/// ISO as analogue gain (ISO 100 = 1x); `None` is auto
pub const ISOS: &[(&str, Option<f64>)] = &[
    ("Auto", None),
    ("100", Some(1.0)),
    ("200", Some(2.0)),
    ("400", Some(4.0)),
    ("800", Some(8.0)),
    ("1600", Some(15.5)),
];

/// photo sizes: the sensor's full mode, or its binned mode (faster, quarter the pixels)
pub const PHOTO_SIZES: &[&str] = &["Full", "Binned"];

/// video sizes (4:3, as the sensor), as the long side x short side, and bit rate (kbit/s)
pub const VIDEO_SIZES: &[(&str, u32, u32, u32)] = &[
    ("1440×1080", 1440, 1080, 12000),
    ("960×720", 960, 720, 6000),
    ("640×480", 640, 480, 3000),
];

pub const FLASH_MODES: &[&str] = &["Off", "Auto", "On", "Torch"];
pub const FLASH_TONES: &[&str] = &["Neutral", "Warm", "Cool"];

#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub camera: usize,
    /// per camera, clockwise; `None`: the driver's
    pub rotation: [Option<u32>; 2],
    pub flash: usize,
    pub tone: usize,
    pub photo_size: usize,
    pub video_size: usize,
    pub shutter: usize,
    pub iso: usize,
    pub video: bool,
    pub focus: i32,
    pub last: Option<PathBuf>,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            camera: 0,
            rotation: [None, None],
            flash: 0,
            tone: 0,
            photo_size: 0,
            video_size: 1,
            shutter: 0,
            iso: 0,
            video: false,
            focus: 256,
            last: None,
        }
    }
}

fn path() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/root".into())).join(".config"));
    base.join("titan-camera").join("settings")
}

impl Settings {
    pub fn parse(text: &str) -> Settings {
        let kv: HashMap<&str, &str> = text.lines().filter_map(|l| l.split_once('=')).collect();
        let d = Settings::default();
        let num = |k: &str, max: usize, def: usize| -> usize {
            kv.get(k).and_then(|v| v.trim().parse().ok()).filter(|v| *v < max).unwrap_or(def)
        };
        let rot = |k: &str| -> Option<u32> {
            kv.get(k).and_then(|v| v.trim().parse().ok()).filter(|r| [0, 90, 180, 270].contains(r))
        };
        Settings {
            camera: num("camera", 2, d.camera),
            rotation: [rot("rotation0"), rot("rotation1")],
            flash: num("flash", FLASH_MODES.len(), d.flash),
            tone: num("tone", FLASH_TONES.len(), d.tone),
            photo_size: num("photo_size", PHOTO_SIZES.len(), d.photo_size),
            video_size: num("video_size", VIDEO_SIZES.len(), d.video_size),
            shutter: num("shutter", SHUTTERS.len(), d.shutter),
            iso: num("iso", ISOS.len(), d.iso),
            video: kv.get("video").map(|v| v.trim() == "1").unwrap_or(false),
            focus: kv.get("focus").and_then(|v| v.trim().parse().ok()).unwrap_or(d.focus),
            last: kv.get("last").map(|v| PathBuf::from(v.trim())).filter(|p| !p.as_os_str().is_empty()),
        }
    }

    pub fn render(&self) -> String {
        let r = |x: Option<u32>| x.map(|v| v.to_string()).unwrap_or_default();
        format!(
            "camera={}\nrotation0={}\nrotation1={}\nflash={}\ntone={}\nphoto_size={}\nvideo_size={}\nshutter={}\niso={}\nvideo={}\nfocus={}\nlast={}\n",
            self.camera,
            r(self.rotation[0]),
            r(self.rotation[1]),
            self.flash,
            self.tone,
            self.photo_size,
            self.video_size,
            self.shutter,
            self.iso,
            self.video as u8,
            self.focus,
            self.last.as_ref().map(|p| p.display().to_string()).unwrap_or_default()
        )
    }

    pub fn load() -> Settings {
        std::fs::read_to_string(path()).map(|t| Settings::parse(&t)).unwrap_or_default()
    }

    pub fn save(&self) {
        let p = path();
        if let Some(d) = p.parent() {
            let _ = std::fs::create_dir_all(d);
        }
        let _ = std::fs::write(p, self.render());
    }

    /// the shutter to use on a 30 fps stream (preview, video): long ones clamp to 1/30 s
    pub fn stream_shutter(&self) -> Option<f64> {
        SHUTTERS[self.shutter].1.map(|s| s.min(STREAM_MAX_S))
    }

    pub fn manual_exposure(&self) -> bool {
        SHUTTERS[self.shutter].1.is_some() || ISOS[self.iso].1.is_some()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let s = Settings {
            camera: 1,
            rotation: [Some(90), None],
            flash: 2,
            tone: 1,
            photo_size: 1,
            video_size: 0,
            shutter: 5,
            iso: 3,
            video: true,
            focus: 400,
            last: Some(PathBuf::from("/root/Pictures/Camera/IMG_1.jpg")),
        };
        assert_eq!(Settings::parse(&s.render()), s);
    }

    #[test]
    fn bad_values_fall_back() {
        let s = Settings::parse("camera=7\nshutter=99\nrotation0=45\nvideo=yes\n");
        assert_eq!(s, Settings::default());
    }

    #[test]
    fn long_shutter_clamps_on_streams() {
        let s = Settings { shutter: SHUTTERS.len() - 1, ..Default::default() };
        assert_eq!(s.stream_shutter(), Some(STREAM_MAX_S));
        assert!(s.manual_exposure());
        assert!(!Settings::default().manual_exposure());
    }
}
