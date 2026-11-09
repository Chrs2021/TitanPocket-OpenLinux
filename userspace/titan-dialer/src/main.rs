//! titan-dialer (Rust): the phone. A keypad, voice calls and USSD codes, replacing the Python
//! titan-dialer (docs/modem.md, "Phone apps"); the same behaviour, plus the proximity sensor.
//!
//! - A number calls it: through titan-ims (VoLTE) while it's registered, else ModemManager's
//!   Voice interface. A code that starts with * or # and ends with # is USSD; the network's
//!   answer is shown, and a menu's reply goes back in the same session. *#06# shows the IMEI,
//!   on the screen only.
//! - Calls: hang up, answer or decline, mute, the loudspeaker, DTMF from the keypad. titan-ims
//!   has its calls' sound; ModemManager calls get titan-call-audio while active.
//! - Incoming calls ring here (a quiet tone, the vibration motor); titan-call-watch starts the
//!   app when one comes in.
//! - Proximity: while a call is up on the earpiece (dialing, ringing out or active; not on the
//!   loudspeaker or a headset, not while one rings in), holding the phone to the ear darkens the
//!   panel and the app ignores touches; taking it away lights it again (src/proximity.rs).
mod modem;
mod proximity;
mod sound;

use gtk::prelude::*;
use gtk::{gdk, glib};
use modem::{Client, Event};
use std::cell::{Cell, RefCell};
use std::process::{Child, Command, Stdio};
use std::rc::Rc;
use std::time::{Duration, Instant};

const THEME: &str = include_str!("../../titan-messages/src/theme.css");
const CSS: &str = "
.number { font-size: 44px; font-weight: bold; color: white; }
.number-hint { font-size: 18px; color: rgba(255, 255, 255, 0.72); }
.call-number { font-size: 40px; font-weight: bold; color: white; }
.call-state { font-size: 22px; color: #9FE3FF; }
.call-time { font-size: 30px; color: white; font-family: 'IBM Plex Mono', monospace; }
.ussd-code { font-size: 18px; color: rgba(255, 255, 255, 0.72); }
.ussd-text { font-size: 22px; color: white; }
.err { color: #FF6B7A; }
.key { font-size: 28px; font-weight: 600; border-radius: 18px; }
.key-small { font-size: 22px; }
.big { min-height: 58px; font-size: 20px; border-radius: 18px; }
.call { background: #2FA36B; color: white; }
.hangup { background: #E5484D; color: white; }
.shield { background: black; }
entry { min-height: 52px; font-size: 22px; }
";
const SETTINGS: &str = "/usr/local/bin/titan-phone-settings";
const KEYS: [&str; 12] = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"];
const RING_EVERY: Duration = Duration::from_millis(2500);
/// near this long before the screen goes dark (a hand passing over doesn't blank it)
const NEAR_DELAY: Duration = Duration::from_millis(250);

fn is_ussd(s: &str) -> bool {
    let b = s.as_bytes();
    b.len() >= 2
        && (b[0] == b'*' || b[0] == b'#')
        && b[b.len() - 1] == b'#'
        && b.iter().all(|c| c.is_ascii_digit() || *c == b'*' || *c == b'#')
}

fn is_emergency(n: &str) -> bool {
    let digits: String = n.chars().filter(|c| c.is_ascii_digit()).collect();
    digits == "911" || digits == "112"
}

fn open_settings() {
    let _ = Command::new(SETTINGS).stdout(Stdio::null()).stderr(Stdio::null()).spawn().map(|mut c| {
        std::thread::spawn(move || c.wait());
    });
}

fn label(text: &str, class: &str) -> gtk::Label {
    let l = gtk::Label::new(Some(text));
    if !class.is_empty() {
        l.add_css_class(class);
    }
    l
}

fn button(text: &str, classes: &[&str]) -> gtk::Button {
    let b = gtk::Button::with_label(text);
    for c in classes {
        b.add_css_class(c);
    }
    b
}

/// twelve keys; `pressed(key)` for each, and 0 held for +
fn keypad(height: i32, small: bool, pressed: impl Fn(&str) + 'static) -> gtk::Grid {
    let g = gtk::Grid::new();
    g.set_row_spacing(8);
    g.set_column_spacing(8);
    g.set_column_homogeneous(true);
    let pressed = Rc::new(pressed);
    let held = Rc::new(Cell::new(false));
    for (i, k) in KEYS.iter().enumerate() {
        let b = button(k, &["key"]);
        if small {
            b.add_css_class("key-small");
        }
        b.set_size_request(-1, height);
        b.set_hexpand(true);
        let (p, h, key) = (pressed.clone(), held.clone(), *k);
        b.connect_clicked(move |_| {
            if !h.replace(false) {
                p(key)
            }
        });
        if *k == "0" {
            let lp = gtk::GestureLongPress::new();
            let (p, h) = (pressed.clone(), held.clone());
            lp.connect_pressed(move |_, _, _| {
                h.set(true);
                p("+");
            });
            b.add_controller(lp);
        }
        g.attach(&b, (i % 3) as i32, (i / 3) as i32, 1, 1);
    }
    g
}

struct App {
    window: gtk::ApplicationWindow,
    mm: Rc<Client>,
    stack: gtk::Stack,
    subtitle: gtk::Label,
    shield: gtk::Box,
    // dial page
    num_label: gtk::Label,
    hint: gtk::Label,
    fix: gtk::Button,
    call_btn: gtk::Button,
    // call page
    call_number: gtk::Label,
    call_state: gtk::Label,
    call_time: gtk::Label,
    dtmf: gtk::Grid,
    toggles: gtk::Box,
    mute_btn: gtk::ToggleButton,
    speaker_btn: gtk::ToggleButton,
    keypad_btn: gtk::ToggleButton,
    answer_btn: gtk::Button,
    hangup_btn: gtk::Button,
    // USSD page
    ussd_code_label: gtk::Label,
    ussd_text: gtk::Label,
    reply_row: gtk::Box,
    reply: gtk::Entry,

    number: RefCell<String>,
    call_path: RefCell<Option<String>>,
    call_started: Cell<Option<Instant>>,
    audio: RefCell<Option<Child>>,
    device: RefCell<&'static str>,
    muted: Cell<bool>,
    ring_id: RefCell<Option<glib::SourceId>>,
    rings: RefCell<Vec<Child>>,
    ussd_code: RefCell<String>,
    prox: RefCell<Option<proximity::Proximity>>,
    screen: RefCell<proximity::Screen>,
    near_timer: RefCell<Option<glib::SourceId>>,
    ignore_toggle: Cell<bool>,
    /// dialing: the call's path isn't known yet
    outgoing: Cell<bool>,
}

impl App {
    fn page(&self) -> String {
        self.stack.visible_child_name().map(|s| s.to_string()).unwrap_or_default()
    }

    // ---- the modem's state ----

    fn refresh(self: &Rc<Self>) {
        self.subtitle.set_text(&self.mm.status_line());
        if self.page() == "dial" {
            self.show_number();
        }
        if self.mm.prop_u32(modem::I_USSD, "State") == Some(modem::USSD_USER_RESPONSE) && self.page() == "ussd" {
            let req = self.mm.prop_str(modem::I_USSD, "NetworkRequest");
            if !req.is_empty() {
                self.ussd_text.set_text(&req);
                self.reply_row.set_visible(true);
            }
        }
    }

    /// what stops a call: (hint, button label)
    fn fix_needed(&self) -> Option<(String, &'static str)> {
        if self.mm.ims_on.get() {
            return None; // VoLTE calls don't need ModemManager's voice
        }
        if !self.mm.present() {
            return Some(("The modem is off".into(), "Phone settings"));
        }
        match self.mm.state() {
            Some(modem::STATE_LOCKED) => Some(("The SIM is locked".into(), "Unlock SIM")),
            Some(modem::STATE_FAILED) => Some((self.mm.status_line(), "Phone settings")),
            Some(s) if s < modem::STATE_ENABLED => Some(("Flight mode: the radio is off".into(), "Turn radio on")),
            _ => None,
        }
    }

    fn fix_action(self: &Rc<Self>) {
        match self.fix_needed() {
            Some((_, "Turn radio on")) => {
                self.hint.set_text("turning the radio on…");
                let h = self.hint.clone();
                self.mm.call(
                    modem::I_MODEM,
                    "Enable",
                    Some((true,).to_variant()),
                    120000,
                    Some(Box::new(move |r| h.set_text(&r.err().unwrap_or_else(|| "radio on".into())))),
                    None,
                );
            }
            _ => open_settings(),
        }
    }

    // ---- dialing ----

    fn show_number(&self) {
        let n = self.number.borrow().clone();
        self.num_label.set_text(if n.is_empty() { " " } else { &n });
        let need = self.fix_needed();
        self.fix.set_visible(need.is_some());
        if let Some((hint, btn)) = need {
            self.fix.set_label(btn);
            self.hint.set_text(&hint);
        } else if is_ussd(&n) {
            self.hint.set_text("USSD code: the answer comes from the network");
        } else {
            self.hint.set_text("");
        }
        self.call_btn.set_label(if is_ussd(&n) { "Send" } else { "Call" });
    }

    fn press(&self, key: &str) {
        if self.page() != "dial" {
            return;
        }
        let mut n = self.number.borrow_mut();
        if n.chars().count() < 40 {
            n.push_str(key);
        }
        drop(n);
        self.show_number();
    }

    fn delete(&self) {
        self.number.borrow_mut().pop();
        self.show_number();
    }

    fn go(self: &Rc<Self>) {
        let n = self.number.borrow().trim().to_string();
        if n.is_empty() {
            return;
        }
        if n == "*#06#" {
            let mut imei = self.mm.prop_str(modem::I_3GPP, "Imei");
            if imei.is_empty() {
                imei = self.mm.prop_str(modem::I_MODEM, "EquipmentIdentifier");
            }
            let text = if imei.is_empty() { "not available (the modem is off)".to_string() } else { imei };
            self.show_ussd(&n, &format!("IMEI\n{text}"));
            return;
        }
        if let Some((hint, _)) = self.fix_needed() {
            self.hint.set_text(&hint);
            return;
        }
        if is_ussd(&n) {
            self.ussd_start(&n);
        } else {
            self.dial(&n);
        }
    }

    fn dial(self: &Rc<Self>, n: &str) {
        if !self.mm.has(modem::I_VOICE) {
            self.hint.set_text("no voice calls: titan-ims isn't registered and ModemManager offers none");
            return;
        }
        *self.call_path.borrow_mut() = None;
        self.outgoing.set(true);
        // no emergency registration yet (docs/modem.md, "IMS")
        let state = if is_emergency(n) { "emergency calling is untested: use another phone if you can" } else { "calling…" };
        self.show_call_page(n, state, false);
        let me = self.clone();
        self.mm.dial(
            n,
            Box::new(move |r| match r {
                Ok(path) => {
                    *me.call_path.borrow_mut() = Some(path);
                    me.update_call();
                }
                Err(e) => {
                    me.outgoing.set(false);
                    me.update_proximity();
                    me.call_state.set_text(&format!("call failed: {e}"));
                    me.call_state.add_css_class("err");
                    let m = me.clone();
                    glib::timeout_add_local_once(Duration::from_secs(3), move || m.back_to_dial());
                }
            }),
        );
    }

    // ---- calls ----

    fn show_call_page(self: &Rc<Self>, number: &str, state: &str, incoming: bool) {
        self.call_number.set_text(if number.is_empty() { "unknown number" } else { number });
        self.call_state.set_text(state);
        self.call_state.remove_css_class("err");
        self.call_time.set_text("");
        self.answer_btn.set_visible(incoming);
        self.hangup_btn.set_label(if incoming { "Decline" } else { "Hang up" });
        self.toggles.set_visible(!incoming);
        self.ignore_toggle.set(true);
        self.mute_btn.set_active(false);
        self.speaker_btn.set_active(false);
        self.keypad_btn.set_active(false);
        self.ignore_toggle.set(false);
        *self.device.borrow_mut() = "earpiece";
        self.dtmf.set_visible(false);
        self.muted.set(false);
        self.stack.set_visible_child_name("call");
        self.window.present();
        self.update_proximity();
    }

    fn on_event(self: &Rc<Self>, e: Event) {
        match e {
            Event::Changed => self.refresh(),
            Event::CallAdded(path) => {
                let st = self.mm.call_state(&path);
                if !self.mm.call_incoming(&path) || !(st == modem::CALL_RINGING_IN || st == modem::CALL_WAITING) {
                    return;
                }
                if self.call_path.borrow().is_some() {
                    return; // a second call while one is on screen: no call waiting yet
                }
                *self.call_path.borrow_mut() = Some(path.clone());
                let num = self.mm.call_number(&path);
                self.show_call_page(&num, "incoming call", true);
                self.start_ringing();
            }
            Event::CallState(path) => {
                if self.call_path.borrow().as_deref() == Some(path.as_str()) {
                    self.update_call();
                }
            }
            Event::CallRemoved(path) => {
                if self.call_path.borrow().as_deref() == Some(path.as_str()) {
                    self.end_call("call ended");
                }
            }
        }
    }

    fn update_call(self: &Rc<Self>) {
        let Some(path) = self.call_path.borrow().clone() else { return };
        let st = self.mm.call_state(&path);
        if st == modem::CALL_TERMINATED {
            self.end_call("call ended");
            return;
        }
        if st == modem::CALL_ACTIVE {
            self.stop_ringing();
            self.answer_btn.set_visible(false);
            self.hangup_btn.set_label("Hang up");
            self.toggles.set_visible(true);
            if self.call_started.get().is_none() {
                self.call_started.set(Some(Instant::now()));
                self.tick();
                self.start_audio();
            }
        }
        self.call_state.set_text(modem::call_state_text(st));
        self.update_proximity();
    }

    fn tick(self: &Rc<Self>) {
        let Some(t) = self.call_started.get() else { return };
        let s = t.elapsed().as_secs();
        self.call_time.set_text(&format!("{}:{:02}", s / 60, s % 60));
        let me = self.clone();
        glib::timeout_add_local_once(Duration::from_millis(500), move || me.tick());
    }

    fn answer(self: &Rc<Self>) {
        if let Some(p) = self.call_path.borrow().clone() {
            self.call_state.set_text("answering…");
            let cs = self.call_state.clone();
            self.mm.call_method(&p, "Accept", None, Some(Box::new(move |r| {
                if let Err(e) = r {
                    cs.set_text(&e);
                }
            })));
        }
    }

    fn hang_up(self: &Rc<Self>) {
        let Some(p) = self.call_path.borrow().clone() else {
            self.end_call("call ended");
            return;
        };
        self.call_state.set_text("hanging up…");
        let me = self.clone();
        self.mm.call_method(&p, "Hangup", None, Some(Box::new(move |r| me.end_call(&r.err().unwrap_or_else(|| "call ended".into())))));
    }

    fn end_call(self: &Rc<Self>, why: &str) {
        let path = self.call_path.borrow_mut().take();
        self.outgoing.set(false);
        self.stop_ringing();
        sound::stop(&mut self.audio.borrow_mut());
        self.call_started.set(None);
        self.update_proximity();
        if self.page() == "call" {
            self.call_state.set_text(why);
            let me = self.clone();
            glib::timeout_add_local_once(Duration::from_millis(1500), move || me.back_to_dial());
        }
        if let Some(p) = path {
            self.mm.delete_call(&p);
        }
    }

    fn back_to_dial(&self) {
        if self.call_path.borrow().is_none() && self.page() == "call" {
            self.stack.set_visible_child_name("dial");
            self.show_number();
        }
    }

    fn send_dtmf(&self, key: &str) {
        if let Some(p) = self.call_path.borrow().clone() {
            if self.mm.call_state(&p) == modem::CALL_ACTIVE {
                self.mm.call_method(&p, "SendDtmf", Some(key), None);
            }
        }
    }

    // ---- the call's sound ----

    fn start_audio(self: &Rc<Self>) {
        sound::stop(&mut self.audio.borrow_mut());
        if *self.device.borrow() != "speaker" {
            *self.device.borrow_mut() = sound::wired_device();
        }
        let dev = *self.device.borrow();
        let path = self.call_path.borrow().clone().unwrap_or_default();
        if Client::is_ims(&path) {
            let route = if dev == "speaker" || dev == "headphones" { dev } else { "earpiece" };
            self.mm.ims_send(serde_json::json!({"cmd": "route", "device": route}));
        } else {
            match sound::call_audio(dev) {
                Some(c) => *self.audio.borrow_mut() = Some(c),
                None => self.call_state.set_text("no call audio"),
            }
        }
        if self.muted.get() {
            self.set_mute(true);
        }
        self.update_proximity();
    }

    fn set_mute(&self, on: bool) {
        self.muted.set(on);
        let path = self.call_path.borrow().clone().unwrap_or_default();
        if Client::is_ims(&path) {
            self.mm.ims_send(serde_json::json!({"cmd": "mute", "on": on}));
        } else {
            sound::call_audio_mute(on);
        }
    }

    fn set_speaker(self: &Rc<Self>, on: bool) {
        *self.device.borrow_mut() = if on { "speaker" } else { "earpiece" };
        if self.call_started.get().is_some() {
            self.start_audio();
        } else {
            self.update_proximity();
        }
    }

    // ---- proximity ----

    /// the sensor runs while a call is up on the earpiece and not ringing in
    fn update_proximity(self: &Rc<Self>) {
        let path = self.call_path.borrow().clone();
        let st = path.as_deref().map(|p| self.mm.call_state(p)).unwrap_or(0);
        let outgoing_or_active = matches!(st, modem::CALL_DIALING | modem::CALL_RINGING_OUT | modem::CALL_ACTIVE)
            || (path.is_none() && self.outgoing.get());
        let earpiece = *self.device.borrow() == "earpiece";
        let want = self.page() == "call" && outgoing_or_active && earpiece;
        if want && self.prox.borrow().is_none() {
            let w = Rc::downgrade(self);
            *self.prox.borrow_mut() = proximity::Proximity::start(move |near| {
                if let Some(a) = w.upgrade() {
                    a.on_near(near);
                }
            });
        } else if !want {
            self.prox.borrow_mut().take();
            self.on_near(false);
        }
    }

    fn on_near(self: &Rc<Self>, near: bool) {
        if let Some(t) = self.near_timer.borrow_mut().take() {
            t.remove();
        }
        if near {
            let me = self.clone();
            *self.near_timer.borrow_mut() = Some(glib::timeout_add_local_once(NEAR_DELAY, move || {
                me.near_timer.borrow_mut().take();
                me.shield.set_visible(true);
                me.screen.borrow_mut().off();
            }));
        } else {
            self.screen.borrow_mut().on();
            self.shield.set_visible(false);
        }
    }

    // ---- ringing ----

    fn start_ringing(self: &Rc<Self>) {
        self.stop_ringing();
        sound::ring_once(&mut self.rings.borrow_mut());
        let me = self.clone();
        *self.ring_id.borrow_mut() = Some(glib::timeout_add_local(RING_EVERY, move || {
            sound::ring_once(&mut me.rings.borrow_mut());
            glib::ControlFlow::Continue
        }));
    }

    fn stop_ringing(&self) {
        if let Some(id) = self.ring_id.borrow_mut().take() {
            id.remove();
        }
    }

    // ---- USSD ----

    fn show_ussd(&self, code: &str, text: &str) {
        *self.ussd_code.borrow_mut() = code.to_string();
        self.ussd_code_label.set_text(code);
        self.ussd_text.set_text(text);
        self.ussd_text.remove_css_class("err");
        self.reply_row.set_visible(false);
        self.stack.set_visible_child_name("ussd");
    }

    fn ussd_start(self: &Rc<Self>, code: &str) {
        if !self.mm.has(modem::I_USSD) {
            self.hint.set_text("this modem offers no USSD to ModemManager");
            return;
        }
        self.show_ussd(code, "waiting for the network…");
        let me = self.clone();
        let code = code.to_string();
        let initiate = move || {
            let m = me.clone();
            me.mm.call(modem::I_USSD, "Initiate", Some((code.as_str(),).to_variant()), 90000, Some(Box::new(move |r| m.ussd_answer(r))), None);
        };
        if self.mm.prop_u32(modem::I_USSD, "State").unwrap_or(modem::USSD_IDLE) != modem::USSD_IDLE {
            self.mm.call(modem::I_USSD, "Cancel", None, 30000, Some(Box::new(move |_| initiate())), None);
        } else {
            initiate();
        }
    }

    fn ussd_answer(&self, r: Result<Option<glib::Variant>, String>) {
        match r {
            Err(e) => {
                self.ussd_text.set_text(&e);
                self.ussd_text.add_css_class("err");
                self.reply_row.set_visible(false);
            }
            Ok(v) => {
                self.ussd_text.remove_css_class("err");
                let text = v.and_then(|v| v.child_value(0).str().map(String::from)).filter(|s| !s.is_empty()).unwrap_or_else(|| "(no text)".into());
                self.ussd_text.set_text(&text);
                let waiting = self.mm.prop_u32(modem::I_USSD, "State") == Some(modem::USSD_USER_RESPONSE);
                self.reply_row.set_visible(waiting);
                if waiting {
                    self.reply.set_text("");
                    self.reply.grab_focus();
                }
            }
        }
    }

    fn ussd_respond(self: &Rc<Self>) {
        let text = self.reply.text().trim().to_string();
        if text.is_empty() {
            return;
        }
        self.ussd_code_label.set_text(&format!("{}  →  {text}", self.ussd_code.borrow()));
        self.ussd_text.set_text("waiting for the network…");
        self.reply_row.set_visible(false);
        let me = self.clone();
        self.mm.call(modem::I_USSD, "Respond", Some((text.as_str(),).to_variant()), 90000, Some(Box::new(move |r| me.ussd_answer(r))), None);
    }

    fn ussd_close(&self) {
        if self.mm.prop_u32(modem::I_USSD, "State").unwrap_or(modem::USSD_IDLE) != modem::USSD_IDLE {
            self.mm.call(modem::I_USSD, "Cancel", None, 30000, None, None);
        }
        self.number.borrow_mut().clear();
        self.stack.set_visible_child_name("dial");
        self.show_number();
    }

    // ---- keys, leaving ----

    fn on_back(&self) {
        match self.page().as_str() {
            "ussd" => self.ussd_close(),
            "call" => {
                if self.call_path.borrow().is_none() {
                    self.back_to_dial();
                } // a call stays on screen until it ends
            }
            _ => self.window.close(),
        }
    }

    fn on_key(self: &Rc<Self>, key: gdk::Key, state: gdk::ModifierType) -> glib::Propagation {
        if state.contains(gdk::ModifierType::CONTROL_MASK) || self.shield.is_visible() {
            return if self.shield.is_visible() { glib::Propagation::Stop } else { glib::Propagation::Proceed };
        }
        let ch = key.to_unicode().map(|c| c.to_string()).unwrap_or_default();
        let name = key.name().map(|n| n.to_string()).unwrap_or_default();
        let page = self.page();
        if page == "call" && KEYS.contains(&ch.as_str()) {
            self.send_dtmf(&ch);
            return glib::Propagation::Stop;
        }
        if page != "dial" {
            if name == "Escape" {
                self.on_back();
                return glib::Propagation::Stop;
            }
            return glib::Propagation::Proceed;
        }
        match name.as_str() {
            "BackSpace" => self.delete(),
            "Return" | "KP_Enter" => self.go(),
            "Escape" => self.on_back(),
            _ if KEYS.contains(&ch.as_str()) || ch == "+" => self.press(&ch),
            _ => return glib::Propagation::Proceed,
        }
        glib::Propagation::Stop
    }
}

fn build(gtk_app: &gtk::Application) {
    let display = gdk::Display::default().expect("a display");
    gtk::Settings::for_display(&display).set_gtk_application_prefer_dark_theme(true);
    let provider = gtk::CssProvider::new();
    provider.load_from_data(&format!("{THEME}\n{CSS}\n.wall {{ background: linear-gradient(160deg, #1B2C66, #10204A 60%, #0E3A52); }}\n"));
    gtk::style_context_add_provider_for_display(&display, &provider, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);

    let window = gtk::ApplicationWindow::new(gtk_app);
    window.set_title(Some("Phone"));
    window.set_decorated(false);
    window.add_css_class("titan-app");
    let root = gtk::Overlay::new();
    let wall = gtk::Box::new(gtk::Orientation::Vertical, 0);
    wall.add_css_class("wall");
    root.set_child(Some(&wall));
    window.set_child(Some(&root));
    let col = gtk::Box::new(gtk::Orientation::Vertical, 12);
    col.set_margin_top(12);
    col.set_margin_start(14);
    col.set_margin_end(14);
    col.set_margin_bottom(12);
    root.add_overlay(&col);

    // header
    let header = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    header.add_css_class("titan-header");
    let back = gtk::Button::from_icon_name("go-previous-symbolic");
    back.set_size_request(46, 46);
    back.set_valign(gtk::Align::Center);
    header.append(&back);
    let texts = gtk::Box::new(gtk::Orientation::Vertical, 0);
    texts.set_hexpand(true);
    texts.set_valign(gtk::Align::Center);
    texts.set_margin_start(6);
    texts.append(&{
        let t = label("Phone", "titan-title");
        t.set_xalign(0.0);
        t
    });
    let subtitle = label("…", "titan-subtitle");
    subtitle.set_xalign(0.0);
    subtitle.set_ellipsize(gtk::pango::EllipsizeMode::End);
    texts.append(&subtitle);
    header.append(&texts);
    let settings_btn = gtk::Button::from_icon_name("view-more-symbolic");
    settings_btn.set_size_request(46, 46);
    settings_btn.set_valign(gtk::Align::Center);
    settings_btn.set_tooltip_text(Some("Phone settings"));
    header.append(&settings_btn);
    col.append(&header);

    let stack = gtk::Stack::new();
    stack.set_transition_type(gtk::StackTransitionType::Crossfade);
    stack.set_vexpand(true);
    col.append(&stack);

    // dial page
    let dial = gtk::Box::new(gtk::Orientation::Vertical, 10);
    let top = gtk::Box::new(gtk::Orientation::Vertical, 2);
    top.add_css_class("water-panel");
    let num_label = label(" ", "number");
    num_label.set_ellipsize(gtk::pango::EllipsizeMode::Start);
    let hint = label("", "number-hint");
    hint.set_wrap(true);
    top.append(&num_label);
    top.append(&hint);
    dial.append(&top);
    let pad_slot = gtk::Box::new(gtk::Orientation::Vertical, 0);
    pad_slot.set_vexpand(true);
    dial.append(&pad_slot);
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    let fix = button("Turn radio on", &["big"]);
    fix.set_visible(false);
    row.append(&fix);
    let spacer = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    spacer.set_hexpand(true);
    row.append(&spacer);
    let del = gtk::Button::from_icon_name("edit-clear-symbolic");
    del.add_css_class("big");
    del.set_size_request(58, 58);
    row.append(&del);
    let call_btn = button("Call", &["big", "call"]);
    call_btn.set_size_request(140, 58);
    row.append(&call_btn);
    dial.append(&row);
    stack.add_named(&dial, Some("dial"));

    // call page
    let call = gtk::Box::new(gtk::Orientation::Vertical, 10);
    let card = gtk::Box::new(gtk::Orientation::Vertical, 4);
    card.add_css_class("water-panel");
    let call_number = label("", "call-number");
    call_number.set_ellipsize(gtk::pango::EllipsizeMode::End);
    let call_state = label("", "call-state");
    call_state.set_wrap(true);
    let call_time = label("", "call-time");
    card.append(&call_number);
    card.append(&call_state);
    card.append(&call_time);
    call.append(&card);
    let dtmf_slot = gtk::Box::new(gtk::Orientation::Vertical, 0);
    call.append(&dtmf_slot);
    let sp = gtk::Box::new(gtk::Orientation::Vertical, 0);
    sp.set_vexpand(true);
    call.append(&sp);
    let toggles = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    toggles.set_homogeneous(true);
    let mute_btn = gtk::ToggleButton::with_label("Mute");
    let speaker_btn = gtk::ToggleButton::with_label("Speaker");
    let keypad_btn = gtk::ToggleButton::with_label("Keypad");
    for b in [&mute_btn, &speaker_btn, &keypad_btn] {
        b.add_css_class("big");
        toggles.append(b);
    }
    call.append(&toggles);
    let crow = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    crow.set_homogeneous(true);
    let answer_btn = button("Answer", &["big", "call"]);
    let hangup_btn = button("Hang up", &["big", "hangup"]);
    crow.append(&answer_btn);
    crow.append(&hangup_btn);
    call.append(&crow);
    stack.add_named(&call, Some("call"));

    // USSD page
    let ussd = gtk::Box::new(gtk::Orientation::Vertical, 10);
    let ucard = gtk::Box::new(gtk::Orientation::Vertical, 6);
    ucard.add_css_class("water-panel");
    ucard.set_vexpand(true);
    let ussd_code_label = label("", "ussd-code");
    ussd_code_label.set_wrap(true);
    ussd_code_label.set_xalign(0.0);
    let ussd_text = label("", "ussd-text");
    ussd_text.set_wrap(true);
    ussd_text.set_xalign(0.0);
    ussd_text.set_valign(gtk::Align::Start);
    ussd_text.set_selectable(true);
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_vexpand(true);
    scroll.set_child(Some(&ussd_text));
    ucard.append(&ussd_code_label);
    ucard.append(&scroll);
    ussd.append(&ucard);
    let reply_row = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    let reply = gtk::Entry::new();
    reply.set_hexpand(true);
    reply.set_input_purpose(gtk::InputPurpose::Phone);
    reply.set_placeholder_text(Some("reply"));
    let send = button("Send", &["big", "call"]);
    reply_row.append(&reply);
    reply_row.append(&send);
    ussd.append(&reply_row);
    let close = button("Close", &["big"]);
    close.set_halign(gtk::Align::Center);
    ussd.append(&close);
    stack.add_named(&ussd, Some("ussd"));

    // the shield: black, over everything, swallowing touches while the phone is at the ear
    let shield = gtk::Box::new(gtk::Orientation::Vertical, 0);
    shield.add_css_class("shield");
    shield.set_hexpand(true);
    shield.set_vexpand(true);
    shield.set_visible(false);
    let swallow = gtk::GestureClick::new();
    swallow.connect_pressed(|g, _, _, _| {
        g.set_state(gtk::EventSequenceState::Claimed);
    });
    shield.add_controller(swallow);
    root.add_overlay(&shield);

    // the keypads, wired to the app through a slot filled once it exists
    let me_slot: Rc<RefCell<std::rc::Weak<App>>> = Rc::new(RefCell::new(std::rc::Weak::new()));
    let s = me_slot.clone();
    pad_slot.append(&keypad(86, false, move |k| {
        if let Some(a) = s.borrow().upgrade() {
            a.press(k)
        }
    }));
    let s = me_slot.clone();
    let dtmf = keypad(50, true, move |k| {
        if let Some(a) = s.borrow().upgrade() {
            a.send_dtmf(k)
        }
    });
    dtmf.set_visible(false);
    dtmf_slot.append(&dtmf);
    let d = dtmf.clone();
    keypad_btn.connect_toggled(move |b| d.set_visible(b.is_active()));

    let mm = Client::new();
    let app = Rc::new(App {
        window: window.clone(),
        mm: mm.clone(),
        stack: stack.clone(),
        subtitle,
        shield,
        num_label,
        hint,
        fix: fix.clone(),
        call_btn: call_btn.clone(),
        call_number,
        call_state,
        call_time,
        dtmf,
        toggles,
        mute_btn: mute_btn.clone(),
        speaker_btn: speaker_btn.clone(),
        keypad_btn: keypad_btn.clone(),
        answer_btn: answer_btn.clone(),
        hangup_btn: hangup_btn.clone(),
        ussd_code_label,
        ussd_text,
        reply_row,
        reply: reply.clone(),
        number: RefCell::new(String::new()),
        call_path: RefCell::new(None),
        call_started: Cell::new(None),
        audio: RefCell::new(None),
        device: RefCell::new("earpiece"),
        muted: Cell::new(false),
        ring_id: RefCell::new(None),
        rings: RefCell::new(Vec::new()),
        ussd_code: RefCell::new(String::new()),
        prox: RefCell::new(None),
        screen: RefCell::new(proximity::Screen::new()),
        near_timer: RefCell::new(None),
        ignore_toggle: Cell::new(false),
        outgoing: Cell::new(false),
    });
    *me_slot.borrow_mut() = Rc::downgrade(&app);

    let a = app.clone();
    mm.connect(move |e| a.on_event(e));
    let a = app.clone();
    back.connect_clicked(move |_| a.on_back());
    settings_btn.connect_clicked(|_| open_settings());
    let a = app.clone();
    fix.connect_clicked(move |_| a.fix_action());
    let a = app.clone();
    del.connect_clicked(move |_| a.delete());
    let a = app.clone();
    call_btn.connect_clicked(move |_| a.go());
    let a = app.clone();
    answer_btn.connect_clicked(move |_| a.answer());
    let a = app.clone();
    hangup_btn.connect_clicked(move |_| a.hang_up());
    let a = app.clone();
    mute_btn.connect_toggled(move |b| {
        if !a.ignore_toggle.get() {
            a.set_mute(b.is_active())
        }
    });
    let a = app.clone();
    speaker_btn.connect_toggled(move |b| {
        if !a.ignore_toggle.get() {
            a.set_speaker(b.is_active())
        }
    });
    let a = app.clone();
    reply.connect_activate(move |_| a.ussd_respond());
    let a = app.clone();
    send.connect_clicked(move |_| a.ussd_respond());
    let a = app.clone();
    close.connect_clicked(move |_| a.ussd_close());

    let keys = gtk::EventControllerKey::new();
    let a = app.clone();
    keys.connect_key_pressed(move |_, key, _, state| a.on_key(key, state));
    window.add_controller(keys);

    let a = app.clone();
    window.connect_close_request(move |_| {
        if a.call_path.borrow().is_some() {
            return glib::Propagation::Stop; // not while a call is up
        }
        a.stop_ringing();
        sound::stop(&mut a.audio.borrow_mut());
        a.prox.borrow_mut().take();
        a.screen.borrow_mut().on();
        glib::Propagation::Proceed
    });
    for sig in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
        let a = app.clone();
        glib::unix_signal_add_local(sig, move || {
            // the screen comes back whatever happens to the call
            a.prox.borrow_mut().take();
            a.screen.borrow_mut().on();
            a.window.application().map(|g| g.quit());
            glib::ControlFlow::Break
        });
    }

    app.refresh();
    // maximize once mapped: before, Weston's 0 x 0 maximized configure is an error (titan_ui4)
    window.connect_map(|w| {
        let w = w.clone();
        glib::idle_add_local_once(move || w.maximize());
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

/// one phone app at a time: the Python titan-dialer's lock, so the two never run together
fn single_instance() -> bool {
    use std::os::fd::IntoRawFd;
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    let Ok(f) = std::fs::File::create(format!("{dir}/titan-dialer.lock")) else { return true };
    unsafe { libc::flock(f.into_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) == 0 }
}

fn main() -> glib::ExitCode {
    std::env::set_var("GTK_A11Y", "none");
    if !single_instance() {
        return glib::ExitCode::SUCCESS;
    }
    use_big_cores();
    let app = gtk::Application::builder()
        .application_id("org.titanpocket.dialer")
        .flags(gtk::gio::ApplicationFlags::NON_UNIQUE)
        .build();
    app.connect_activate(build);
    app.run_with_args::<&str>(&[])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ussd_codes() {
        assert!(is_ussd("*100#"));
        assert!(is_ussd("#31#"));
        assert!(is_ussd("*#06#"));
        assert!(!is_ussd("5551234"));
        assert!(!is_ussd("*100"));
        assert!(!is_ussd("#"));
    }

    #[test]
    fn emergency() {
        assert!(is_emergency("911"));
        assert!(is_emergency("1-1-2"));
        assert!(!is_emergency("9110"));
    }
}
