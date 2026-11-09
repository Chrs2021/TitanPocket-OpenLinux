"""titan_mm: the modem through ModemManager's D-Bus API (docs/modem.md,
"Phone apps"), for titan-dialer, titan-phone-settings and titan-call-watch.

Plain Gio proxies on the system bus (no libmm-glib): the first modem
ModemManager exports, its interfaces (Modem, Modem3gpp, Ussd, Voice), its
SIM and its calls, followed as ModemManager and the modem come and go.

    client = Client()       "changed" when anything about the modem
                            changes (it appears or goes, a property);
                            "call-added" / "call-removed" (the call's
                            path), "call-state" (path, old, new)
    client.prop(I_MODEM, "State")       a cached property, unpacked
    client.call(I_MODEM, "Enable", "(b)", (True,), done)
                            async; done(result, error) with the result
                            unpacked (a tuple) or error a short message
    client.call_prop(path, "State"), client.call_method(path, ...)

The modem itself runs as titan-modem.service (off by default: it holds a
wake lock), in flight mode until ModemManager enables it. Identities
(IMEI, IMSI, ICCID) are for the phone's screen only: mask() them anywhere
else.
"""

import json
import socket
import os
import subprocess

from gi.repository import Gio, GLib, GObject

MM = "org.freedesktop.ModemManager1"
MM_PATH = "/org/freedesktop/ModemManager1"
I_MODEM = MM + ".Modem"
I_3GPP = MM + ".Modem.Modem3gpp"
I_USSD = MM + ".Modem.Modem3gpp.Ussd"
I_VOICE = MM + ".Modem.Voice"
I_SIGNAL = MM + ".Modem.Signal"
I_SIM = MM + ".Sim"
I_CALL = MM + ".Call"
MODEM_SERVICE = "titan-modem"

# MMModemState
STATES = {-1: "failed", 0: "unknown", 1: "starting", 2: "locked", 3: "radio off",
          4: "turning off", 5: "turning on", 6: "radio on", 7: "searching",
          8: "registered", 9: "disconnecting", 10: "connecting", 11: "connected"}
STATE_FAILED, STATE_LOCKED, STATE_DISABLED, STATE_ENABLED = -1, 2, 3, 6
# MMModemStateFailedReason
FAILED_REASONS = {1: "unknown", 2: "no SIM", 3: "SIM error", 4: "unknown capabilities",
                  5: "eSIM without profiles"}
# MMModem3gppRegistrationState
REGISTRATION = {0: "not registered", 1: "home network", 2: "searching", 3: "denied",
                4: "unknown", 5: "roaming", 6: "home (SMS only)", 7: "roaming (SMS only)",
                8: "emergency only", 9: "home (CSFB not preferred)",
                10: "roaming (CSFB not preferred)", 11: "attached (RLOS)"}
# MMModemLock
LOCKS = {0: "unknown", 1: "none", 2: "SIM PIN", 3: "SIM PIN2", 4: "SIM PUK", 5: "SIM PUK2",
         6: "service provider PIN", 7: "service provider PUK", 8: "network PIN",
         9: "network PUK", 10: "PH-SIM PIN", 11: "corporate PIN", 12: "corporate PUK",
         13: "PH-FSIM PIN", 14: "PH-FSIM PUK", 15: "network subset PIN",
         16: "network subset PUK"}
LOCK_NONE, LOCK_SIM_PIN, LOCK_SIM_PUK = 1, 2, 4
# MMModemAccessTechnology (bits)
ACCESS = ((1 << 14, "LTE"), (1 << 15, "5G"), (1 << 5, "HSPA+"), (1 << 4, "HSPA"),
          (1 << 3, "HSUPA"), (1 << 2, "HSDPA"), (1 << 1, "UMTS"), (1 << 8, "EDGE"),
          (1 << 7, "GPRS"), (1 << 6, "GSM"))
# MMModemMode (bits)
MODE_2G, MODE_3G, MODE_4G, MODE_5G = 1 << 1, 1 << 2, 1 << 3, 1 << 4
# MMCallState, MMCallDirection
CALL_STATES = {0: "unknown", 1: "dialing", 2: "ringing", 3: "incoming", 4: "active",
               5: "on hold", 6: "waiting", 7: "ended"}
CALL_DIALING, CALL_RINGING_OUT, CALL_RINGING_IN, CALL_ACTIVE = 1, 2, 3, 4
CALL_HELD, CALL_WAITING, CALL_TERMINATED = 5, 6, 7
CALL_INCOMING, CALL_OUTGOING = 1, 2
IMS_SOCKET = "/run/titan-ims.sock"
IMS_STATES = {"dialing": CALL_DIALING, "ringing": CALL_RINGING_OUT, "incoming": CALL_RINGING_IN,
              "active": CALL_ACTIVE, "ended": CALL_TERMINATED}
# MMModem3gppUssdSessionState
USSD_IDLE, USSD_ACTIVE, USSD_USER_RESPONSE = 1, 2, 3
# MMModem3gppNetworkAvailability
AVAILABILITY = {0: "unknown", 1: "available", 2: "current", 3: "forbidden"}


def access_text(bits):
    for bit, name in ACCESS:
        if bits & bit:
            return name
    return ""


def modes_text(modes):
    return "/".join(n for bit, n in ((MODE_5G, "5G"), (MODE_4G, "4G"), (MODE_3G, "3G"),
                                     (MODE_2G, "2G")) if modes & bit) or "any"


def mask(value, keep=4):
    """an identity with only its last digits"""
    value = value or ""
    tail = value[-keep:] if keep > 0 else ""
    return "•" * (len(value) - len(tail)) + tail


def error_text(e):
    """a GLib.Error from ModemManager without its D-Bus prefix"""
    msg = getattr(e, "message", str(e))
    if msg.startswith("GDBus.Error:"):
        msg = msg.split(": ", 1)[-1]
    return msg


def service_active(name=MODEM_SERVICE):
    return subprocess.run(["systemctl", "is-active", "--quiet", name]).returncode == 0


def run_async(cmd, done=None):
    """run a command without blocking the UI; done(returncode, output)"""
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
    except OSError as e:
        if done:
            GLib.idle_add(lambda: done(127, str(e)) and False)
        return

    def reaped(pid, status):
        # GLib reaped it: the code comes from its wait status
        out = proc.stdout.read() if proc.stdout else ""
        proc.returncode = os.waitstatus_to_exitcode(status)
        if done:
            done(proc.returncode, out.strip())
    GLib.child_watch_add(GLib.PRIORITY_DEFAULT, proc.pid, reaped)


class Client(GObject.Object):
    __gsignals__ = {
        "changed": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "call-added": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
        "call-removed": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
        "call-state": (GObject.SignalFlags.RUN_FIRST, None, (str, int, int)),
        "text-added": (GObject.SignalFlags.RUN_FIRST, None, ()),
    }

    def __init__(self):
        super().__init__()
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self.ims = None             # titan-ims's control socket
        self.ims_buf = b""
        self.ims_on = False         # registered: calls go there
        self.ims_calls = {}         # "ims:N" -> {id, state, dir, number}
        self.ims_dial_done = None
        self._ims_connect()
        GLib.timeout_add_seconds(5, self._ims_connect)
        self.path = None            # the modem's object path
        self.ifaces = {}            # interface name -> proxy
        self.sim = None             # the SIM's proxy
        self.calls = {}             # call path -> proxy
        self.owner = None
        self._refresh_pending = False
        self.bus.signal_subscribe(MM, "org.freedesktop.DBus.ObjectManager", None, MM_PATH,
                                  None, Gio.DBusSignalFlags.NONE,
                                  lambda *a: self.refresh_later())
        Gio.bus_watch_name(Gio.BusType.SYSTEM, MM, Gio.BusNameWatcherFlags.NONE,
                           self._appeared, self._vanished)

    # ---- following ModemManager and the modem ----

    def _appeared(self, conn, name, owner):
        self.owner = owner
        self.refresh_later()

    def _vanished(self, conn, name):
        self.owner = None
        self._drop()
        self.emit("changed")

    def _drop(self):
        for path in list(self.calls):
            self.calls.pop(path)
            self.emit("call-removed", path)
        self.path, self.ifaces, self.sim = None, {}, None

    def refresh_later(self):
        if not self._refresh_pending:
            self._refresh_pending = True
            GLib.timeout_add(150, self._refresh)

    def _refresh(self):
        self._refresh_pending = False
        try:
            objs = self.bus.call_sync(MM, MM_PATH, "org.freedesktop.DBus.ObjectManager",
                                      "GetManagedObjects", None,
                                      GLib.VariantType("(a{oa{sa{sv}}})"),
                                      Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
        except GLib.Error:
            objs = {}
        modems = sorted(p for p, ifs in objs.items() if I_MODEM in ifs)
        path = modems[0] if modems else None
        if path != self.path:
            self._drop()
            self.path = path
        if path:
            for iface in (I_MODEM, I_3GPP, I_USSD, I_VOICE, I_SIGNAL):
                if iface in objs[path] and iface not in self.ifaces:
                    self.ifaces[iface] = self._proxy(path, iface)
            for iface in list(self.ifaces):
                if iface not in objs[path]:
                    self.ifaces.pop(iface)
            self._follow_sim()
            if I_VOICE in self.ifaces:
                for call in self.prop(I_VOICE, "Calls", []):
                    self._add_call(call)
        self.emit("changed")
        return False

    def _proxy(self, path, iface):
        p = Gio.DBusProxy.new_sync(self.bus, Gio.DBusProxyFlags.NONE, None, MM, path, iface, None)
        p.connect("g-properties-changed", self._props_changed)
        p.connect("g-signal", self._signal)
        return p

    def _props_changed(self, proxy, changed, invalidated):
        if proxy.get_interface_name() == I_MODEM and "Sim" in changed.keys():
            self._follow_sim()
        if proxy.get_interface_name() == I_CALL:
            return
        self.emit("changed")

    def _follow_sim(self):
        sim = self.prop(I_MODEM, "Sim", "/")
        if sim in ("/", "", None):
            self.sim = None
        elif not self.sim or self.sim.get_object_path() != sim:
            self.sim = self._proxy(sim, I_SIM)

    def _signal(self, proxy, sender, name, params):
        iface = proxy.get_interface_name()
        if iface == I_VOICE and name == "CallAdded":
            self._add_call(params.unpack()[0])
        elif iface == I_VOICE and name == "CallDeleted":
            path = params.unpack()[0]
            if self.calls.pop(path, None) is not None:
                self.emit("call-removed", path)
        elif iface == I_CALL and name == "StateChanged":
            old, new, _ = params.unpack()
            self.emit("call-state", proxy.get_object_path(), old, new)

    def _add_call(self, path):
        if path in self.calls:
            return
        self.calls[path] = self._proxy(path, I_CALL)
        self.emit("call-added", path)

    # ---- reading ----

    # ---- titan-ims: calls over LTE, the same signals and call properties as ModemManager's ----

    def _ims_connect(self):
        if self.ims is None:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(IMS_SOCKET)
            except OSError:
                s.close()
                return True
            self.ims = s
            GLib.io_add_watch(s.fileno(), GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self._ims_read)
            self.ims_send({"cmd": "status"})
        return True

    def ims_send(self, msg):
        if self.ims is not None:
            try:
                self.ims.sendall((json.dumps(msg) + "\n").encode())
            except OSError:
                pass

    def _ims_read(self, fd, cond):
        try:
            d = self.ims.recv(8192)
        except OSError:
            d = b""
        if not d:
            self.ims.close()
            self.ims = None
            self.ims_buf = b""
            self.ims_on = False
            for path in list(self.ims_calls):
                self._ims_call({"id": int(path[4:]), "state": "ended"})
            self.emit("changed")
            return False
        self.ims_buf += d
        while b"\n" in self.ims_buf:
            line, self.ims_buf = self.ims_buf.split(b"\n", 1)
            try:
                self._ims_event(json.loads(line))
            except ValueError:
                pass
        return True

    def _ims_event(self, ev):
        kind = ev.get("ev")
        if kind == "registered":
            self.ims_on = bool(ev.get("on"))
            self.emit("changed")
        elif kind == "status":
            self.ims_on = bool(ev.get("registered"))
            for c in ev.get("calls", ()):
                self._ims_call(c)
            self.emit("changed")
        elif kind == "call":
            self._ims_call(ev)
        elif kind == "sms":
            self.emit("text-added")
        elif kind == "error" and self.ims_dial_done:
            done, self.ims_dial_done = self.ims_dial_done, None
            done(None, ev.get("text", "call failed"))

    def _ims_call(self, ev):
        path = "ims:%d" % ev["id"]
        old = self.ims_calls.get(path)
        new = {**(old or {}), **ev}
        self.ims_calls[path] = new
        st = IMS_STATES.get(new["state"], 0)
        if old is None:
            self.emit("call-added", path)
            if not new.get("dir") == "in" and self.ims_dial_done:
                done, self.ims_dial_done = self.ims_dial_done, None
                done(path, None)
        else:
            self.emit("call-state", path, IMS_STATES.get(old["state"], 0), st)
        if new["state"] == "ended":
            del self.ims_calls[path]
            self.emit("call-removed", path)

    def ims_call_prop(self, path, name, default):
        c = self.ims_calls.get(path)
        if c is None:
            return CALL_TERMINATED if name == "State" else default
        if name == "State":
            return IMS_STATES.get(c["state"], 0)
        if name == "Direction":
            return CALL_INCOMING if c.get("dir") == "in" else CALL_OUTGOING
        if name == "Number":
            return c.get("number", "")
        return default

    def is_ims(self, path):
        return bool(path) and path.startswith("ims:")

    def present(self):
        return self.path is not None

    def has(self, iface):
        if iface == I_VOICE and self.ims_on:
            return True
        return iface in self.ifaces

    def prop(self, iface, name, default=None):
        proxy = self.sim if iface == I_SIM else self.ifaces.get(iface)
        v = proxy.get_cached_property(name) if proxy else None
        return default if v is None else v.unpack()

    def call_prop(self, path, name, default=None):
        if self.is_ims(path):
            return self.ims_call_prop(path, name, default)
        proxy = self.calls.get(path)
        v = proxy.get_cached_property(name) if proxy else None
        return default if v is None else v.unpack()

    def state(self):
        return self.prop(I_MODEM, "State", 0) if self.path else None

    def signal_quality(self):
        q = self.prop(I_MODEM, "SignalQuality", (0, False))
        return q[0] if q else 0

    def status_line(self):
        """one line for a header: the state, the network, the signal"""
        if not self.owner:
            return "ModemManager is not running"
        if not self.path:
            return "modem off" if not service_active() else "modem starting…"
        st = self.state()
        if st == STATE_FAILED:
            return "modem failed: " + FAILED_REASONS.get(
                self.prop(I_MODEM, "StateFailedReason", 1), "unknown")
        if st == STATE_LOCKED:
            return "SIM locked: " + LOCKS.get(self.prop(I_MODEM, "UnlockRequired", 0), "PIN")
        if st is not None and st <= STATE_DISABLED:
            return "radio off (flight mode)"
        op = self.prop(I_3GPP, "OperatorName", "") or self.prop(I_3GPP, "OperatorCode", "")
        reg = REGISTRATION.get(self.prop(I_3GPP, "RegistrationState", 4), "")
        tech = access_text(self.prop(I_MODEM, "AccessTechnologies", 0))
        parts = [op or reg, tech, "VoLTE" if self.ims_on else "", "%d %%" % self.signal_quality() if op else ""]
        return " · ".join(p for p in parts if p) or STATES.get(st, "")

    # ---- doing ----

    def call(self, iface, method, sig=None, args=None, done=None, timeout=60000, path=None):
        """call a method; done(result, error)"""
        if path:
            proxy = self.calls.get(path)
        else:
            proxy = self.sim if iface == I_SIM else self.ifaces.get(iface)
        if proxy is None:
            if done:
                GLib.idle_add(lambda: done(None, "the modem is not available") and False)
            return
        params = GLib.Variant(sig, args) if sig else None

        def finished(p, res, _):
            try:
                out = p.call_finish(res).unpack()
            except GLib.Error as e:
                if done:
                    done(None, error_text(e))
                return
            if done:
                done(out, None)
        proxy.call(method, params, Gio.DBusCallFlags.NONE, timeout, None, finished, None)

    def call_method(self, path, method, sig=None, args=None, done=None, timeout=30000):
        if self.is_ims(path):
            cid = int(path[4:])
            if method == "Accept":
                self.ims_send({"cmd": "answer", "id": cid})
            elif method == "Hangup":
                self.ims_send({"cmd": "hangup", "id": cid})
            elif method == "SendDtmf":
                self.ims_send({"cmd": "dtmf", "digit": args[0]})
            if done:
                GLib.idle_add(lambda: done(None, None) and False)
            return
        self.call(I_CALL, method, sig, args, done, timeout, path=path)

    def dial(self, number, done):
        """create a call to number and start it; done(path, error)"""
        if self.ims_on:
            self.ims_dial_done = done
            self.ims_send({"cmd": "dial", "number": number})
            return
        def created(res, err):
            if err:
                done(None, err)
                return
            path = res[0]
            self._add_call(path)
            self.call_method(path, "Start", done=lambda r, e: done(path, e))
        self.call(I_VOICE, "CreateCall", "(a{sv})", ({"number": GLib.Variant("s", number)},),
                  created)
