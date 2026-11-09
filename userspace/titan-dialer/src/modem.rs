//! The modem for the phone app: ModemManager's D-Bus API (plain gio proxies on the system bus,
//! as titan_mm.py) and titan-ims's control socket (calls over LTE). Calls go to titan-ims while
//! it is registered; its calls look like ModemManager's here ("ims:N" paths, the same states).
//! Everything runs on the GLib main loop; changes reach the app through one callback.
//! Identities (IMEI) are for the phone's screen only.
use gtk::gio::prelude::*;
use gtk::{gio, glib};
use serde_json::{json, Value};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::rc::Rc;

const MM: &str = "org.freedesktop.ModemManager1";
const MM_PATH: &str = "/org/freedesktop/ModemManager1";
pub const I_MODEM: &str = "org.freedesktop.ModemManager1.Modem";
pub const I_3GPP: &str = "org.freedesktop.ModemManager1.Modem.Modem3gpp";
pub const I_USSD: &str = "org.freedesktop.ModemManager1.Modem.Modem3gpp.Ussd";
pub const I_VOICE: &str = "org.freedesktop.ModemManager1.Modem.Voice";
const I_CALL: &str = "org.freedesktop.ModemManager1.Call";
const IMS_SOCKET: &str = "/run/titan-ims.sock";

// MMModemState
pub const STATE_FAILED: i32 = -1;
pub const STATE_LOCKED: i32 = 2;
pub const STATE_DISABLED: i32 = 3;
pub const STATE_ENABLED: i32 = 6;
// MMCallState, MMCallDirection
pub const CALL_DIALING: i32 = 1;
pub const CALL_RINGING_OUT: i32 = 2;
pub const CALL_RINGING_IN: i32 = 3;
pub const CALL_ACTIVE: i32 = 4;
pub const CALL_WAITING: i32 = 6;
pub const CALL_TERMINATED: i32 = 7;
pub const CALL_INCOMING: i32 = 1;
// MMModem3gppUssdSessionState
pub const USSD_IDLE: u32 = 1;
pub const USSD_USER_RESPONSE: u32 = 3;

pub fn call_state_text(s: i32) -> &'static str {
    match s {
        1 => "dialing",
        2 => "ringing",
        3 => "incoming",
        4 => "active",
        5 => "on hold",
        6 => "waiting",
        7 => "ended",
        _ => "",
    }
}

fn state_text(s: i32) -> &'static str {
    match s {
        -1 => "failed",
        1 => "starting",
        2 => "locked",
        3 => "radio off",
        4 => "turning off",
        5 => "turning on",
        6 => "radio on",
        7 => "searching",
        8 => "registered",
        9 => "disconnecting",
        10 => "connecting",
        11 => "connected",
        _ => "unknown",
    }
}

fn registration_text(r: u32) -> &'static str {
    match r {
        0 => "not registered",
        1 => "home network",
        2 => "searching",
        3 => "denied",
        5 => "roaming",
        6 => "home (SMS only)",
        7 => "roaming (SMS only)",
        8 => "emergency only",
        _ => "",
    }
}

fn access_text(bits: u32) -> &'static str {
    for (bit, name) in [(1 << 14, "LTE"), (1 << 15, "5G"), (1 << 5, "HSPA+"), (1 << 4, "HSPA"), (1 << 1, "UMTS"), (1 << 8, "EDGE"), (1 << 7, "GPRS"), (1 << 6, "GSM")] {
        if bits & bit != 0 {
            return name;
        }
    }
    ""
}

fn ims_state(s: &str) -> i32 {
    match s {
        "dialing" => CALL_DIALING,
        "ringing" => CALL_RINGING_OUT,
        "incoming" => CALL_RINGING_IN,
        "active" => CALL_ACTIVE,
        "ended" => CALL_TERMINATED,
        _ => 0,
    }
}

/// a D-Bus error without its "GDBus.Error:name: " prefix
fn error_text(e: &glib::Error) -> String {
    let m = e.message();
    if m.starts_with("GDBus.Error:") {
        m.split_once(": ").map(|x| x.1).unwrap_or(m).to_string()
    } else {
        m.to_string()
    }
}

pub enum Event {
    Changed,
    CallAdded(String),
    CallState(String),
    CallRemoved(String),
}

pub type Done = Box<dyn FnOnce(Result<Option<glib::Variant>, String>)>;
type DialDone = Box<dyn FnOnce(Result<String, String>)>;

#[derive(Clone, Default)]
struct ImsCall {
    state: String,
    incoming: bool,
    number: String,
}

pub struct Client {
    bus: gio::DBusConnection,
    owner: Cell<bool>,
    path: RefCell<Option<String>>,
    ifaces: RefCell<HashMap<&'static str, gio::DBusProxy>>,
    calls: RefCell<HashMap<String, gio::DBusProxy>>,
    refresh_pending: Cell<bool>,
    ims: RefCell<Option<UnixStream>>,
    ims_buf: RefCell<Vec<u8>>,
    pub ims_on: Cell<bool>,
    ims_calls: RefCell<HashMap<String, ImsCall>>,
    dial_done: RefCell<Option<DialDone>>,
    on_event: RefCell<Option<Rc<dyn Fn(Event)>>>,
}

impl Client {
    pub fn new() -> Rc<Client> {
        let bus = gio::bus_get_sync(gio::BusType::System, None::<&gio::Cancellable>).expect("system bus");
        let c = Rc::new(Client {
            bus: bus.clone(),
            owner: Cell::new(false),
            path: RefCell::new(None),
            ifaces: RefCell::new(HashMap::new()),
            calls: RefCell::new(HashMap::new()),
            refresh_pending: Cell::new(false),
            ims: RefCell::new(None),
            ims_buf: RefCell::new(Vec::new()),
            ims_on: Cell::new(false),
            ims_calls: RefCell::new(HashMap::new()),
            dial_done: RefCell::new(None),
            on_event: RefCell::new(None),
        });
        let w = Rc::downgrade(&c);
        #[allow(deprecated)]
        bus.signal_subscribe(Some(MM), Some("org.freedesktop.DBus.ObjectManager"), None, Some(MM_PATH), None, gio::DBusSignalFlags::NONE, move |_, _, _, _, _, _| {
            if let Some(c) = w.upgrade() {
                c.refresh_later();
            }
        });
        let (wa, wv) = (Rc::downgrade(&c), Rc::downgrade(&c));
        // the watcher lives as long as the process
        std::mem::forget(gio::bus_watch_name(
            gio::BusType::System,
            MM,
            gio::BusNameWatcherFlags::NONE,
            move |_, _, _| {
                if let Some(c) = wa.upgrade() {
                    c.owner.set(true);
                    c.refresh_later();
                }
            },
            move |_, _| {
                if let Some(c) = wv.upgrade() {
                    c.owner.set(false);
                    c.drop_modem();
                    c.emit(Event::Changed);
                }
            },
        ));
        c.ims_connect();
        let w = Rc::downgrade(&c);
        glib::timeout_add_seconds_local(5, move || match w.upgrade() {
            Some(c) => {
                c.ims_connect();
                glib::ControlFlow::Continue
            }
            None => glib::ControlFlow::Break,
        });
        c
    }

    pub fn connect(&self, f: impl Fn(Event) + 'static) {
        *self.on_event.borrow_mut() = Some(Rc::new(f));
    }

    fn emit(&self, e: Event) {
        let f = self.on_event.borrow().clone();
        if let Some(f) = f {
            f(e);
        }
    }

    // ---- following ModemManager and the modem ----

    fn drop_modem(&self) {
        let paths: Vec<String> = self.calls.borrow_mut().drain().map(|(p, _)| p).collect();
        for p in paths {
            self.emit(Event::CallRemoved(p));
        }
        *self.path.borrow_mut() = None;
        self.ifaces.borrow_mut().clear();
    }

    fn refresh_later(self: &Rc<Self>) {
        if self.refresh_pending.replace(true) {
            return;
        }
        let w = Rc::downgrade(self);
        glib::timeout_add_local_once(std::time::Duration::from_millis(150), move || {
            if let Some(c) = w.upgrade() {
                c.refresh();
            }
        });
    }

    fn refresh(self: &Rc<Self>) {
        self.refresh_pending.set(false);
        let objs = self
            .bus
            .call_sync(Some(MM), MM_PATH, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", None, Some(glib::VariantTy::new("(a{oa{sa{sv}}})").unwrap()), gio::DBusCallFlags::NONE, 5000, None::<&gio::Cancellable>)
            .ok();
        let mut modems: Vec<(String, Vec<String>)> = Vec::new();
        if let Some(o) = objs {
            let dict = o.child_value(0);
            for i in 0..dict.n_children() {
                let e = dict.child_value(i);
                let path = e.child_value(0).str().unwrap_or_default().to_string();
                let ifs = e.child_value(1);
                let names: Vec<String> = (0..ifs.n_children()).filter_map(|j| ifs.child_value(j).child_value(0).str().map(String::from)).collect();
                if names.iter().any(|n| n == I_MODEM) {
                    modems.push((path, names));
                }
            }
        }
        modems.sort();
        let first = modems.into_iter().next();
        let path = first.as_ref().map(|m| m.0.clone());
        if path != *self.path.borrow() {
            self.drop_modem();
            *self.path.borrow_mut() = path.clone();
        }
        if let (Some(path), Some((_, names))) = (path, first) {
            for iface in [I_MODEM, I_3GPP, I_USSD, I_VOICE] {
                let has = names.iter().any(|n| n == iface);
                let known = self.ifaces.borrow().contains_key(iface);
                if has && !known {
                    if let Some(p) = self.proxy(&path, iface) {
                        self.ifaces.borrow_mut().insert(iface, p);
                    }
                } else if !has && known {
                    self.ifaces.borrow_mut().remove(iface);
                }
            }
            if let Some(calls) = self.prop(I_VOICE, "Calls") {
                for i in 0..calls.n_children() {
                    if let Some(p) = calls.child_value(i).str() {
                        self.add_call(p);
                    }
                }
            }
        }
        self.emit(Event::Changed);
    }

    fn proxy(self: &Rc<Self>, path: &str, iface: &str) -> Option<gio::DBusProxy> {
        let p = gio::DBusProxy::new_sync(&self.bus, gio::DBusProxyFlags::NONE, None::<&gio::DBusInterfaceInfo>, Some(MM), path, iface, None::<&gio::Cancellable>).ok()?;
        let w = Rc::downgrade(self);
        let is_call = iface == I_CALL;
        // connect_local: gio-rs's typed connectors want Send closures, and these hold Rcs
        p.connect_local("g-properties-changed", false, move |_| {
            if let Some(c) = w.upgrade() {
                if !is_call {
                    c.emit(Event::Changed);
                }
            }
            None
        });
        let w = Rc::downgrade(self);
        p.connect_local("g-signal", false, move |v| {
            let (Ok(p), Ok(name), Ok(params)) = (v[0].get::<gio::DBusProxy>(), v[2].get::<String>(), v[3].get::<glib::Variant>()) else { return None };
            let name = name.as_str();
            let Some(c) = w.upgrade() else { return None };
            let iface = p.interface_name();
            match (iface.as_str(), name) {
                (I_VOICE, "CallAdded") => {
                    if let Some(path) = params.child_value(0).str() {
                        c.add_call(path);
                    }
                }
                (I_VOICE, "CallDeleted") => {
                    if let Some(path) = params.child_value(0).str() {
                        if c.calls.borrow_mut().remove(path).is_some() {
                            c.emit(Event::CallRemoved(path.to_string()));
                        }
                    }
                }
                (I_CALL, "StateChanged") => {
                    c.emit(Event::CallState(p.object_path().to_string()));
                }
                _ => {}
            }
            None
        });
        Some(p)
    }

    fn add_call(self: &Rc<Self>, path: &str) {
        if self.calls.borrow().contains_key(path) {
            return;
        }
        if let Some(p) = self.proxy(path, I_CALL) {
            self.calls.borrow_mut().insert(path.to_string(), p);
            self.emit(Event::CallAdded(path.to_string()));
        }
    }

    // ---- titan-ims ----

    fn ims_connect(self: &Rc<Self>) {
        if self.ims.borrow().is_some() {
            return;
        }
        let Ok(s) = UnixStream::connect(IMS_SOCKET) else { return };
        let _ = s.set_nonblocking(true);
        let fd = s.as_raw_fd();
        *self.ims.borrow_mut() = Some(s);
        let w = Rc::downgrade(self);
        glib::source::unix_fd_add_local(fd, glib::IOCondition::IN | glib::IOCondition::HUP | glib::IOCondition::ERR, move |_, _| match w.upgrade() {
            Some(c) if c.ims_read() => glib::ControlFlow::Continue,
            _ => glib::ControlFlow::Break,
        });
        self.ims_send(json!({"cmd": "status"}));
    }

    pub fn ims_send(&self, msg: Value) {
        if let Some(s) = self.ims.borrow_mut().as_mut() {
            let _ = s.write_all(format!("{msg}\n").as_bytes());
        }
    }

    /// false: the socket closed (calls on it end)
    fn ims_read(self: &Rc<Self>) -> bool {
        let mut buf = [0u8; 8192];
        let n = match self.ims.borrow_mut().as_mut().map(|s| s.read(&mut buf)) {
            Some(Ok(n)) => n,
            Some(Err(e)) if e.kind() == std::io::ErrorKind::WouldBlock => return true,
            _ => 0,
        };
        if n == 0 {
            *self.ims.borrow_mut() = None;
            self.ims_buf.borrow_mut().clear();
            self.ims_on.set(false);
            let ids: Vec<String> = self.ims_calls.borrow().keys().cloned().collect();
            for path in ids {
                let id: i64 = path[4..].parse().unwrap_or(0);
                self.ims_call(&json!({"id": id, "state": "ended"}));
            }
            self.emit(Event::Changed);
            return false;
        }
        self.ims_buf.borrow_mut().extend_from_slice(&buf[..n]);
        loop {
            let line = {
                let mut b = self.ims_buf.borrow_mut();
                let Some(i) = b.iter().position(|&c| c == b'\n') else { break };
                let line: Vec<u8> = b.drain(..=i).collect();
                line
            };
            if let Ok(v) = serde_json::from_slice::<Value>(&line) {
                self.ims_event(&v);
            }
        }
        true
    }

    fn ims_event(self: &Rc<Self>, ev: &Value) {
        match ev["ev"].as_str() {
            Some("registered") => {
                self.ims_on.set(ev["on"].as_bool().unwrap_or(false));
                self.emit(Event::Changed);
            }
            Some("status") => {
                self.ims_on.set(ev["registered"].as_bool().unwrap_or(false));
                if let Some(calls) = ev["calls"].as_array() {
                    for c in calls {
                        self.ims_call(c);
                    }
                }
                self.emit(Event::Changed);
            }
            Some("call") => self.ims_call(ev),
            Some("error") => {
                if let Some(done) = self.dial_done.borrow_mut().take() {
                    done(Err(ev["text"].as_str().unwrap_or("call failed").to_string()));
                }
            }
            _ => {}
        }
    }

    fn ims_call(self: &Rc<Self>, ev: &Value) {
        let Some(id) = ev["id"].as_i64() else { return };
        let path = format!("ims:{id}");
        let old = self.ims_calls.borrow().get(&path).cloned();
        let mut new = old.clone().unwrap_or_default();
        if let Some(s) = ev["state"].as_str() {
            new.state = s.to_string();
        }
        if let Some(d) = ev["dir"].as_str() {
            new.incoming = d == "in";
        }
        if let Some(n) = ev["number"].as_str() {
            new.number = n.to_string();
        }
        self.ims_calls.borrow_mut().insert(path.clone(), new.clone());
        if old.is_none() {
            self.emit(Event::CallAdded(path.clone()));
            if !new.incoming {
                if let Some(done) = self.dial_done.borrow_mut().take() {
                    done(Ok(path.clone()));
                }
            }
        } else {
            self.emit(Event::CallState(path.clone()));
        }
        if new.state == "ended" {
            self.ims_calls.borrow_mut().remove(&path);
            self.emit(Event::CallRemoved(path));
        }
    }

    pub fn is_ims(path: &str) -> bool {
        path.starts_with("ims:")
    }

    // ---- reading ----

    pub fn present(&self) -> bool {
        self.path.borrow().is_some()
    }

    pub fn has(&self, iface: &str) -> bool {
        (iface == I_VOICE && self.ims_on.get()) || self.ifaces.borrow().contains_key(iface)
    }

    pub fn prop(&self, iface: &str, name: &str) -> Option<glib::Variant> {
        self.ifaces.borrow().get(iface)?.cached_property(name)
    }

    pub fn prop_i32(&self, iface: &str, name: &str) -> Option<i32> {
        self.prop(iface, name).and_then(|v| v.get::<i32>())
    }

    pub fn prop_u32(&self, iface: &str, name: &str) -> Option<u32> {
        self.prop(iface, name).and_then(|v| v.get::<u32>())
    }

    pub fn prop_str(&self, iface: &str, name: &str) -> String {
        self.prop(iface, name).and_then(|v| v.str().map(String::from)).unwrap_or_default()
    }

    pub fn state(&self) -> Option<i32> {
        self.present().then(|| self.prop_i32(I_MODEM, "State").unwrap_or(0))
    }

    pub fn call_state(&self, path: &str) -> i32 {
        if Self::is_ims(path) {
            return self.ims_calls.borrow().get(path).map(|c| ims_state(&c.state)).unwrap_or(CALL_TERMINATED);
        }
        self.calls.borrow().get(path).and_then(|p| p.cached_property("State")).and_then(|v| v.get::<i32>()).unwrap_or(0)
    }

    pub fn call_incoming(&self, path: &str) -> bool {
        if Self::is_ims(path) {
            return self.ims_calls.borrow().get(path).map(|c| c.incoming).unwrap_or(false);
        }
        self.calls.borrow().get(path).and_then(|p| p.cached_property("Direction")).and_then(|v| v.get::<i32>()) == Some(CALL_INCOMING)
    }

    pub fn call_number(&self, path: &str) -> String {
        if Self::is_ims(path) {
            return self.ims_calls.borrow().get(path).map(|c| c.number.clone()).unwrap_or_default();
        }
        self.calls.borrow().get(path).and_then(|p| p.cached_property("Number")).and_then(|v| v.str().map(String::from)).unwrap_or_default()
    }

    /// one line for the header: the state, the network, the signal
    pub fn status_line(&self) -> String {
        if !self.owner.get() {
            return "ModemManager is not running".into();
        }
        if !self.present() {
            return "modem off".into();
        }
        let st = self.state().unwrap_or(0);
        if st == STATE_FAILED {
            return "modem failed".into();
        }
        if st == STATE_LOCKED {
            return "SIM locked".into();
        }
        if st <= STATE_DISABLED {
            return "radio off (flight mode)".into();
        }
        let mut op = self.prop_str(I_3GPP, "OperatorName");
        if op.is_empty() {
            op = self.prop_str(I_3GPP, "OperatorCode");
        }
        let reg = registration_text(self.prop_u32(I_3GPP, "RegistrationState").unwrap_or(4));
        let tech = access_text(self.prop_u32(I_MODEM, "AccessTechnologies").unwrap_or(0));
        let q = self.prop(I_MODEM, "SignalQuality").and_then(|v| v.child_value(0).get::<u32>()).unwrap_or(0);
        let sig = if op.is_empty() { String::new() } else { format!("{q} %") };
        let parts = [if op.is_empty() { reg.to_string() } else { op }, tech.to_string(), if self.ims_on.get() { "VoLTE".into() } else { String::new() }, sig];
        let line = parts.iter().filter(|p| !p.is_empty()).cloned().collect::<Vec<_>>().join(" · ");
        if line.is_empty() { state_text(st).into() } else { line }
    }

    // ---- doing ----

    /// a method on one of the modem's interfaces (or a call's); done(result or error)
    pub fn call(&self, iface: &str, method: &str, args: Option<glib::Variant>, timeout_ms: i32, done: Option<Done>, call_path: Option<&str>) {
        let proxy = match call_path {
            Some(p) => self.calls.borrow().get(p).cloned(),
            None => self.ifaces.borrow().get(iface).cloned(),
        };
        let Some(proxy) = proxy else {
            if let Some(d) = done {
                glib::idle_add_local_once(move || d(Err("the modem is not available".into())));
            }
            return;
        };
        proxy.call(method, args.as_ref(), gio::DBusCallFlags::NONE, timeout_ms, None::<&gio::Cancellable>, move |r| {
            if let Some(d) = done {
                d(r.map(Some).map_err(|e| error_text(&e)));
            }
        });
    }

    /// Accept, Hangup, SendDtmf on a call, wherever it lives
    pub fn call_method(&self, path: &str, method: &str, arg: Option<&str>, done: Option<Done>) {
        if Self::is_ims(path) {
            let id: i64 = path[4..].parse().unwrap_or(0);
            match method {
                "Accept" => self.ims_send(json!({"cmd": "answer", "id": id})),
                "Hangup" => self.ims_send(json!({"cmd": "hangup", "id": id})),
                "SendDtmf" => self.ims_send(json!({"cmd": "dtmf", "digit": arg.unwrap_or("")})),
                _ => {}
            }
            if let Some(d) = done {
                glib::idle_add_local_once(move || d(Ok(None)));
            }
            return;
        }
        let args = arg.map(|a| (a,).to_variant());
        self.call(I_CALL, method, args, 30000, done, Some(path));
    }

    /// a call to `number`: done(path or error)
    pub fn dial(self: &Rc<Self>, number: &str, done: DialDone) {
        if self.ims_on.get() {
            *self.dial_done.borrow_mut() = Some(done);
            self.ims_send(json!({"cmd": "dial", "number": number}));
            return;
        }
        let d = glib::VariantDict::new(None);
        d.insert_value("number", &number.to_variant());
        let args = (d.end(),).to_variant();
        let me = self.clone();
        self.call(
            I_VOICE,
            "CreateCall",
            Some(args),
            60000,
            Some(Box::new(move |r| match r {
                Err(e) => done(Err(e)),
                Ok(v) => {
                    let path = v.and_then(|v| v.child_value(0).str().map(String::from)).unwrap_or_default();
                    me.add_call(&path);
                    let p = path.clone();
                    me.call_method(&path, "Start", None, Some(Box::new(move |r| done(r.map(|_| p)))));
                }
            })),
            None,
        );
    }

    pub fn delete_call(&self, path: &str) {
        if Self::is_ims(path) {
            return;
        }
        if let Ok(op) = glib::variant::ObjectPath::try_from(path.to_string()) {
            self.call(I_VOICE, "DeleteCall", Some((op,).to_variant()), 30000, None, None);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn texts() {
        assert_eq!(access_text(1 << 14 | 1 << 6), "LTE");
        assert_eq!(ims_state("active"), CALL_ACTIVE);
        assert_eq!(call_state_text(CALL_RINGING_IN), "incoming");
        assert_eq!(state_text(STATE_ENABLED), "radio on");
    }
}
