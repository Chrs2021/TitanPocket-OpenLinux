"""titan_ui4: the Titan Pocket's app frame for GTK 4 apps (docs/desktop.md,
"GTK 4 and GPU water").

titan_ui (GTK 3) in titan_water4's glass. An app is a column on the
wallpaper: the design's header pill, then its content in glass panels, and
everything you touch is water drawn on the GPU (titan_water4's controls).

    run(app_id, build)      the app: one at a time (a second launch just
                            quits), on the A73s; build(app) makes and returns
                            its AppWindow, which is shown maximized
    AppWindow(app, title)   the window: Home's wallpaper as a texture (so the
                            water can refract it), the header, then body (a
                            vertical box: the app's content), Esc or Back
                            does what the back button does, and the phone's
                            tilt for the water while the window is active.
                            Water widgets get the wallpaper and the tilt
                            when they're realized (adopt_one), lists filled
                            in later included.
    Header                  the design's app frame: a glass pill, a droplet
                            back button, the title and a subtitle, actions
    GlassButton, RoundButton    a capsule of the glass; a small droplet
    Choice                  one of a few options as glass buttons (lit)
    ICONS                   the board's 24-unit stroke icons by name
    row(label, widget)      a label with a control at the end, in a panel
    CSS (THEME_CSS)         GTK's own widgets (labels, entries, lists,
                            dialogs) in the TitanWater look

The water controls (titan_water4): WaterSlider, WaterRadio, WaterCheck (a
row with a liquid check box: a switch), NavSlider (segmented navigation),
water_scroll (kinetic scrolling with the water scroll bar), panel().
"""

import fcntl
import os

os.environ.setdefault("GTK_A11Y", "none")
import gi  # noqa: E402

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, GObject, Gtk  # noqa: E402

import titan_water4 as water  # noqa: E402
from titan_water4 import GlassButton  # noqa: E402,F401

# the board's icons: 24-unit SVG stroke paths
ICONS = {
    "back": "M15 5l-7 7 7 7",
    "forward": "M9 5l7 7-7 7",
    "up": "M5 15l7-7 7 7",
    "down": "M5 9l7 7 7-7",
    "close": "M6 6l12 12M18 6L6 18",
    "check": "M5 12.5l4.5 4.5L19 7",
    "plus": "M12 5v14M5 12h14",
    "minus": "M5 12h14",
    "search": "M11 4a7 7 0 1 0 0 14a7 7 0 1 0 0-14zM20 20l-4-4",
    "refresh": "M19.5 12a7.5 7.5 0 1 1-2.2-5.3M19.5 4.5v4.5H15",
    "more": "M5 12h.01M12 12h.01M19 12h.01",
    "play": "M8 5.5v13l10.5-6.5z",
    "pause": "M8.5 5.5v13M15.5 5.5v13",
    "stop": "M7 7h10v10H7z",
    "record": "M12 6a6 6 0 1 0 0 12a6 6 0 1 0 0-12z",
    "trash": "M5 7h14M10 7V5h4v2M7 7l1 12h8l1-12",
    "share": "M12 4v11M8 8l4-4 4 4M5 13v6h14v-6",
    "edit": "M5 19l1-4L16 5l3 3L9 18zM14 7l3 3",
    "sun": "M16 12a4 4 0 1 1-8 0a4 4 0 0 1 8 0zM12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2"
           "M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4",
    "sound": "M4 9h4l5-4v14l-5-4H4zM17 9a4 4 0 0 1 0 6M19.5 6.5a8 8 0 0 1 0 11",
    "drop": "M12 3c3.5 4.5 6 7.8 6 11a6 6 0 0 1-12 0c0-3.2 2.5-6.5 6-11z",
    "wifi": "M2.5 9a14 14 0 0 1 19 0M5.5 12.5a9.5 9.5 0 0 1 13 0M8.7 16a5 5 0 0 1 6.6 0M12 19.5h.01",
    "lock": "M7 11V8a5 5 0 0 1 10 0v3M5.5 11h13v9h-13z",
    "location": "M12 21C12 21 19 14.8 19 9.5A7 7 0 0 0 5 9.5C5 14.8 12 21 12 21z"
                "M12 7a2.5 2.5 0 1 0 0 5a2.5 2.5 0 1 0 0-5z",
    "battery": "M4 8h14v8H4zM20 11v2",
    "usb": "M12 3v14M9 6l3-3 3 3M7 10v3l5 3M17 9v3l-5 3M12 17a2 2 0 1 0 0 4a2 2 0 1 0 0-4z",
    "clock": "M12 3.5a8.5 8.5 0 1 0 0 17a8.5 8.5 0 1 0 0-17zM12 7.5V12l3 2",
    "mic": "M12 3.5a3 3 0 0 0-3 3v5a3 3 0 0 0 6 0v-5a3 3 0 0 0-3-3zM6 11a6 6 0 0 0 12 0M12 17v3.5",
    "radio": "M4 9h16v11H4zM8 9l9-5M8 13.5a1.5 1.5 0 1 0 0 3a1.5 1.5 0 1 0 0-3zM14 14h3M14 17h3",
    "photo": "M4 5h16v14H4zM4 16l5-5 4 4 3-3 4 4M15.5 8.5h.01",
    "camera": "M4 8h3.5L9 5.5h6L16.5 8H20v11H4zM12 10a3.5 3.5 0 1 0 0 7a3.5 3.5 0 1 0 0-7z",
    "compass": "M12 3.5a8.5 8.5 0 1 0 0 17a8.5 8.5 0 1 0 0-17zM15.5 8.5l-2 5-5 2 2-5z",
    "keyboard": "M3 7h18v11H3zM7 11h.01M11 11h.01M15 11h.01M8 14.5h8",
    "nfc": "M6 18V6l12 12V6",
    "remote": "M9 3h6v18H9zM12 7h.01M12 11h.01M12 15h.01",
    "info": "M12 3.5a8.5 8.5 0 1 0 0 17a8.5 8.5 0 1 0 0-17zM12 11v5M12 8h.01",
}

THEME_CSS = """
/* GTK's own widgets in the TitanWater look (the GTK 3 theme, ported); the
   water controls draw themselves */
@define-color titan_text #FFFFFF;
@define-color titan_dim rgba(255, 255, 255, 0.78);
@define-color titan_accent #FFD24A;
@define-color titan_danger #FF6B7A;
@define-color titan_ok #7BE07B;
@define-color titan_ink #141821;
@define-color titan_rim rgba(255, 255, 255, 0.62);
@define-color titan_edge rgba(255, 255, 255, 0.36);
@define-color theme_fg_color #FFFFFF;
@define-color theme_text_color #FFFFFF;
@define-color theme_bg_color #1B2C66;
@define-color theme_base_color rgba(255, 255, 255, 0.06);
@define-color theme_selected_bg_color rgba(255, 255, 255, 0.85);
@define-color theme_selected_fg_color #141821;
@define-color insensitive_fg_color rgba(255, 255, 255, 0.45);
@define-color borders rgba(255, 255, 255, 0.30);

* { font-family: "Atkinson Hyperlegible", sans-serif; outline-color: rgba(255, 255, 255, 0.55); }
window.titan-app { background: #10204A; color: @titan_text; }
label { color: @titan_text; text-shadow: 0 1px 2px rgba(0, 0, 0, 0.40); }
label:disabled { color: rgba(255, 255, 255, 0.45); text-shadow: none; }
label selection { background-color: rgba(255, 255, 255, 0.85); color: @titan_ink; }

/* the header pill (the design's app frame) */
.titan-header {
  min-height: 60px; padding: 0 7px; border-radius: 30px;
  border: 1px solid rgba(255, 255, 255, 0.40);
  background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.20),
                    rgba(255, 255, 255, 0.096) 55%, rgba(20, 26, 40, 0.20));
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.65), 0 10px 30px rgba(0, 0, 0, 0.35);
}
.titan-title { font-size: 21px; font-weight: bold; }
.titan-subtitle { font-size: 14px; color: @titan_dim; }
/* a glass bar for other floating groups (the design's bottom segment bar) */
.titan-bar {
  padding: 5px; border-radius: 30px; border: 1px solid rgba(255, 255, 255, 0.40);
  background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.20),
                    rgba(255, 255, 255, 0.096) 55%, rgba(20, 26, 40, 0.20));
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.65), 0 10px 30px rgba(0, 0, 0, 0.35);
}
label.titan-section { font-size: 14px; font-weight: bold; color: @titan_dim; }

/* GTK's own buttons (dialogs, anything not yet a GlassButton): still glass */
button {
  min-height: 44px; padding: 0 16px; border-radius: 22px;
  border: 1px solid @titan_rim; color: @titan_text;
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.45);
  background-color: rgba(255, 255, 255, 0.10); background-image: none;
  box-shadow: inset 0 0 12px rgba(255, 255, 255, 0.26), 0 6px 12px rgba(0, 0, 0, 0.22);
}
button:hover { background-color: rgba(255, 255, 255, 0.14); }
button:active, button:checked { background-color: rgba(255, 255, 255, 0.86); color: @titan_ink; text-shadow: none; }
button:disabled { color: rgba(255, 255, 255, 0.40); background-color: rgba(255, 255, 255, 0.03); box-shadow: none; }
button.flat { border-color: transparent; background: none; box-shadow: none; }
button.suggested-action { color: @titan_ink; text-shadow: none; background-color: rgba(255, 210, 74, 0.92); }
button.destructive-action { background-color: rgba(255, 90, 110, 0.55); }

/* groups: pools of water */
list, listview, .view, flowbox, gridview, textview text { background: none; color: @titan_text; }
list row, listview row {
  color: @titan_text; background: none; padding: 6px 4px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.12);
}
list row:last-child { border-bottom: none; }
list row:hover { background-color: rgba(255, 255, 255, 0.06); }
list row:active { background-color: rgba(255, 255, 255, 0.12); }
list row:selected, flowbox flowboxchild:selected, gridview child:selected {
  background-color: rgba(255, 255, 255, 0.85); color: @titan_ink;
}
list row:selected label { color: @titan_ink; text-shadow: none; }
flowbox flowboxchild { border-radius: 16px; padding: 0; }

/* fields */
entry, spinbutton, textview {
  min-height: 46px; padding: 0 16px; border-radius: 23px;
  border: 1px solid rgba(255, 255, 255, 0.50); color: @titan_text;
  caret-color: @titan_accent; background-color: rgba(10, 16, 34, 0.22);
  box-shadow: inset 0 2px 6px rgba(0, 0, 0, 0.25), inset 0 0 12px rgba(255, 255, 255, 0.14);
}
entry:focus-within, spinbutton:focus-within {
  border-color: rgba(255, 255, 255, 0.90);
  box-shadow: inset 0 2px 6px rgba(0, 0, 0, 0.25), inset 0 0 14px rgba(255, 255, 255, 0.26),
              0 0 0 3px rgba(255, 255, 255, 0.16);
}
entry selection { background-color: rgba(255, 255, 255, 0.85); color: @titan_ink; }
entry image { color: @titan_dim; }
entry placeholder, entry > text > placeholder { color: rgba(255, 255, 255, 0.55); }

/* bars */
progressbar trough, levelbar trough {
  min-height: 10px; border-radius: 9999px; border: 1px solid rgba(255, 255, 255, 0.40);
  background-color: rgba(10, 16, 34, 0.25); box-shadow: inset 0 1px 3px rgba(0, 0, 0, 0.30);
}
progressbar progress, levelbar block.filled {
  min-height: 10px; border-radius: 9999px; border: none;
  background-image: linear-gradient(180deg, #FFFFFF, #7FD8FF);
  box-shadow: 0 0 8px rgba(127, 216, 255, 0.5);
}
levelbar block.empty { background: none; border: none; }

/* GTK's own switch, check, scale (where an app still uses one) */
switch {
  min-width: 64px; min-height: 34px; border-radius: 9999px;
  border: 1px solid rgba(255, 255, 255, 0.50); color: transparent;
  background-color: rgba(10, 16, 34, 0.25);
}
switch:checked { background-color: rgba(255, 255, 255, 0.80); }
switch slider {
  min-width: 32px; min-height: 32px; border-radius: 9999px; border: 1px solid @titan_rim;
  background-color: rgba(255, 255, 255, 0.35); box-shadow: 0 4px 8px rgba(0, 0, 0, 0.30);
}
check, radio {
  min-width: 26px; min-height: 26px; border: 1px solid @titan_rim; color: @titan_ink;
  background-color: rgba(255, 255, 255, 0.06);
}
check { border-radius: 9px; }
radio { border-radius: 9999px; }
check:checked, radio:checked { background-color: rgba(255, 255, 255, 0.90); }
scale trough { min-height: 10px; border-radius: 9999px; background-color: rgba(10, 16, 34, 0.25); }
scale highlight { border-radius: 9999px; background-color: rgba(255, 255, 255, 0.82); }
scale slider { min-width: 34px; min-height: 34px; margin: -13px; border-radius: 9999px;
  border: 1px solid @titan_rim; background-color: rgba(255, 255, 255, 0.25); }

/* stacks and scrollers: nothing of their own */
stack, scrolledwindow, viewport { background: none; }
scrolledwindow undershoot, scrolledwindow overshoot { background: none; box-shadow: none; }
scrollbar { background: none; border: none; }
scrollbar slider { min-width: 8px; min-height: 40px; border-radius: 9999px;
  background-color: rgba(255, 255, 255, 0.40); }

/* floating: popovers, menus, tooltips, dialogs (dark glass) */
popover > contents, popover > arrow, tooltip, window.dialog, window.messagedialog,
.titan-dialog {
  color: @titan_text; border-radius: 24px; border: 1px solid rgba(255, 255, 255, 0.40);
  background-color: rgba(18, 24, 48, 0.94);
  background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.12), rgba(255, 255, 255, 0.0));
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.45), 0 12px 30px rgba(0, 0, 0, 0.45);
}
tooltip { border-radius: 14px; }
.titan-scrim { background-color: rgba(6, 10, 24, 0.55); }
popover modelbutton { min-height: 44px; padding: 0 16px; border-radius: 14px; }
popover modelbutton:hover { background-color: rgba(255, 255, 255, 0.85); color: @titan_ink; }
separator { background-color: rgba(255, 255, 255, 0.18); min-width: 1px; min-height: 1px; }
.dim-label { color: @titan_dim; }
spinner { color: @titan_text; }

/* named looks the apps use */
.titan-accent { color: @titan_accent; }
.titan-danger, .danger, .error { color: @titan_danger; }
.titan-ok, .ok { color: @titan_ok; }
.titan-dim, .hint, .key { color: @titan_dim; }
.badge {
  padding: 2px 10px; border-radius: 9px; border: 1px solid rgba(255, 255, 255, 0.45);
  color: @titan_dim; background-color: rgba(255, 255, 255, 0.06);
}
.badge.on { color: @titan_ink; text-shadow: none; background-color: rgba(255, 255, 255, 0.86); }
"""

def weston_ini():
    home = os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config"))
    user = os.path.join(home, "weston.ini")
    return user if os.path.exists(user) else "/etc/xdg/weston/weston.ini"


def home_wallpaper():
    """the Home screen's wallpaper ([shell] in weston.ini, set by Display):
    ("wall", name) for one of the board's, ("image", path), ("color", 0xRRGGBB)"""
    keys, section = {}, None
    try:
        for line in open(weston_ini()):
            t = line.strip()
            if t.startswith("["):
                section = t.strip("[]").strip()
            elif section == "shell" and "=" in t and not t.startswith("#"):
                k, v = (x.strip() for x in t.split("=", 1))
                keys[k] = v
    except OSError:
        pass
    home = keys.get("home-wallpaper", "aurora").lower()
    if home in water.WALLPAPERS:
        return ("wall", home)
    if home == "image" and keys.get("background-image") and \
            os.path.exists(keys["background-image"]):
        return ("image", keys["background-image"])
    if home == "image" and keys.get("background-color"):
        try:
            return ("color", int(keys["background-color"], 16) & 0xFFFFFF)
        except ValueError:
            pass
    return ("wall", "aurora")


def wallpaper_texture(w=720, h=720):
    """the Home wallpaper as a texture, w x h (an image cropped to fill)"""
    kind, value = home_wallpaper()
    if kind == "image":
        try:
            gi.require_version("GdkPixbuf", "2.0")
            from gi.repository import GdkPixbuf
            pb = GdkPixbuf.Pixbuf.new_from_file(value)
            k = max(w / pb.get_width(), h / pb.get_height())
            sw, sh = max(w, round(pb.get_width() * k)), max(h, round(pb.get_height() * k))
            pb = pb.scale_simple(sw, sh, GdkPixbuf.InterpType.BILINEAR)
            pb = pb.new_subpixbuf((sw - w) // 2, (sh - h) // 2, w, h)
            return Gdk.Texture.new_for_pixbuf(pb)
        except Exception:               # unreadable: the default
            kind, value = "wall", "aurora"
    if kind == "color":
        data = bytes([value >> 16 & 255, value >> 8 & 255, value & 255]) * (w * h)
        return Gdk.MemoryTexture.new(w, h, Gdk.MemoryFormat.R8G8B8, GLib.Bytes.new(data), w * 3)
    return water.wallpaper_texture(value, w, h)


# the frame's sizes
MARGIN = 14             # the design's: the header 14 px from the edges
GAP = 12


def install_theme(display):
    """the TitanWater look for GTK's widgets, under each app's own CSS"""
    if getattr(install_theme, "done", None) is display:
        return
    install_theme.done = display
    Gtk.Settings.get_for_display(display).set_property("gtk-application-prefer-dark-theme",
                                                       True)
    css = Gtk.CssProvider()
    css.load_from_data(THEME_CSS.encode())
    # under each app's own CSS (APPLICATION, 600), over GTK's theme: that
    # one loads its dark variant at the same priority as SETTINGS (400)
    # when prefer-dark is set, so at 300 Adwaita's lists and entries won
    Gtk.StyleContext.add_provider_for_display(display, css,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION - 1)
    water.install_css(display)


def add_css(display, data):
    """an app's own CSS, above the theme"""
    css = Gtk.CssProvider()
    css.load_from_data(data if isinstance(data, bytes) else data.encode())
    Gtk.StyleContext.add_provider_for_display(display, css,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


def classes(widget, *names):
    for n in names:
        widget.add_css_class(n)
    return widget


def label(text="", *css, xalign=0.0, wrap=False, ellipsize=False):
    """a label with the given CSS classes"""
    lab = Gtk.Label(label=text, xalign=xalign)
    classes(lab, *css)
    if wrap:
        lab.set_wrap(True)
        lab.set_wrap_mode(2)            # Pango.WrapMode.WORD_CHAR
    if ellipsize:
        lab.set_ellipsize(3)            # Pango.EllipsizeMode.END
    return lab


def RoundButton(icon, size=46, tooltip=None):
    """a small droplet button with an icon (ICONS name or a path): the
    header's back button and its actions"""
    d = water.Droplet(ICONS.get(icon, icon), size=size, radius=size / 2 - 2,
                      icon_size=int(size * 0.46), stroke=2.4)
    if tooltip:
        d.set_tooltip_text(tooltip)
    d.set_valign(Gtk.Align.CENTER)
    return d


class Choice(Gtk.Box):
    """one of a few options as glass buttons, the chosen one lit (bright
    water, dark text): a range, a limit. expand: the buttons share the
    width. Emits "changed" (the index) when the user picks one."""

    __gsignals__ = {"changed": (GObject.SignalFlags.RUN_FIRST, None, (int,))}

    def __init__(self, labels, selected=0, expand=False, height=46, size=17):
        super().__init__(spacing=6)
        self.selected = selected
        self.buttons = []
        for i, text in enumerate(labels):
            b = GlassButton(text, height=height, size=size,
                            kind="lit" if i == selected else "")
            b.set_hexpand(expand)
            b.connect("clicked", lambda b, i=i: self.pick(i))
            self.append(b)
            self.buttons.append(b)

    def set_selected(self, i):
        """set it from the app (no "changed" signal)"""
        self.selected = i
        for k, b in enumerate(self.buttons):
            b.set_kind("lit" if k == i else "")

    def pick(self, i):
        changed = i != self.selected
        self.set_selected(i)
        if changed:
            self.emit("changed", i)


class Header(Gtk.Box):
    """the design's app frame: a glass pill, 60 px: a droplet back button
    (on_back, or the window closes), the title and a subtitle, and the
    actions on the right (GlassButtons, RoundButtons)"""

    def __init__(self, title, subtitle=None, on_back=None, actions=()):
        super().__init__(spacing=6)
        self.add_css_class("titan-header")
        self.on_back = on_back
        self.back = RoundButton("back", tooltip="Back (Esc)")
        self.back.connect("clicked", lambda b: self.go_back())
        self.append(self.back)
        texts = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER)
        texts.set_margin_start(6)
        texts.set_hexpand(True)
        self.title = label(title, "titan-title", ellipsize=True)
        self.subtitle = label(subtitle or "", "titan-subtitle", ellipsize=True)
        self.subtitle.set_visible(bool(subtitle))
        texts.append(self.title)
        texts.append(self.subtitle)
        self.append(texts)
        self.actions = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        self.actions.set_margin_end(1)
        self.append(self.actions)
        for w in actions:
            self.add_action(w)

    def add_action(self, widget):
        widget.set_valign(Gtk.Align.CENTER)
        self.actions.append(widget)
        return widget

    def set_title(self, text):
        self.title.set_text(text)

    def set_subtitle(self, text):
        self.subtitle.set_text(text or "")
        self.subtitle.set_visible(bool(text))

    def go_back(self):
        if self.on_back:
            self.on_back()
        else:
            self.get_root().close()


class AppWindow(Gtk.ApplicationWindow):
    """an app's window: the wallpaper, the header, then body (a vertical box
    for the content; put a long page in scroll()). Esc and Back do what the
    header's back button does."""

    def __init__(self, app, title, subtitle=None, on_back=None, actions=(),
                 wallpaper=None, tilt=True):
        super().__init__(application=app, title=title)
        self.set_decorated(False)
        self.add_css_class("titan-app")
        install_theme(self.get_display())
        # Home's wallpaper (or the one named), as a texture the water refracts
        self.wall = (water.wallpaper_texture(wallpaper, 720, 720) if wallpaper
                     else wallpaper_texture())
        self.slosh = water.Slosh()
        root = Gtk.Overlay()
        root.set_child(water.Wallpaper(self.wall))
        self.set_child(root)
        self.root_overlay = root
        col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=GAP)
        col.set_margin_top(MARGIN - 2)
        col.set_margin_start(MARGIN)
        col.set_margin_end(MARGIN)
        root.add_overlay(col)
        self.header = Header(title, subtitle, on_back, actions)
        col.append(self.header)
        self.body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=GAP)
        self.body.set_vexpand(True)
        col.append(self.body)
        self.column = col

        keys = Gtk.EventControllerKey()
        keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        # the water's tilt (tilt False: an app that reads the accelerometer
        # itself, the compass, the maze: one reader at a time)
        if tilt:
            self.connect("notify::is-active",
                         lambda w, p: self.slosh.start() if w.is_active() else self.slosh.stop())
            self.connect("close-request", lambda w: self.slosh.stop() or False)
        self.set_default_size(720, 688)
        # maximize once mapped: before, Weston's 0 x 0 maximized configure
        # kills a GTK 4 window (docs/desktop.md)
        self.connect("map", lambda w: GLib.idle_add(lambda: w.maximize() or False))

    def on_key(self, ctl, keyval, code, state):
        if (Gdk.keyval_name(keyval) or "") in ("Escape", "XF86Back"):
            if getattr(self, "_dialog", None):
                self._dialog.close(False)       # Esc cancels a question first
            else:
                self.header.go_back()
            return True
        return False

    def float_over(self, content):
        """content fills the window under the header, which floats over it
        (the design's photo frame: the grid edge to edge); show_header()
        hides and shows the header (a full screen viewer)"""
        self.body.set_visible(False)
        self.column.set_valign(Gtk.Align.START)     # covers only the header
        self.root_overlay.remove_overlay(self.column)
        self.root_overlay.add_overlay(content)
        self.root_overlay.add_overlay(self.column)

    def show_header(self, on):
        self.column.set_visible(on)

    def confirm(self, title, detail="", ok="Delete", on_ok=None, destructive=True):
        """ask before doing something: a glass card over the dimmed window
        with Cancel and ok; on_ok() if it's chosen (Esc or Cancel: nothing)"""
        scrim = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        scrim.add_css_class("titan-scrim")
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        card.add_css_class("titan-dialog")
        card.set_valign(Gtk.Align.CENTER)
        card.set_vexpand(True)
        card.set_margin_start(36)
        card.set_margin_end(36)
        inner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        for m in ("top", "bottom", "start", "end"):
            getattr(inner, "set_margin_" + m)(20)
        inner.append(label(title, "titan-title", wrap=True))
        if detail:
            inner.append(label(detail, "titan-subtitle", wrap=True))
        row = Gtk.Box(spacing=10, halign=Gtk.Align.END)
        row.set_margin_top(8)
        cancel = GlassButton("Cancel", height=50)
        go = GlassButton(ok, height=50, kind="destructive" if destructive else "suggested")
        row.append(cancel)
        row.append(go)
        inner.append(row)
        card.append(inner)
        scrim.append(card)
        self.root_overlay.add_overlay(scrim)
        self._dialog = scrim

        def close(chosen):
            if self._dialog is scrim:
                self._dialog = None
            self.root_overlay.remove_overlay(scrim)
            if chosen and on_ok:
                on_ok()
        cancel.connect("clicked", lambda b: close(False))
        go.connect("clicked", lambda b: close(True))
        scrim.close = close
        cancel.grab_focus()

    # the water widgets announce themselves when realized (titan_water4.adopt_on_realize)
    def adopt_one(self, widget):
        if hasattr(widget, "set_backdrop") and getattr(widget, "backdrop", None) is None:
            widget.set_backdrop(self.wall, self)
        if hasattr(widget, "set_tilt"):
            self.slosh.attach(widget)

    def drop_one(self, widget):
        self.slosh.detach(widget)


def scroll(child, accent=water.ACCENT):
    """a page that scrolls: kinetic, with the water scroll bar; it takes the
    height it's given"""
    sc = water.water_scroll(child, accent)
    sc.set_vexpand(True)
    return sc


def row(text, widget=None, hint=None):
    """a line in a panel: a label (and a hint under it), a control at the end"""
    box = Gtk.Box(spacing=12)
    texts = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER)
    texts.set_hexpand(True)
    texts.append(label(text, "titan-row"))
    if hint:
        texts.append(label(hint, "hint", wrap=True))
    box.append(texts)
    if widget is not None:
        widget.set_valign(Gtk.Align.CENTER)
        box.append(widget)
    return box


def section(text):
    """a panel's heading: small, bold, dim, upper case (the board's)"""
    lab = label(text.upper(), "titan-section")
    return lab


def single_instance(name):
    """True if no other copy of the app runs (the lock is held while it does)"""
    path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), name + ".lock")
    lock = open(path, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock.close()
        return False
    single_instance.locks = getattr(single_instance, "locks", []) + [lock]
    return True


def run(app_id, build, lock=None):
    """run an app: build(app) returns its AppWindow. One at a time (lock: the
    lock file's name; a second launch quits); GTK on the big cores (on the
    A53s GTK 4 renders at 30 fps)"""
    if lock and not single_instance(lock):
        return 0
    water.use_big_cores()
    app = Gtk.Application(application_id=app_id, flags=Gio.ApplicationFlags.NON_UNIQUE)

    def activate(app):
        win = build(app)
        water.debug(win, win.root_overlay, getattr(win, "script", ()))
        win.present()
    app.connect("activate", activate)
    return app.run([])


__all__ = ["run", "AppWindow", "Header", "GlassButton", "RoundButton", "Choice", "ICONS", "row",
           "section", "label", "classes", "scroll", "add_css", "install_theme", "GObject"]
