"""titan_ui: the Titan Pocket's app style for our GTK 3 apps.

The TitanWater theme (/usr/share/themes/TitanWater) draws every GTK 3 app in
the Home screen's design: the Aurora wallpaper and controls made of water.
This module adds what CSS can't:

    install(window)     every button in the window (and those added later)
                        gets the water's membrane: a finger near its edge
                        dents it there, in the middle pushes it down (it
                        spreads, a dimple forms); the dent follows the finger
                        and on release the skin springs back and ripples. It
                        never breaks. The same physics as the shell's droplets
                        (clients/desktop-shell.c, struct water).
    header(...)         the design's app frame: a glass pill with a round back
                        button, the title and a subtitle, and actions.

At rest the theme's CSS draws a button; while its membrane moves, this module
draws the water itself (the same recipe) and then the button's content.
"""

import math

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, GObject, Gtk  # noqa: E402

# the shell's droplet constants (desktop-shell.c)
DROP_K = 250.0
DROP_T = 200.0
DROP_D = 9.0
DROP_MAX_IN = 0.45
DROP_MAX_OUT = 0.35
STEP_PX = 6.0           # membrane points about this far apart
MAX_POINTS = 96
TICK_MS = 16


class Water:
    """A rounded box's membrane: points along the outline that move along
    its normal (o, px), on a spring to rest, coupled to their neighbours."""

    def __init__(self, w, h, r):
        self.w, self.h = float(w), float(h)
        self.r = max(0.0, min(float(r), self.w / 2, self.h / 2))
        self.R = min(self.w, self.h) / 2
        r = self.r
        sw, sh, arc = self.w - 2 * r, self.h - 2 * r, math.pi / 2 * r
        self.L = 2 * sw + 2 * sh + 4 * arc
        self.n = max(16, min(MAX_POINTS, int(round(self.L / STEP_PX))))
        self.step = self.L / self.n
        self.pts = [self._outline(k * self.step) for k in range(self.n)]
        self.o = [0.0] * self.n
        self.v = [0.0] * self.n
        self.poking = False
        self.moving = False
        self.at = 0
        self.depth = self.sigma = self.spread = 0.0

    def _outline(self, s):
        r, w, h = self.r, self.w, self.h
        sw, sh, arc = w - 2 * r, h - 2 * r, math.pi / 2 * r
        segs = [sw, arc, sh, arc, sw, arc, sh, arc]
        centres = [(w - r, r), (w - r, h - r), (r, h - r), (r, r)]
        i = 0
        while i < 7 and s > segs[i]:
            s -= segs[i]
            i += 1
        if i % 2:
            a = -math.pi / 2 + (i // 2) * math.pi / 2 + (s / r if r > 0 else 0)
            cx, cy = centres[i // 2]
            return (cx + r * math.cos(a), cy + r * math.sin(a), math.cos(a), math.sin(a))
        if i == 0:
            return (r + s, 0.0, 0.0, -1.0)
        if i == 2:
            return (w, r + s, 1.0, 0.0)
        if i == 4:
            return (w - r - s, h, 0.0, 1.0)
        return (0.0, h - r - s, -1.0, 0.0)

    def still(self):
        return not self.poking and not self.moving

    def poke(self, fx, fy):
        best, at = 1e9, 0
        for k, (px, py, _, _) in enumerate(self.pts):
            d = math.hypot(fx - px, fy - py)
            if d < best:
                best, at = d, k
        inside = 0 <= fx <= self.w and 0 <= fy <= self.h
        dist = 1 - min(1.0, best / self.R) if inside else 1.0
        w = min(1.0, max(0.0, (dist - 0.12) / 0.63))
        w = w * w * (3 - 2 * w)
        self.at = at
        self.poking = self.moving = True
        self.depth = (0.12 + 0.26 * dist) * w * self.R
        self.sigma = (0.55 - 0.18 * dist) * self.R
        self.spread = 0.14 * (1 - w) * self.R

    def splash(self, amount=1.2):
        self.v = [v + amount * self.R for v in self.v]
        self.moving = True

    def advance(self, dt):
        n, o, v = self.n, self.o, self.v
        tension = DROP_T * (11.8 / self.step) ** 2
        tgt = [0.0] * n
        if self.poking:
            two_s2 = 2 * self.sigma * self.sigma
            for k in range(n):
                ds = k - self.at
                ds = (ds - n * round(ds / n)) * self.step
                tgt[k] = -self.depth * math.exp(-ds * ds / two_s2)
            mean = sum(tgt) / n
            tgt = [t + self.spread - mean for t in tgt]
        lo, hi = -DROP_MAX_IN * self.R, DROP_MAX_OUT * self.R
        for k in range(n):
            lap = o[k - 1] + o[(k + 1) % n] - 2 * o[k]
            v[k] += (DROP_K * (tgt[k] - o[k]) - DROP_D * v[k] + tension * lap) * dt
        active = self.poking
        eps_o, eps_v = 0.0008 * self.R, 0.002 * self.R
        for k in range(n):
            o[k] = min(hi, max(lo, o[k] + v[k] * dt))
            if abs(o[k]) > eps_o or abs(v[k]) > eps_v:
                active = True
        if not active:
            self.o = [0.0] * n
            self.v = [0.0] * n
        self.moving = active
        return active

    def down(self):
        z = sum(self.o) / self.n / (0.14 * self.R)
        return min(1.0, max(0.0, z))

    def drift(self):
        sx = sum(o * p[2] for o, p in zip(self.o, self.pts))
        sy = sum(o * p[3] for o, p in zip(self.o, self.pts))
        m = sum(abs(o) for o in self.o) / self.n / self.R
        return (sx / self.n * 2, sy / self.n * 2,
                (1 - m * 0.6) * (1 - 0.12 * self.down()))

    def path(self, cr, x, y):
        n = self.n
        pts = [(x + p[0] + o * p[2], y + p[1] + o * p[3]) for p, o in zip(self.pts, self.o)]
        cr.new_sub_path()
        cr.move_to(*pts[0])
        for k in range(n):
            p0, p1, p2, p3 = pts[k - 1], pts[k], pts[(k + 1) % n], pts[(k + 2) % n]
            cr.curve_to(p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6,
                        p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6,
                        p2[0], p2[1])
        cr.close_path()

    def rim_point(self, x, y, ang, f):
        """The skin at angle `ang` of the box as a stretched circle, pulled
        toward the middle by f; and how far out the skin is there."""
        cx, cy = self.w / 2, self.h / 2
        ex, ey = cx + cx * math.cos(ang), cy + cy * math.sin(ang)
        at = min(range(self.n), key=lambda k: math.hypot(self.pts[k][0] - ex,
                                                         self.pts[k][1] - ey))
        p, o = self.pts[at], self.o[at]
        X, Y = p[0] + o * p[2] - cx, p[1] + o * p[3] - cy
        D = math.hypot(X, Y)
        if D > 75:
            f = 1 - (1 - f) * 75 / D
        return x + cx + X * f, y + cy + Y * f, 1 + o / self.R


def _spot(cr, water, x, y, rad, alpha):
    """A soft white spot, stretched along a long box like its highlights."""
    import cairo
    aspect = min(4.0, max(water.w, water.h) / max(1.0, min(water.w, water.h)))
    sx, sy = (math.sqrt(aspect), 1.0) if water.w >= water.h else (1.0, math.sqrt(aspect))
    cr.save()
    cr.translate(x, y)
    cr.scale(sx, sy)
    g = cairo.RadialGradient(0, 0, 0, 0, 0, rad)
    g.add_color_stop_rgba(0, 1, 1, 1, alpha)
    g.add_color_stop_rgba(1, 1, 1, 1, 0)
    cr.set_source(g)
    cr.arc(0, 0, rad, 0, 2 * math.pi)
    cr.fill()
    cr.restore()


def draw_water(cr, water, x, y, body=(1, 1, 1, 0.05)):
    """The theme's water (body, edge glow, rim, shine, caustic, dimple and
    shadow) on the membrane's current shape; `body` is the water's colour
    (the button's CSS background colour: clear, lit or tinted)."""
    lit = body[3] > 0.5
    import cairo
    z = water.down()
    cap = min(water.R, 75)
    # the water's shadow: a few soft layers below it
    for i, a in enumerate((0.07, 0.05, 0.035)):
        cr.save()
        cr.translate(0, 5 + 2 * i)
        water.path(cr, x - i, y - i * 0.5)
        cr.set_source_rgba(0, 0, 0, a)
        cr.fill()
        cr.restore()
    # body; lit (a toggle that is on) is bright water
    cr.save()
    water.path(cr, x, y)
    cr.clip()
    cr.set_source_rgba(*body)
    cr.paint()
    # brighter toward the edge: wide soft strokes of the outline, clipped
    for width, a in ((32, 0.03), (26, 0.03), (20, 0.04), (15, 0.04), (11, 0.05),
                     (7, 0.06), (4, 0.07), (2, 0.08)):
        water.path(cr, x, y)
        cr.set_line_width(width)
        cr.set_source_rgba(1, 1, 1, a)
        cr.stroke()
    if z > 0.01:
        g = cairo.RadialGradient(0, 0, 0, 0, 0, 1)
        g.add_color_stop_rgba(0.0, 0, 0, 0, 0.26 * z)
        g.add_color_stop_rgba(0.5, 0, 0, 0, 0.07 * z)
        g.add_color_stop_rgba(0.72, 1, 1, 1, 0.16 * z)
        g.add_color_stop_rgba(1.0, 1, 1, 1, 0)
        m = cairo.Matrix()
        m.translate(x + water.w / 2, y + water.h / 2)
        m.scale(water.w / 2 * 0.85 * (1 + 0.14 * z), water.h / 2 * 0.85 * (1 + 0.14 * z))
        m.invert()
        g.set_matrix(m)
        cr.set_source(g)
        cr.paint()
    cr.restore()
    # the rim
    water.path(cr, x, y)
    cr.set_line_width(1.3)
    cr.set_source_rgba(1, 1, 1, 0.62 if not lit else 0.95)
    cr.stroke()
    # shine and caustic ride on the skin
    hx, hy, sw = water.rim_point(x, y, -2.3, 0.55 + 0.15 * z)
    _spot(cr, water, hx, hy, 0.30 * cap * sw * (1 - 0.35 * z), 0.95)
    hx, hy, sw = water.rim_point(x, y, 0.95, 0.62)
    _spot(cr, water, hx, hy, 0.34 * cap * sw, 0.45)


# ---- buttons with a membrane ----

class _Membrane:
    def __init__(self, button):
        self.button = button
        self.water = None
        self.timer = 0
        self.last = 0
        button.add_events(Gdk.EventMask.BUTTON_PRESS_MASK |
                          Gdk.EventMask.BUTTON_RELEASE_MASK |
                          Gdk.EventMask.BUTTON_MOTION_MASK)
        button.connect("draw", self.on_draw)
        button.connect("button-press-event", self.on_press)
        button.connect("motion-notify-event", self.on_motion)
        button.connect("button-release-event", self.on_release)
        button.connect("leave-notify-event", self.on_leave)
        button.connect("unmap", self.on_unmap)

    def ensure(self):
        a = self.button.get_allocation()
        style = self.button.get_style_context()
        r = style.get_property("border-radius", style.get_state())
        try:
            r = float(r)
        except (TypeError, ValueError):
            r = 22.0
        r = min(r, a.width / 2, a.height / 2)
        w = self.water
        if not w or w.w != a.width or w.h != a.height or w.r != r:
            self.water = Water(a.width, a.height, r)
        return self.water

    def kick(self):
        if not self.timer:
            self.last = GLib.get_monotonic_time()
            self.timer = GLib.timeout_add(TICK_MS, self.tick)
        self.button.queue_draw()

    def tick(self):
        now = GLib.get_monotonic_time()
        dt = min(0.033, (now - self.last) / 1e6)
        self.last = now
        busy = False
        t = 0.0
        while t < dt:
            h = min(0.004, dt - t)
            busy = self.water.advance(h)
            t += h
        self.button.queue_draw()
        if not busy:
            self.timer = 0
            return False
        return True

    def on_press(self, button, event):
        if event.button == 1 and button.get_sensitive():
            self.ensure().poke(event.x, event.y)
            self.kick()
        return False

    def on_motion(self, button, event):
        if self.water and self.water.poking:
            self.water.poke(event.x, event.y)
            self.kick()
        return False

    def on_release(self, button, event):
        if self.water and self.water.poking:
            self.water.poking = False
            self.kick()
        return False

    def on_leave(self, button, event):
        return self.on_release(button, event)

    def on_unmap(self, button):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = 0
        self.water = None

    def on_draw(self, button, cr):
        w = self.water
        if not w or w.still():
            return False            # the theme draws it at rest
        style = button.get_style_context()
        # the water's colour at rest (not the pressed look: we draw that)
        rest = Gtk.StateFlags(int(style.get_state()) &
                              ~int(Gtk.StateFlags.ACTIVE | Gtk.StateFlags.PRELIGHT))
        c = style.get_property("background-color", rest)
        draw_water(cr, w, 0, 0, (c.red, c.green, c.blue, max(0.05, c.alpha)))
        child = button.get_child()
        if child:
            dx, dy, sc = w.drift()
            a = child.get_allocation()
            ba = button.get_allocation()
            cx, cy = a.x - ba.x + a.width / 2, a.y - ba.y + a.height / 2
            cr.save()
            cr.translate(cx + dx, cy + dy)
            cr.scale(sc, sc)
            cr.translate(-cx, -cy)
            button.propagate_draw(child, cr)
            cr.restore()
        return True


def _wants_membrane(widget):
    if not isinstance(widget, Gtk.Button) or isinstance(widget, Gtk.ModelButton):
        return False
    # checks and radios draw an indicator; as plain buttons (set_mode(False))
    # they are water like any button
    if isinstance(widget, Gtk.CheckButton) and widget.get_mode():
        return False
    return not getattr(widget, "_titan_membrane", None)


def membrane(button):
    """Give one button the water's membrane."""
    if _wants_membrane(button):
        button._titan_membrane = _Membrane(button)
    return button


def _walk(widget):
    membrane(widget)
    if isinstance(widget, Gtk.Container):
        widget.forall(_walk)


# the header's sizes, above the apps' own CSS (theirs is at APPLICATION
# priority and sets every button's size and every label's font)
HEADER_CSS = b"""
.titan-header button { min-height: 46px; min-width: 46px; padding: 0 16px; font-size: 18px; }
.titan-header button.titan-back { min-width: 46px; padding: 0; }
.titan-header .titan-title { font-size: 21px; font-weight: bold; }
.titan-header .titan-subtitle { font-size: 15px; }
.titan-segmented button { min-height: 46px; min-width: 0; padding: 0 20px; font-size: 18px; }
window .titlebar.titan-no-titlebar { min-height: 0; padding: 0; margin: 0; border: none; }
"""


def _header_css():
    if getattr(_header_css, "done", False):
        return
    provider = Gtk.CssProvider()
    provider.load_from_data(HEADER_CSS)
    Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider,
                                             Gtk.STYLE_PROVIDER_PRIORITY_USER + 1)
    _header_css.done = True


def install(window):
    """Call before the window is shown. The design's header replaces GTK's
    title bar; and every button in the window (and those added later: lists
    fill in) gets the membrane."""
    _header_css()
    if not window.get_realized() and window.get_titlebar() is None:
        nothing = Gtk.Box()                 # no title bar
        nothing.get_style_context().add_class("titan-no-titlebar")
        window.set_titlebar(nothing)
    _walk(window)
    # new buttons: catch them as they are realized anywhere in the app
    if not getattr(install, "_hooked", False):
        GObject.add_emission_hook(Gtk.Widget, "realize", _realize_hook)
        install._hooked = True
    return window


def _realize_hook(widget, *args):
    membrane(widget)
    return True


def header(title, subtitle=None, on_back=None, actions=()):
    """The design's app frame: a glass pill, 60 px, with a round back button
    (on_back, or the window closes), the title and a subtitle, and the action
    widgets on the right. Returns (bar, title_label, subtitle_label)."""
    _header_css()
    bar = Gtk.Box(spacing=6)
    bar.get_style_context().add_class("titan-header")
    back = Gtk.Button.new_from_icon_name("go-previous-symbolic", Gtk.IconSize.BUTTON)
    back.get_style_context().add_class("titan-back")
    back.get_style_context().add_class("circular")
    back.set_valign(Gtk.Align.CENTER)
    back.set_tooltip_text("Back (Esc)")

    def go_back(b):
        if on_back:
            on_back()
        else:
            b.get_toplevel().destroy()
    back.connect("clicked", go_back)
    bar.pack_start(back, False, False, 0)

    texts = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER)
    texts.set_margin_start(6)
    title_label = Gtk.Label(label=title, xalign=0)
    title_label.get_style_context().add_class("titan-title")
    texts.pack_start(title_label, False, False, 0)
    sub = Gtk.Label(label=subtitle or "", xalign=0)
    sub.get_style_context().add_class("titan-subtitle")
    sub.set_ellipsize(3)        # Pango.EllipsizeMode.END
    sub.set_no_show_all(not subtitle)
    texts.pack_start(sub, False, False, 0)
    bar.pack_start(texts, True, True, 0)

    for w in reversed(list(actions)):
        w.set_valign(Gtk.Align.CENTER)
        bar.pack_end(w, False, False, 0)
    return bar, title_label, sub


def set_subtitle(label, text):
    """Show or hide a header's subtitle."""
    label.set_text(text or "")
    label.set_no_show_all(False)
    label.set_visible(bool(text))


def escape_closes(window, on_back=None):
    """Esc (and the Back key) does what the header's back button does."""
    def on_key(w, event):
        if event.keyval in (Gdk.KEY_Escape, Gdk.KEY_Back):
            if on_back:
                on_back()
            else:
                w.destroy()
            return True
        return False
    window.connect("key-press-event", on_key)

