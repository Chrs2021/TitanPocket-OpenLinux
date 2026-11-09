//! The two cameras and their sensors' V4L2 controls (titan_camera driver, docs/camera.md).
//!
//! libcamera names each camera by its device-tree path on 6.12; the sensor subdev under the
//! same device carries exposure, gain, frame length, focus and the mounting rotation.
use std::fs::{File, OpenOptions};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};

pub const CID_EXPOSURE: u32 = 0x0098_0911; // lines
pub const CID_ANALOGUE_GAIN: u32 = 0x009e_0903; // 128 = 1x
pub const CID_VBLANK: u32 = 0x009e_0901; // lines
pub const CID_HBLANK: u32 = 0x009e_0902; // pixels
pub const CID_PIXEL_RATE: u32 = 0x009f_0902; // pixels/s (64-bit)
pub const CID_FOCUS_ABSOLUTE: u32 = 0x009a_090a;
pub const CID_SENSOR_ROTATION: u32 = 0x009a_0923; // degrees counter-clockwise

pub const GAIN_UNIT: i32 = 128;
pub const GAIN_MAX: f64 = 15.5;
pub const FOCUS_MAX: i32 = 1023;
/// line time of both rear modes (HTS / pixel rate), when the controls can't be read
const FALLBACK_LINE_US: f64 = 2512.0 / 288.0;

#[derive(Clone, Debug)]
pub struct Camera {
    pub label: &'static str,
    /// libcamera's camera name
    pub id: String,
    pub sysdir: PathBuf,
    /// the front camera's preview is mirrored
    pub mirror: bool,
    /// rear: the AK7371 lens motor, and the flash
    pub focus: bool,
    /// clockwise, when the driver doesn't say
    pub rotation: u32,
    /// the sensor's two modes: binned and full
    pub binned: (u32, u32),
    /// the preview: the binned mode scaled down on the ISP (its resizer, CRZ) to about the
    /// screen's size, a sixth of the binned frame's memory traffic
    pub preview: (u32, u32),
    pub full: (u32, u32),
}

fn camsv_dir() -> PathBuf {
    let mut found: Vec<PathBuf> = Vec::new();
    for pat in ["/sys/devices/platform", "/sys/devices/platform/soc"] {
        if let Ok(rd) = std::fs::read_dir(pat) {
            for e in rd.flatten() {
                if e.file_name().to_string_lossy().starts_with("1a050000.camsv") {
                    found.push(e.path());
                }
            }
        }
    }
    found.sort();
    found.into_iter().next().unwrap_or_else(|| PathBuf::from("/sys/devices/platform/1a050000.camsv1"))
}

/// libcamera's name: the device-tree path of the sensor's device (6.12), else its sysfs path
/// and model (4.14)
fn camera_id(sysdir: &Path, model: &str) -> String {
    if let Ok(node) = std::fs::canonicalize(sysdir.join("of_node")) {
        let s = node.to_string_lossy();
        if let Some(rest) = s.strip_prefix("/sys/firmware/devicetree") {
            return rest.to_string();
        }
    }
    let rel = sysdir.strip_prefix("/sys/devices").unwrap_or(sysdir);
    format!("{} {model}", rel.display())
}

pub fn cameras() -> Vec<Camera> {
    let base = camsv_dir();
    let rear = base.join("rear");
    let front = base.join("front");
    vec![
        Camera {
            label: "Rear",
            id: camera_id(&rear, "ov16880"),
            sysdir: rear,
            mirror: false,
            focus: true,
            rotation: 90,
            binned: (2336, 1752),
            preview: (960, 720),
            full: (4672, 3504),
        },
        Camera {
            label: "Front",
            id: camera_id(&front, "ov8865"),
            sysdir: front,
            mirror: true,
            focus: false,
            rotation: 270,
            binned: (1632, 1224),
            preview: (816, 612),
            full: (3264, 2448),
        },
    ]
}

const VIDIOC_G_CTRL: libc::c_ulong = 0xc008_561b;
const VIDIOC_S_CTRL: libc::c_ulong = 0xc008_561c;
const VIDIOC_G_EXT_CTRLS: libc::c_ulong = 0xc020_5647;

#[repr(C)]
struct Control {
    id: u32,
    value: i32,
}

#[repr(C, packed)]
struct ExtControl {
    id: u32,
    size: u32,
    reserved2: u32,
    value64: i64,
}

#[repr(C)]
struct ExtControls {
    which: u32,
    count: u32,
    error_idx: u32,
    request_fd: i32,
    reserved: u32,
    controls: *mut ExtControl,
}

/// a sensor's /dev/v4l-subdevN
pub struct Sensor {
    f: File,
}

impl Sensor {
    pub fn open(cam: &Camera) -> Option<Sensor> {
        let dir = cam.sysdir.join("video4linux");
        for e in std::fs::read_dir(dir).ok()?.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            if name.starts_with("v4l-subdev") {
                let f = OpenOptions::new().read(true).write(true).open(format!("/dev/{name}")).ok()?;
                return Some(Sensor { f });
            }
        }
        None
    }

    pub fn get(&self, id: u32) -> Option<i32> {
        let mut c = Control { id, value: 0 };
        let r = unsafe { libc::ioctl(self.f.as_raw_fd(), VIDIOC_G_CTRL, &mut c) };
        (r == 0).then_some(c.value)
    }

    pub fn set(&self, id: u32, value: i32) -> bool {
        let mut c = Control { id, value };
        unsafe { libc::ioctl(self.f.as_raw_fd(), VIDIOC_S_CTRL, &mut c) == 0 }
    }

    fn get64(&self, id: u32) -> Option<i64> {
        let mut c = ExtControl { id, size: 0, reserved2: 0, value64: 0 };
        let mut cs = ExtControls { which: 0, count: 1, error_idx: 0, request_fd: 0, reserved: 0, controls: &mut c };
        let r = unsafe { libc::ioctl(self.f.as_raw_fd(), VIDIOC_G_EXT_CTRLS, &mut cs) };
        (r == 0).then(|| c.value64)
    }

    /// microseconds a line of the current mode takes: (width + hblank) / pixel rate
    pub fn line_us(&self, width: u32) -> f64 {
        match (self.get(CID_HBLANK), self.get64(CID_PIXEL_RATE)) {
            (Some(hb), Some(rate)) if rate > 0 => (width as f64 + hb as f64) * 1e6 / rate as f64,
            _ => FALLBACK_LINE_US,
        }
    }

    /// clockwise correction from the driver's counter-clockwise mounting rotation
    pub fn rotation(&self) -> Option<u32> {
        let ccw = self.get(CID_SENSOR_ROTATION)?;
        matches!(ccw, 0 | 90 | 180 | 270).then(|| ((360 - ccw) % 360) as u32)
    }

    /// set a manual exposure (microseconds) and gain (1.0 = 1x) on the running stream. The
    /// frame grows when the exposure needs it (long shutter: photos); the driver's own auto
    /// exposure backs off for the rest of the stream. Returns the exposure actually set.
    pub fn set_exposure(&self, height: u32, width: u32, us: f64, gain: f64) -> f64 {
        let line = self.line_us(width);
        let lines = ((us / line).round() as i64).max(4);
        let vblank = self.get(CID_VBLANK).unwrap_or(0) as i64;
        if lines + 8 > height as i64 + vblank {
            self.set(CID_VBLANK, (lines + 8 - height as i64) as i32);
        }
        let max = height as i64 + self.get(CID_VBLANK).unwrap_or(vblank as i32) as i64 - 8;
        let lines = lines.min(max);
        self.set(CID_EXPOSURE, lines as i32);
        self.set(CID_ANALOGUE_GAIN, (gain.clamp(1.0, GAIN_MAX) * GAIN_UNIT as f64).round() as i32);
        lines as f64 * line
    }
}

/// GstVideoOrientationMethod nick for a clockwise rotation, then an optional mirror
pub fn video_direction(rotation: u32, mirror: bool) -> &'static str {
    match (rotation, mirror) {
        (90, false) => "90r",
        (180, false) => "180",
        (270, false) => "90l",
        (_, false) => "identity",
        (90, true) => "ul-lr",
        (180, true) => "vert",
        (270, true) => "ur-ll",
        (_, true) => "horiz",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn directions() {
        assert_eq!(video_direction(90, false), "90r");
        assert_eq!(video_direction(270, true), "ur-ll");
        assert_eq!(video_direction(0, true), "horiz");
    }

    #[test]
    fn ext_control_layout() {
        assert_eq!(std::mem::size_of::<ExtControl>(), 20); // packed, as the kernel's
        assert_eq!(std::mem::size_of::<ExtControls>(), 32);
    }
}
