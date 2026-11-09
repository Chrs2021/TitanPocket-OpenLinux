//! titan-camera: the camera app for the Titan Pocket in Rust (GTK 4 + GStreamer), replacing the
//! Python titan-camera-gui. Preview, photos and video on the hardware ISP (docs/camera.md).
//!
//! - Preview: the driver's YUYV (MediaTek's pass-2 ISP), rotated and scaled on the GPU.
//! - Photo: the camera restarts at the chosen size (the sensor's full or binned mode); after
//!   the settle frames the sharpest of a short burst is kept, rotated and saved as JPEG in
//!   ~/Pictures/Camera.
//! - Video: H.264 (the best encoder installed) + Opus sound, in ~/Videos/Camera.
//! - Settings: photo and video size, shutter speed and ISO. Fixing either takes exposure over
//!   from the driver: shutter priority or ISO priority on the preview and in video; photos may
//!   use shutter speeds down to 1 s (the frame stretches for them).
//! - Autofocus (rear): contrast detection on the preview, tap to focus, a manual focus slider.
//! - Flash (rear): Off, Auto, On, Torch; hold the button for the tone (Neutral, Warm, Cool).
//!   A flash photo is metered with a pre-flash first (meter.rs), then taken with a strobe
//!   pulse at the metered exposure.
//! - QR codes, found passively in the preview: a chip each (Copy, Open for links), a short buzz
//!   for a new one; nothing opens on its own.
//!
//! Keys: Space/Enter shutter, V photo/video, F switch camera, R rotate, T flash, C tone,
//! A autofocus, S settings, Escape closes the settings, else exits (as the back button does).
mod af;
mod camera;
mod exposure;
mod flash;
mod meter;
mod pipeline;
mod qr;
mod settings;

use camera::{Camera, Sensor};
use gtk::prelude::*;
use gtk::{gdk, glib};
use pipeline::{Msg, Preview, Record, Still, StillPlan, Tx};
use settings::*;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const THEME: &str = include_str!("../../titan-messages/src/theme.css");
const CSS: &str = "
.viewer { background: black; }
.bar { background: #0B0D12; padding: 8px 12px; }
.thumb { min-width: 60px; min-height: 60px; padding: 0; border-radius: 16px; }
.shutter { min-width: 72px; min-height: 72px; border-radius: 40px; padding: 0;
           background: radial-gradient(circle, #FFFFFF 55%, rgba(255,255,255,0.35) 58%); }
.shutter.video { background: radial-gradient(circle, #E5484D 48%, rgba(255,255,255,0.35) 52%); }
.shutter.recording { background: radial-gradient(circle, #E5484D 30%, rgba(255,255,255,0.35) 34%); }
.status { color: white; background-color: rgba(18, 24, 48, 0.55);
          border: 1px solid rgba(255,255,255,0.40); border-radius: 20px; padding: 6px 16px; font-size: 18px; }
.chip { color: #141821; background: #FFD24A; border-radius: 7px; padding: 1px 7px;
        font-size: 14px; font-weight: bold; text-shadow: none; }
.chip.dim { background: rgba(255, 255, 255, 0.80); }
.rec { color: white; background: #E5484D; border-radius: 7px; padding: 1px 8px; font-size: 15px; font-weight: bold; }
.panel { background: rgba(11, 13, 18, 0.92); padding: 10px 12px; border-radius: 18px 18px 0 0; }
.panel-label { color: @titan_dim; font-size: 13px; }
.opt { min-height: 38px; padding: 0 8px; font-size: 14px; }
.qr { background-color: rgba(18, 24, 48, 0.72); border-radius: 24px; padding: 5px 6px 5px 12px;
      border: 1px solid rgba(255,255,255,0.40);
      box-shadow: inset 0 1px 0 rgba(255,255,255,0.45), 0 6px 18px rgba(0,0,0,0.35); }
.qr.fresh { border-color: #FFD24A; }
.qrkind { color: #141821; background: #FFD24A; border-radius: 6px; padding: 0 6px;
          font-size: 13px; font-weight: bold; text-shadow: none; }
.qrtext { color: white; font-size: 16px; }
.qr button { min-height: 42px; }
";
const QR_MAX: usize = 3; // QR chips kept on screen
const QR_FRESH_MS: u64 = 1200; // a chip stays outlined this long after its code was seen
const QR_BUZZ: (u32, u32, u32) = (40, 90, 40); // ms on, off, on: a new QR code
const VIBRATE: &str = "/usr/local/bin/titan-vibrate";
const BROWSER: &str = "/usr/local/bin/titan-chromium";
const PHOTO_SETTLE: u32 = 8; // the still starts at the preview's exposure and white balance
const MANUAL_SETTLE: u32 = 4; // frames after a manual exposure reaches the sensor
const BURST: u32 = 6;
const LONG_S: f64 = 1.0 / 15.0; // longer: one frame, no burst (on a support, presumably)
const AUTO_FLASH_GAIN: i32 = 4 * camera::GAIN_UNIT; // auto exposure at 4x or more: dark
const FLASH_SETTLE: u32 = 6; // still frames after the strobe fires (the first frame) and the metered exposure
const PHOTO_MAX_US: f64 = 10000.0; // shake: longer exposures blur; then gain (as the driver's AE)

/// the flash for a photo
enum FlashPlan {
    None,
    /// a continuous light through the capture (no metering possible)
    Torch,
    /// a strobe pulse at `level` on the first frame, at the metered exposure (lines, gain)
    Strobe(u32, (f64, f64)),
}

fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/root".into()))
}

struct App {
    window: gtk::ApplicationWindow,
    stack: gtk::Stack,
    view: gtk::Picture,
    photo: gtk::Picture,
    status: gtk::Label,
    chip: gtk::Label,
    rec: gtk::Label,
    shutter: gtk::Button,
    thumb: gtk::Button,
    flash_btn: gtk::Button,
    panel: gtk::Revealer,
    focus_row: gtk::Box,
    focus: gtk::Scale,
    expo_label: gtk::Label,
    qr_box: gtk::Box,
    /// code text -> its chip and the timer that ends its outline
    qr_chips: RefCell<HashMap<String, (gtk::Box, Rc<RefCell<Option<glib::SourceId>>>)>>,

    cams: Vec<Camera>,
    st: RefCell<Settings>,
    flash: RefCell<flash::Flash>,
    af: RefCell<af::AutoFocus>,
    af_region: Arc<Mutex<(f64, f64)>>,
    preview: RefCell<Option<Preview>>,
    still: RefCell<Option<Still>>,
    sensor: RefCell<Option<Sensor>>,
    expo: RefCell<Option<exposure::Exposure>>,
    meter: RefCell<Option<meter::Meter>>,
    gen: Cell<u64>,
    busy: Cell<bool>,
    rec_start: Cell<Option<Instant>>,
    started: Instant,
    status_timer: Rc<RefCell<Option<glib::SourceId>>>,
    tx: Tx,
}

impl App {
    fn cam(&self) -> &Camera {
        &self.cams[self.st.borrow().camera]
    }

    fn rotation(&self) -> u32 {
        let st = self.st.borrow();
        st.rotation[st.camera]
            .or_else(|| self.sensor.borrow().as_ref().and_then(|s| s.rotation()))
            .unwrap_or(self.cam().rotation)
    }

    fn set_status(&self, text: Option<&str>, ms: u32) {
        if let Some(id) = self.status_timer.borrow_mut().take() {
            id.remove();
        }
        match text {
            Some(t) => {
                self.status.set_text(t);
                self.status.set_visible(true);
            }
            None => self.status.set_visible(false),
        }
        if ms > 0 {
            let s = self.status.clone();
            let timer = self.status_timer.clone();
            *self.status_timer.borrow_mut() = Some(glib::timeout_add_local_once(Duration::from_millis(ms as u64), move || {
                timer.borrow_mut().take(); // fired: nothing left to remove
                s.set_visible(false);
            }));
        }
    }

    fn save(&self) {
        self.st.borrow().save();
    }

    fn clock_s(&self) -> f64 {
        self.started.elapsed().as_secs_f64()
    }

    // ---- preview ----

    fn start_preview(self: &Rc<Self>, record: Option<Record>, refocus: bool) {
        self.stop_preview();
        let cam = self.cam().clone();
        *self.sensor.borrow_mut() = Sensor::open(&cam);
        let rot = self.rotation();
        let gen = self.gen.get() + 1;
        self.gen.set(gen);
        let dir = camera::video_direction(rot, cam.mirror);
        let recording = record.is_some();
        // the ISP's scaled-down preview, unless a recording needs more (no upscaling)
        let size = match &record {
            Some(r) if r.w.max(r.h) > cam.preview.0.max(cam.preview.1) => cam.binned,
            _ => cam.preview,
        };
        match Preview::new(&cam.id, size, dir, rot % 180 != 0, record, gen, self.tx.clone(), self.af_region.clone()) {
            Ok(p) => {
                if !p.start() {
                    self.set_status(Some("Camera not available"), 0);
                    return;
                }
                *self.preview.borrow_mut() = Some(p);
            }
            Err(e) => {
                self.set_status(Some(&e), 4000);
                if recording {
                    self.start_preview(None, false); // back to plain preview
                }
                return;
            }
        }
        *self.expo.borrow_mut() = None; // set from the sensor on the first measurement
        self.focus_row.set_visible(cam.focus);
        self.show_flash();
        self.apply_torch();
        if cam.focus {
            self.set_lens(self.st.borrow().focus);
            if refocus && self.af.borrow().state != af::State::Manual {
                let me = self.clone();
                glib::timeout_add_local_once(Duration::from_millis(400), move || me.autofocus(None));
            }
        }
        self.show_chip();
    }

    fn stop_preview(&self) {
        if let Some(p) = self.preview.borrow_mut().take() {
            p.stop();
        }
        let mut af = self.af.borrow_mut();
        if af.state != af::State::Manual {
            af.state = af::State::Off;
        }
    }

    fn running_time(&self) -> u64 {
        self.preview.borrow().as_ref().map(|p| p.running_time()).unwrap_or(0)
    }

    // ---- focus ----

    fn set_lens(&self, pos: i32) {
        if let Some(s) = self.sensor.borrow().as_ref() {
            s.set(camera::CID_FOCUS_ABSOLUTE, pos);
        }
    }

    fn autofocus(self: &Rc<Self>, region: Option<(f64, f64)>) {
        if !self.cam().focus || self.busy.get() || self.preview.borrow().is_none() {
            return;
        }
        if let Some(r) = region {
            *self.af_region.lock().unwrap() = r;
        }
        let now = self.running_time();
        let act = self.af.borrow_mut().start(region, now);
        self.af_action(act);
        self.set_status(Some("Focusing…"), 0);
    }

    fn af_action(self: &Rc<Self>, act: af::Action) {
        match act {
            af::Action::Move(p) => self.set_lens(p),
            af::Action::Done(p) => {
                let pos = self.af.borrow().pos;
                self.set_lens(pos);
                if let Some(p) = p {
                    self.st.borrow_mut().focus = p;
                    self.focus.set_value(p as f64);
                    self.save();
                    self.set_status(None, 0);
                } else {
                    self.set_status(Some("Nothing to focus on"), 1500);
                }
                self.show_chip();
            }
            af::Action::Refocus => self.autofocus(None),
        }
    }

    fn show_chip(&self) {
        let st = self.st.borrow();
        let mut parts = Vec::new();
        if self.cam().focus {
            parts.push(if self.af.borrow().state == af::State::Manual { "MF" } else { "AF" });
        }
        let exp = SHUTTERS[st.shutter].0;
        let iso = ISOS[st.iso].0;
        let text = format!(
            "{}{}{}",
            parts.join(" "),
            if exp != "Auto" { format!("  {exp}") } else { String::new() },
            if iso != "Auto" { format!("  ISO {iso}") } else { String::new() }
        );
        self.chip.set_text(text.trim());
        self.chip.set_visible(!text.trim().is_empty());
        if self.af.borrow().state == af::State::Manual {
            self.chip.add_css_class("dim");
        } else {
            self.chip.remove_css_class("dim");
        }
    }

    /// tap on the preview: focus there (mapped back to the unrotated frame)
    fn on_tap(self: &Rc<Self>, x: f64, y: f64) {
        let Some(p) = self.view.paintable() else { return };
        let (ww, wh) = (self.view.width() as f64, self.view.height() as f64);
        let (pw, ph) = (p.intrinsic_width() as f64, p.intrinsic_height() as f64);
        if pw <= 0.0 || ph <= 0.0 {
            return;
        }
        let k = (ww / pw).min(wh / ph);
        let (dw, dh) = (pw * k, ph * k);
        let (u, v) = ((x - (ww - dw) / 2.0) / dw, (y - (wh - dh) / 2.0) / dh);
        if !(0.0..=1.0).contains(&u) || !(0.0..=1.0).contains(&v) {
            return;
        }
        let u = if self.cam().mirror { 1.0 - u } else { u };
        let r = match self.rotation() {
            90 => (v, 1.0 - u),
            180 => (1.0 - u, 1.0 - v),
            270 => (1.0 - v, u),
            _ => (u, v),
        };
        self.autofocus(Some(r));
    }

    // ---- exposure ----

    /// the first measurement of a stream: take the driver's exposure as the start, then set the
    /// manual values; later ones step shutter or ISO priority
    fn on_luma(&self, luma: f64) {
        let st = self.st.borrow().clone();
        if !st.manual_exposure() {
            return;
        }
        let sensor = self.sensor.borrow();
        let Some(s) = sensor.as_ref() else { return };
        let (w, h) = self.cam().binned;
        let line = s.line_us(w);
        let mut expo = self.expo.borrow_mut();
        if expo.is_none() {
            let lines = s.get(camera::CID_EXPOSURE).unwrap_or(1000) as f64;
            let gain = s.get(camera::CID_ANALOGUE_GAIN).unwrap_or(camera::GAIN_UNIT) as f64 / camera::GAIN_UNIT as f64;
            *expo = Some(exposure::Exposure::new(lines * line, gain));
        }
        let e = expo.as_mut().unwrap();
        let fixed_us = st.stream_shutter().map(|s| s * 1e6);
        if let Some((us, gain)) = e.step(luma, fixed_us, ISOS[st.iso].1, STREAM_MAX_S * 1e6, camera::GAIN_MAX) {
            let set = s.set_exposure(h, w, us, gain);
            self.expo_label.set_text(&format!("1/{:.0} s · ISO {:.0}", 1e6 / set.max(1.0), gain * 100.0));
        }
    }

    // ---- flash ----

    fn show_flash(&self) {
        let st = self.st.borrow();
        self.flash_btn.set_visible(self.cam().focus && flash::Flash::present());
        let mut label = FLASH_MODES[st.flash].to_string();
        if st.flash != 0 && st.tone != 0 {
            label += &format!(" {}", FLASH_TONES[st.tone].to_lowercase());
        }
        self.flash_btn.set_label(&label);
    }

    fn apply_torch(&self) {
        let torch = self.st.borrow().flash == 3 && self.cam().focus;
        let mut f = self.flash.borrow_mut();
        if torch && !f.lit() {
            f.torch(flash::TORCH_LEVEL, 0);
        } else if !torch && !self.busy.get() {
            f.off();
        }
    }

    fn cycle_flash(&self) {
        if self.busy.get() {
            return;
        }
        {
            let mut st = self.st.borrow_mut();
            st.flash = (st.flash + 1) % FLASH_MODES.len();
        }
        self.save();
        self.apply_torch();
        self.show_flash();
    }

    fn cycle_tone(&self) {
        let tone = {
            let mut st = self.st.borrow_mut();
            st.tone = (st.tone + 1) % FLASH_TONES.len();
            st.tone
        };
        self.save();
        let mut f = self.flash.borrow_mut();
        f.tone = tone;
        if f.lit() && self.st.borrow().flash == 3 {
            f.torch(flash::TORCH_LEVEL, 0);
        }
        drop(f);
        self.show_flash();
    }

    // ---- shutter: photo or video ----

    fn shutter_pressed(self: &Rc<Self>) {
        if self.st.borrow().video {
            self.toggle_recording();
        } else {
            self.take_photo();
        }
    }

    fn take_photo(self: &Rc<Self>) {
        if self.busy.get() || self.rec_start.get().is_some() {
            return;
        }
        let st = self.st.borrow().clone();
        let cam = self.cam().clone();
        let dark = self
            .sensor
            .borrow()
            .as_ref()
            .and_then(|s| s.get(camera::CID_ANALOGUE_GAIN))
            .map(|g| g >= AUTO_FLASH_GAIN)
            .unwrap_or(false);
        let use_flash = cam.focus && flash::Flash::present() && (st.flash == 2 || (st.flash == 1 && dark));
        // Torch mode keeps its light; a fixed exposure keeps its values: neither is metered
        if use_flash && st.flash != 3 && !st.manual_exposure() && self.meter_start() {
            return; // capture() follows the metering
        }
        self.capture(if use_flash && st.flash != 3 { FlashPlan::Torch } else { FlashPlan::None });
    }

    // ---- flash metering (meter.rs) ----

    /// pre-flash: the preview at a short manual exposure, the flash on; false when there are
    /// no statistics (then the flash is a continuous light)
    fn meter_start(self: &Rc<Self>) -> bool {
        let cam = self.cam().clone();
        let Some(amb) = meter::read_stats(&cam.sysdir.join("af_stats")) else { return false };
        let (lines, gain, line) = {
            let sensor = self.sensor.borrow();
            let Some(s) = sensor.as_ref() else { return false };
            let (Some(e), Some(g)) = (s.get(camera::CID_EXPOSURE), s.get(camera::CID_ANALOGUE_GAIN)) else { return false };
            let line = s.line_us(cam.binned.0);
            s.set_exposure(cam.binned.1, cam.binned.0, meter::START_US, 1.0);
            (e as f64, g as f64 / camera::GAIN_UNIT as f64, line)
        };
        let mut m = meter::Meter::new(&amb, lines, gain, line, flash::PHOTO_LEVEL, (PHOTO_MAX_US / line).floor(), camera::GAIN_MAX);
        m.exposing(meter::START_US);
        *self.meter.borrow_mut() = Some(m);
        self.busy.set(true);
        self.shutter.set_sensitive(false);
        self.set_status(Some("Metering\u{2026}"), 0);
        self.flash.borrow_mut().torch(meter::PRE_LEVEL, meter::TIMEOUT_MS as u32 + 1000);
        let me = self.clone();
        glib::timeout_add_local(Duration::from_millis(15), move || me.meter_tick());
        true
    }

    fn meter_tick(self: &Rc<Self>) -> glib::ControlFlow {
        let cam = self.cam().clone();
        let st = meter::read_stats(&cam.sysdir.join("af_stats"));
        let step = {
            let mut mb = self.meter.borrow_mut();
            let Some(m) = mb.as_mut() else { return glib::ControlFlow::Break };
            let has = self
                .sensor
                .borrow()
                .as_ref()
                .map(|s| {
                    let e = s.get(camera::CID_EXPOSURE).unwrap_or(-100) as f64;
                    (e - m.lines()).abs() <= 1.0 && s.get(camera::CID_ANALOGUE_GAIN) == Some(camera::GAIN_UNIT)
                })
                .unwrap_or(false);
            m.tick(st.as_ref(), has)
        };
        match step {
            meter::Step::Wait => glib::ControlFlow::Continue,
            meter::Step::Expose(us) => {
                if let Some(s) = self.sensor.borrow().as_ref() {
                    s.set_exposure(cam.binned.1, cam.binned.0, us, 1.0);
                }
                if let Some(m) = self.meter.borrow_mut().as_mut() {
                    m.exposing(us);
                }
                glib::ControlFlow::Continue
            }
            meter::Step::Dim(level) => {
                self.flash.borrow_mut().torch(level, meter::TIMEOUT_MS as u32 + 1000);
                glib::ControlFlow::Continue
            }
            meter::Step::Done(level, exposure) => {
                if let Some(m) = self.meter.borrow_mut().take() {
                    eprintln!("meter: {}", m.log);
                }
                self.flash.borrow_mut().off();
                // the preview stops for the still; the next one starts on auto exposure
                self.busy.set(false);
                self.capture(match exposure {
                    Some(e) => FlashPlan::Strobe(level, e),
                    None => FlashPlan::Torch,
                });
                glib::ControlFlow::Break
            }
        }
    }

    fn capture(self: &Rc<Self>, plan: FlashPlan) {
        let st = self.st.borrow().clone();
        let cam = self.cam().clone();
        // before the preview stops: the scene's exposure (exposure x gain)
        let (prod_us, line) = match self.sensor.borrow().as_ref() {
            Some(s) => {
                let lines = s.get(camera::CID_EXPOSURE).unwrap_or(1000) as f64;
                let g = s.get(camera::CID_ANALOGUE_GAIN).unwrap_or(camera::GAIN_UNIT);
                let line = s.line_us(cam.binned.0);
                (lines * line * g as f64 / camera::GAIN_UNIT as f64, line)
            }
            None => (10000.0, camera::FALLBACK_LINE_US),
        };
        let size = if st.photo_size == 0 { cam.full } else { cam.binned };
        let shutter = SHUTTERS[st.shutter].1;
        let iso = ISOS[st.iso].1;
        // manual: the fixed value(s), the other from the scene's exposure; or the metered one
        let manual = match plan {
            FlashPlan::Strobe(_, (lines, gain)) => Some((lines * line, gain)),
            _ => st.manual_exposure().then(|| {
                let us = shutter.map(|s| s * 1e6).unwrap_or_else(|| (prod_us / iso.unwrap_or(1.0)).clamp(40.0, 1e6));
                let gain = iso.unwrap_or_else(|| (prod_us / us).clamp(1.0, camera::GAIN_MAX));
                (us, gain)
            }),
        };
        let long = shutter.map(|s| s > LONG_S).unwrap_or(false) && !matches!(plan, FlashPlan::Strobe(..));
        // the strobe fires on the first frame (its pulse lasts flash::PULSE_MS); dim levels
        // under the strobe's floor are a short torch, lit before the still starts
        let strobe = match plan {
            FlashPlan::Strobe(level, _) if flash::strobe_can(st.tone, level) => Some((st.tone, level)),
            _ => None,
        };
        if manual.is_some() {
            // set before the stream starts too: the driver applies it at stream on
            if let (Some(s), Some((us, gain))) = (Sensor::open(&cam), manual) {
                s.set_exposure(size.1, size.0, us, gain);
            }
        }
        let plan_first: Option<Box<dyn FnOnce() + Send>> = if manual.is_some() || strobe.is_some() {
            let c = cam.clone();
            Some(Box::new(move || {
                if let Some((tone, level)) = strobe {
                    flash::strobe(tone, level);
                }
                if let (Some(s), Some((us, gain))) = (Sensor::open(&c), manual) {
                    s.set_exposure(size.1, size.0, us, gain);
                }
            }) as Box<dyn FnOnce() + Send>)
        } else {
            None
        };
        let plan_settle = match plan {
            FlashPlan::Strobe(..) => FLASH_SETTLE,
            _ if manual.is_some() => MANUAL_SETTLE,
            _ => PHOTO_SETTLE,
        };
        let still_plan = StillPlan { settle: plan_settle, burst: if long { 1 } else { BURST }, on_first: plan_first };
        let region = if cam.focus { *self.af_region.lock().unwrap() } else { (0.5, 0.5) };
        self.busy.set(true);
        self.shutter.set_sensitive(false);
        self.set_status(Some(if long { "Long exposure \u{2014} hold still\u{2026}" } else { "Taking photo\u{2026}" }), 0);
        self.stop_preview();
        match plan {
            FlashPlan::Torch => {
                self.flash.borrow_mut().torch(flash::TORCH_MAX, 8000);
            }
            FlashPlan::Strobe(level, _) => {
                if strobe.is_some() {
                    self.flash.borrow_mut().strobing();
                } else {
                    self.flash.borrow_mut().torch(level, 4000);
                }
            }
            FlashPlan::None => {}
        }
        let gen = self.gen.get() + 1;
        self.gen.set(gen);
        match Still::new(&cam.id, size, still_plan, region, gen, self.tx.clone()) {
            Ok(s) if s.start() => {
                *self.still.borrow_mut() = Some(s);
                let frames = PHOTO_SETTLE + BURST + 4;
                let secs = 10 + (shutter.unwrap_or(0.0) * frames as f64) as u32;
                let me = self.clone();
                glib::timeout_add_seconds_local_once(secs, move || {
                    if me.busy.get() && me.gen.get() == gen {
                        me.finish_photo(Err("Photo timed out".into()));
                    }
                });
            }
            Ok(_) => self.finish_photo(Err("Camera not available".into())),
            Err(e) => self.finish_photo(Err(e)),
        }
    }

    fn still_done(self: &Rc<Self>, f: pipeline::Frame) {
        if let Some(s) = self.still.borrow_mut().take() {
            s.stop();
        }
        if self.st.borrow().flash != 3 {
            self.flash.borrow_mut().off();
        }
        self.set_status(Some("Saving…"), 0);
        let dir = camera::video_direction(self.rotation(), false);
        pipeline::save_jpeg(f, dir, home().join("Pictures/Camera"), self.tx.clone());
    }

    fn finish_photo(self: &Rc<Self>, r: Result<PathBuf, String>) {
        if !self.busy.get() {
            return;
        }
        if let Some(s) = self.still.borrow_mut().take() {
            s.stop();
        }
        if self.st.borrow().flash != 3 {
            self.flash.borrow_mut().off();
        }
        self.busy.set(false);
        self.shutter.set_sensitive(true);
        match r {
            Ok(p) => {
                self.set_status(Some(&format!("Saved {}", p.file_name().unwrap().to_string_lossy())), 1500);
                self.st.borrow_mut().last = Some(p);
                self.save();
                self.update_thumb();
            }
            Err(e) => self.set_status(Some(&e), 3000),
        }
        let manual = self.af.borrow().state == af::State::Manual;
        self.start_preview(None, false);
        if self.cam().focus && !manual {
            let now = self.running_time();
            self.af.borrow_mut().resume(now);
        }
    }

    fn toggle_recording(self: &Rc<Self>) {
        if self.busy.get() {
            return;
        }
        if self.rec_start.get().is_some() {
            if let Some(p) = self.preview.borrow().as_ref() {
                p.finish(); // Msg::Recorded follows when the file is closed
            }
            self.shutter.set_sensitive(false);
            self.set_status(Some("Saving video…"), 0);
            return;
        }
        let st = self.st.borrow().clone();
        let (_, long, short, kbps) = VIDEO_SIZES[st.video_size];
        if pipeline::video_encoder(kbps, 30).is_none() {
            self.set_status(Some("No H.264 encoder (install gstreamer1.0-plugins-ugly)"), 4000);
            return;
        }
        let folder = home().join("Videos/Camera");
        if let Err(e) = std::fs::create_dir_all(&folder) {
            self.set_status(Some(&format!("Can't save: {e}")), 3000);
            return;
        }
        let (w, h) = if self.rotation() % 180 != 0 { (short, long) } else { (long, short) };
        let path = pipeline::new_path(&folder, "VID", pipeline::record_ext());
        self.start_preview(Some(Record { path, w, h, kbps }), false);
        if self.preview.borrow().as_ref().map(|p| p.recording.is_some()).unwrap_or(false) {
            self.rec_start.set(Some(Instant::now()));
            self.shutter.add_css_class("recording");
            self.rec.set_visible(true);
            self.tick_rec();
        }
    }

    fn tick_rec(self: &Rc<Self>) {
        let Some(t) = self.rec_start.get() else { return };
        let s = t.elapsed().as_secs();
        self.rec.set_text(&format!("● {:02}:{:02}", s / 60, s % 60));
        let me = self.clone();
        glib::timeout_add_local_once(Duration::from_millis(500), move || me.tick_rec());
    }

    fn recorded(self: &Rc<Self>, r: Result<PathBuf, String>) {
        self.rec_start.set(None);
        self.rec.set_visible(false);
        self.shutter.remove_css_class("recording");
        self.shutter.set_sensitive(true);
        match r {
            Ok(p) => {
                self.set_status(Some(&format!("Saved {}", p.file_name().unwrap().to_string_lossy())), 1500);
                self.st.borrow_mut().last = Some(p);
                self.save();
                self.update_thumb();
            }
            Err(e) => self.set_status(Some(&format!("Recording failed: {e}")), 4000),
        }
        self.start_preview(None, false);
    }

    // ---- camera, rotation, mode ----

    fn switch_camera(self: &Rc<Self>) {
        if self.busy.get() || self.rec_start.get().is_some() {
            return;
        }
        self.stop_preview();
        {
            let mut st = self.st.borrow_mut();
            st.camera = (st.camera + 1) % self.cams.len();
        }
        self.save();
        self.set_status(Some(&format!("{} camera", self.cam().label)), 1200);
        self.start_preview(None, true);
    }

    fn rotate(self: &Rc<Self>) {
        if self.busy.get() || self.rec_start.get().is_some() {
            return;
        }
        let r = (self.rotation() + 90) % 360;
        {
            let mut st = self.st.borrow_mut();
            let c = st.camera;
            st.rotation[c] = Some(r);
        }
        self.save();
        self.start_preview(None, false);
    }

    fn set_video(&self, video: bool) {
        if self.rec_start.get().is_some() {
            return;
        }
        self.st.borrow_mut().video = video;
        self.save();
        if video {
            self.shutter.add_css_class("video");
        } else {
            self.shutter.remove_css_class("video");
        }
    }

    /// shutter or ISO changed: back to the driver's auto exposure needs a new stream
    fn exposure_changed(self: &Rc<Self>, was_manual: bool) {
        self.save();
        self.show_chip();
        let manual = self.st.borrow().manual_exposure();
        self.expo_label.set_visible(manual);
        if was_manual && !manual && self.rec_start.get().is_none() && !self.busy.get() {
            self.start_preview(None, false);
        } else {
            *self.expo.borrow_mut() = None;
        }
    }

    // ---- thumbnail and viewer ----

    fn update_thumb(&self) {
        let last = self.st.borrow().last.clone().filter(|p| p.exists());
        let child: gtk::Widget = match &last {
            Some(p) if p.extension().map(|e| e == "jpg").unwrap_or(false) => {
                match gtk::gdk_pixbuf::Pixbuf::from_file_at_scale(p, 60, 60, true) {
                    Ok(pix) => {
                        let pic = gtk::Picture::for_paintable(&gdk::Texture::for_pixbuf(&pix));
                        pic.set_content_fit(gtk::ContentFit::Cover);
                        pic.set_size_request(60, 60);
                        pic.set_overflow(gtk::Overflow::Hidden);
                        pic.upcast()
                    }
                    Err(_) => gtk::Image::from_icon_name("image-x-generic-symbolic").upcast(),
                }
            }
            Some(_) => gtk::Image::from_icon_name("video-x-generic-symbolic").upcast(),
            None => gtk::Image::from_icon_name("image-x-generic-symbolic").upcast(),
        };
        self.thumb.set_child(Some(&child));
        self.thumb.set_sensitive(last.is_some());
    }

    fn open_last(self: &Rc<Self>) {
        if self.busy.get() || self.rec_start.get().is_some() {
            return;
        }
        let Some(p) = self.st.borrow().last.clone() else { return };
        if p.extension().map(|e| e != "jpg").unwrap_or(true) {
            self.set_status(Some(&format!("Video: {}", p.display())), 2500);
            return;
        }
        let (w, h) = (self.window.width().max(1), self.window.height().max(1));
        if let Ok(pix) = gtk::gdk_pixbuf::Pixbuf::from_file_at_scale(&p, w, h, true) {
            self.photo.set_paintable(Some(&gdk::Texture::for_pixbuf(&pix)));
            self.stop_preview();
            self.stack.set_visible_child_name("photo");
        }
    }

    fn show_camera(self: &Rc<Self>) {
        self.stack.set_visible_child_name("camera");
        self.start_preview(None, false);
    }

    // ---- messages from the pipelines ----

    // ---- QR codes ----

    fn qr_found(self: &Rc<Self>, text: String) {
        let existing = self.qr_chips.borrow().get(&text).cloned();
        let (chip, timer) = match existing {
            Some(c) => c,
            None => {
                let c = (self.qr_chip(&text), Rc::new(RefCell::new(None)));
                self.qr_box.prepend(&c.0); // newest at the top
                self.qr_chips.borrow_mut().insert(text.clone(), c.clone());
                while self.qr_chips.borrow().len() > QR_MAX {
                    // the oldest: the last child
                    let Some(last) = self.qr_box.last_child() else { break };
                    let key = self.qr_chips.borrow().iter().find(|(_, (b, _))| b.upcast_ref::<gtk::Widget>() == &last).map(|(k, _)| k.clone());
                    match key {
                        Some(k) => self.qr_dismiss(&k),
                        None => break,
                    }
                }
                buzz();
                c
            }
        };
        chip.add_css_class("fresh");
        if let Some(id) = timer.borrow_mut().take() {
            id.remove();
        }
        let (c, t) = (chip.clone(), timer.clone());
        *timer.borrow_mut() = Some(glib::timeout_add_local_once(Duration::from_millis(QR_FRESH_MS), move || {
            t.borrow_mut().take();
            c.remove_css_class("fresh");
        }));
    }

    fn qr_chip(self: &Rc<Self>, text: &str) -> gtk::Box {
        let (kind, shown, url) = qr::describe(text);
        let chip = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        chip.add_css_class("qr");
        let badge = label(&format!("QR \u{b7} {kind}"), "qrkind");
        badge.set_valign(gtk::Align::Center);
        chip.append(&badge);
        let lab = label(&shown, "qrtext");
        lab.set_wrap(true);
        lab.set_wrap_mode(gtk::pango::WrapMode::WordChar);
        lab.set_lines(2);
        lab.set_ellipsize(gtk::pango::EllipsizeMode::End);
        lab.set_xalign(0.0);
        lab.set_hexpand(true);
        lab.set_tooltip_text(Some(text));
        chip.append(&lab);
        if let Some(url) = url {
            let b = gtk::Button::with_label("Open");
            b.add_css_class("suggested-action");
            b.set_valign(gtk::Align::Center);
            let a = self.clone();
            b.connect_clicked(move |_| a.qr_open(&url));
            chip.append(&b);
        }
        let b = gtk::Button::with_label("Copy");
        b.set_valign(gtk::Align::Center);
        let (a, t) = (self.clone(), text.to_string());
        b.connect_clicked(move |_| {
            a.window.clipboard().set_text(&t);
            a.set_status(Some("Copied"), 1000);
        });
        chip.append(&b);
        let b = gtk::Button::from_icon_name("window-close-symbolic");
        b.set_valign(gtk::Align::Center);
        let (a, t) = (self.clone(), text.to_string());
        b.connect_clicked(move |_| a.qr_dismiss(&t));
        chip.append(&b);
        chip
    }

    fn qr_dismiss(&self, text: &str) {
        if let Some((chip, timer)) = self.qr_chips.borrow_mut().remove(text) {
            if let Some(id) = timer.borrow_mut().take() {
                id.remove();
            }
            self.qr_box.remove(&chip);
        }
    }

    fn qr_open(&self, url: &str) {
        match std::process::Command::new(BROWSER)
            .arg(url)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
        {
            Ok(mut c) => {
                std::thread::spawn(move || c.wait());
                self.set_status(Some("Opening link\u{2026}"), 1500);
            }
            Err(e) => self.set_status(Some(&format!("Can't open: {e}")), 2500),
        }
    }

    fn on_msg(self: &Rc<Self>, m: Msg) {
        let gen = self.gen.get();
        match m {
            Msg::View(g, f) => {
                if g != gen {
                    return;
                }
                let bytes = glib::Bytes::from_owned(f.data);
                let tex = gdk::MemoryTexture::new(f.w as i32, f.h as i32, gdk::MemoryFormat::R8g8b8a8, &bytes, f.stride);
                self.view.set_paintable(Some(&tex));
                if let Some(p) = self.preview.borrow().as_ref() {
                    p.view_shown();
                }
            }
            Msg::Qr(g, text) => {
                if g == gen && !self.busy.get() {
                    self.qr_found(text);
                }
            }
            Msg::Measure(g, pts, sharp, luma) => {
                if g != gen || self.busy.get() {
                    return;
                }
                self.on_luma(luma);
                let clock = self.clock_s();
                let act = self.af.borrow_mut().sample(pts, sharp, clock);
                if let Some(a) = act {
                    self.af_action(a);
                }
            }
            Msg::Still(g, f) => {
                if g == gen && self.busy.get() {
                    self.still_done(f);
                }
            }
            Msg::Saved(r) => self.finish_photo(r),
            Msg::Recorded(g, r) => {
                if g == gen {
                    self.recorded(r);
                }
            }
            Msg::Error(g, e) => {
                if g != gen {
                    return;
                }
                if self.busy.get() {
                    self.finish_photo(Err(format!("Photo failed: {e}")));
                } else {
                    self.set_status(Some(&format!("Camera: {e}")), 4000);
                }
            }
        }
    }
}

/// the vibration motor: on, off, on (QR_BUZZ), without blocking
fn buzz() {
    let pulse = |ms: u32| {
        if let Ok(mut c) = std::process::Command::new(VIBRATE).arg(ms.to_string()).spawn() {
            std::thread::spawn(move || c.wait());
        }
    };
    pulse(QR_BUZZ.0);
    glib::timeout_add_local_once(Duration::from_millis((QR_BUZZ.0 + QR_BUZZ.1) as u64), move || pulse(QR_BUZZ.2));
}

// ---- layout ----

fn label(text: &str, class: &str) -> gtk::Label {
    let l = gtk::Label::new(Some(text));
    if !class.is_empty() {
        l.add_css_class(class);
    }
    l
}

/// a row of linked toggle buttons, one active; `on` gets the index
fn choice_row(title: &str, options: &[&str], active: usize, on: impl Fn(usize) + 'static) -> gtk::Box {
    let col = gtk::Box::new(gtk::Orientation::Vertical, 4);
    col.append(&{
        let l = label(title, "panel-label");
        l.set_xalign(0.0);
        l
    });
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    row.add_css_class("linked");
    let on = Rc::new(on);
    let mut first: Option<gtk::ToggleButton> = None;
    for (i, o) in options.iter().enumerate() {
        let b = gtk::ToggleButton::with_label(o);
        b.add_css_class("opt");
        if let Some(f) = &first {
            b.set_group(Some(f));
        } else {
            first = Some(b.clone());
        }
        b.set_active(i == active);
        let on = on.clone();
        b.connect_toggled(move |b| {
            if b.is_active() {
                on(i)
            }
        });
        row.append(&b);
    }
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Never);
    scroll.set_child(Some(&row));
    col.append(&scroll);
    col
}

fn icon_button(icon: &str) -> gtk::Button {
    let b = gtk::Button::from_icon_name(icon);
    b.set_size_request(52, 52);
    b.set_valign(gtk::Align::Center);
    b
}

fn build(gtk_app: &gtk::Application) {
    gst::init().expect("GStreamer");
    let display = gdk::Display::default().expect("a display");
    gtk::Settings::for_display(&display).set_gtk_application_prefer_dark_theme(true);
    let provider = gtk::CssProvider::new();
    provider.load_from_data(&format!("{THEME}\n{CSS}"));
    gtk::style_context_add_provider_for_display(&display, &provider, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);

    let window = gtk::ApplicationWindow::new(gtk_app);
    window.set_title(Some("Camera"));
    window.set_decorated(false);
    window.add_css_class("titan-app");

    let stack = gtk::Stack::new();
    window.set_child(Some(&stack));

    // camera page: the preview with overlays, the panel, the bar
    let page = gtk::Box::new(gtk::Orientation::Vertical, 0);
    let over = gtk::Overlay::new();
    over.set_vexpand(true);
    over.add_css_class("viewer");
    let view = gtk::Picture::new();
    view.set_content_fit(gtk::ContentFit::Contain);
    view.set_can_shrink(true);
    over.set_child(Some(&view));

    let top = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    top.set_valign(gtk::Align::Start);
    top.set_margin_top(10);
    top.set_margin_start(10);
    top.set_margin_end(10);
    // back: leave the app (the window's close request ends a recording properly)
    let back_btn = icon_button("go-previous-symbolic");
    back_btn.set_tooltip_text(Some("Back (Esc)"));
    let flash_btn = gtk::Button::with_label("Off");
    let chip = label("", "chip");
    chip.set_valign(gtk::Align::Center);
    let rec = label("● 00:00", "rec");
    rec.set_visible(false);
    rec.set_valign(gtk::Align::Center);
    let spacer = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    spacer.set_hexpand(true);
    let mode_btn = gtk::Button::with_label("Photo");
    let settings_btn = icon_button("emblem-system-symbolic");
    top.append(&back_btn);
    top.append(&flash_btn);
    top.append(&chip);
    top.append(&rec);
    top.append(&spacer);
    top.append(&mode_btn);
    top.append(&settings_btn);
    over.add_overlay(&top);

    // QR chips, over the bottom of the preview, only as tall as they are
    let qr_box = gtk::Box::new(gtk::Orientation::Vertical, 6);
    qr_box.set_valign(gtk::Align::End);
    qr_box.set_margin_start(10);
    qr_box.set_margin_end(10);
    qr_box.set_margin_bottom(10);
    over.add_overlay(&qr_box);

    let status = label("", "status");
    status.set_halign(gtk::Align::Center);
    status.set_valign(gtk::Align::End);
    status.set_margin_bottom(14);
    status.set_visible(false);
    over.add_overlay(&status);

    let expo_label = label("", "status");
    expo_label.set_halign(gtk::Align::End);
    expo_label.set_valign(gtk::Align::Start);
    expo_label.set_margin_top(70);
    expo_label.set_margin_end(10);
    expo_label.set_visible(false);
    over.add_overlay(&expo_label);

    // settings panel, sliding up over the preview (Weston 10 can't place popovers)
    let panel = gtk::Revealer::new();
    panel.set_transition_type(gtk::RevealerTransitionType::SlideUp);
    panel.set_valign(gtk::Align::End);
    let panel_box = gtk::Box::new(gtk::Orientation::Vertical, 10);
    panel_box.add_css_class("panel");
    panel.set_child(Some(&panel_box));
    over.add_overlay(&panel);
    page.append(&over);

    let bar = gtk::CenterBox::new();
    bar.add_css_class("bar");
    let thumb = gtk::Button::new();
    thumb.add_css_class("thumb");
    let shutter = gtk::Button::new();
    shutter.add_css_class("shutter");
    shutter.set_halign(gtk::Align::Center);
    let right = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    let rotate_btn = icon_button("object-rotate-right-symbolic");
    let switch_btn = icon_button("camera-switch-symbolic");
    right.append(&rotate_btn);
    right.append(&switch_btn);
    bar.set_start_widget(Some(&thumb));
    bar.set_center_widget(Some(&shutter));
    bar.set_end_widget(Some(&right));
    page.append(&bar);
    stack.add_named(&page, Some("camera"));

    let photo = gtk::Picture::new();
    photo.set_content_fit(gtk::ContentFit::Contain);
    photo.add_css_class("viewer");
    stack.add_named(&photo, Some("photo"));

    let focus = gtk::Scale::with_range(gtk::Orientation::Horizontal, 0.0, camera::FOCUS_MAX as f64, 1.0);
    focus.set_draw_value(false);
    focus.set_hexpand(true);
    let focus_row = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    focus_row.append(&label("Focus", "panel-label"));
    focus_row.append(&focus);

    let st = Settings::load();
    let (tx, rx) = async_channel::bounded::<Msg>(16);
    let app = Rc::new(App {
        window: window.clone(),
        stack: stack.clone(),
        view: view.clone(),
        photo: photo.clone(),
        status,
        chip,
        rec,
        shutter: shutter.clone(),
        thumb: thumb.clone(),
        flash_btn: flash_btn.clone(),
        panel: panel.clone(),
        focus_row: focus_row.clone(),
        focus: focus.clone(),
        expo_label,
        qr_box,
        qr_chips: RefCell::new(HashMap::new()),
        cams: camera::cameras(),
        flash: RefCell::new(flash::Flash::new(st.tone)),
        af: RefCell::new(af::AutoFocus::new(st.focus)),
        af_region: Arc::new(Mutex::new((0.5, 0.5))),
        st: RefCell::new(st.clone()),
        preview: RefCell::new(None),
        still: RefCell::new(None),
        sensor: RefCell::new(None),
        expo: RefCell::new(None),
        meter: RefCell::new(None),
        gen: Cell::new(0),
        busy: Cell::new(false),
        rec_start: Cell::new(None),
        started: Instant::now(),
        status_timer: Rc::new(RefCell::new(None)),
        tx,
    });
    focus.set_value(st.focus as f64);

    // the panel's rows
    let a = app.clone();
    let mode_btn2 = mode_btn.clone();
    panel_box.append(&choice_row("Mode", &["Photo", "Video"], st.video as usize, move |i| {
        a.set_video(i == 1);
        mode_btn2.set_label(if i == 1 { "Video" } else { "Photo" });
    }));
    let a = app.clone();
    let photo_labels: Vec<String> = {
        let c = &app.cams[st.camera];
        let mp = |s: (u32, u32)| format!("{:.0} MP", s.0 as f64 * s.1 as f64 / 1e6);
        vec![format!("{} ({})", PHOTO_SIZES[0], mp(c.full)), format!("{} ({})", PHOTO_SIZES[1], mp(c.binned))]
    };
    let pl: Vec<&str> = photo_labels.iter().map(|s| s.as_str()).collect();
    panel_box.append(&choice_row("Photo size", &pl, st.photo_size, move |i| {
        a.st.borrow_mut().photo_size = i;
        a.save();
    }));
    let a = app.clone();
    let vl: Vec<&str> = VIDEO_SIZES.iter().map(|v| v.0).collect();
    panel_box.append(&choice_row("Video size", &vl, st.video_size, move |i| {
        a.st.borrow_mut().video_size = i;
        a.save();
    }));
    let a = app.clone();
    let sl: Vec<&str> = SHUTTERS.iter().map(|s| s.0).collect();
    panel_box.append(&choice_row("Shutter (longer than 1/30 s: photos)", &sl, st.shutter, move |i| {
        let was = a.st.borrow().manual_exposure();
        a.st.borrow_mut().shutter = i;
        a.exposure_changed(was);
    }));
    let a = app.clone();
    let il: Vec<&str> = ISOS.iter().map(|s| s.0).collect();
    panel_box.append(&choice_row("ISO", &il, st.iso, move |i| {
        let was = a.st.borrow().manual_exposure();
        a.st.borrow_mut().iso = i;
        a.exposure_changed(was);
    }));
    panel_box.append(&focus_row);
    app.expo_label.set_visible(st.manual_exposure());
    mode_btn.set_label(if st.video { "Video" } else { "Photo" });
    app.set_video(st.video);

    // controls
    let a = app.clone();
    shutter.connect_clicked(move |_| a.shutter_pressed());
    let a = app.clone();
    switch_btn.connect_clicked(move |_| a.switch_camera());
    let a = app.clone();
    rotate_btn.connect_clicked(move |_| a.rotate());
    let a = app.clone();
    thumb.connect_clicked(move |_| a.open_last());
    let p = panel.clone();
    settings_btn.connect_clicked(move |_| p.set_reveal_child(!p.reveals_child()));
    let a = app.clone();
    mode_btn.connect_clicked(move |b| {
        let video = !a.st.borrow().video;
        a.set_video(video);
        b.set_label(if video { "Video" } else { "Photo" });
    });
    let held = Rc::new(Cell::new(false));
    let a = app.clone();
    let h = held.clone();
    flash_btn.connect_clicked(move |_| {
        if !h.replace(false) {
            a.cycle_flash();
        }
    });
    let lp = gtk::GestureLongPress::new();
    let a = app.clone();
    let h = held.clone();
    lp.connect_pressed(move |_, _, _| {
        h.set(true);
        a.cycle_tone();
    });
    flash_btn.add_controller(lp);
    let a = app.clone();
    focus.connect_change_value(move |_, _, v| {
        let pos = v.round().clamp(0.0, camera::FOCUS_MAX as f64) as i32;
        a.af.borrow_mut().state = af::State::Manual;
        a.af.borrow_mut().pos = pos;
        a.set_lens(pos);
        a.st.borrow_mut().focus = pos;
        a.save();
        a.show_chip();
        glib::Propagation::Proceed
    });
    let tap = gtk::GestureClick::new();
    let a = app.clone();
    tap.connect_released(move |_, _, x, y| {
        if a.panel.reveals_child() {
            a.panel.set_reveal_child(false);
            return;
        }
        if a.af.borrow().state == af::State::Manual {
            a.af.borrow_mut().state = af::State::Off; // a tap goes back to autofocus
        }
        a.on_tap(x, y);
    });
    view.add_controller(tap);
    let back = gtk::GestureClick::new();
    let a = app.clone();
    back.connect_released(move |_, _, _, _| a.show_camera());
    photo.add_controller(back);

    let keys = gtk::EventControllerKey::new();
    let a = app.clone();
    keys.connect_key_pressed(move |_, key, _, _| {
        if a.stack.visible_child_name().as_deref() == Some("photo") {
            a.show_camera();
            return glib::Propagation::Stop;
        }
        let name = key.name().map(|n| n.to_string()).unwrap_or_default();
        match name.as_str() {
            "space" | "Return" | "KP_Enter" => a.shutter_pressed(),
            "v" | "V" => {
                let video = !a.st.borrow().video;
                a.set_video(video);
            }
            "f" | "F" => a.switch_camera(),
            "r" | "R" => a.rotate(),
            "t" | "T" => a.cycle_flash(),
            "c" | "C" => a.cycle_tone(),
            "a" | "A" => a.autofocus(Some((0.5, 0.5))),
            "s" | "S" => a.panel.set_reveal_child(!a.panel.reveals_child()),
            "Escape" => {
                if a.panel.reveals_child() {
                    a.panel.set_reveal_child(false);
                } else {
                    a.window.close(); // close_request ends a recording properly
                }
            }
            _ => return glib::Propagation::Proceed,
        }
        glib::Propagation::Stop
    });
    window.add_controller(keys);
    let w = window.clone();
    back_btn.connect_clicked(move |_| w.close());

    let a = app.clone();
    glib::spawn_future_local(async move {
        while let Ok(m) = rx.recv().await {
            a.on_msg(m);
        }
    });
    let a = app.clone();
    window.connect_close_request(move |_| {
        if let Some(p) = a.preview.borrow().as_ref() {
            if p.recording.is_some() {
                p.finish();
                std::thread::sleep(Duration::from_millis(500)); // let the muxer close the file
            }
        }
        a.stop_preview();
        if let Some(s) = a.still.borrow_mut().take() {
            s.stop();
        }
        a.flash.borrow_mut().off();
        glib::Propagation::Proceed
    });
    for sig in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
        let w = window.clone();
        glib::unix_signal_add_local(sig, move || {
            w.close();
            glib::ControlFlow::Break
        });
    }

    app.update_thumb();
    app.show_flash();
    let a = app.clone();
    // maximize once mapped: before, Weston's 0 x 0 maximized configure is an error (titan_ui4)
    window.connect_map(move |w| {
        let w = w.clone();
        let a = a.clone();
        glib::idle_add_local_once(move || {
            w.maximize();
            a.start_preview(None, true);
        });
    });
    window.present();
}

/// GTK on the big cores: on the little ones it renders at half the frame rate
fn use_big_cores() {
    let mut caps: Vec<(usize, u32)> = Vec::new();
    for cpu in 0..16 {
        let p = format!("/sys/devices/system/cpu/cpu{cpu}/cpu_capacity");
        if let Some(c) = std::fs::read_to_string(p).ok().and_then(|s| s.trim().parse().ok()) {
            caps.push((cpu, c));
        }
    }
    let Some(top) = caps.iter().map(|b| b.1).max() else { return };
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        for (cpu, c) in &caps {
            if *c == top {
                libc::CPU_SET(*cpu, &mut set);
            }
        }
        libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set);
    }
}

/// one at a time: a second launch quits (the camera allows one stream)
fn single_instance() -> bool {
    use std::os::fd::IntoRawFd;
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    let Ok(f) = std::fs::File::create(format!("{dir}/titan-camera.lock")) else { return true };
    unsafe { libc::flock(f.into_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) == 0 }
}

fn main() -> glib::ExitCode {
    std::env::set_var("GTK_A11Y", "none");
    // libcamera's GStreamer element lives in /usr/local (userspace/libcamera/build.sh)
    let gst_path = "/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0";
    let cur = std::env::var("GST_PLUGIN_PATH").unwrap_or_default();
    if !cur.contains(gst_path) {
        std::env::set_var("GST_PLUGIN_PATH", if cur.is_empty() { gst_path.to_string() } else { format!("{gst_path}:{cur}") });
    }
    if std::env::var_os("LIBCAMERA_LOG_LEVELS").is_none() {
        std::env::set_var("LIBCAMERA_LOG_LEVELS", "*:ERROR");
    }
    if !single_instance() {
        return glib::ExitCode::SUCCESS;
    }
    use_big_cores();
    let app = gtk::Application::builder()
        .application_id("org.titanpocket.camera")
        .flags(gtk::gio::ApplicationFlags::NON_UNIQUE)
        .build();
    app.connect_activate(build);
    app.run_with_args::<&str>(&[])
}
