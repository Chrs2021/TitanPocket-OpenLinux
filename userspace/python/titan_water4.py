"""
titan_water4: the TitanWater droplet for GTK 4, drawn on the GPU.

The droplet of the "Droplet buttons" design board (Titan Pocket UI canvas):
a clear lens of water with a bright rim, a shine up left, a caustic down
right and a soft shadow. Its skin is a membrane of N points on springs; a
press near the edge dents it there, a press in the middle pushes the drop
down (it spreads and a dimple forms), and on release it ripples back.

Split between the CPU and the GPU:
- the physics (N floats a frame) runs here, in Python, while the skin moves;
- a GskGLShader draws everything else per pixel: the outline from the N
  offsets, the refracted backdrop, the body, the rim, the dimple, the lights
  and the shadow. GTK allows a custom shader 8 uniforms, so the offsets go
  in as their Fourier series (the mean and HARMONICS cosine/sine pairs, 5
  vec4s): surface tension keeps the skin smooth, so a dent's harmonics above
  the 9th are below 0.1 % of it;
- the icon is a texture, floated and squeezed by a transform.

The water feels the phone move (Slosh): what the accelerometer reads beyond
gravity pushes on the membranes, so the drops slosh and jiggle when the phone
moves, and a still or steadily tilted phone leaves them be.

The refraction needs what's behind the droplet. GTK 4 can't read back the
window, so the window gives the droplet its backdrop: a Gdk.Texture of the
wallpaper (set_backdrop), which the droplet draws as the shader's child
texture, offset to where the droplet sits in the window.
"""
import math
import os
import random

import gi
import numpy as np

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gsk", "4.0")
gi.require_version("Graphene", "1.0")
from gi.repository import Gdk, GLib, GObject, Graphene, Gsk, Gtk  # noqa: E402

N = 40                  # membrane points
HARMONICS = 9           # Fourier terms the shader gets: 1 + 2 * 9 = 19 floats
SIZE = 150              # the widget, as on the board
R = 54.0                # the droplet's radius at rest
MARGIN = 32             # drawn outside the widget: bulges and the shadow
CLAMP_IN, CLAMP_OUT = -0.45, 0.35       # stressed, never torn
# the phone's motion on the skin, per m/s^2 beyond the dead zone (offsets are
# fractions of R, K 250: 3 m/s^2, a brisk move, bulges a side by about 0.08)
SLOSH_SHIFT = 7.0
SLOSH_STRETCH = 4.0
SLOSH_SQUASH = 2.0
# tilt (the in-plane part of gravity, 0 flat .. 1 on edge): how far the light
# swings toward the edge that's up, and the view through the lens shifts (px)
TILT_LIGHT = 0.7
TILT_SHIFT = 6.0
# tilted, a droplet leans downhill as if about to roll off: its skin bulges
# toward the slope by LEAN (of its radius, on edge) and tapers away from it
# (LEAN_TAPER of that); the drop shifts downhill by LEAN_SHIFT of its radius
# (at least of LEAN_MIN_R px: a small bead still shows it)
LEAN = 0.132
LEAN_TAPER = 0.35
LEAN_SHIFT = 0.072
LEAN_MIN_R = 24.0


def lean_shift(up, r):
    """how far a droplet of radius r sits downhill of its place (px, x y)"""
    k = LEAN_SHIFT * max(r, LEAN_MIN_R)
    return -up[0] * k, -up[1] * k

SHADER = """
/* (no comments on the uniform lines: GTK's parser then skips them)
   u_f0..u_f4: the skin: mean, a1, b1, a2, b2, ... a9, b9; u_f4.w: the
     tilt's shift of the view through the lens, x (px)
   u_geo: centre x, y (in the node, px), radius, pushed down 0..1
   u_shine: x, y, radius; w: the tint's opacity over the view (0.35);
     negative: dyed, a droplet of the accent itself (lighter at the top)
   u_caustic: the tint's accent (r, g, b); w: the tilt's shift, y (px) */
uniform vec4 u_f0;
uniform vec4 u_f1;
uniform vec4 u_f2;
uniform vec4 u_f3;
uniform vec4 u_f4;
uniform vec4 u_geo;
uniform vec4 u_shine;
uniform vec4 u_caustic;

/* the skin's offset in direction (c, s) = (cos a, sin a): its Fourier series,
   the multiples of a by rotation. Written out term by term: with the terms in
   a local array indexed in a loop, the GPU's compiler kept the array in
   scratch memory, and the per-pixel cost made a slider's 89 px thumb drop the
   frame rate to ~30 (f = [mean, a1, b1, ... a9, b9] in u_f0..u_f4) */
float skin(float c1, float s1)
{
  float c2 = c1 * c1 - s1 * s1, s2 = 2.0 * s1 * c1;
  float c3 = c2 * c1 - s2 * s1, s3 = s2 * c1 + c2 * s1;
  float c4 = c3 * c1 - s3 * s1, s4 = s3 * c1 + c3 * s1;
  float c5 = c4 * c1 - s4 * s1, s5 = s4 * c1 + c4 * s1;
  float c6 = c5 * c1 - s5 * s1, s6 = s5 * c1 + c5 * s1;
  float c7 = c6 * c1 - s6 * s1, s7 = s6 * c1 + c6 * s1;
  float c8 = c7 * c1 - s7 * s1, s8 = s7 * c1 + c7 * s1;
  float c9 = c8 * c1 - s8 * s1, s9 = s8 * c1 + c8 * s1;
  return u_f0.x
       + dot(u_f0.yzw, vec3(c1, s1, c2))
       + dot(u_f1, vec4(s2, c3, s3, c4))
       + dot(u_f2, vec4(s4, c5, s5, c6))
       + dot(u_f3, vec4(s6, c7, s7, c8))
       + dot(u_f4.xyz, vec3(s8, c9, s9));
}

/* signed distance to the skin (negative inside), px */
float drop(vec2 p, out float edge_r)
{
  vec2 d = p - u_geo.xy;
  float l = length(d);
  vec2 u = l > 0.001 ? d / l : vec2(1.0, 0.0);
  edge_r = u_geo.z * (1.0 + skin(u.x, u.y));
  return l - edge_r;
}

vec4 over(vec4 top, vec4 under)     /* premultiplied */
{
  return top + under * (1.0 - top.a);
}

/* the shader's first child texture. GTK 4.8 counts a shader's textures from
   its "uniform sampler2D u_textureN" declarations, but its GL renderer binds
   them as u_source, u_source2..4 (in GTK's preamble): u_texture1 is declared
   for the count, unused, and the code reads u_source */
uniform sampler2D u_texture1;

vec4 backdrop(vec2 p, vec2 resolution)
{
  /* p is top-down px (fragCoord's space); texture coordinates count up from
     the bottom, as uv does */
  vec2 t = clamp(p / resolution, vec2(0.0), vec2(1.0));
  return GskTexture(u_source, vec2(t.x, 1.0 - t.y));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord, in vec2 resolution, in vec2 uv)
{
  vec2 p = fragCoord;         /* px, top-down, like the uniforms (uv counts up) */
  float er;
  float d = drop(p, er);
  float inside = clamp(0.5 - d, 0.0, 1.0);          /* 1 px antialiasing */
  float ta = abs(u_shine.w);
  bool dyed = u_shine.w < 0.0;
  /* the board's droplet is R 54: smaller drops get a smaller shadow and
     refracting outline (a slider's 17 px bead), down to half */
  float k = max(u_geo.z / 54.0, 0.5);

  /* the shadow: 0 12px 14px of the water, see-through in the middle, darker
     under the rim, lighter the clearer the drop */
  float ser;
  vec2 sp = p - vec2(0.0, 12.0 * k);
  float sd = drop(sp, ser);
  float st = clamp(length(sp - u_geo.xy) / max(ser, 1.0), 0.0, 1.0);
  float shadow = mix(0.14, 0.30, ta) * (1.0 - smoothstep(-10.0 * k, 14.0 * k, sd))
               * mix(0.35, 1.0, st * st);
  vec4 col = vec4(0.0, 0.0, 0.0, shadow * (1.0 - inside));

  if (inside > 0.0) {
    /* the TitanWater glass (as the nav slider's vial): the view through it
       as it is, under a light tint; within the outline (9 px at R 54) it
       bends outward as through a lens's edge, the channels a little apart */
    vec2 rel = p - u_geo.xy;
    float len = length(rel);
    float t = clamp(len / er, 0.0, 1.0);
    vec2 nrm = len > 0.001 ? rel / len : vec2(0.0, 1.0);
    float rimw = 9.0 * k;
    float band = clamp((d + rimw) / rimw, 0.0, 1.0);
    float bend = band * band * 7.0 * k;
    /* tilted, gravity pulls the water to the low side and the view through
       it shifts: most in the thick middle, none at the rim */
    vec2 q = p + vec2(u_f4.w, u_caustic.w) * (1.0 - t * t) + nrm * bend;
    vec4 bg = backdrop(q, resolution);
    vec3 view = vec3(backdrop(q + nrm * bend * 0.18, resolution).r, bg.g,
                     backdrop(q - nrm * bend * 0.18, resolution).b);
    view = min(view * (1.0 + 0.18 * band), vec3(1.0));
    /* the tint: lit white, the accent coming in toward the bottom */
    float ty = clamp((p.y - (u_geo.y - er)) / max(2.0 * er, 1.0), 0.0, 1.0);
    vec3 tint = dyed ? mix(min(u_caustic.xyz * 1.2 + 0.18, vec3(1.0)), u_caustic.xyz, ty)
                     : mix(vec3(1.0), mix(vec3(1.0), u_caustic.xyz, 0.55), ty * ty);
    /* the view is drawn here (the drop is opaque, its clarity in how much of
       the view comes through); with no backdrop, see-through by alpha */
    vec4 water = bg.a > 0.5 ? vec4(mix(view, tint, ta), 1.0) : vec4(tint, 1.0) * ta;

    /* pushed down: the dimple (dark middle, bright ring), about 0.85 of the
       drop's mean radius */
    float dt = len / max(u_geo.z * (1.0 + u_f0.x) * 0.85, 1.0);
    if (dt < 1.0) {
      float a;
      vec3 dc;
      if (dt < 0.5) { a = mix(0.26, 0.07, dt / 0.5); dc = vec3(0.0); }
      else if (dt < 0.72) { float m = (dt - 0.5) / 0.22; a = mix(0.07, 0.16, m); dc = vec3(m); }
      else { a = mix(0.16, 0.0, (dt - 0.72) / 0.28); dc = vec3(1.0); }
      water = over(vec4(dc * a, a) * u_geo.w, water);
    }

    /* a soft shine toward the light (it follows the tilt) */
    float sh = 0.6 * max(0.0, 1.0 - length(p - u_shine.xy) / max(u_shine.z, 1.0));
    water = over(vec4(vec3(sh), sh), water);
    col = over(water * inside, col);
  }

  /* the rim: a 1.3 px bright stroke */
  float rim = 0.62 * clamp(1.0 - abs(d) / 0.65, 0.0, 1.0);
  col = over(vec4(vec3(rim), rim), col);

  fragColor = col;
}
"""

_shader = None
_uniforms = None


def shader():
    global _shader
    if _shader is None:
        _shader = Gsk.GLShader.new_from_bytes(GLib.Bytes.new(SHADER.encode()))
    return _shader


def uniforms():
    """the shader's uniforms: {name: float index into its argument block},
    and the block's size in floats. The arguments are written straight into
    a float32 array at GTK's own offsets and handed over as the GBytes
    push_gl_shader takes, instead of a Graphene vector and a builder call per
    uniform each frame (all of ours are vec4s)"""
    global _uniforms
    if _uniforms is None:
        sh = shader()
        _uniforms = ({sh.get_uniform_name(i): sh.get_uniform_offset(i) // 4
                      for i in range(sh.get_n_uniforms())}, sh.get_args_size() // 4)
    return _uniforms


_compiled = None


def compiled(widget):
    """the shader compiled for the widget's renderer (False: it couldn't be,
    e.g. GTK's Cairo renderer: then only what GTK draws itself shows)"""
    global _compiled
    if _compiled is None:
        try:
            _compiled = shader().compile(widget.get_native().get_renderer())
        except GLib.Error:
            _compiled = False
    return _compiled


_STILL = None


def push_drop(s, widget, bounds, cx, cy, radius, o, up, child, tint=0.35, accent=None,
              dyed=False):
    """draw a water droplet with the shader, in the widget's coordinates:
    centre (cx, cy) and radius in px; o the skin (N offsets, fractions of the
    radius; None for a still circle); up the tilt (Slosh: the screen's up, 0
    flat .. 1 on edge); bounds the node, big enough for bulges and the
    shadow; child(s) draws what's behind it, in the same coordinates (it
    shows through, bent in the outline); tint the opacity of the light tint
    over it, its accent (ACCENT) coming in toward the bottom: the TitanWater
    glass of everything grippable; dyed: a droplet of the accent itself (a
    radio's choice). False when the shader isn't available."""
    global _STILL
    if not compiled(widget):
        return False
    if o is None:
        if _STILL is None:
            _STILL = np.zeros(N)
        o = _STILL
    f = _FOURIER @ o                            # the skin's series; f[0] is its mean
    mean = float(f[0])
    z = max(0.0, min(1.0, mean / 0.14))         # how far it's pushed down
    lcx, lcy = cx - bounds.get_x(), cy - bounds.get_y()     # the node's px

    def on_rim(a, frac):
        k = round((a % (2 * math.pi)) / (2 * math.pi) * N) % N
        r = radius * (1 + o[k]) * frac
        return lcx + r * math.cos(a), lcy + r * math.sin(a), 1 + o[k]
    # the shine up left; tilted, the light moves toward the edge that's up
    ux, uy = up
    w = min(1.0, math.hypot(ux, uy)) * TILT_LIGHT
    lx, ly = math.cos(-2.3) * (1 - w) + ux * TILT_LIGHT, math.sin(-2.3) * (1 - w) + uy * TILT_LIGHT
    la = math.atan2(ly, lx) if (lx or ly) else -2.3
    hx, hy, hf = on_rim(la, 0.55 + 0.15 * z)
    acc = rgba(accent or ACCENT)
    u, size = uniforms()
    a = np.zeros(size, np.float32)
    for i in range(4):                          # u_f0..u_f3: f[0..15]
        a[u["u_f%d" % i]:u["u_f%d" % i] + 4] = f[4 * i:4 * i + 4]
    k = u["u_f4"]
    a[k:k + 3] = f[16:19]
    a[k + 3] = -ux * TILT_SHIFT * radius / R
    k = u["u_geo"]
    a[k:k + 4] = (lcx, lcy, radius, z)
    k = u["u_shine"]
    a[k:k + 4] = (hx, hy, radius * 0.30 * hf * (1 - 0.35 * z), -tint if dyed else tint)
    k = u["u_caustic"]
    a[k:k + 4] = (acc.red, acc.green, acc.blue, -uy * TILT_SHIFT * radius / R)
    s.push_gl_shader(shader(), bounds, GLib.Bytes.new(a.tobytes()))
    child(s)
    s.gl_shader_pop_texture()
    s.pop()
    return True


_ANG = np.arange(N) * (2 * np.pi / N)     # each point's angle
_COS1, _SIN1 = np.cos(_ANG), np.sin(_ANG)  # and its outward normal
# the skin's Fourier series as one matrix: rows mean, a1, b1, ... aH, bH
_FOURIER = np.vstack([np.full(N, 1.0 / N)] + [
    row for n in range(1, HARMONICS + 1)
    for row in (np.cos(n * _ANG) * (2.0 / N), np.sin(n * _ANG) * (2.0 / N))])


def fourier(o):
    """the skin's Fourier series: [mean, a1, b1, ... aH, bH]"""
    return (_FOURIER @ o).tolist()


class Membrane:
    """the droplet's skin: N radial offsets (fractions of R) on springs,
    coupled to their neighbours like surface tension, damped (the board's
    numbers: K 150 + 200 t, T 700 + 2200 t, D 9, tension t = 0.5). The
    springs pull toward a rest shape: round, or leaning downhill when the
    phone is tilted (set_lean)."""

    def __init__(self, tension=0.5, clamp=(CLAMP_IN, CLAMP_OUT)):
        self.o = np.zeros(N)
        self.v = np.zeros(N)
        self.lo, self.hi = clamp         # how far it may dent and bulge
        self.poke = None
        self.K = 150 + 200 * tension
        self.T = 700 + 2200 * tension
        self.D = 9.0
        self.ext = (0.0, 0.0, 0.0)  # the phone's motion felt by the water (Slosh), m/s^2
        # the springs and the surface tension are linear: one matrix (with 40
        # points numpy's per-call cost, not the arithmetic, is what counts on
        # the phone, so fewer, bigger calls)
        eye = np.eye(N)
        self.L = -self.K * eye + (self.T * 0.11) * (np.roll(eye, 1, 0) + np.roll(eye, -1, 0)
                                                     - 2 * eye)
        self._poke = self._poke_f = None    # the press's force, for the press it was made for
        self._ext = self._ext_f = None      # the same for the phone's motion
        self.rest = np.zeros(N)             # the shape the springs pull toward
        self._Lrest = np.zeros(N)           # L @ rest, kept with it
        self._lean = (0.0, 0.0)

    def set_lean(self, lx, ly):
        """lean downhill: (lx, ly) points down the slope on the screen, its
        length the tilt (0 flat .. 1 on edge). The rest shape bulges toward
        it and tapers away from it, a drop about to roll; True if it changed
        enough to move the skin"""
        if math.hypot(lx - self._lean[0], ly - self._lean[1]) < 0.02:
            return False                # < 0.3 px: a hand's drift doesn't keep it ticking
        self._lean = (lx, ly)
        t = math.hypot(lx, ly)
        if t < 1e-3:
            self.rest = np.zeros(N)
        else:
            c = (lx * _COS1 + ly * _SIN1) / t           # cos(a - downhill)
            self.rest = LEAN * t * (c + LEAN_TAPER * (2 * c * c - 1))     # both average 0: no volume change
        self._Lrest = self.L @ self.rest
        return True

    def poke_at(self, x, y, radius=R):
        """finger at (x, y) from the centre, px (the drop's radius): toward
        the middle the drop is pushed down (it spreads), toward the rim it's
        dented there"""
        dist = min(1.0, math.hypot(x, y) / radius)
        w = max(0.0, min(1.0, (dist - 0.12) / 0.63))
        w = w * w * (3 - 2 * w)
        self.poke = (math.atan2(y, x), (0.12 + 0.26 * dist) * w,
                     0.55 - 0.18 * dist, 0.14 * (1 - w))

    def poke_force(self):
        """the pull toward the pressed shape (made again only when the press moves)"""
        if self.poke != self._poke:
            ang, depth, sigma, spread = self.poke
            a = np.angle(np.exp(1j * (_ANG - ang)))         # wrapped to -pi..pi
            tgt = -depth * np.exp(-(a * a) / (2 * sigma * sigma))
            self._poke, self._poke_f = self.poke, self.K * (tgt + (spread - tgt.mean()))
        return self._poke_f

    def ext_force(self):
        """the inertial force on the water along each point's normal: the side
        it pushes toward bulges, the other flattens (no volume change: the
        normals' projections sum to zero), and it stretches the drop along the
        motion; in and out of the screen squashes it (made again only when the
        motion changes)"""
        if self.ext != self._ext:
            fx, fy, fz = self.ext
            f = math.hypot(fx, fy)
            pr = fx * _COS1 + fy * _SIN1
            force = SLOSH_SHIFT * pr + SLOSH_SQUASH * fz
            if f:
                force += SLOSH_STRETCH * (pr * pr / f - f / 2)
            self._ext, self._ext_f = self.ext, force
        return self._ext_f

    def step(self, dt):
        """advance; True while it still moves"""
        o, v = self.o, self.v
        acc = self.L @ o
        acc -= self._Lrest                  # toward the rest shape, not round
        acc -= self.D * v
        active = False
        if self.poke:
            acc += self.poke_force()
            active = True
        if self.ext[0] or self.ext[1] or self.ext[2]:
            acc += self.ext_force()
            active = True
        acc *= dt
        v += acc
        o += v * dt
        np.minimum(o, self.hi, out=o)          # np.clip's scalar checks cost more than this
        np.maximum(o, self.lo, out=o)
        off = o - self.rest
        return active or bool(off.max() > 0.0008 or off.min() < -0.0008
                              or v.max() > 0.002 or v.min() < -0.002)

    def offset(self, a):
        k = round((a % (2 * math.pi)) / (2 * math.pi) * N) % N
        return float(self.o[k])


def icon_texture(path_d, size=44, stroke=2.0, color="#FFFFFF", shadow=True):
    """a 24-unit SVG stroke icon (the board's), white (or color), with its
    soft shadow (unless not shadow), rasterised once to a texture at 2x for
    the GL renderer to scale. Returns (texture, its size in px with the
    padding the shadow needs)."""
    import cairo
    scale = 2
    pad = 6
    px = (size + 2 * pad) * scale
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, px, px)
    cr = cairo.Context(surf)
    rgb = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    for under in ((True, False) if shadow else (False,)):
        cr.save()
        cr.scale(scale, scale)
        cr.translate(pad, pad + (1 if under else 0))
        cr.scale(size / 24.0, size / 24.0)
        _svg_path(cr, path_d)
        cr.set_line_width(stroke)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        cr.set_line_join(cairo.LINE_JOIN_ROUND)
        cr.set_source_rgba(0, 0, 0, 0.30) if under else cr.set_source_rgb(*rgb)
        cr.stroke()
        cr.restore()
    surf.flush()
    data = GLib.Bytes.new(bytes(surf.get_data()))
    return Gdk.MemoryTexture.new(px, px, Gdk.MemoryFormat.B8G8R8A8_PREMULTIPLIED, data,
                                 surf.get_stride()), (size + 2 * pad)


def _svg_path(cr, d):
    """the subset of SVG path syntax the board's icons use: M L H V C A Z,
    absolute and relative"""
    import re
    toks = re.findall(r"[MmLlHhVvCcAaZz]|-?\d*\.?\d+(?:e-?\d+)?", d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0

    def num():
        nonlocal i
        i += 1
        return float(toks[i - 1])
    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                cr.close_path()
                x, y = sx, sy
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cr.move_to(x, y)
            sx, sy = x, y
            cmd = "l" if rel else "L"
        elif c == "L":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cr.line_to(x, y)
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            cr.line_to(x, y)
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            cr.line_to(x, y)
        elif c == "C":
            pts = [num() for _ in range(6)]
            if rel:
                pts = [pts[j] + (x if j % 2 == 0 else y) for j in range(6)]
            cr.curve_to(*pts)
            x, y = pts[4], pts[5]
        elif c == "A":
            rx, ry, rot, large, sweep, nx, ny = [num() for _ in range(7)]
            if rel:
                nx, ny = x + nx, y + ny
            _arc(cr, x, y, rx, ry, rot, int(large), int(sweep), nx, ny)
            x, y = nx, ny


def _arc(cr, x1, y1, rx, ry, phi, fa, fs, x2, y2):
    """SVG endpoint arc -> cairo (circular arcs suffice for the icons)"""
    if rx == 0 or ry == 0:
        cr.line_to(x2, y2)
        return
    r = rx
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    d2 = dx * dx + dy * dy
    r = max(r, math.sqrt(d2))
    h = math.sqrt(max(0.0, r * r - d2))
    mx, my = (x1 + x2) / 2, (y1 + y2) / 2
    ln = math.sqrt(d2) or 1.0
    ux, uy = -dy / ln, dx / ln
    sgn = -1 if fa != fs else 1         # (SVG's: + when they differ, for (dy, -dx))
    cx, cy = mx + sgn * h * ux, my + sgn * h * uy
    a1 = math.atan2(y1 - cy, x1 - cx)
    a2 = math.atan2(y2 - cy, x2 - cx)
    (cr.arc if fs else cr.arc_negative)(cx, cy, r, a1, a2)


class Droplet(Gtk.Widget):
    """a droplet button: size x size (SIZE, the board's), the drop's radius
    radius (R), the icon icon_size px; emits "clicked" on a tap that didn't
    slide off. Small ones (an app header's back button) draw less outside
    themselves."""

    __gsignals__ = {"clicked": (GObject.SignalFlags.RUN_FIRST, None, ())}

    def __init__(self, icon_d, tension=0.5, size=SIZE, radius=R, icon_size=44, stroke=2.0,
                 tint=0.35, accent=None, dyed=False):
        super().__init__()
        self.size, self.radius = size, radius
        self.look = (tint, accent, dyed)        # push_drop's: the glass, or a dyed drop
        self.margin = MARGIN if radius >= R else max(14, int(radius * 0.6 + 6))
        self.mem = Membrane(tension)
        self.icon_px = icon_size
        self.icon, self.icon_size = icon_texture(icon_d, size=icon_size, stroke=stroke)
        self.backdrop = None        # (Gdk.Texture, the window widget)
        self.up = (0.0, 0.0)        # the screen's "up" from gravity (Slosh), 0 flat .. 1 on edge
        self.tick_id = 0
        self.last = None
        self.press = None
        drag = Gtk.GestureDrag()
        drag.connect("drag-begin", self.on_begin)
        drag.connect("drag-update", self.on_update)
        drag.connect("drag-end", self.on_end)
        self.add_controller(drag)
        self.set_overflow(Gtk.Overflow.VISIBLE)
        adopt_on_realize(self)

    def set_tilt(self, ux, uy):
        """tilted, it leans downhill: the skin springs (and jiggles) into
        the lean, and it sits a little down the slope"""
        self.up = (ux, uy)
        if self.mem.set_lean(-ux, -uy):
            self.kick()
        self.queue_draw()

    def set_backdrop(self, texture, window):
        self.backdrop = (texture, window)
        self.queue_draw()

    def set_look(self, tint=0.35, accent=None, dyed=False):
        self.look = (tint, accent, dyed)
        self.queue_draw()

    def set_icon(self, icon_d, icon_size=None, stroke=2.0):
        self.icon, self.icon_size = icon_texture(icon_d, size=icon_size or self.icon_px,
                                                 stroke=stroke)
        self.queue_draw()

    # ---- size ----

    def do_measure(self, orientation, for_size):
        return self.size, self.size, -1, -1

    # ---- input ----

    def on_begin(self, g, x, y):
        self.press = (x, y)
        self.poke(x - self.size / 2, y - self.size / 2)

    def on_update(self, g, dx, dy):
        if self.press is None:
            return
        x, y = self.press[0] + dx, self.press[1] + dy
        n = self.size
        if not (-12 <= x <= n + 12 and -12 <= y <= n + 12):
            self.press = None       # slid off: let go, and the press opens nothing
            self.release()
            return
        self.poke(x - n / 2, y - n / 2)

    def on_end(self, g, dx, dy):
        if self.press is not None:
            self.press = None
            self.release()
            self.emit("clicked")

    def poke(self, x, y):
        self.mem.poke_at(x, y, self.radius)
        self.kick()

    def release(self):
        self.mem.poke = None
        self.kick()

    def splash(self):
        """a key press: a quick push in the middle"""
        self.poke(0, 0)
        GLib.timeout_add(160, lambda: self.release() or False)

    # ---- animation ----

    def kick(self):
        if not self.tick_id:
            self.last = None
            self.tick_id = self.add_tick_callback(self.on_tick)

    def on_tick(self, widget, clock):
        t = clock.get_frame_time() / 1e6
        dt = 0.016 if self.last is None else min(0.033, t - self.last)
        self.last = t
        active = self.mem.step(dt)
        self.queue_draw()
        if not active:
            self.tick_id = 0
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    # ---- drawing ----

    def do_snapshot(self, s):
        o = self.mem.o
        c, r, m = self.size / 2, self.radius, self.margin
        lx, ly = lean_shift(self.up, r)
        full = self.size + 2 * m
        bounds = Graphene.Rect().init(-m, -m, full, full)
        tint, accent, dyed = self.look
        if not push_drop(s, self, bounds, c + lx, c + ly, r, o, self.up,
                         lambda s: self.snapshot_backdrop(s, bounds),
                         tint=tint, accent=accent, dyed=dyed):
            bead = rrect(c - r, c - r, 2 * r, 2 * r, r)
            s.push_rounded_clip(bead)
            s.append_color(rgba("#FFFFFF", 0.18), rect(c - r, c - r, 2 * r, 2 * r))
            s.pop()
        # the icon floats: it drifts away from a dent (and down a lean, toward
        # the swollen side) and squeezes a little
        z = max(0.0, min(1.0, float(o.mean()) / 0.14))
        sx, sy = float(o @ _COS1), float(o @ _SIN1)
        iscale = (1 - float(np.abs(o).sum()) / N * 0.6) * (1 - 0.12 * z)
        s.save()
        drift = 2.2 * r / R
        s.translate(Graphene.Point().init(c + lx + sx * drift, c + ly + sy * drift))
        s.scale(iscale, iscale)
        h = self.icon_size / 2
        s.append_texture(self.icon, Graphene.Rect().init(-h, -h, 2 * h, 2 * h))
        s.restore()

    def snapshot_backdrop(self, s, bounds):
        """what's behind the droplet, in the shader's node: the window's
        wallpaper texture, offset to where the droplet sits, under the glass
        of the panel it's in"""
        snapshot_wallpaper(s, self, bounds)


# ---- the phone's motion ----

class Slosh:
    """the accelerometer for the water: a low-pass of the readings tracks
    gravity (so tilting slowly, or holding the phone tilted, does nothing);
    what's left is the phone's own motion, and the water feels its opposite
    (it lags behind). That motion is low-passed first (MOTION_HZ, two poles):
    a tremor (4-12 Hz) is filtered out while a deliberate move (below ~3 Hz)
    comes through. Below the dead zone (m/s^2) what's left does nothing, so a
    resting hand doesn't make the drops shiver either; gain scales the rest. Each droplet takes it a little differently, so they don't move in
    step. The low-pass is also the tilt (tilt()): it swings the droplets'
    light, shifts the view through their lenses and leans them downhill
    (set_tilt). TITAN_WATER_TILT="x,y" holds the tilt there instead (the
    screen's up, for screenshots on a phone lying flat).

    Runs the sensor only while started: call start() when the window becomes
    active and stop() when it isn't (the app does; it also pauses while the
    screen is off). Readings are Android's device axes (x right, y to the
    top of the screen, z out of it); the screen's y points down."""

    GRAVITY_TAU = 0.4           # s: the low-pass that follows gravity and slow tilts
    MOTION_HZ = 2.5             # the motion's low-pass: deliberate moves pass, tremor doesn't
    LIMIT = 8.0                 # m/s^2 felt at most

    def __init__(self, gain=1.0, deadzone=0.6):
        self.gain = gain
        self.deadzone = deadzone
        self.drops = []
        self.sensor = None
        self.watch = 0
        self.timer = 0
        self.g = None
        self.t = None
        self.up = (0.0, 0.0)
        self.m1 = [0.0, 0.0, 0.0]   # the motion low-pass, two first-order stages
        self.m2 = [0.0, 0.0, 0.0]
        fixed = os.environ.get("TITAN_WATER_TILT")
        self.fixed = tuple(float(v) for v in fixed.split(",")) if fixed else None

    def detach(self, droplet):
        if droplet in self.drops:
            self.drops.remove(droplet)

    def attach(self, droplet):
        if droplet in self.drops:
            return
        droplet.slosh_k = random.uniform(0.85, 1.15)
        self.drops.append(droplet)
        if self.up != (0.0, 0.0):
            droplet.set_tilt(*self.up)
        if self.fixed:
            # already leaning: with the screen off (screenshots) no frame
            # ticks, and the skins would never get there
            self.up = self.fixed
            droplet.set_tilt(*self.fixed)
            for v in vars(droplet).values():
                for m in v if isinstance(v, list) else (v,):
                    if isinstance(m, Membrane):
                        m.o[:] = m.rest

    def start(self):
        if self.sensor:
            return True
        if self.fixed:
            return True
        try:
            import titan_sensors
        except ImportError:
            return False
        if titan_sensors.screen_off():
            self._idle_timer()
            return False
        s = titan_sensors.Sensor("acc", hz=50)
        if not s.available() or not s.start():
            return False
        self.sensor = s
        self.g = self.t = None
        self.watch = GLib.io_add_watch(s.fd, GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN,
                                       self.on_data)
        self._idle_timer()
        return True

    def stop(self):
        if self.timer:
            GLib.source_remove(self.timer)
            self.timer = 0
        if self.watch:
            GLib.source_remove(self.watch)
            self.watch = 0
        if self.sensor:
            self.sensor.stop()
            self.sensor = None
        self.feel(0.0, 0.0, 0.0)

    def _idle_timer(self):
        if not self.timer:
            self.timer = GLib.timeout_add(600, self.on_timer)

    def on_timer(self):
        """keep the sensor on (the lock screen switches it off when it
        unlocks), and off while the screen is"""
        import titan_sensors
        if titan_sensors.screen_off():
            if self.sensor:
                timer, self.timer = self.timer, 0
                self.stop()
                self.timer = timer
        elif not self.sensor:
            timer, self.timer = self.timer, 0
            self.start()
            self.timer = timer
        else:
            self.sensor.keep_alive()
        return True

    def on_data(self, fd, cond):
        for t, (x, y, z), status in self.sensor.read():
            if self.g is None:
                self.g, self.t = [x, y, z], t
                self.m1, self.m2 = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]
                continue
            dt = max(0.0, min(0.1, t - self.t))
            self.t = t
            a = dt / (self.GRAVITY_TAU + dt)
            g = self.g
            for i, v in enumerate((x, y, z)):
                g[i] += a * (v - g[i])
            self.tilt(g)
            # the motion on the screen (x right, y down), tremor filtered out
            b = 1 - math.exp(-2 * math.pi * self.MOTION_HZ * dt)
            for i, v in enumerate((x - g[0], -(y - g[1]), z - g[2])):
                self.m1[i] += b * (v - self.m1[i])
                self.m2[i] += b * (self.m1[i] - self.m2[i])
            mx, my, mz = self.m2
            m = math.sqrt(mx * mx + my * my + mz * mz)
            if m <= self.deadzone:
                self.feel(0.0, 0.0, 0.0)
                continue
            k = self.gain * min(m - self.deadzone, self.LIMIT) / m
            self.feel(-mx * k, -my * k, -mz * k)
        return True

    def tilt(self, g):
        """the screen's "up" (x right, y down) from the low-passed gravity:
        its in-plane part, 0 lying flat, 1 standing on an edge; redraw the
        drops when it moved"""
        ux, uy = g[0] / 9.81, -g[1] / 9.81
        n = math.hypot(ux, uy)
        if n > 1:
            ux, uy = ux / n, uy / n
        if math.hypot(ux - self.up[0], uy - self.up[1]) > 0.004:
            self.up = (ux, uy)
            for d in self.drops:
                d.set_tilt(ux, uy)

    def feel(self, fx, fy, fz):
        for d in self.drops:
            if not hasattr(d, "mem"):       # tilt only (a slider's bead)
                continue
            j = getattr(d, "slosh_k", 1.0)
            d.mem.ext = (fx * j, fy * j, fz * j)
            if fx or fy or fz:
                d.kick()


# ---- the board's Aurora wallpaper ----

def aurora_texture(w, h):
    """'linear-gradient(160deg, #10204A, #1B3F8F, #23125C)' with three radial
    washes (teal up left, pink right, amber at the bottom), as a texture"""
    import cairo
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
    cr = cairo.Context(surf)
    a = math.radians(160 - 90)
    dx, dy = math.cos(a), math.sin(a)
    half = (abs(dx) * w + abs(dy) * h) / 2
    lg = cairo.LinearGradient(w / 2 - dx * half, h / 2 - dy * half,
                              w / 2 + dx * half, h / 2 + dy * half)
    for off, col in ((0, "10204A"), (0.5, "1B3F8F"), (1, "23125C")):
        lg.add_color_stop_rgb(off, *[int(col[i:i + 2], 16) / 255 for i in (0, 2, 4)])
    cr.set_source(lg)
    cr.paint()
    for (x, y, r, col) in ((-120 + 210, -80 + 210, 210, "2BE3C9"),
                           (w + 140 - 230, 120 + 230, 230, "FF5FA2"),
                           (160 + 240, h + 200 - 240, 240, "FFC94A")):
        rg = cairo.RadialGradient(x, y, 0, x, y, r)
        rgb = [int(col[i:i + 2], 16) / 255 for i in (0, 2, 4)]
        rg.add_color_stop_rgba(0, *rgb, 1)
        rg.add_color_stop_rgba(0.7, *rgb, 0)
        cr.set_source(rg)
        cr.paint()
    surf.flush()
    return Gdk.MemoryTexture.new(w, h, Gdk.MemoryFormat.B8G8R8A8_PREMULTIPLIED,
                                 GLib.Bytes.new(bytes(surf.get_data())), surf.get_stride())


def adopt_on_realize(widget):
    """when the widget is realized, its window (an app's, titan_ui4) gives it
    the wallpaper to refract and the phone's tilt; unrealized, it lets go"""
    def on_realize(w):
        adopt = getattr(w.get_root(), "adopt_one", None)
        if adopt:
            adopt(w)

    def on_unrealize(w):
        drop = getattr(w.get_root(), "drop_one", None)
        if drop:
            drop(w)
    widget.connect("realize", on_realize)
    widget.connect("unrealize", on_unrealize)


class Wallpaper(Gtk.Widget):
    """draws a texture filling the widget; the window's root"""

    def __init__(self, texture):
        super().__init__()
        self.texture = texture

    def do_snapshot(self, s):
        s.append_texture(self.texture, Graphene.Rect().init(0, 0, self.get_width(),
                                                            self.get_height()))


def demo_poker(droplets, interval_ms=950, hold_ms=420):
    """the board's autoplay: poke droplets in turn at random spots, every
    third one in the middle"""
    state = {"step": 0}

    def tick():
        i = state["step"] % len(droplets)
        state["step"] += 1
        d = droplets[i]
        if state["step"] % 3 == 0:
            d.mem.poke = (0.0, 0.0, 0.5, 0.14)
        else:
            d.mem.poke = (random.uniform(0, 2 * math.pi), 0.22 + random.random() * 0.14, 0.42, 0.0)
        d.kick()
        GLib.timeout_add(hold_ms, lambda: d.release() or False)
        return True
    return GLib.timeout_add(interval_ms, tick)


# ======================================================================
# The controls of the "Controls" design board: nav slider, sliders,
# radios, checkboxes, scroll bar, and the glass panels that hold them.
# Drawn with GTK's render nodes (gradients, rounded clips, shadows, borders,
# text: the GL renderer draws them on the GPU) and, where the material needs
# it, shaders: the slider's thumb is a droplet (push_drop), the checkbox's
# liquid has its own. Every moving part is a damped spring, as on the board.
# ======================================================================

from gi.repository import Pango  # noqa: E402

FONT = "Atkinson Hyperlegible"
INK = "#141821"             # dark text on lit water
ACCENT = "#7FD8FF"          # the board's default accent


_RGBA = {}


def rgba(spec, alpha=None):
    """a colour, cached: a control parses ~20 a frame otherwise (render nodes
    copy it; don't change one you got from here)"""
    c = _RGBA.get((spec, alpha))
    if c is None:
        c = Gdk.RGBA()
        c.parse(spec)
        if alpha is not None:
            c.alpha = alpha
        _RGBA[spec, alpha] = c
    return c


def rect(x, y, w, h):
    return Graphene.Rect().init(x, y, w, h)


def rrect(x, y, w, h, radius):
    r = Gsk.RoundedRect()
    r.init_from_rect(rect(x, y, w, h), radius)
    return r


_STOPS = {}


def stops(*pairs):
    """[(offset, Gdk.RGBA), ...] -> Gsk.ColorStops, cached (the colours come
    from rgba()'s cache, so the same stops are the same objects)"""
    key = tuple((off, id(col)) for off, col in pairs)
    out = _STOPS.get(key)
    if out is None:
        out = []
        for off, col in pairs:
            st = Gsk.ColorStop()
            st.offset = off
            st.color = col
            out.append(st)
        _STOPS[key] = out
    return out


def point(x, y):
    return Graphene.Point().init(x, y)


class Spring:
    """a mass on a damped spring: x moves toward t; k stiffness, c damping
    (per unit mass); resting within eps (the board's)"""

    def __init__(self, x, k, c, eps=0.01):
        self.x = self.t = float(x)
        self.v = 0.0
        self.k, self.c, self.eps = k, c, eps

    def step(self, dt):
        self.v += (self.k * (self.t - self.x) - self.c * self.v) * dt
        self.x += self.v * dt

    def resting(self):
        return abs(self.t - self.x) < self.eps and abs(self.v) < self.eps * 10

    def snap(self, x=None):
        if x is not None:
            self.t = float(x)
        self.x, self.v = self.t, 0.0


class Springy:
    """mixin for a Gtk.Widget whose look follows springs: kick() runs them
    each frame (two half steps, as the board) until all rest and held() is
    False; step() is called after each frame's physics"""

    WEIGHT = 1.0            # the board's weight tweak at 50 %: heavier is slower

    def springs(self):
        return []

    def held(self):
        return False

    def step(self, dt):
        pass

    def redraw(self):
        """after a frame's physics; a widget whose moving parts are separate
        children redraws just those (GTK keeps every other widget's render)"""
        self.queue_draw()

    def kick(self):
        if not getattr(self, "_tick", 0):
            self._last = None
            self._tick = self.add_tick_callback(self._on_tick)

    def _on_tick(self, widget, clock):
        t = clock.get_frame_time() / 1e6
        dt = 1 / 60 if self._last is None else min(1 / 30, t - self._last)
        self._last = t
        moving = self.held()
        for sp in self.springs():
            for _ in range(2):
                sp.step(dt / 2 / self.WEIGHT)
            if not sp.resting():
                moving = True
        if self.step(dt):
            moving = True
        self.redraw()
        if not moving:
            self._tick = 0
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE


_FONTS = {}


def layout(widget, string, size, bold=False):
    """a Pango layout of the text, cached on the widget: making one costs
    ~0.5 ms on the phone, and a control's labels are redrawn every frame it
    moves"""
    cache = widget.__dict__.setdefault("_layouts", {})
    key = (string, size, bold)
    lay = cache.get(key)
    if lay is None:
        if len(cache) > 256:            # changing values (a slider's) don't pile up
            cache.clear()
        fd = _FONTS.get((size, bold))
        if fd is None:
            fd = _FONTS[size, bold] = Pango.FontDescription.from_string(
                "%s %s %dpx" % (FONT, "Bold" if bold else "", size))
        lay = cache[key] = widget.create_pango_layout(string)
        lay.set_font_description(fd)
        lay.size_px = lay.get_pixel_size()
    return lay


def text(s, widget, x, y, string, size, bold=False, color="#FFFFFF", align=0.0,
         alpha=1.0, shadow=True, width=None):
    """draw a line of text at (x, y) (its top), aligned 0 left .. 1 right of x
    (or within width); the board's text-shadow 0 1px 2px rgba(0,0,0,.35) as a
    1 px offset copy. Returns its (width, height)."""
    lay = layout(widget, string, size, bold)
    w, h = lay.size_px
    if width is not None:
        x = x + (width - w) * align
    else:
        x = x - w * align
    if shadow:
        s.save()
        s.translate(point(x, y + 1))
        s.append_layout(lay, rgba("#000000", 0.35 * alpha))
        s.restore()
    s.save()
    s.translate(point(x, y))
    s.append_layout(lay, rgba(color, alpha))
    s.restore()
    return w, h


def text_size(widget, string, size, bold=False):
    return layout(widget, string, size, bold).size_px


def fitted(widget, string, size, bold, width):
    """a layout of the text cut to width px with an ellipsis (cached on the
    widget for that width)"""
    cache = widget.__dict__.setdefault("_fitted", {})
    key = (string, size, bold, int(width))
    lay = cache.get(key)
    if lay is None:
        if len(cache) > 32:
            cache.clear()
        lay = layout(widget, string, size, bold).copy()
        lay.set_width(int(width) * Pango.SCALE)
        lay.set_ellipsize(Pango.EllipsizeMode.END)
        lay.size_px = lay.get_pixel_size()
        cache[key] = lay
    return lay


def glow(s, outline, accent, blur=10):
    """box-shadow: 0 0 10px <accent>88"""
    s.append_outset_shadow(outline, rgba(accent, 0x88 / 255), 0, 0, 0, blur)


# ---- the board's wallpapers, from its CSS ----

WALLPAPERS = {
    # radial-gradient(RX% RY% at X% Y%, colour 0%, transparent STOP%) over a
    # linear-gradient(ANGLE, colours)
    "aurora": ([(1.2, .8, .1, 0, "#2BD9C3", .55), (.9, .7, 1, .2, "#7B5CFF", .6),
                (1.1, .9, .6, 1, "#FF6FA8", .6)],
               160, [(0, "#10204A"), (.5, "#1B3F8F"), (1, "#23125C")]),
    "lagoon": ([(1, .7, 0, 1, "#00C2D1", .6), (.9, .8, 1, 0, "#6EE7B7", .55),
                (.8, .6, .7, .7, "#2563EB", .7)],
               180, [(0, "#0B3B5C"), (.6, "#0E6A8F"), (1, "#0B2F4A")]),
    "dusk": ([(1.1, .7, .5, 0, "#FFB36B", .6), (.9, .8, 0, .8, "#FF5E7E", .6),
              (.9, .8, 1, .9, "#6D4AFF", .6)],
             180, [(0, "#3A1C5E"), (.55, "#6B2A6E"), (1, "#24123F")]),
}


def wallpaper_texture(name, w, h):
    """a board wallpaper ("aurora", "lagoon", "dusk") as a texture: CSS
    radial gradients (ellipses sized in % of the box) over a linear one"""
    import cairo
    washes, angle, base = WALLPAPERS[name]
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
    cr = cairo.Context(surf)
    a = math.radians(angle - 90)
    dx, dy = math.cos(a), math.sin(a)
    half = (abs(dx) * w + abs(dy) * h) / 2
    lg = cairo.LinearGradient(w / 2 - dx * half, h / 2 - dy * half, w / 2 + dx * half, h / 2 + dy * half)
    for off, col in base:
        lg.add_color_stop_rgb(off, *[int(col[i:i + 2], 16) / 255 for i in (1, 3, 5)])
    cr.set_source(lg)
    cr.paint()
    for rx, ry, x, y, col, stop in reversed(washes):     # CSS paints the first on top
        rgb = [int(col[i:i + 2], 16) / 255 for i in (1, 3, 5)]
        cr.save()
        cr.translate(x * w, y * h)
        cr.scale(rx * w, ry * h)
        rg = cairo.RadialGradient(0, 0, 0, 0, 0, 1)
        rg.add_color_stop_rgba(0, *rgb, 1)
        rg.add_color_stop_rgba(stop, *rgb, 0)
        cr.set_source(rg)
        cr.paint()
        cr.restore()
    surf.flush()
    return Gdk.MemoryTexture.new(w, h, Gdk.MemoryFormat.B8G8R8A8_PREMULTIPLIED,
                                 GLib.Bytes.new(bytes(surf.get_data())), surf.get_stride())


CSS = """
.water-panel { border-radius: 24px; background: rgba(255,255,255,0.12);
  border: 1px solid rgba(255,255,255,0.34);
  box-shadow: inset 0 1px 0 rgba(255,255,255,0.5), 0 10px 24px rgba(0,0,0,0.18); }
.water-scroll scrollbar { opacity: 0; }
"""


def install_css(display):
    """the glass panels' CSS (and hides GTK's scroll bars inside a
    WaterScroll: the water one replaces them)"""
    css = Gtk.CssProvider()
    css.load_from_data(CSS.encode())
    Gtk.StyleContext.add_provider_for_display(display, css,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


def panel(padding=(6, 18, 8, 18), spacing=0):
    """a glass panel (a pool of water) holding controls in a column"""
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=spacing)
    box.add_css_class("water-panel")
    t, r, b, l = padding
    box.set_margin_top(0)
    inner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=spacing)
    inner.set_margin_top(t)
    inner.set_margin_end(r)
    inner.set_margin_bottom(b)
    inner.set_margin_start(l)
    box.append(inner)
    box.inner = inner
    return box


# ---- the navigation slider ----

VIAL = """
/* the nav slider's pill: a glass vial of clear, lit liquid, its shape and the
   liquid drawn here (stretching it with a transform would make GTK render
   the node offscreen every frame). What's behind it (the wallpaper and the
   track's glass, the child texture) shows through as it is, under a light
   tint, and bends in a lens-like outline.
   (no comments on the uniform lines: GTK's parser then skips them)
   u_geo: the pill's left and right x, its middle y, its height (node px)
   u_form: the neck (0..1: the middle pinched by that much of the height),
     the accent (r, g, b)
   u_liq: how full (0..1), the surface's tilt (slope), the wave's phase (px),
     its amplitude (px)
   u_look: the tint's opacity over the view (0.35), the refracting outline's
     width (px) and how far it bends the view at the very edge (px); w: 1 for
     an upright vial (a scroll bar: x and y swap, "left and right" are then
     its top and bottom) */
uniform vec4 u_geo;
uniform vec4 u_form;
uniform vec4 u_liq;
uniform vec4 u_look;

/* GTK 4.8 counts textures from these declarations but binds them as
   u_source (see the droplet's shader) */
uniform sampler2D u_texture1;

vec4 over(vec4 top, vec4 under)     /* premultiplied */
{
  return top + under * (1.0 - top.a);
}

vec4 behind(vec2 p, vec2 resolution)
{
  vec2 t = clamp(p / resolution, vec2(0.0), vec2(1.0));
  return GskTexture(u_source, vec2(t.x, 1.0 - t.y));
}

/* signed distance to the pill (negative inside): a capsule whose straight
   part narrows toward the middle by the neck */
float pill(vec2 p)
{
  float L = u_geo.x, R = u_geo.y, cy = u_geo.z, h = u_geo.w;
  float r = h * 0.5;
  float a = L + r, b = max(R - r, a);
  if (p.x < a)
    return length(p - vec2(a, cy)) - r;
  if (p.x > b)
    return length(p - vec2(b, cy)) - r;
  float m = (a + b) * 0.5, hw = max((b - a) * 0.5, 1.0);
  float k = clamp(1.0 - pow((p.x - m) / hw, 2.0), 0.0, 1.0);
  return abs(p.y - cy) - r * (1.0 - u_form.x * k * k);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord, in vec2 resolution, in vec2 uv)
{
  /* upright, the pill's own coordinates have x and y swapped */
  bool up = u_look.w > 0.5;
  vec2 p = up ? fragCoord.yx : fragCoord;
  float cy = u_geo.z, h = u_geo.w, mid = (u_geo.x + u_geo.y) * 0.5;
  float ta = u_look.x;
  float d = pill(p);
  float inside = clamp(0.5 - d, 0.0, 1.0);

  /* its shadow, 0 6px 14px (down the screen), lighter the clearer it is */
  float sd = pill(p - (up ? vec2(6.0, 0.0) : vec2(0.0, 6.0)));
  float sa = mix(0.12, 0.28, ta);
  vec4 col = vec4(0.0, 0.0, 0.0, sa * (1.0 - smoothstep(-7.0, 14.0, sd)) * (1.0 - inside));

  if (inside > 0.0) {
    float top = cy - h * 0.5, bot = cy + h * 0.5;
    float t = clamp((p.y - top) / h, 0.0, 1.0);
    /* the outline: within u_look.y px of it the view bends outward, as
       through a lens's edge, the channels a little apart; inside it, the
       view as it is */
    float a0 = u_geo.x + h * 0.5, b0 = max(u_geo.y - h * 0.5, a0);
    vec2 nrm = p - vec2(clamp(p.x, a0, b0), cy);
    nrm = length(nrm) > 0.001 ? normalize(nrm) : vec2(0.0, 1.0);
    float band = clamp((d + u_look.y) / u_look.y, 0.0, 1.0);
    float bend = band * band * u_look.z;
    vec2 q = p + nrm * bend;
    vec2 dq = nrm * bend * 0.18;
    if (up) {                   /* back to the screen's coordinates */
      q = q.yx;
      dq = dq.yx;
    }
    vec4 bg = behind(q, resolution);
    vec3 view = vec3(behind(q + dq, resolution).r, bg.g, behind(q - dq, resolution).b);
    view = min(view * (1.0 + 0.18 * band), vec3(1.0));     /* brighter toward the edge */

    /* the liquid below its surface: the level for how full, tilted while
       the pill accelerates, with a wave on it */
    float level = bot - u_liq.x * h;
    float ys = level + u_liq.y * (p.x - mid) + u_liq.w * sin(6.2831853 * (p.x + u_liq.z) / 40.0);
    float wet = clamp(p.y - ys + 0.5, 0.0, 1.0);
    /* a light tint over the view: the board's lit white, the accent coming
       in toward the bottom; the empty glass a third of it, and a bright line
       along the surface when it shows */
    vec3 tint = mix(vec3(1.0), mix(vec3(1.0), u_form.yzw, 0.55), t * t);
    vec3 water = mix(view, tint, ta);
    vec3 glass = mix(view, vec3(1.0), ta * 0.33);
    float crest = 0.35 * clamp(1.0 - abs(p.y - ys) / 1.5, 0.0, 1.0) * step(u_liq.x, 0.995);
    water = min(water + crest, vec3(1.0));

    vec4 inner;
    if (bg.a > 0.5) {
      /* the view is drawn here, so the vial is opaque: its transparency is
         in how much of the view comes through */
      inner = vec4(mix(glass, water, wet), 1.0);
    } else {
      /* no backdrop given: see-through by alpha over what's under */
      inner = over(vec4(tint, 1.0) * ta * wet, vec4(1.0) * ta * 0.33);
    }
    col = over(inner * inside, col);
    /* the rim: bright along the top (inset 0 1px 0 #fff), softer below */
    float edge = clamp(1.0 - abs(d + 0.6) / 0.9, 0.0, 1.0);
    float rim = edge * mix(0.95, 0.40, t);
    col = over(vec4(vec3(rim), rim), col);
  }
  fragColor = col;
}
"""

_vial = None


def vial_shader():
    global _vial
    if _vial is None:
        _vial = Gsk.GLShader.new_from_bytes(GLib.Bytes.new(VIAL.encode()))
    return _vial

class NavSlider(Springy, Gtk.Widget):
    """the board's segmented navigation: a glass track with a pill over the
    chosen tab, a glass vial of clear liquid (the VIAL shader): the window's
    wallpaper (set_backdrop) shows through it as it is, under a light tint
    (tint, 0.35), and bends in a refracting outline (rim px wide, bend px at
    its edge). The pill's
    two edges are springs, the trailing one soft, so it stretches as it
    travels: stretched it squashes and necks in the middle like a liquid
    bridge, and its liquid, a fixed volume, no longer fills it: the surface
    shows, tilts as the pill speeds up and slows down (the liquid lags), and
    sloshes level again with a wave. Pressed it squashes; dragged it follows
    the finger with a rubber band past the ends and, let go, the throw's
    momentum picks the tab. Left/Right switch from the keyboard. Emits
    "changed" (the tab's index)."""

    __gsignals__ = {"changed": (GObject.SignalFlags.RUN_FIRST, None, (int,))}
    H, PAD = 58, 5
    # the liquid: its surface's slope per px/s^2 of the pill's acceleration
    # (at most TILT_MAX), the wave's height per px/s of its speed (at most
    # WAVE_MAX px)
    TILT_PER, TILT_MAX = 0.00005, 0.45
    WAVE_PER, WAVE_MAX = 1 / 220, 3.5

    def __init__(self, tabs, index=0, accent=ACCENT, tint=0.35, rim=9.0, bend=7.0):
        super().__init__()
        self.tabs = tabs                    # [(name, svg path d), ...]
        self.index = index
        self.accent = accent
        self.tint = tint                    # the liquid's tint over what shows through
        self.rim, self.bend = rim, bend     # the refracting outline: width, bend at its edge (px)
        self.backdrop = None                # (texture, window): what shows through the vial
        self.icons = {}
        self.L = Spring(0, 260, 22, 0.05)
        self.Rt = Spring(0, 260, 22, 0.05)
        self.sq = Spring(0, 320, 16, 0.001)
        self.tilt = Spring(0, 90, 5, 0.002)     # the liquid's surface: it sloshes
        self.wave = Spring(0, 40, 6, 0.02)
        self.phase = 0.0
        self.vc = 0.0                           # the pill's speed, for its acceleration
        self.nd = None
        self.laid = None
        self.set_focusable(True)
        drag = Gtk.GestureDrag()
        drag.connect("drag-begin", self.on_begin)
        drag.connect("drag-update", self.on_update)
        drag.connect("drag-end", self.on_end)
        self.add_controller(drag)
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        adopt_on_realize(self)

    def do_measure(self, orientation, for_size):
        if orientation == Gtk.Orientation.HORIZONTAL:
            return 120 * len(self.tabs), 686, -1, -1
        return self.H, self.H, -1, -1

    def seg(self):
        return (self.get_width() - 2 * self.PAD) / len(self.tabs)

    def set_backdrop(self, texture, window):
        """what's behind the nav slider (the window's wallpaper): it shows
        through the vial, bent at its rim"""
        self.backdrop = (texture, window)
        self.queue_draw()

    def springs(self):
        return (self.L, self.Rt, self.sq, self.tilt, self.wave)

    def held(self):
        return self.nd is not None

    def step(self, dt):
        # the liquid lags the pill: speeding up it piles at the back (the
        # surface slopes up toward it), and a wave runs while it moves
        vc = (self.L.v + self.Rt.v) / 2
        acc = (vc - self.vc) / max(dt, 1e-3)
        self.vc = vc
        self.tilt.t = max(-self.TILT_MAX, min(self.TILT_MAX, acc * self.TILT_PER))
        self.wave.t = min(self.WAVE_MAX, abs(vc) * self.WAVE_PER)
        self.phase = (self.phase + dt * (60 if vc >= 0 else -60)) % 40
        return False

    def select(self, i, vel=0.0):
        """go to tab i (from code, a tap or the keys); vel: a throw's speed"""
        i = max(0, min(len(self.tabs) - 1, i))
        target = self.PAD + i * self.seg()
        right = target > self.L.x
        self.L.t, self.Rt.t = target, target + self.seg()
        # the leading edge springs first, the trailing edge follows (soft:
        # the pill stretches, pulled out like a drop of liquid)
        self.L.k, self.Rt.k = (75, 260) if right else (260, 75)
        self.L.c, self.Rt.c = (11, 22) if right else (22, 11)
        if vel:
            self.L.v = self.Rt.v = vel
        if i != self.index:
            self.index = i
            self.emit("changed", i)
        self.kick()

    def on_begin(self, g, x, y):
        self.grab_focus()
        self.nd = {"x0": x, "left0": self.L.x, "moved": False, "lx": x,
                   "lt": GLib.get_monotonic_time(), "vel": 0.0}
        self.sq.t = 1
        self.kick()

    def on_update(self, g, dx, dy):
        nd = self.nd
        if not nd:
            return
        x, now = nd["x0"] + dx, GLib.get_monotonic_time()
        if not nd["moved"] and abs(dx) < 6:
            return
        nd["moved"] = True
        raw = nd["left0"] + dx
        lo, hi = self.PAD, self.get_width() - self.PAD - self.seg()
        if raw < lo:
            raw = lo - (lo - raw) * 0.3
        if raw > hi:
            raw = hi + (raw - hi) * 0.3
        nd["vel"] = 0.6 * nd["vel"] + 0.4 * (x - nd["lx"]) / max(4e-3, (now - nd["lt"]) / 1e6)
        nd["lx"], nd["lt"] = x, now
        # the edges chase the finger: the leading edge stiff, the trailing soft
        right = nd["vel"] >= 0
        self.L.t, self.Rt.t = raw, raw + self.seg()
        self.L.k, self.Rt.k = (150, 900) if right else (900, 150)
        self.L.c, self.Rt.c = (18, 50) if right else (50, 18)
        self.kick()

    def on_end(self, g, dx, dy):
        nd, self.nd = self.nd, None
        if not nd:
            return
        if nd["moved"]:
            rest = self.L.x + nd["vel"] * 0.22         # where momentum would carry it
            i = round((rest - self.PAD) / self.seg())
        else:
            i = int((nd["x0"] - self.PAD) // self.seg())
        self.sq.t = 0
        self.select(i, nd["vel"] if nd["moved"] else 0.0)

    def on_key(self, ctl, keyval, code, state):
        name = Gdk.keyval_name(keyval) or ""
        if name in ("Left", "Right"):
            self.select(self.index + (1 if name == "Right" else -1))
            return True
        return False

    def draw_pill(self, s, W, seg, L, Rx):
        """the vial: its shape from the edges, the press and the stretch, and
        how full it is from its liquid's volume (that of the pill at rest)"""
        H, PAD = self.H, self.PAD
        h0 = H - 2 * PAD
        stretch = max(0.0, (Rx - L) / seg - 1)
        sq = self.sq.x
        mid = (L + Rx) / 2
        w = (Rx - L) * (1 + 0.02 * sq)
        h = h0 * (1 - 0.12 * sq) / (1 + stretch * 1.2)
        neck = min(0.6, stretch * 1.6)

        def area(w, h, neck):
            r = h / 2           # a capsule, less what the neck pinches off
            return w * h - (4 - math.pi) * r * r - r * neck * (16 / 15) * max(0.0, w - h)
        fill = max(0.3, min(1.0, area(seg, h0, 0) / max(1.0, area(w, h, neck))))
        oy = 8                                  # the node starts above, for nothing; below, the shadow
        bounds = rect(0, -oy, W, H + oy + 24)
        sh = vial_shader()
        try:
            ok = sh.compile(self.get_native().get_renderer())
        except GLib.Error:
            ok = False
        if ok:
            acc = rgba(self.accent)
            a = np.array([mid - w / 2, mid + w / 2, H / 2 + oy, h,
                          neck, acc.red, acc.green, acc.blue,
                          fill, self.tilt.x, self.phase, self.wave.x,
                          self.tint, self.rim, self.bend, 0], np.float32)
            s.push_gl_shader(sh, bounds, GLib.Bytes.new(a.tobytes()))
            # what shows through: the wallpaper and the track's glass (none
            # given: the shader falls back to alpha over the track)
            if self.backdrop:
                tex, win = self.backdrop
                ok2, pt = self.compute_point(win, point(0, 0))
                if ok2:
                    s.push_clip(bounds)
                    s.append_texture(tex, rect(-pt.x, -pt.y, win.get_width(), win.get_height()))
                    # the track's glass, its lines included: the outline bends
                    # them (over a smooth wallpaper alone it'd show nothing)
                    track = rrect(0, 0, W, H, H / 2)
                    s.push_rounded_clip(track)
                    s.append_color(rgba("#FFFFFF", 0.14), rect(0, 0, W, H))
                    s.pop()
                    s.append_inset_shadow(track, rgba("#FFFFFF", 0.55), 0, 1, 0, 0)
                    s.append_inset_shadow(track, rgba("#000000", 0.12), 0, -1, 0, 0)
                    s.append_border(track, [1] * 4, [rgba("#FFFFFF", 0.38)] * 4)
                    s.pop()
            s.gl_shader_pop_texture()
            s.pop()
        else:
            pill = rrect(mid - w / 2, H / 2 - h / 2, w, h, h / 2)
            s.append_outset_shadow(pill, rgba("#000000", 0.28), 0, 6, 0, 14)
            s.push_rounded_clip(pill)
            s.append_color(rgba("#FFFFFF", 0.92), rect(mid - w / 2, H / 2 - h / 2, w, h))
            s.pop()

    def icon(self, i, dark):
        key = (i, dark)
        if key not in self.icons:
            self.icons[key] = icon_texture(self.tabs[i][1], size=20, stroke=2.2,
                                           color=INK if dark else "#FFFFFF",
                                           shadow=not dark)
        return self.icons[key]

    def do_snapshot(self, s):
        W, H, PAD = self.get_width(), self.H, self.PAD
        seg = self.seg()
        if self.laid != W:                  # first layout, or resized: rest on the tab
            self.laid = W
            self.L.snap(PAD + self.index * seg)
            self.Rt.snap(PAD + (self.index + 1) * seg)
        track = rrect(0, 0, W, H, H / 2)
        s.append_outset_shadow(track, rgba("#000000", 0.25), 0, 10, 0, 26)
        s.push_rounded_clip(track)
        s.append_color(rgba("#FFFFFF", 0.14), rect(0, 0, W, H))
        s.pop()
        s.append_inset_shadow(track, rgba("#FFFFFF", 0.55), 0, 1, 0, 0)
        s.append_inset_shadow(track, rgba("#000000", 0.12), 0, -1, 0, 0)
        s.append_border(track, [1] * 4, [rgba("#FFFFFF", 0.38)] * 4)

        # the pill: its edges are two springs; stretched, it squashes and necks
        L = self.L.x
        Rx = max(self.Rt.x, L + 40)
        self.draw_pill(s, W, seg, L, Rx)

        # the tabs: dark where the pill covers more than half of one
        for i, (name, _) in enumerate(self.tabs):
            a = PAD + i * seg
            dark = (min(a + seg, Rx) - max(a, L)) / seg > 0.5
            tex, isz = self.icon(i, dark)
            tw, th = text_size(self, name, 18, True)
            x0 = a + (seg - (20 + 8 + tw)) / 2
            s.append_texture(tex, rect(x0 - (isz - 20) / 2, (H - isz) / 2, isz, isz))
            text(s, self, x0 + 28, (H - th) / 2, name, 18, True,
                 INK if dark else "#FFFFFF", shadow=not dark)


# ---- the slider ----

class WaterSlider(Springy, Gtk.Widget):
    """the board's slider: a label and its value over a water channel, the
    level lit in the accent, and a droplet for a thumb (the water shader,
    refracting the channel). The thumb is a spring that lags the finger a
    little, and let go it glides on with the flick's momentum and settles,
    bouncing off the ends. Its skin is a membrane: the gap to the finger and
    its speed pull it into a teardrop toward where it's going (the far side
    flattens, the volume stays), and when it stops it wobbles back; it swells
    while held. While it moves a bubble shows the value. Left/Right (Up/Down)
    step 5. Emits "value-changed" (0..100) as the value it shows changes."""

    __gsignals__ = {"value-changed": (GObject.SignalFlags.RUN_FIRST, None, (float,))}
    LABEL = 29              # the label row (8 px padding, 17 px text)
    AREA = 48               # the track area below it
    THUMB = 17.0
    # the pull on the thumb's skin: per thumb radius of gap to the finger, and
    # per thumb radius a second of speed; at most PULL_MAX of its radius; how
    # narrow the tip (rad); and the stretch along the pull (the membrane's
    # slosh force, per unit of pull)
    PULL_GAP, PULL_SPEED, PULL_MAX = 0.55, 0.010, 0.75
    PULL_TIP = 0.7
    PULL_STRETCH = 14.0

    def __init__(self, label, value=50.0, accent=ACCENT, lo=0.0, hi=100.0, step=1.0,
                 fmt=None):
        super().__init__()
        self.label = label
        self.accent = accent
        # what it sets: lo..hi in steps, shown by fmt (a percentage); the
        # thumb itself moves in percent of the channel
        self.lo, self.hi, self.step_by = lo, hi, step
        self.fmt = fmt or (lambda v: "%d %%" % v)
        self.th = Spring(self.to_pct(value), 170, 19, 0.02)
        self.grab = Spring(0, 320, 12, 0.001)
        self.skin = Membrane(clamp=(-0.45, 0.8))      # pulled further than a button
        self.sd = None
        self.quiet = False          # moving to a value set from code: no "value-changed"
        self.shown = self.real()
        self.backdrop = None        # (texture, window): what the thumb refracts, behind the panel
        self.up = (0.0, 0.0)
        self.set_focusable(True)
        self.set_overflow(Gtk.Overflow.VISIBLE)
        drag = Gtk.GestureDrag()
        drag.connect("drag-begin", self.on_begin)
        drag.connect("drag-update", self.on_update)
        drag.connect("drag-end", self.on_end)
        self.add_controller(drag)
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        adopt_on_realize(self)

    def set_backdrop(self, texture, window):
        self.backdrop = (texture, window)

    def set_tilt(self, ux, uy):
        """tilted, the thumb leans downhill (see Droplet.set_tilt)"""
        self.up = (ux, uy)
        if self.skin.set_lean(-ux, -uy):
            self.kick()
        self.queue_draw()

    @property
    def value(self):
        """where the thumb is, in percent of the channel"""
        return max(0.0, min(100.0, self.th.x))

    def to_pct(self, v):
        return max(0.0, min(100.0, (v - self.lo) / ((self.hi - self.lo) or 1) * 100))

    def real(self):
        """the value it sets (lo..hi, in steps)"""
        v = self.lo + self.value / 100 * (self.hi - self.lo)
        if self.step_by:
            v = round(v / self.step_by) * self.step_by
        return max(min(self.lo, self.hi), min(max(self.lo, self.hi), v))

    def do_measure(self, orientation, for_size):
        if orientation == Gtk.Orientation.HORIZONTAL:
            return 160, 300, -1, -1
        return self.LABEL + self.AREA + 2, self.LABEL + self.AREA + 2, -1, -1

    def springs(self):
        return (self.th, self.grab)

    def held(self):
        return self.sd is not None

    def step(self, dt):
        th = self.th
        if th.x < 0:                    # bounce off the ends
            th.x, th.v, th.t = 0.0, -th.v * 0.35, max(th.t, 0.0)
        if th.x > 100:
            th.x, th.v, th.t = 100.0, -th.v * 0.35, min(th.t, 100.0)
        v = self.real()
        if v != self.shown:
            self.shown = v
            if not self.quiet:
                self.emit("value-changed", float(v))
        # the pull: toward the finger it lags, and the way it's going
        W, r = max(1, self.get_width()), self.THUMB
        gap = (self.sd["lv"] - th.x) / 100 * W if self.sd is not None else 0.0
        pull = gap / r * self.PULL_GAP + th.v / 100 * W / r * self.PULL_SPEED
        amount = min(self.PULL_MAX, abs(pull))
        # a bulge (a negative dent) toward it, the rest flattening: a teardrop;
        # and stretched along the pull
        self.skin.poke = (0.0 if pull > 0 else math.pi, -amount, self.PULL_TIP, 0.0) \
            if amount > 0.015 else None
        f = max(-12.0, min(12.0, pull * self.PULL_STRETCH)) if amount > 0.015 else 0.0
        self.skin.ext = (f, 0.0, 0.0)
        return self.skin.step(dt)

    def x_value(self, x):
        return x / max(1, self.get_width()) * 100

    def on_begin(self, g, x, y):
        if y < self.LABEL - 6:
            g.set_state(Gtk.EventSequenceState.DENIED)
            return
        self.grab_focus()
        v = self.x_value(x)
        self.quiet = False
        self.sd = {"x0": x, "lv": v, "lt": GLib.get_monotonic_time(), "vel": 0.0}
        self.th.k, self.th.c = 240, 24          # it lags the finger: the pull shows
        self.th.t = max(0.0, min(100.0, v))
        self.grab.t = 1
        self.kick()

    def on_update(self, g, dx, dy):
        sd = self.sd
        if not sd:
            return
        v, now = self.x_value(sd["x0"] + dx), GLib.get_monotonic_time()
        sd["vel"] = 0.6 * sd["vel"] + 0.4 * (v - sd["lv"]) / max(4e-3, (now - sd["lt"]) / 1e6)
        sd["lv"], sd["lt"] = v, now
        self.th.t = max(0.0, min(100.0, v))
        self.kick()

    def on_end(self, g, dx, dy):
        sd, self.sd = self.sd, None
        if sd:
            self.fling(sd["vel"])

    def fling(self, vel):
        """let go at vel (%/s): glide where its momentum would stop, settle"""
        th = self.th
        th.k, th.c = 170, 19
        th.v = vel
        th.t = max(0.0, min(100.0, th.x + vel * 0.12))
        self.grab.t = 0
        self.grab.v -= min(6, abs(vel) / 60)            # the release jolts it
        self.kick()

    def set_value(self, v, notify=True, glide=True):
        """glide to v (lo..hi; glide False: jump); notify False: a value from
        the app (what the slider controls changed elsewhere), so no
        "value-changed" on the way and no bubble"""
        self.go(self.to_pct(v), notify, glide)

    def go(self, pct, notify=True, glide=True):
        self.quiet = not notify
        self.th.k, self.th.c = 170, 19
        self.th.t = max(0.0, min(100.0, pct))
        if not glide:
            self.th.snap()
            self.shown = self.real()
            self.queue_draw()
        self.kick()

    def set_label(self, label):
        if label != self.label:
            self.label = label
            self.queue_draw()

    def on_key(self, ctl, keyval, code, state):
        name = Gdk.keyval_name(keyval) or ""
        step = {"Right": 5, "Up": 5, "Left": -5, "Down": -5}.get(name)
        if step:
            self.go(round(self.th.t) + step)
            return True
        return False

    def draw_channel(self, s, W, ty, lean=False):
        """the channel and its lit level; lean: without the shadows and the
        glow (what the thumb refracts: a 34 px lens can't show them, and
        they'd be blurred again in its offscreen pass every frame)"""
        pct = self.value / 100
        track = rrect(0, ty, W, 10, 5)
        if not lean:
            s.append_outset_shadow(track, rgba("#FFFFFF", 0.25), 0, 1, 0, 0)
        s.push_rounded_clip(track)
        s.append_color(rgba("#000000", 0.22), rect(0, ty, W, 10))
        s.pop()
        if not lean:
            s.append_inset_shadow(track, rgba("#000000", 0.35), 0, 1, 0, 3)
        if pct > 0:
            fill = rrect(0, ty, max(10, W * pct), 10, 5)
            if not lean:
                glow(s, fill, self.accent)
            s.push_rounded_clip(fill)
            s.append_linear_gradient(rect(0, ty, W, 10), point(0, ty), point(0, ty + 10),
                                     stops((0, rgba("#FFFFFF")), (1, rgba(self.accent))))
            s.pop()

    def do_snapshot(self, s):
        W = self.get_width()
        text(s, self, 0, 8, self.label, 17, True)
        text(s, self, W, 8, self.fmt(self.real()), 17, True, alpha=0.92, align=1.0)
        top = self.LABEL
        ty = top + 19
        self.draw_channel(s, W, ty)
        # the thumb: a droplet, its skin pulled by the finger and its speed,
        # swollen while held. The distortion is the drop's own shape, never a
        # transform: under a scale GTK renders the shader node offscreen first,
        # every frame (half the frame rate)
        r = self.THUMB
        lx, ly = lean_shift(self.up, r)
        cx, cy = W * self.value / 100 + lx, top + 24 + ly
        skin = self.skin.o + 0.12 * self.grab.x
        m = 14 + r * 0.8
        bounds = rect(cx - r - m, cy - r - m, 2 * (r + m), 2 * (r + m) + 6)

        def behind(s):
            # what's under the bead: the wallpaper (if the window gave it),
            # the panel's glass, the channel, all within the bead's node
            s.push_clip(bounds)
            if self.backdrop:
                tex, win = self.backdrop
                ok, pt = self.compute_point(win, point(0, 0))
                if ok:
                    s.append_texture(tex, rect(-pt.x, -pt.y, win.get_width(), win.get_height()))
                    s.append_color(rgba("#FFFFFF", 0.12), bounds)
            self.draw_channel(s, W, ty, lean=True)
            s.pop()
        if not push_drop(s, self, bounds, cx, cy, r, skin, self.up, behind):
            bead = rrect(cx - r, cy - r, 2 * r, 2 * r, r)
            s.append_outset_shadow(bead, rgba("#000000", 0.3), 0, 6, 0, 12)
            s.push_rounded_clip(bead)
            s.append_color(rgba("#FFFFFF", 0.35), rect(cx - r, cy - r, 2 * r, 2 * r))
            s.pop()
        # while it moves, the value rides above in a bubble
        if self.sd is not None or not (self.th.resting() or self.quiet):
            bw, bh = 52, 36
            bx, by = cx - bw / 2, top - 34
            bub = rrect(bx, by, bw, bh, 18)
            s.append_outset_shadow(bub, rgba("#000000", 0.3), 0, 6, 0, 14)
            s.push_rounded_clip(bub)
            s.append_radial_gradient(rect(bx, by, bw, bh), point(bx + 0.3 * bw, by + 0.25 * bh),
                                     bw, bw, 0, 1,
                                     stops((0, rgba("#FFFFFF")), (0.4, rgba("#FFFFFF", 0.9)),
                                           (1, rgba("#E6ECF8", 0.85))))
            s.pop()
            num = "%g" % round(self.real(), 2)
            tw, th = text_size(self, num, 15, True)
            text(s, self, bx + (bw - tw) / 2, by + (bh - th) / 2, num, 15, True, INK,
                 shadow=False)


# ---- the radio group ----

class _Part(Gtk.Widget):
    """a child that draws a part of its parent (draw(s) on the parent), so it
    can be redrawn on its own"""

    def __init__(self, draw):
        super().__init__()
        self.draw = draw
        self.set_can_target(False)
        self.set_overflow(Gtk.Overflow.VISIBLE)

    def do_snapshot(self, s):
        self.draw(s)


class WaterRadio(Springy, Gtk.Widget):
    """the board's radio group, in the TitanWater glass: a heading, then a row
    per option (a ring, the name, a note). Each ring is a clear droplet (the
    water shader: the wallpaper, set_backdrop, showing through, bent in its
    outline) with a membrane: pressed, it's pushed down, dimples and
    spreads, and ripples back. The choice is a smaller droplet of the accent,
    see-through, that travels between the rings on a spring, its skin pulled
    out along its path, and wobbles round when it lands. Up/Down move the
    choice. Emits "changed" (the index).

    The rows and the droplet are separate child widgets: while the droplet
    travels only it is redrawn (GTK keeps the rows' render)."""

    __gsignals__ = {"changed": (GObject.SignalFlags.RUN_FIRST, None, (int,))}
    HEAD, ROW, RING = 22, 52, 30
    RING_TINT = 0.18            # the rings: clearer than the rest, the rows stay light
    DOT, DOT_TINT = 10.0, 0.85  # the travelling droplet: radius, the accent's opacity
    # its pull: per row a second of speed, at most PULL_MAX of its radius, and
    # the stretch along it (the membrane's slosh force per unit of pull)
    PULL_SPEED, PULL_MAX, PULL_STRETCH = 0.09, 0.6, 14.0

    def __init__(self, heading, options, selected=0, accent=ACCENT):
        super().__init__()
        self.heading = heading
        self.options = options              # [(name, note), ...]
        self.selected = selected
        self.accent = accent
        self.blob = Spring(selected, 200, 19, 0.001)
        self.dot_skin = Membrane(clamp=(-0.45, 0.7))
        self.ring_skins = [Membrane() for _ in options]
        self.up = (0.0, 0.0)                # the screen's "up" (Slosh)
        self.pressed = None
        self.backdrop = None                # (texture, window): what shows through the rings
        self.rows = _Part(self.draw_rows)
        self.dot = _Part(self.draw_dot)
        self.rows.set_parent(self)
        self.dot.set_parent(self)
        self.set_focusable(True)
        click = Gtk.GestureClick()
        click.connect("pressed", self.on_pressed)
        click.connect("released", self.on_released)
        click.connect("stopped", self.on_stopped)
        self.add_controller(click)
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        adopt_on_realize(self)

    def do_dispose(self):
        for c in (self.rows, self.dot):
            if c.get_parent():
                c.unparent()

    def do_measure(self, orientation, for_size):
        if orientation == Gtk.Orientation.HORIZONTAL:
            return 200, 400, -1, -1
        h = self.HEAD + self.ROW * len(self.options)
        return h, h, -1, -1

    def do_size_allocate(self, width, height, baseline):
        self.rows.allocate(width, height, baseline, None)
        self.dot.allocate(width, height, baseline, None)

    def do_snapshot(self, s):
        self.snapshot_child(self.rows, s)
        self.snapshot_child(self.dot, s)

    def set_backdrop(self, texture, window):
        self.backdrop = (texture, window)
        self.rows.queue_draw()

    def set_tilt(self, ux, uy):
        """tilted, the rings and the choice lean downhill (see
        Droplet.set_tilt)"""
        self.up = (ux, uy)
        moved = self.dot_skin.set_lean(-ux, -uy)
        for m in self.ring_skins:
            moved = m.set_lean(-ux, -uy) or moved
        if moved:
            self.rings_moving = True
            self.kick()
        self.rows.queue_draw()
        self.dot.queue_draw()

    def springs(self):
        return [self.blob]

    def step(self, dt):
        # the travelling droplet: pulled out along its path by its speed
        v = self.blob.v
        pull = v * self.PULL_SPEED
        amount = min(self.PULL_MAX, abs(pull))
        self.dot_skin.poke = (math.pi / 2 if pull > 0 else -math.pi / 2, -amount, 0.7, 0.0) \
            if amount > 0.015 else None
        f = max(-12.0, min(12.0, pull * self.PULL_STRETCH)) if amount > 0.015 else 0.0
        self.dot_skin.ext = (0.0, f, 0.0)
        moving = self.dot_skin.step(dt)
        self.rings_moving = False
        for m in self.ring_skins:
            if m.poke is not None or m.step(dt):
                self.rings_moving = True
        return moving or self.rings_moving

    def redraw(self):
        self.dot.queue_draw()
        if getattr(self, "rings_moving", False):
            self.rows.queue_draw()

    def row_at(self, y):
        i = int((y - self.HEAD) // self.ROW)
        return i if 0 <= i < len(self.options) else None

    def select(self, i):
        i = max(0, min(len(self.options) - 1, i))
        self.blob.t = i
        if i != self.selected:
            self.selected = i
            self.emit("changed", i)
        self.kick()

    def on_pressed(self, g, n, x, y):
        self.grab_focus()
        i = self.row_at(y)
        self.pressed = i
        if i is not None:
            self.ring_skins[i].poke = (0.0, 0.0, 0.5, 0.14)     # pushed down in the middle
            self.kick()

    def on_released(self, g, n, x, y):
        i = self.row_at(y)
        self.on_stopped(g)
        if i is not None:
            self.select(i)

    def on_stopped(self, g):
        if self.pressed is not None:
            self.ring_skins[self.pressed].poke = None          # and it ripples back
            self.pressed = None
            self.kick()

    def on_key(self, ctl, keyval, code, state):
        name = Gdk.keyval_name(keyval) or ""
        if name in ("Up", "Down"):
            self.select(self.selected + (1 if name == "Down" else -1))
            return True
        return False

    def draw_rows(self, s):
        W = self.get_width()
        text(s, self.rows, 0, 2, self.heading.upper(), 14, True, alpha=0.92)
        R2 = self.RING / 2
        for i, (name, note) in enumerate(self.options):
            cy = self.HEAD + i * self.ROW + self.ROW / 2
            m = 12
            bounds = rect(R2 - R2 - m, cy - R2 - m, self.RING + 2 * m, self.RING + 2 * m + 4)
            lx, ly = lean_shift(self.up, R2)

            def behind(s, bounds=bounds):
                # the wallpaper and the panel's glass, within the ring's node
                if self.backdrop:
                    tex, win = self.backdrop
                    ok, pt = self.rows.compute_point(win, point(0, 0))
                    if ok:
                        s.push_clip(bounds)
                        s.append_texture(tex, rect(-pt.x, -pt.y, win.get_width(),
                                                   win.get_height()))
                        s.append_color(rgba("#FFFFFF", 0.12), bounds)
                        s.pop()
            if not push_drop(s, self.rows, bounds, R2 + lx, cy + ly, R2, self.ring_skins[i].o,
                             self.up, behind, tint=self.RING_TINT, accent="#FFFFFF"):
                ring = rrect(0, cy - R2, self.RING, self.RING, R2)
                s.append_border(ring, [1.5] * 4, [rgba("#FFFFFF", 0.75)] * 4)
            nw, nh = text_size(self.rows, name, 18, True)
            text(s, self.rows, self.RING + 14, cy - nh / 2, name, 18, True)
            tw, th = text_size(self.rows, note, 15)
            text(s, self.rows, W, cy - th / 2, note, 15, alpha=0.88, align=1.0)

    def draw_dot(self, s):
        # the droplet that travels between the rings: the accent, see-through
        # (no backdrop given: the ring under it shows through by alpha), its
        # skin pulled out along its path
        R2 = self.RING / 2
        cy = self.HEAD + self.ROW / 2 + self.blob.x * self.ROW
        r = self.DOT
        m = 10 + r * 0.7
        bounds = rect(R2 - r - m, cy - r - m, 2 * (r + m), 2 * (r + m) + 4)
        lx, ly = lean_shift(self.up, R2)       # with its ring
        if not push_drop(s, self.dot, bounds, R2 + lx, cy + ly, r, self.dot_skin.o, self.up,
                         lambda s: None, tint=self.DOT_TINT, accent=self.accent, dyed=True):
            dot = rrect(R2 - r, cy - r, 2 * r, 2 * r, r)
            s.push_rounded_clip(dot)
            s.append_color(rgba(self.accent, 0.8), rect(R2 - r, cy - r, 2 * r, 2 * r))
            s.pop()


# ---- the checkbox ----

LIQUID = """
/* (no comments on the uniform lines: GTK's parser then skips them)
   u_box: the box's width, height (px), the liquid's top (px from the box's
     top), the wave's height (the board's scaleY)
   u_wave: the wave's phase (px), the accent (r, g, b)
   u_check: how much of the check mark is drawn, 0..1 */
uniform vec4 u_box;
uniform vec4 u_wave;
uniform vec4 u_check;

const float PI2 = 6.2831853;

float segment(vec2 p, vec2 a, vec2 b)
{
  vec2 pa = p - a, ba = b - a;
  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord, in vec2 resolution, in vec2 uv)
{
  vec2 p = fragCoord;
  float top = u_box.z;
  /* the board's wave: an SVG of period 30 px, 2.5 px either side of a line
     5 px under its top, laid 8 px above the liquid and scaled from its
     bottom; it slides 30 px every 700 ms */
  float wy = top + 2.0 - (5.0 + 2.5 * sin(PI2 * (p.x + u_wave.x) / 30.0)) * u_box.w;
  vec4 col = vec4(0.0);
  float liquid = clamp(p.y - wy + 0.5, 0.0, 1.0);
  if (liquid > 0.0) {
    /* white at the surface, the accent at the bottom */
    float t = clamp((p.y - top) / max(u_box.y - top, 1.0), 0.0, 1.0);
    vec3 c = p.y < top ? vec3(1.0) : mix(vec3(1.0), u_wave.yzw, t);
    col = vec4(c, 1.0) * liquid;
  }
  /* the check mark, drawn as the liquid rises: M8 15.5 L13 20.5 L22.5 10 in
     the board's 30 px box, 3.2 px round strokes, along its length */
  float l1 = length(vec2(5.0, 5.0)), l2 = length(vec2(9.5, -10.5));
  float drawn = u_check.x * (l1 + l2);
  vec2 q = p * (30.0 / u_box.x) + vec2(1.5);
  vec2 a = vec2(8.0, 15.5), b = vec2(13.0, 20.5), c2 = vec2(22.5, 10.0);
  float d = 1e3;
  if (drawn > 0.0)
    d = segment(q, a, a + (b - a) * min(1.0, drawn / l1));
  if (drawn > l1)
    d = min(d, segment(q, b, b + (c2 - b) * min(1.0, (drawn - l1) / l2)));
  float ink = clamp(1.6 - d + 0.5, 0.0, 1.0);
  col = mix(col, vec4(0.078, 0.094, 0.129, 1.0), ink);
  fragColor = col;
}
"""

_liquid = None


def liquid_shader():
    global _liquid
    if _liquid is None:
        _liquid = Gsk.GLShader.new_from_bytes(GLib.Bytes.new(LIQUID.encode()))
    return _liquid


class WaterCheck(Springy, Gtk.Widget):
    """the board's checkbox: a glass box that fills with lit liquid when
    checked (the level is a spring, overshooting a little), a wave on its
    surface that grows with how fast it moves, and the check mark drawn as
    the liquid rises; then the label and a hint. Click or Space toggles.
    Emits "toggled" (the new state).

    The wave slides while the level moves and for WAVE_LINGER s after, then
    stands still (the board's slides forever; that would redraw the window
    every frame for as long as a checkbox is on screen)."""

    __gsignals__ = {"toggled": (GObject.SignalFlags.RUN_FIRST, None, (bool,))}
    BOX, H = 30, 54
    WAVE_LINGER = 1.5

    def __init__(self, label, hint="", active=False, accent=ACCENT):
        super().__init__()
        self.label, self.hint = label, hint
        self.active = active
        self.accent = accent
        self.fill = Spring(1.0 if active else 0.0, 150, 11, 0.001)
        self.phase = 0.0
        self.waving = 0.0               # s of wave left after the level settles
        self.set_focusable(True)
        click = Gtk.GestureClick()
        click.connect("released", lambda g, n, x, y: self.toggle())
        self.add_controller(click)
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        # greyed out while it can't be changed
        self.connect("notify::sensitive",
                     lambda w, p: w.set_opacity(1.0 if w.get_sensitive() else 0.5))

    def do_measure(self, orientation, for_size):
        if orientation == Gtk.Orientation.HORIZONTAL:
            return 200, 400, -1, -1
        return self.H, self.H, -1, -1

    def set_active(self, on):
        """set it from the app (no "toggled"): the liquid rises or drains"""
        on = bool(on)
        if on != self.active:
            self.active = on
            self.fill.t = 1.0 if on else 0.0
            self.kick()

    def get_active(self):
        return self.active

    def springs(self):
        return (self.fill,)

    def step(self, dt):
        self.phase = (self.phase + dt * 30 / 0.7) % 30
        if not self.fill.resting():
            self.waving = self.WAVE_LINGER
        else:
            self.waving = max(0.0, self.waving - dt)
        return self.waving > 0

    def toggle(self):
        self.grab_focus()
        self.active = not self.active
        self.fill.t = 1.0 if self.active else 0.0
        self.emit("toggled", self.active)
        self.kick()

    def on_key(self, ctl, keyval, code, state):
        if (Gdk.keyval_name(keyval) or "") in ("space", "Return"):
            self.toggle()
            return True
        return False

    def do_snapshot(self, s):
        W, B = self.get_width(), self.BOX
        bx, by = 1.5, (self.H - B) / 2
        box = rrect(bx, by, B, B, 10)
        f = self.fill
        level = max(0.0, min(1.15, f.x))
        drawn = max(0.0, min(1.0, (f.x - 0.35) / 0.5))
        s.append_outset_shadow(box, rgba("#000000", 0.2), 0, 3, 0, 6)
        s.push_rounded_clip(box)
        s.append_color(rgba("#FFFFFF", 0.10), rect(bx, by, B, B))
        sh = liquid_shader()
        ok = False
        try:
            ok = sh.compile(self.get_native().get_renderer())
        except GLib.Error:
            ok = False
        if ok:
            acc = rgba(self.accent)
            a = np.array([B, B, B * (1 - level), min(1.4, 0.25 + abs(f.v) * 0.35),
                          self.phase, acc.red, acc.green, acc.blue,
                          drawn, 0, 0, 0], np.float32)
            s.push_gl_shader(sh, rect(bx, by, B, B), GLib.Bytes.new(a.tobytes()))
            s.pop()
        else:
            s.append_color(rgba(self.accent), rect(bx, by + B * (1 - level), B, B * level))
        s.pop()
        s.append_inset_shadow(box, rgba("#000000", 0.25), 0, 2, 0, 4)
        s.append_border(box, [1.5] * 4, [rgba("#FFFFFF", 0.75)] * 4)
        x = B + 3 + 14
        lw, lh = text_size(self, self.label, 18, True)
        if self.hint:
            hw, hh = text_size(self, self.hint, 14)
            y = (self.H - lh - hh) / 2
            text(s, self, x, y, self.label, 18, True)
            text(s, self, x, y + lh, self.hint, 14, alpha=0.88)
        else:
            text(s, self, x, (self.H - lh) / 2, self.label, 18, True)


# ---- the scroll bar ----

class WaterScrollBar(Springy, Gtk.Widget):
    """the board's scroll bar, grippable, in the TitanWater glass: an upright
    vial (the VIAL shader) on the right edge, the page showing through its
    light tint, its outline catching the light. It follows the scroll on a
    spring, squashes when the scroll hits an end (the faster, the more),
    shows while scrolling and fades 0.9 s after. Grip it (water_scroll's
    gesture) and it widens and drags the page, held in view.

    Not a target for input itself: the gesture is the scroller's, in the
    capture phase, so a press elsewhere on the edge still reaches the page."""

    W_REST, W_HELD = 8.0, 14.0          # px; the widget is wide enough for both and the shadow
    WIDTH = 24

    def __init__(self, adjustment, accent=ACCENT, tint=0.35):
        super().__init__()
        self.adj = adjustment
        self.accent, self.tint = accent, tint
        self.top = Spring(6, 380, 32, 0.05)
        self.sq = Spring(0, 300, 14, 0.001)
        self.op = Spring(0, 90, 19, 0.002)
        self.wide = Spring(self.W_REST, 400, 26, 0.02)
        self.h = 80.0
        self.end = 0
        self.origin = "top"
        self.fade = 0
        self.gripped = None             # (the y the grip started, the value then)
        self.set_can_target(False)
        self.set_halign(Gtk.Align.END)
        self.set_valign(Gtk.Align.FILL)
        adjustment.connect("value-changed", self.on_scroll)
        adjustment.connect("changed", self.on_scroll)

    def do_measure(self, orientation, for_size):
        if orientation == Gtk.Orientation.HORIZONTAL:
            return self.WIDTH, self.WIDTH, -1, -1
        return 0, 0, -1, -1

    def springs(self):
        return (self.top, self.sq, self.op, self.wide)

    def held(self):                     # Springy: keep running while gripped
        return self.gripped is not None

    def bar_rect(self):
        """the bar, in the widget: x (right-aligned), y, w, h"""
        w = self.wide.x
        return self.WIDTH - 4 - w, self.top.x, w, self.h

    def on_scroll(self, adj):
        page, upper, value = adj.get_page_size(), adj.get_upper(), adj.get_value()
        track = self.get_height() - 12
        if track <= 0 or upper <= 0:
            return
        mx = upper - page
        self.h = max(36.0, track * page / upper)
        self.top.t = 6 + ((track - self.h) * value / mx if mx > 0 else 0)
        end = -1 if value <= 0 else 1 if value >= mx - 1 else 0
        if end and end != self.end:
            self.sq.v -= min(5, abs(self.top.v) / 120 + 1)     # it hits the end
            self.origin = "top" if end < 0 else "bottom"
        self.end = end
        self.op.t = 1.0 if mx > 0 else 0.0
        if self.fade:
            GLib.source_remove(self.fade)
        if self.gripped is None:
            self.fade = GLib.timeout_add(900, self.on_fade)
        self.kick()

    def on_fade(self):
        self.fade = 0
        if self.gripped is None:
            self.op.t = 0.0
            self.kick()
        return False

    # the grip, from water_scroll's gesture (coordinates in this widget)
    def grip(self, x, y):
        """a press at (x, y): True if it's on the bar, which then shows,
        widens and follows the finger. Faded out it still grips where it
        sits: an idle bar is invisible, and must still be grippable"""
        if self.adj.get_upper() <= self.adj.get_page_size():
            return False
        bx, by, bw, bh = self.bar_rect()
        if not (bx - 8 <= x <= self.WIDTH and by - 8 <= y <= by + bh + 8):
            return False
        self.gripped = (y, self.adj.get_value())
        self.wide.t = self.W_HELD
        self.op.t = 1.0
        if self.fade:
            GLib.source_remove(self.fade)
            self.fade = 0
        self.kick()
        return True

    def drag(self, dy):
        y0, v0 = self.gripped
        adj = self.adj
        track = self.get_height() - 12 - self.h
        mx = adj.get_upper() - adj.get_page_size()
        if track > 0:
            adj.set_value(max(0.0, min(mx, v0 + dy * mx / track)))

    def release(self):
        self.gripped = None
        self.wide.t = self.W_REST
        self.fade = GLib.timeout_add(900, self.on_fade)
        self.kick()

    def do_snapshot(self, s):
        op = max(0.0, min(1.0, self.op.x))
        if op <= 0.01:
            return
        bx, top, w, h = self.bar_rect()
        k = max(0.6, 1 + self.sq.x * 0.25)          # squashed from the end it hit
        if self.origin == "bottom":
            top = top + h - h * k
        h = h * k
        H = self.get_height()
        sh = vial_shader()
        try:
            ok = sh.compile(self.get_native().get_renderer())
        except GLib.Error:
            ok = False
        s.push_opacity(op)
        if ok:
            # an upright vial: "left and right" are its top and bottom, its
            # "middle line" x the bar's centre, its "height" the bar's width;
            # full and still (no liquid surface); nothing behind given, so the
            # page shows through by alpha
            acc = rgba(self.accent)
            a = np.array([top, top + h, bx + w / 2, w,
                          0.0, acc.red, acc.green, acc.blue,
                          1.0, 0.0, 0.0, 0.0,
                          self.tint, min(9.0, w / 2), min(7.0, w * 0.4), 1.0], np.float32)
            s.push_gl_shader(sh, rect(0, 0, self.WIDTH, H), GLib.Bytes.new(a.tobytes()))
            s.gl_shader_pop_texture()
            s.pop()
        else:
            bar = rrect(bx, top, w, h, w / 2)
            s.push_rounded_clip(bar)
            s.append_color(rgba("#FFFFFF", 0.6), rect(bx, top, w, h))
            s.pop()
        s.pop()



def water_scroll(child, accent=ACCENT):
    """child in a kinetic scroller with the water scroll bar (on the right
    edge) instead of GTK's. The bar can be gripped: a press on it (a capture
    gesture on the scroller, claimed only there) drags the page."""
    sw = Gtk.ScrolledWindow()
    sw.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
    sw.set_kinetic_scrolling(True)
    sw.add_css_class("water-scroll")
    sw.set_child(child)
    over = Gtk.Overlay()
    over.set_child(sw)
    bar = WaterScrollBar(sw.get_vadjustment(), accent)
    bar.set_margin_end(1)
    over.add_overlay(bar)
    grip = Gtk.GestureDrag()
    grip.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)

    def begin(g, x, y):
        ok, pt = over.compute_point(bar, point(x, y))
        if ok and bar.grip(pt.x, pt.y):
            g.set_state(Gtk.EventSequenceState.CLAIMED)
        else:
            g.set_state(Gtk.EventSequenceState.DENIED)

    def update(g, dx, dy):
        if bar.gripped is not None:
            bar.drag(dy)

    def end(g, dx, dy):
        if bar.gripped is not None:
            bar.release()
    grip.connect("drag-begin", begin)
    grip.connect("drag-update", update)
    grip.connect("drag-end", end)
    over.add_controller(grip)
    over.scroller, over.bar = sw, bar
    return over


# ---- the button ----

def push_vial(s, widget, bounds, left, right, cy, h, child, tint=0.35, rim=9.0, bend=7.0,
              fill=1.0, slope=0.0, phase=0.0, amp=0.0, neck=0.0, accent=None, upright=False):
    """draw a glass vial (the VIAL shader: a capsule from x left to right,
    its middle at y cy, h tall, in the widget's coordinates) holding clear
    liquid fill full, its surface sloped and waving (phase, amp px);
    child(s) draws what shows through it (the window's wallpaper: it bends in
    the outline, rim px wide, bend px at the edge; nothing drawn: see-through
    by alpha), under a tint of that opacity. False without the shader."""
    sh = vial_shader()
    try:
        ok = sh.compile(widget.get_native().get_renderer())
    except GLib.Error:
        ok = False
    if not ok:
        return False
    acc = rgba(accent or ACCENT)
    ox, oy = bounds.get_x(), bounds.get_y()
    a = np.array([left - ox, right - ox, cy - oy, h,
                  neck, acc.red, acc.green, acc.blue,
                  fill, slope, phase, amp,
                  tint, rim, bend, 1.0 if upright else 0.0], np.float32)
    s.push_gl_shader(sh, bounds, GLib.Bytes.new(a.tobytes()))
    child(s)
    s.gl_shader_pop_texture()
    s.pop()
    return True


# what the app frame's surfaces lay over the wallpaper (CSS class: colour,
# opacity), for the water in them to refract the same view
SURFACES = {"water-panel": ("#FFFFFF", 0.12), "titan-header": ("#FFFFFF", 0.15),
            "titan-bar": ("#FFFFFF", 0.15), "titan-scrim": ("#060A18", 0.55),
            "titan-dialog": ("#121830", 0.94), "viewer": ("#000000", 1.0),
            "bar": ("#0B0D12", 1.0)}


def glass_under(widget):
    """the surfaces between a widget and the wallpaper, from the wallpaper
    up: [(colour, opacity)] (drawn into what its water refracts, so the view
    through it matches what's around it)"""
    layers, w = [], widget.get_parent()
    while w is not None:
        for cls, layer in SURFACES.items():
            if w.has_css_class(cls):
                layers.append(layer)
        w = w.get_parent()
    return layers[::-1]


def snapshot_wallpaper(s, widget, bounds):
    """the window's wallpaper behind a widget, within bounds (widget px), as
    its water's child: what it refracts. Nothing without a backdrop."""
    bd = getattr(widget, "backdrop", None)
    if not bd:
        return
    tex, win = bd
    ok, pt = widget.compute_point(win, point(0, 0))
    if not ok:
        return
    s.push_clip(bounds)
    s.append_texture(tex, rect(-pt.x, -pt.y, win.get_width(), win.get_height()))
    layers = getattr(widget, "_glass", None)
    if layers is None:
        layers = widget._glass = glass_under(widget)
    for colour, alpha in layers:
        s.append_color(rgba(colour, alpha), bounds)
    s.pop()


class GlassButton(Springy, Gtk.Widget):
    """a button in the TitanWater glass: a capsule vial of clear liquid (the
    VIAL shader), the window's wallpaper showing through under a light tint
    and bending in its outline, a label and/or an icon on it. Pressed, the
    finger pushes the liquid down: the vial squashes a little, the surface
    shows and sloshes, and let go it fills back with a ripple. Leans its
    liquid with the phone's tilt while the surface shows.

    label, icon (a 24-unit stroke path, see ICONS in titan_ui4) or
    icon_name (a theme icon); height px (the design's 46 in a header, 52 in
    a page); kind "" (clear), "suggested" (the accent: the action to take),
    "destructive", or "lit" (bright water, dark text: a toggle that's on);
    toggle: it's a toggle button (active, "toggled"). Emits "clicked" on a
    tap that didn't slide off, Space or Return."""

    __gsignals__ = {"clicked": (GObject.SignalFlags.RUN_FIRST, None, ()),
                    "toggled": (GObject.SignalFlags.RUN_FIRST, None, (bool,))}
    PAD = 20
    GAP = 8
    TINTS = {"": 0.18, "suggested": 0.82, "destructive": 0.62, "lit": 0.86}
    COLOURS = {"suggested": "#FFD24A", "destructive": "#FF5A6E"}

    def __init__(self, label=None, icon=None, icon_name=None, height=52, kind="",
                 toggle=False, active=False, size=18):
        super().__init__()
        self.label, self.icon_d, self.icon_name = label, icon, icon_name
        self.height, self.kind, self.size = height, kind, size
        self.toggle, self.active = toggle, active
        self.sq = Spring(0, 420, 22, 0.002)         # the squash while held
        self.fill = Spring(1, 160, 9, 0.002)        # the liquid, pushed down
        self.wave = Spring(0, 60, 4, 0.05)          # the ripple's height, px
        self.phase = 0.0
        self.pressed = None
        self.backdrop = None
        self.up = (0.0, 0.0)
        self._icons = {}
        self.set_focusable(True)
        self.set_overflow(Gtk.Overflow.VISIBLE)
        drag = Gtk.GestureDrag()
        drag.connect("drag-begin", self.on_begin)
        drag.connect("drag-update", self.on_update)
        drag.connect("drag-end", self.on_end)
        self.add_controller(drag)
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)
        adopt_on_realize(self)

    # ---- the look's state ----

    def set_label(self, label):
        if label != self.label:
            self.label = label
            self.queue_resize()

    def set_kind(self, kind):
        if kind != self.kind:
            self.kind = kind
            self.queue_draw()

    def set_icon(self, icon):
        """a new icon (a 24-unit stroke path)"""
        if icon != self.icon_d:
            self.icon_d = icon
            self._icons = {}
            self.queue_draw()

    def set_active(self, on):
        on = bool(on)
        if on != self.active:
            self.active = on
            self.queue_draw()
            self.emit("toggled", on)

    def get_active(self):
        return self.active

    def set_backdrop(self, texture, window):
        self.backdrop = (texture, window)
        self.queue_draw()

    def set_tilt(self, ux, uy):
        self.up = (ux, uy)
        if not self.fill.resting():
            self.queue_draw()

    def look(self):
        kind = "lit" if self.toggle and self.active else self.kind
        return kind, self.TINTS.get(kind, 0.18), self.COLOURS.get(kind)

    # ---- size ----

    def content(self):
        """the icon's texture and size, the label's size, the content's width"""
        tex = isz = None
        if self.icon_d or self.icon_name:
            dark = self.look()[1] > 0.5
            tex = self._icons.get(dark)
            if tex is None and self.icon_d:
                tex = self._icons[dark] = icon_texture(self.icon_d, size=22, stroke=2.2,
                                                       color=INK if dark else "#FFFFFF",
                                                       shadow=not dark)
            isz = 22
        tw = th = 0
        if self.label:
            tw, th = text_size(self, self.label, self.size, True)
        w = (isz or 0) + tw + (self.GAP if isz and self.label else 0)
        return tex, isz, tw, th, w

    def do_measure(self, orientation, for_size):
        if orientation == Gtk.Orientation.HORIZONTAL:
            _, isz, tw, _, w = self.content()
            if not self.label:                      # an icon alone: round
                return self.height, self.height, -1, -1
            n = int(w + 2 * self.PAD)
            # squeezed, the label ends in an ellipsis
            return min(n, int(self.height * 1.6)), n, -1, -1
        return self.height, self.height, -1, -1

    # ---- input ----

    def on_begin(self, g, x, y):
        if not self.is_sensitive():
            g.set_state(Gtk.EventSequenceState.DENIED)
            return
        self.pressed = (x, y)
        self.sq.t = 1
        self.fill.t = 0.72
        self.wave.v += 30
        self.kick()

    def on_update(self, g, dx, dy):
        if self.pressed is None:
            return
        x, y = self.pressed[0] + dx, self.pressed[1] + dy
        if not (-16 <= x <= self.get_width() + 16 and -16 <= y <= self.get_height() + 16):
            self.pressed = None         # slid off: nothing happens
            self.let_go()

    def on_end(self, g, dx, dy):
        if self.pressed is not None:
            self.pressed = None
            self.let_go()
            self.activate_button()

    def let_go(self):
        self.sq.t = 0
        self.fill.t = 1
        self.wave.v += 60               # it fills back with a ripple
        self.kick()

    def activate_button(self):
        if self.toggle:
            self.set_active(not self.active)
        self.emit("clicked")

    def on_key(self, ctl, keyval, code, state):
        if (Gdk.keyval_name(keyval) or "") in ("space", "Return", "KP_Enter"):
            self.sq.v += 6
            self.wave.v += 60
            self.kick()
            self.activate_button()
            return True
        return False

    # ---- springs ----

    def springs(self):
        return (self.sq, self.fill, self.wave)

    def held(self):
        return self.pressed is not None

    def step(self, dt):
        self.phase += dt * 70           # the ripple runs along the surface
        return False

    # ---- drawing ----

    def do_snapshot(self, s):
        W, H = self.get_width(), self.get_height()
        kind, tint, colour = self.look()
        sq = self.sq.x
        h = (self.height - 2) * (1 - 0.10 * sq)
        w = W * (1 + 0.03 * sq)
        cx, cy = W / 2, H / 2
        lx, ly = lean_shift(self.up, h / 2)
        lx *= 0.4                       # a capsule leans less than a drop
        ly *= 0.4
        cx, cy = cx + lx, cy + ly
        bounds = rect(-12, -10, W + 24, H + 34)
        k = max(h / 46, 0.5)
        fill = min(1.0, self.fill.x)
        slope = -self.up[0] * 0.5 if fill < 0.995 else 0.0
        ok = push_vial(s, self, bounds, cx - w / 2, cx + w / 2, cy, h,
                       lambda s: snapshot_wallpaper(s, self, bounds),
                       tint=tint, rim=6.0 * k, bend=5.0 * k, fill=fill, slope=slope,
                       phase=self.phase, amp=abs(self.wave.x) * 0.6, accent=colour)
        if not ok:
            pill = rrect(cx - w / 2, cy - h / 2, w, h, h / 2)
            s.push_rounded_clip(pill)
            s.append_color(rgba(colour or "#FFFFFF", tint), rect(cx - w / 2, cy - h / 2, w, h))
            s.pop()
            s.append_border(pill, [1] * 4, [rgba("#FFFFFF", 0.5)] * 4)
        if self.has_visible_focus():
            ring = rrect(cx - w / 2 - 3, cy - h / 2 - 3, w + 6, h + 6, h / 2 + 3)
            s.append_border(ring, [2] * 4, [rgba("#FFFFFF", 0.6)] * 4)
        # the content floats on it, pressed down with it
        dark = tint > 0.5
        alpha = 1.0 if self.is_sensitive() else 0.45
        tex, isz, tw, th, cw = self.content()
        lay = None
        if self.label:
            room = W - 1.2 * self.PAD - (isz + self.GAP if isz else 0)
            if tw > room:                       # squeezed: the label ends in an ellipsis
                lay = fitted(self, self.label, self.size, True, room)
                cw = cw - tw + lay.size_px[0]
        x = cx - cw / 2
        s.save()
        s.translate(point(0, 1.5 * sq))
        if tex is not None:
            t, full = tex
            pad = (full - isz) / 2
            if alpha < 1:
                s.push_opacity(alpha)
            s.append_texture(t, rect(x - pad, cy - isz / 2 - pad, full, full))
            if alpha < 1:
                s.pop()
            x += isz + self.GAP
        elif isz:                       # a theme icon
            self.snapshot_theme_icon(s, x, cy - isz / 2, isz, dark, alpha)
            x += isz + self.GAP
        if lay is not None:
            col = INK if dark else "#FFFFFF"
            for dy, c, a in ((1, "#000000", 0.35 * alpha), (0, col, alpha)):
                if dy and dark:
                    continue            # no shadow on lit water
                s.save()
                s.translate(point(x, cy - th / 2 + dy))
                s.append_layout(lay, rgba(c, a))
                s.restore()
        elif self.label:
            text(s, self, x, cy - th / 2, self.label, self.size, True,
                 INK if dark else "#FFFFFF", alpha=alpha, shadow=not dark)
        s.restore()

    def snapshot_theme_icon(self, s, x, y, size, dark, alpha):
        key = ("theme", dark)
        icon = self._icons.get(key)
        if icon is None:
            theme = Gtk.IconTheme.get_for_display(self.get_display())
            icon = self._icons[key] = theme.lookup_icon(
                self.icon_name, None, size, self.get_scale_factor(),
                Gtk.TextDirection.NONE, 0)
        c = rgba(INK if dark else "#FFFFFF", alpha)
        s.save()
        s.translate(point(x, y))
        try:
            icon.snapshot_symbolic(s, size, size, [c, c, c, c])
        except (AttributeError, TypeError):
            icon.snapshot(s, size, size)
        s.restore()


# ---- the big cores ----

def use_big_cores():
    """run this process on the fastest cores (the A73s: cpu_capacity 1024 vs
    the A53s' 604). Left to the scheduler, GTK's main thread stays on the
    little cores, and a slider dragging its droplet thumb ran at 30 fps
    instead of 58: the frame just doesn't fit there. Every thread there is
    now (GLib's start when gi is imported) is moved, and threads made later
    (GTK's, Mesa's) inherit it. True if it pinned."""
    import glob
    import os
    caps = {}
    for path in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpu_capacity"):
        try:
            with open(path) as f:
                caps[int(path.split("/")[-2][3:])] = int(f.read())
        except (OSError, ValueError):
            pass
    if not caps:
        return False
    top = max(caps.values())
    big = {c for c, v in caps.items() if v == top}
    if len(big) == len(caps):           # all alike: nothing to choose
        return False
    try:
        os.sched_setaffinity(0, big)
    except OSError:
        return False
    for task in os.listdir("/proc/self/task"):
        try:
            os.sched_setaffinity(int(task), big)
        except (OSError, ValueError):
            pass
    return True


# ---- debugging: frame rate, CPU, and the window as PNGs ----

def debug(win, root, script=()):
    """TITAN_WATER_STATS=1: the frame rate and the CPU used, every 2 s, on
    stderr. TITAN_WATER_SHOT=/tmp/x: the window's scene, rendered through its
    own GL renderer, saved as /tmp/x-N.png at TITAN_WATER_SHOT_AT (seconds,
    comma-separated); Weston keeps screenshots to itself. script: (seconds,
    callable) pairs run from the start, to drive the controls for the shots."""
    import os
    import sys
    import time
    if os.environ.get("TITAN_WATER_STATS"):
        last = [time.monotonic(), time.process_time()]

        def stats():
            now, cpu = time.monotonic(), time.process_time()
            fc = win.get_frame_clock()
            print("fps %.1f  cpu %.0f%% of a core  shader %s" % (
                fc.get_fps() if fc else 0, 100 * (cpu - last[1]) / (now - last[0]),
                {True: "on GPU", False: "FAILED", None: "?"}[_compiled]),
                file=sys.stderr, flush=True)
            last[:] = [now, cpu]
            return True
        GLib.timeout_add(2000, stats)
    shot = os.environ.get("TITAN_WATER_SHOT")
    if shot:
        import ctypes
        gtk4 = ctypes.CDLL("libgtk-4.so.1")
        gtk4.gtk_snapshot_to_node.restype = ctypes.c_void_p
        gtk4.gtk_snapshot_to_node.argtypes = [ctypes.c_void_p]
        gtk4.gsk_renderer_render_texture.restype = ctypes.c_void_p
        gtk4.gsk_renderer_render_texture.argtypes = [ctypes.c_void_p] * 3
        gtk4.gdk_texture_save_to_png.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        gtk4.gsk_render_node_unref.argtypes = [ctypes.c_void_p]
        gtk4.g_object_unref.argtypes = [ctypes.c_void_p]
        cap = ctypes.pythonapi.PyCapsule_GetPointer
        cap.restype = ctypes.c_void_p
        cap.argtypes = [ctypes.py_object, ctypes.c_char_p]

        def snap(n):
            # PyGObject can't pass GTK 4.8's render nodes, so through C
            # lay out and snapshot the tree afresh: with the screen off the
            # frame clock stops, so after a change (a label's new text) the
            # window is neither laid out again nor drawn, and a
            # WidgetPaintable would reuse render nodes that are gone
            win.allocate(win.get_width(), win.get_height(), -1, None)
            w, h = root.get_width(), root.get_height()
            sn = Gtk.Snapshot()
            root.get_parent().snapshot_child(root, sn)
            node = gtk4.gtk_snapshot_to_node(cap(sn.__gpointer__, None))
            r = (ctypes.c_float * 4)(0, 0, w, h)
            if not node:                # nothing laid out yet
                print("TITAN_WATER_SHOT: nothing to draw (%d x %d)" % (w, h),
                      file=sys.stderr, flush=True)
                return False
            tex = gtk4.gsk_renderer_render_texture(
                cap(win.get_native().get_renderer().__gpointer__, None), node, r)
            if tex:                     # none while the screen is off
                gtk4.gdk_texture_save_to_png(tex, ("%s-%d.png" % (shot, n)).encode())
                gtk4.g_object_unref(tex)
            else:
                print("TITAN_WATER_SHOT: nothing rendered (the screen off?)",
                      file=sys.stderr, flush=True)
            gtk4.gsk_render_node_unref(node)
            return False
        at = os.environ.get("TITAN_WATER_SHOT_AT", "1.2,3.05,3.3,4.1,6")
        for n, t in enumerate(float(x) for x in at.split(",")):
            GLib.timeout_add(int(t * 1000), snap, n)
    for t, fn in script:
        GLib.timeout_add(int(t * 1000), lambda fn=fn: fn() and False)
