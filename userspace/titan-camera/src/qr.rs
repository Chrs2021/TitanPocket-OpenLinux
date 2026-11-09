//! QR codes in the preview, found passively (docs/camera.md "QR codes").
//!
//! The decoder is libzbar, loaded at run time (no build dependency; without it the app runs
//! without QR codes), set for QR only and every second line and column: about 14 ms a 640x480
//! frame, against ~140 ms for GStreamer's zbar element, which tries every symbology on every
//! line. The preview's QR branch gives it a few frames a second; it reads the YUYV frame's luma
//! directly, every n-th pixel for large frames.
use std::ffi::{c_char, c_int, c_uint, c_ulong, c_void};
use std::sync::OnceLock;

/// frames a second the decoder looks at
pub const RATE: u32 = 3;
/// luma width the decoder gets at most (wider frames are subsampled)
const MAX_W: usize = 1000;

const Y800: c_ulong = 0x3030_3859;
const QRCODE: c_int = 64;
const CFG_ENABLE: c_int = 0;
const CFG_X_DENSITY: c_int = 0x100;
const CFG_Y_DENSITY: c_int = 0x101;

type P = *mut c_void;

struct Zbar {
    scanner_create: unsafe extern "C" fn() -> P,
    scanner_destroy: unsafe extern "C" fn(P),
    scanner_set_config: unsafe extern "C" fn(P, c_int, c_int, c_int) -> c_int,
    image_create: unsafe extern "C" fn() -> P,
    image_destroy: unsafe extern "C" fn(P),
    image_set_format: unsafe extern "C" fn(P, c_ulong),
    image_set_size: unsafe extern "C" fn(P, c_uint, c_uint),
    image_set_data: unsafe extern "C" fn(P, *const c_void, c_ulong, P),
    scan_image: unsafe extern "C" fn(P, P) -> c_int,
    first_symbol: unsafe extern "C" fn(P) -> P,
    symbol_next: unsafe extern "C" fn(P) -> P,
    symbol_get_type: unsafe extern "C" fn(P) -> c_int,
    symbol_get_data: unsafe extern "C" fn(P) -> *const c_char,
    symbol_get_data_length: unsafe extern "C" fn(P) -> c_uint,
}

// the functions are plain C entry points of a library that stays loaded
unsafe impl Send for Zbar {}
unsafe impl Sync for Zbar {}

fn zbar() -> Option<&'static Zbar> {
    static Z: OnceLock<Option<Zbar>> = OnceLock::new();
    Z.get_or_init(|| unsafe {
        let h = libc::dlopen(c"libzbar.so.0".as_ptr(), libc::RTLD_NOW);
        if h.is_null() {
            return None;
        }
        macro_rules! sym {
            ($n:literal) => {{
                let p = libc::dlsym(h, concat!($n, "\0").as_ptr() as *const c_char);
                if p.is_null() {
                    return None;
                }
                std::mem::transmute(p)
            }};
        }
        Some(Zbar {
            scanner_create: sym!("zbar_image_scanner_create"),
            scanner_destroy: sym!("zbar_image_scanner_destroy"),
            scanner_set_config: sym!("zbar_image_scanner_set_config"),
            image_create: sym!("zbar_image_create"),
            image_destroy: sym!("zbar_image_destroy"),
            image_set_format: sym!("zbar_image_set_format"),
            image_set_size: sym!("zbar_image_set_size"),
            image_set_data: sym!("zbar_image_set_data"),
            scan_image: sym!("zbar_scan_image"),
            first_symbol: sym!("zbar_image_first_symbol"),
            symbol_next: sym!("zbar_symbol_next"),
            symbol_get_type: sym!("zbar_symbol_get_type"),
            symbol_get_data: sym!("zbar_symbol_get_data"),
            symbol_get_data_length: sym!("zbar_symbol_get_data_length"),
        })
    })
    .as_ref()
}

/// libzbar can be loaded: the preview gets its QR branch
pub fn available() -> bool {
    zbar().is_some()
}

/// one decoder; used by one thread (the QR branch's streaming thread)
pub struct Scanner {
    z: &'static Zbar,
    sc: P,
    gray: Vec<u8>,
}

unsafe impl Send for Scanner {}

impl Scanner {
    pub fn new() -> Option<Scanner> {
        let z = zbar()?;
        let sc = unsafe { (z.scanner_create)() };
        if sc.is_null() {
            return None;
        }
        unsafe {
            (z.scanner_set_config)(sc, 0, CFG_ENABLE, 0);
            (z.scanner_set_config)(sc, QRCODE, CFG_ENABLE, 1);
            (z.scanner_set_config)(sc, 0, CFG_X_DENSITY, 2);
            (z.scanner_set_config)(sc, 0, CFG_Y_DENSITY, 2);
        }
        Some(Scanner { z, sc, gray: Vec::new() })
    }

    /// the QR codes in a YUYV frame, as text
    pub fn scan_yuyv(&mut self, data: &[u8], w: usize, h: usize, stride: usize) -> Vec<String> {
        let step = w.div_ceil(MAX_W).max(1);
        let (gw, gh) = (w / step, h / step);
        if gw == 0 || gh == 0 || data.len() < (h - 1) * stride + 2 * w {
            return Vec::new();
        }
        self.gray.clear();
        self.gray.reserve(gw * gh);
        for y in 0..gh {
            let row = &data[y * step * stride..];
            self.gray.extend((0..gw).map(|x| row[2 * x * step]));
        }
        let z = self.z;
        let mut found = Vec::new();
        unsafe {
            let im = (z.image_create)();
            if im.is_null() {
                return found;
            }
            (z.image_set_format)(im, Y800);
            (z.image_set_size)(im, gw as c_uint, gh as c_uint);
            (z.image_set_data)(im, self.gray.as_ptr() as *const c_void, self.gray.len() as c_ulong, std::ptr::null_mut());
            if (z.scan_image)(self.sc, im) > 0 {
                let mut sym = (z.first_symbol)(im);
                while !sym.is_null() {
                    if (z.symbol_get_type)(sym) == QRCODE {
                        let p = (z.symbol_get_data)(sym);
                        let n = (z.symbol_get_data_length)(sym) as usize;
                        if !p.is_null() && n > 0 {
                            let b = std::slice::from_raw_parts(p as *const u8, n);
                            found.push(String::from_utf8_lossy(b).into_owned());
                        }
                    }
                    sym = (z.symbol_next)(sym);
                }
            }
            (z.image_destroy)(im);
        }
        found
    }
}

impl Drop for Scanner {
    fn drop(&mut self) {
        unsafe { (self.z.scanner_destroy)(self.sc) }
    }
}

/// what a code holds: (kind, what to show, a link to open)
pub fn describe(text: &str) -> (&'static str, String, Option<String>) {
    let t = text.trim();
    let lower = t.to_ascii_lowercase();
    if (lower.starts_with("http://") || lower.starts_with("https://")) && !t.contains(char::is_whitespace) {
        // host, path and query, without the scheme
        let rest = &t[t.find("://").map(|i| i + 3).unwrap_or(0)..];
        let (main, query) = rest.split_once('?').unwrap_or((rest, ""));
        let main = main.split('#').next().unwrap_or(main).trim_end_matches('/');
        let shown = if query.is_empty() { main.to_string() } else { format!("{main}?{}", query.split('#').next().unwrap_or(query)) };
        return ("LINK", shown, Some(t.to_string()));
    }
    if lower.starts_with("wifi:") {
        return ("WI-FI", format!("Network \u{201c}{}\u{201d}", wifi_ssid(&t[5..]).unwrap_or_else(|| "?".into())), None);
    }
    for (prefix, kind) in [("mailto:", "EMAIL"), ("tel:", "PHONE"), ("geo:", "PLACE")] {
        if lower.starts_with(prefix) {
            return (kind, t[prefix.len()..].trim().to_string(), None);
        }
    }
    for (prefix, kind) in [("begin:vcard", "CONTACT"), ("begin:vevent", "EVENT")] {
        if lower.starts_with(prefix) {
            return (kind, t.to_string(), None);
        }
    }
    ("TEXT", t.split_whitespace().collect::<Vec<_>>().join(" "), None)
}

/// the S: field of a WIFI: code (fields split by unescaped ';', '\' escapes)
fn wifi_ssid(fields: &str) -> Option<String> {
    let mut field = String::new();
    let mut chars = fields.chars();
    let mut out = None;
    let mut done = |f: &str| {
        if out.is_none() && (f.starts_with("S:") || f.starts_with("s:")) {
            out = Some(f[2..].to_string());
        }
    };
    while let Some(c) = chars.next() {
        match c {
            '\\' => {
                if let Some(n) = chars.next() {
                    field.push(n);
                }
            }
            ';' => {
                done(&field);
                field.clear();
            }
            _ => field.push(c),
        }
    }
    done(&field);
    out
}

#[cfg(test)]
mod tests {
    use super::describe;

    #[test]
    fn kinds() {
        let (k, s, u) = describe("https://example.org/a/b/?x=1#frag");
        assert_eq!((k, s.as_str(), u.as_deref()), ("LINK", "example.org/a/b?x=1", Some("https://example.org/a/b/?x=1#frag")));
        let (k, s, u) = describe(r"WIFI:T:WPA;S:my\;net;P:secret;;");
        assert_eq!((k, s.as_str(), u), ("WI-FI", "Network \u{201c}my;net\u{201d}", None));
        let (k, s, _) = describe("mailto:someone@example.org");
        assert_eq!((k, s.as_str()), ("EMAIL", "someone@example.org"));
        let (k, s, _) = describe("  hello\n  world ");
        assert_eq!((k, s.as_str()), ("TEXT", "hello world"));
        assert_eq!(describe("http://a b").0, "TEXT");
    }
}
