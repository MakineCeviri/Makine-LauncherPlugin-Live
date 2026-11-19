# -*- coding: utf-8 -*-
"""
Makine Live — test paneli bileşen kiti (TEST HARNESS).

Tk Canvas köşeleri ve eğrileri yumuşatmadığı için tüm yüzeyler (yuvarlak kutular, ikonlar,
grafik) Pillow ile 4× süper örneklenip PhotoImage olarak çizilir; metinler Tk'nın ClearType
metnidir. Renkler ve ölçüler: spec/design_tokens.json ("altyazı stüdyosu").

İkonlar 24'lük ızgarada, 1.75 çizgi, yuvarlak uç. Her ikonun tek, anlamlı bir hareketi var:
  play_stop  oynat ↔ durdur biçim dönüşümü      spinner   çeviri beklenirken dönen yay
  check      onay işareti çizilerek gelir        refresh   oyun yeniden algılanırken döner
  region     bölge seçerken köşeler nefes alır   lock      kilit halkası açılır/kapanır
  eye        göz kırpar (kaynak metni göster)    subtitle  altyazı satırları kayarak gelir
  copy       kopyalanınca öndeki kâğıt kayar     trash     kapak kalkar
  chevron    açılır listede döner                link      bağlantı halkası dalgalanır
  game       hover'da hafifçe sallanır           key       (durağan)
"""

from __future__ import annotations

import json
import math
import os
import time
import tkinter as tk
import tkinter.font as tkfont

from PIL import Image, ImageDraw, ImageTk

# ─────────────────────────────────────────────────────────────
# Tema
# ─────────────────────────────────────────────────────────────

_TOK_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spec", "design_tokens.json")
try:
    with open(_TOK_PATH, encoding="utf-8") as _f:
        _TOK = json.load(_f)
except (OSError, ValueError):
    _TOK = {}
_D = _TOK.get("color", {}).get("dark", {})
_S = _TOK.get("color", {}).get("status", {})

BG = _D.get("bg", "#0f1012")
SURFACE = _D.get("surface", "#16171a")
SURFACE2 = _D.get("surface2", "#1d1f23")
RAISED = _D.get("raised", "#24262b")
LINE = _D.get("line", "#2a2c31")
LINE_STRONG = _D.get("lineStrong", "#3a3d44")
TEXT = _D.get("text", "#ecebe6")
TEXT2 = _D.get("text2", "#a9a8a2")
TEXT3 = _D.get("text3", "#76767a")
ACCENT = _TOK.get("color", {}).get("accent", "#f2c230")
ACCENT_HOVER = "#f6d063"
ACCENT_PRESS = "#dcae1f"
ACCENT_INK = _TOK.get("color", {}).get("accentInk", "#16171a")
VERIFIED = _S.get("verified", "#3fae72")
DISPUTED = _S.get("disputed", "#e2674a")
PENDING = _S.get("pending", "#8a8f98")

EASE = lambda t: 1 - (1 - t) ** 3            # noqa: E731  ease-out cubic
EASE_IO = lambda t: 3 * t * t - 2 * t * t * t  # noqa: E731


def rgb(c: str) -> tuple:
    c = c.lstrip("#")
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def mix(a: str, b: str, t: float) -> str:
    ra, rb = rgb(a), rgb(b)
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(ra, rb))


# ─────────────────────────────────────────────────────────────
# Yazı tipleri
# ─────────────────────────────────────────────────────────────

class Fonts:
    def __init__(self, root):
        fam = set(tkfont.families(root))

        def pick(*names):
            for n in names:
                if n in fam:
                    return n
            return "Segoe UI"

        self.text = pick("Segoe UI Variable Text", "Segoe UI")
        self.text_semi = pick("Segoe UI Variable Text Semibold", "Segoe UI Semibold")
        self.display = pick("Segoe UI Variable Display Semib", "Segoe UI Semibold")
        self.mono = pick("Cascadia Mono", "Consolas")

    def ui(self, px=13, semi=False):
        return (self.text_semi if semi else self.text, -px)

    def disp(self, px=18):
        return (self.display, -px)

    def code(self, px=12):
        return (self.mono, -px)


# ─────────────────────────────────────────────────────────────
# Zamanlayıcı tabanlı animasyon
# ─────────────────────────────────────────────────────────────

class Tween:
    """after() ile ~60 fps ara değer. fn(e) her karede, done() sonda çağrılır."""

    def __init__(self, root, ms, fn, done=None, ease=EASE):
        self.root, self.ms, self.fn, self.done, self.ease = root, max(1, ms), fn, done, ease
        self.t0 = time.perf_counter()
        self.job = None
        self._step()

    def _step(self):
        t = min(1.0, (time.perf_counter() - self.t0) * 1000 / self.ms)
        self.fn(self.ease(t))
        if t < 1.0:
            self.job = self.root.after(16, self._step)
        else:
            self.job = None
            if self.done:
                self.done()

    def cancel(self):
        if self.job:
            self.root.after_cancel(self.job)
            self.job = None


# ─────────────────────────────────────────────────────────────
# Çizim yardımcıları (Pillow, 4× süper örnekleme)
# ─────────────────────────────────────────────────────────────

_SS = 4
_cache: dict = {}


def _photo(key, build):
    img = _cache.get(key)
    if img is None:
        img = ImageTk.PhotoImage(build())
        if len(_cache) > 3000:
            _cache.clear()
        _cache[key] = img
    return img


def rrect_image(w, h, r, fill=None, outline=None, width=1, dash=False):
    """Yuvarlak köşeli dikdörtgen (yumuşak kenarlı)."""
    def build():
        W, H = w * _SS, h * _SS
        im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        half = width * _SS / 2
        d.rounded_rectangle((half, half, W - half - 1, H - half - 1), radius=r * _SS,
                            fill=(rgb(fill) + (255,)) if fill else None,
                            outline=(rgb(outline) + (255,)) if outline else None,
                            width=round(width * _SS) if outline else 0)
        return im.resize((w, h), Image.LANCZOS)
    return _photo(("rr", w, h, r, fill, outline, width), build)


def circle_image(d, fill, outline=None, width=1):
    def build():
        D = d * _SS
        im = Image.new("RGBA", (D, D), (0, 0, 0, 0))
        dr = ImageDraw.Draw(im)
        half = width * _SS / 2 if outline else 0
        dr.ellipse((half, half, D - half - 1, D - half - 1), fill=rgb(fill) + (255,),
                   outline=(rgb(outline) + (255,)) if outline else None, width=round(width * _SS) if outline else 0)
        return im.resize((d, d), Image.LANCZOS)
    return _photo(("circ", d, fill, outline, width), build)


class _Pen:
    """24'lük ızgarada yuvarlak uçlu çizgi/yay çizen yardımcı."""

    def __init__(self, size, color, stroke=1.75):
        self.k = size * _SS / 24.0
        self.im = Image.new("RGBA", (size * _SS, size * _SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
        self.col = rgb(color) + (255,)
        self.w = stroke * self.k
        self.size = size

    def p(self, x, y):
        return (x * self.k, y * self.k)

    def dot(self, x, y, r=None, fill=None):
        r = (self.w / 2) if r is None else r * self.k
        cx, cy = self.p(x, y)
        self.d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill or self.col)

    def line(self, pts, closed=False):
        pts = [self.p(x, y) for x, y in pts]
        if closed:
            pts = pts + [pts[0]]
        self.d.line(pts, fill=self.col, width=max(1, round(self.w)), joint="curve")
        r = self.w / 2
        for x, y in (pts if closed else [pts[0], pts[-1]]):
            self.d.ellipse((x - r, y - r, x + r, y + r), fill=self.col)

    def arc(self, cx, cy, rad, a0, a1, steps=None):
        """Derece cinsinden a0→a1 (saat yönü, 0 = sağ)."""
        n = steps or max(6, int(abs(a1 - a0) / 6))
        pts = [(cx + rad * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                cy + rad * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
        self.line(pts)
        return pts

    def poly(self, pts, fill=True):
        pts = [self.p(x, y) for x, y in pts]
        self.d.polygon(pts, fill=self.col if fill else None)
        self.d.line(pts + [pts[0]], fill=self.col, width=max(1, round(self.w)), joint="curve")

    def rrect(self, x0, y0, x1, y1, r, fill=False):
        self.d.rounded_rectangle((*self.p(x0, y0), *self.p(x1, y1)), radius=r * self.k,
                                 outline=self.col, width=max(1, round(self.w)),
                                 fill=self.col if fill else None)

    def done(self):
        return self.im.resize((self.size, self.size), Image.LANCZOS)


def _lerp(a, b, t):
    return a + (b - a) * t


def _rot(pts, cx, cy, deg):
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * ca - (y - cy) * sa, cy + (x - cx) * sa + (y - cy) * ca) for x, y in pts]


def _partial(pts, t):
    """Çoklu çizginin baştan t oranındaki kısmı (çizilerek gelme efekti)."""
    if t >= 1:
        return pts
    seg = [math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    total = sum(seg) * max(0.0, t)
    out = [pts[0]]
    for i, L in enumerate(seg):
        if total <= 0:
            break
        if total >= L:
            out.append(pts[i + 1])
            total -= L
        else:
            f = total / L
            out.append((_lerp(pts[i][0], pts[i + 1][0], f), _lerp(pts[i][1], pts[i + 1][1], f)))
            total = 0
    return out


def draw_icon(name, size, color, t=0.0):
    """İkonu t (0..1) animasyon durumunda çizer ve PIL görüntüsü döndürür."""
    pen = _Pen(size, color)
    if name == "play_stop":           # t=0 oynat, t=1 durdur
        tri = [(8.5, 5.5), (18.5, 12), (18.5, 12), (8.5, 18.5)]
        sq = [(7, 7), (17, 7), (17, 17), (7, 17)]
        pen.poly([(_lerp(a[0], b[0], t), _lerp(a[1], b[1], t)) for a, b in zip(tri, sq)])
    elif name == "spinner":           # t = faz
        a0 = t * 360
        pen.arc(12, 12, 8, a0, a0 + 270)
    elif name == "check":             # t = çizilme oranı
        pts = _partial([(5.5, 12.5), (10, 17), (18.5, 7.5)], t)
        if len(pts) > 1:
            pen.line(pts)
    elif name == "refresh":           # t = dönüş (0..1 tam tur)
        rot = t * 360
        pts = pen_arc_pts(12, 12, 7, -60, 210)
        pts = _rot(pts, 12, 12, rot)
        pen.line(pts)
        ex, ey = pts[-1]
        head = _rot([(ex - 3.2, ey - 0.8), (ex, ey), (ex + 0.6, ey - 3.3)], ex, ey, rot + 15)
        pen.line(head)
    elif name == "region":            # t = nefes (0..1..0)
        g = 1.5 * math.sin(math.pi * t)
        a, b, L = 4 - g * 0.6, 20 + g * 0.6, 4.5
        for (x, y, dx, dy) in ((a, a, 1, 1), (b, a, -1, 1), (a, b, 1, -1), (b, b, -1, -1)):
            pen.line([(x + dx * L, y), (x, y), (x, y + dy * L)])
        pen.dot(12, 12, 1.4 + 0.9 * math.sin(math.pi * t))
    elif name == "lock":              # t=0 kilitli, t=1 açık
        pen.rrect(5.5, 11, 18.5, 20, 2.2)
        lift = 2.6 * t
        right_end = 11 - 3.6 * t
        pts = [(8.5, 11 - lift)] + pen_arc_pts(12, 7.5 - lift, 3.5, 180, 360) + [(15.5, right_end - lift)]
        pen.line(pts)
        pen.dot(12, 15.5, 1.2)
    elif name in ("eye", "eye_off"):  # t = kırpma (0 açık → 1 kapalı)
        o = max(0.08, 1 - t)
        top = [(x, 12 - 6.2 * o * (1 - ((x - 12) / 9) ** 2)) for x in [3 + i * 0.6 for i in range(31)]]
        bot = [(x, 12 + 6.2 * o * (1 - ((x - 12) / 9) ** 2)) for x in [3 + i * 0.6 for i in range(31)]]
        pen.line(top)
        pen.line(bot)
        if o > 0.35:
            pen.dot(12, 12, 2.6 * min(1, (o - 0.35) / 0.4), fill=pen.col)
        if name == "eye_off":
            pen.line([(4.5, 4.5), (19.5, 19.5)])
    elif name == "subtitle":          # t = altyazı satırının kayarak gelişi (0 gizli → 1 yerinde)
        pen.rrect(3, 4.5, 21, 17.5, 2.5)
        e = EASE(t) if t < 1 else 1.0
        x0 = _lerp(12, 7, e)
        x1 = _lerp(12, 17, e)
        pen.line([(x0, 13.5), (x1, 13.5)])
        pen.line([(9, 20.2), (15, 20.2)])
    elif name == "copy":              # t = öndeki kâğıdın kayması
        dx = 1.6 * math.sin(math.pi * t)
        pen.rrect(8.5 + dx, 8.5 - dx, 19.5 + dx, 19.5 - dx, 2)
        pen.line([(15.5, 5.5), (6.5, 5.5), (4.5, 7.5), (4.5, 15.5)])
    elif name == "trash":             # t = kapak kalkması
        lift = 2.2 * math.sin(math.pi * t)
        lid = _rot([(4.5, 7), (19.5, 7)], 19.5, 7, -14 * math.sin(math.pi * t))
        pen.line([(lid[0][0], lid[0][1] - lift), (lid[1][0], lid[1][1] - lift)])
        pen.line([(9.5, 7 - lift), (10, 4.8 - lift), (14, 4.8 - lift), (14.5, 7 - lift)])
        pen.line([(6.5, 9.5), (7.5, 19.5), (16.5, 19.5), (17.5, 9.5)])
    elif name == "chevron":           # t = 0 aşağı, 1 yukarı
        pts = _rot([(7, 10), (12, 15), (17, 10)], 12, 12.5, 180 * t)
        pen.line(pts)
    elif name == "link":              # t = dalga (bağlantı canlı)
        pen.dot(12, 12, 3.2)
        if t > 0:
            r = 3.5 + 6 * t
            a = max(0.0, 1 - t)
            col = tuple(round(c) for c in rgb(color)) + (round(255 * a),)
            k = pen.k
            pen.d.ellipse((12 * k - r * k, 12 * k - r * k, 12 * k + r * k, 12 * k + r * k),
                          outline=col, width=max(1, round(pen.w * 0.8)))
    elif name == "game":              # t = sallanma
        ang = 7 * math.sin(2 * math.pi * t) * (1 - t)
        k2 = _Pen(size, color)
        k2.rrect(3, 7.5, 21, 17.5, 4.8)
        k2.line([(7, 12.5), (10, 12.5)])
        k2.line([(8.5, 11), (8.5, 14)])
        k2.dot(15, 11.8, 1.1)
        k2.dot(17, 13.6, 1.1)
        out = k2.im.rotate(ang, resample=Image.BICUBIC, center=(12 * k2.k, 12 * k2.k))
        return out.resize((size, size), Image.LANCZOS)
    elif name == "phone":             # t = ekranda altyazı satırının belirmesi (0 boş → 1 satır)
        pen.rrect(7, 3, 17, 21, 2.4)
        pen.line([(10.8, 18), (13.2, 18)])
        e = EASE(t) if t < 1 else 1.0
        if e > 0:
            pen.line([(12 - 2.8 * e, 12.5), (12 + 2.8 * e, 12.5)])
            pen.line([(12 - 1.6 * e, 14.8), (12 + 1.6 * e, 14.8)])
    elif name == "key":
        pen.d.ellipse((*pen.p(4, 9), *pen.p(10.5, 15.5)), outline=pen.col, width=max(1, round(pen.w)))
        pen.line([(10.5, 12.25), (20, 12.25)])
        pen.line([(17, 12.25), (17, 15)])
        pen.line([(19.5, 12.25), (19.5, 14.5)])
    elif name == "plus":
        pen.line([(12, 6), (12, 18)])
        pen.line([(6, 12), (18, 12)])
    elif name == "arrow":
        pen.line([(5, 12), (19, 12)])
        pen.line([(14, 7), (19, 12), (14, 17)])
    return pen.done()


def pen_arc_pts(cx, cy, r, a0, a1, n=24):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def icon_photo(name, size, color, t=0.0):
    tq = round(t * 48) / 48
    return _photo(("ic", name, size, color, tq), lambda: draw_icon(name, size, color, tq))


# ─────────────────────────────────────────────────────────────
# Canvas bileşenleri
# ─────────────────────────────────────────────────────────────

class UI:
    """Tek bir Canvas üzerinde bileşenleri yöneten bağlam."""

    def __init__(self, root, canvas, fonts: Fonts):
        self.root = root
        self.c = canvas
        self.f = fonts
        self._uid = 0

    def tag(self, prefix="w"):
        self._uid += 1
        return f"{prefix}{self._uid}"


class AnimatedIcon:
    def __init__(self, ui: UI, x, y, name, size=18, color=TEXT, tags=(), anchor="center"):
        self.ui, self.name, self.size, self.color = ui, name, size, color
        self.t = 0.0
        self._tw = None
        self._loop_job = None
        self.item = ui.c.create_image(x, y, image=icon_photo(name, size, color, 0), anchor=anchor, tags=tags)

    def set(self, t=None, color=None, name=None):
        if t is not None:
            self.t = t
        if color is not None:
            self.color = color
        if name is not None:
            self.name = name
        self.ui.c.itemconfigure(self.item, image=icon_photo(self.name, self.size, self.color, self.t))

    def animate(self, to, ms=260, ease=EASE, done=None, frm=None):
        self.stop()
        start = self.t if frm is None else frm
        self._tw = Tween(self.ui.root, ms, lambda e: self.set(start + (to - start) * e), done, ease)

    def play(self, ms=360, done=None):
        """0 → 1 oynat, sonra 0'a dön (tek seferlik hareketler: sallanma, kırpma, kapak)."""
        self.animate(1.0, ms, ease=lambda t: t, frm=0.0, done=lambda: (self.set(0.0), done and done()))

    def loop(self, period_ms=900):
        self.stop()
        t0 = time.perf_counter()

        def step():
            self.set(((time.perf_counter() - t0) * 1000 / period_ms) % 1.0)
            self._loop_job = self.ui.root.after(16, step)
        step()

    def stop(self):
        if self._tw:
            self._tw.cancel()
            self._tw = None
        if self._loop_job:
            self.ui.root.after_cancel(self._loop_job)
            self._loop_job = None

    def move(self, x, y):
        self.ui.c.coords(self.item, x, y)


_KIND_COLORS = {
    #           normal          hover          press          border        text
    "primary": (ACCENT, ACCENT_HOVER, ACCENT_PRESS, None, ACCENT_INK),
    "secondary": (SURFACE2, RAISED, SURFACE, LINE, TEXT),
    "ghost": (None, SURFACE2, SURFACE, None, TEXT2),
    "running": (RAISED, "#2b2d33", SURFACE2, LINE_STRONG, TEXT),
}


class Button:
    def __init__(self, ui: UI, x, y, w, h, text="", icon=None, kind="secondary", command=None,
                 keycap=None, tags=(), font=None, icon_size=18, align="center", radius=6,
                 on_hover=None, parent_bg=BG):
        self.ui, self.x, self.y, self.w, self.h = ui, x, y, w, h
        self.parent_bg = parent_bg
        self.kind, self.command, self.radius = kind, command, radius
        self.on_hover = on_hover
        self.tag = ui.tag("btn")
        self.tags = tuple(tags) + (self.tag,)
        self.font = font or ui.f.ui(13, semi=(kind == "primary"))
        self.align = align
        self._state = "normal"
        self.enabled = True
        c = ui.c
        self.bg = c.create_image(x, y, anchor="nw", tags=self.tags)
        self.icon = AnimatedIcon(ui, 0, 0, icon, icon_size, tags=self.tags) if icon else None
        self.label = c.create_text(0, 0, text=text, font=self.font, anchor="w", tags=self.tags)
        self.keycap = None
        if keycap:
            self.keycap = Keycap(ui, 0, 0, keycap, tags=self.tags, dark=(kind == "primary"))
        self._layout()
        self._paint()
        for seq, fn in (("<Enter>", self._enter), ("<Leave>", self._leave),
                        ("<ButtonPress-1>", self._press), ("<ButtonRelease-1>", self._release)):
            c.tag_bind(self.tag, seq, fn)

    def _layout(self):
        c = self.ui.c
        fnt = tkfont.Font(font=self.font)
        tw = fnt.measure(c.itemcget(self.label, "text"))
        iw = (self.icon.size + 8) if self.icon else 0
        cy = self.y + self.h / 2
        if self.align == "left":
            x0 = self.x + 12
        else:
            x0 = self.x + (self.w - tw - iw) / 2
            if self.keycap:
                x0 -= 10
        if self.icon:
            self.icon.move(x0 + self.icon.size / 2, cy)
        c.coords(self.label, x0 + iw, cy)
        if self.keycap:
            self.keycap.move(self.x + self.w - 10 - self.keycap.w, cy - self.keycap.h / 2)

    def _paint(self):
        normal, hover, press, border, fg = _KIND_COLORS[self.kind]
        fill = {"normal": normal, "hover": hover, "press": press}[self._state]
        if self.kind == "ghost" and self._state != "normal":
            fg = TEXT
        if not self.enabled:
            fill, fg = (SURFACE2 if self.kind != "ghost" else None), TEXT3
        if fill is None:   # hayalet düğme: altındaki yüzeyle aynı renk
            img = rrect_image(self.w, self.h, self.radius, fill=self.parent_bg)
        else:
            img = rrect_image(self.w, self.h, self.radius, fill=fill, outline=border)
        self.ui.c.itemconfigure(self.bg, image=img)
        self.ui.c.itemconfigure(self.label, fill=fg)
        if self.icon:
            self.icon.set(color=fg)

    def _enter(self, _):
        if not self.enabled:
            return
        self._state = "hover"
        self._paint()
        self.ui.c.configure(cursor="hand2")
        if self.on_hover:
            self.on_hover(True)

    def _leave(self, _):
        self._state = "normal"
        self._paint()
        self.ui.c.configure(cursor="")
        if self.on_hover:
            self.on_hover(False)

    def _press(self, _):
        if self.enabled:
            self._state = "press"
            self._paint()

    def _release(self, e):
        if not self.enabled:
            return
        inside = self.x <= e.x <= self.x + self.w and self.y <= e.y <= self.y + self.h
        self._state = "hover" if inside else "normal"
        self._paint()
        if inside and self.command:
            self.command()

    def set_text(self, text):
        self.ui.c.itemconfigure(self.label, text=text)
        self._layout()

    def set_kind(self, kind):
        self.kind = kind
        self.font = self.ui.f.ui(13, semi=(kind in ("primary", "running")))
        self.ui.c.itemconfigure(self.label, font=self.font)
        if self.keycap:
            self.keycap.set_dark(kind == "primary")
        self._layout()
        self._paint()

    def set_enabled(self, enabled):
        self.enabled = enabled
        self._paint()


class Keycap:
    def __init__(self, ui: UI, x, y, text, tags=(), dark=False, px=11):
        self.ui = ui
        f = ui.f.code(px)
        self.w = max(22, tkfont.Font(font=f).measure(text) + 12)
        self.h = 18
        self.dark = dark
        self.bg = ui.c.create_image(x, y, anchor="nw", tags=tags)
        self.txt = ui.c.create_text(x + self.w / 2, y + self.h / 2, text=text, font=f, tags=tags)
        self._paint()

    def _paint(self):
        if self.dark:
            img = rrect_image(self.w, self.h, 4, fill=None, outline=mix(ACCENT_INK, ACCENT, 0.55))
            fg = ACCENT_INK
        else:
            img = rrect_image(self.w, self.h, 4, fill=SURFACE2, outline=LINE_STRONG)
            fg = TEXT2
        self.ui.c.itemconfigure(self.bg, image=img)
        self.ui.c.itemconfigure(self.txt, fill=fg)

    def set_dark(self, dark):
        self.dark = dark
        self._paint()

    def move(self, x, y):
        self.ui.c.coords(self.bg, x, y)
        self.ui.c.coords(self.txt, x + self.w / 2, y + self.h / 2)


class Toggle:
    """Sağda anahtar, solda etiket (ve isteğe bağlı ikon)."""

    TW, TH = 34, 20

    def __init__(self, ui: UI, x, y, w, label, value=False, command=None, tags=(), icon=None):
        self.ui, self.x, self.y, self.w = ui, x, y, w
        self.value, self.command = bool(value), command
        self.tag = ui.tag("tg")
        tags = tuple(tags) + (self.tag,)
        c = ui.c
        cy = y + self.TH / 2
        lx = x
        self.icon = None
        if icon:
            self.icon = AnimatedIcon(ui, x + 9, cy, icon, 18, TEXT2, tags=tags)
            lx = x + 26
        self.label = c.create_text(lx, cy, text=label, anchor="w", font=ui.f.ui(13), fill=TEXT, tags=tags)
        sx = x + w - self.TW
        self.track = c.create_image(sx, y, anchor="nw", tags=tags)
        self.knob = c.create_image(0, cy, tags=tags)
        self._pos = 1.0 if self.value else 0.0
        self._tw = None
        self._paint()
        c.tag_bind(self.tag, "<Button-1>", self._click)
        c.tag_bind(self.tag, "<Enter>", lambda e: c.configure(cursor="hand2"))
        c.tag_bind(self.tag, "<Leave>", lambda e: c.configure(cursor=""))

    def _paint(self):
        p = self._pos
        sx = self.x + self.w - self.TW
        fill = mix(SURFACE2, ACCENT, p)
        outline = mix(LINE_STRONG, ACCENT, p)
        self.ui.c.itemconfigure(self.track, image=rrect_image(self.TW, self.TH, 10, fill=fill, outline=outline))
        kx = sx + 10 + p * (self.TW - 20)
        self.ui.c.coords(self.knob, kx, self.y + self.TH / 2)
        self.ui.c.itemconfigure(self.knob, image=circle_image(14, mix(TEXT2, ACCENT_INK, p)))

    def _click(self, _=None):
        self.set(not self.value, animate=True)
        if self.command:
            self.command(self.value)

    def set(self, value, animate=False):
        self.value = bool(value)
        target = 1.0 if self.value else 0.0
        if self._tw:
            self._tw.cancel()
        if not animate:
            self._pos = target
            self._paint()
            return
        start = self._pos
        self._tw = Tween(self.ui.root, 160, lambda e: (setattr(self, "_pos", start + (target - start) * e), self._paint()))
        if self.icon:
            self.icon.play(320)


class Segmented:
    def __init__(self, ui: UI, x, y, w, h, options, value, command=None, tags=()):
        self.ui, self.x, self.y, self.w, self.h = ui, x, y, w, h
        self.options = options               # [(value, label)]
        self.value, self.command = value, command
        c = ui.c
        self.tag = ui.tag("seg")
        tags = tuple(tags) + (self.tag,)
        self.seg_w = (w - 4) / len(options)
        c.create_image(x, y, anchor="nw", image=rrect_image(w, h, 6, fill=SURFACE2, outline=LINE), tags=tags)
        self.pill = c.create_image(x + 2, y + 2, anchor="nw",
                                   image=rrect_image(round(self.seg_w), h - 4, 4, fill=RAISED, outline=LINE_STRONG),
                                   tags=tags)
        self.labels = []
        for i, (val, lab) in enumerate(options):
            t = c.create_text(x + 2 + self.seg_w * (i + 0.5), y + h / 2, text=lab, font=ui.f.ui(12, semi=True),
                              tags=tags)
            self.labels.append(t)
        self._pos = self._index(value)
        self._tw = None
        self._paint()
        c.tag_bind(self.tag, "<Button-1>", self._click)
        c.tag_bind(self.tag, "<Enter>", lambda e: c.configure(cursor="hand2"))
        c.tag_bind(self.tag, "<Leave>", lambda e: c.configure(cursor=""))

    def _index(self, value):
        for i, (v, _) in enumerate(self.options):
            if v == value:
                return i
        return 0

    def _paint(self):
        c = self.ui.c
        c.coords(self.pill, self.x + 2 + self._pos * self.seg_w, self.y + 2)
        for i, t in enumerate(self.labels):
            near = max(0.0, 1 - abs(self._pos - i))
            c.itemconfigure(t, fill=mix(TEXT3, TEXT, near))

    def _click(self, e):
        i = int((e.x - self.x - 2) // self.seg_w)
        i = max(0, min(len(self.options) - 1, i))
        if self.options[i][0] != self.value:
            self.set(self.options[i][0], animate=True)
            if self.command:
                self.command(self.value)

    def set(self, value, animate=False):
        self.value = value
        target = self._index(value)
        if self._tw:
            self._tw.cancel()
        if not animate:
            self._pos = target
            self._paint()
            return
        start = self._pos
        self._tw = Tween(self.ui.root, 200, lambda e: (setattr(self, "_pos", start + (target - start) * e), self._paint()))


class Slider:
    def __init__(self, ui: UI, x, y, w, label, lo, hi, value, fmt="{}", step=1, command=None, tags=()):
        self.ui, self.x, self.y, self.w = ui, x, y, w
        self.lo, self.hi, self.step, self.fmt, self.command = lo, hi, step, fmt, command
        self.value = value
        c = ui.c
        self.tag = ui.tag("sl")
        tags = tuple(tags) + (self.tag,)
        c.create_text(x, y + 8, text=label, anchor="w", font=ui.f.ui(13), fill=TEXT, tags=tags)
        self.val_txt = c.create_text(x + w, y + 8, anchor="e", font=ui.f.code(12), fill=TEXT2, tags=tags)
        self.ty = y + 30
        self.track = c.create_image(x, self.ty - 2, anchor="nw",
                                    image=rrect_image(w, 4, 2, fill=SURFACE2, outline=None), tags=tags)
        self.fill = c.create_image(x, self.ty - 2, anchor="nw", tags=tags)
        self.knob = c.create_image(x, self.ty, tags=tags)
        hit = c.create_rectangle(x - 8, self.ty - 12, x + w + 8, self.ty + 12, outline="", fill="", tags=tags)
        self._hover = False
        self._paint()
        for seq in ("<ButtonPress-1>", "<B1-Motion>"):
            c.tag_bind(self.tag, seq, self._drag)
        c.tag_bind(self.tag, "<ButtonRelease-1>", lambda e: self.command and self.command(self.value, True))
        c.tag_bind(self.tag, "<Enter>", lambda e: (c.configure(cursor="hand2"), self._set_hover(True)))
        c.tag_bind(self.tag, "<Leave>", lambda e: (c.configure(cursor=""), self._set_hover(False)))

    def _set_hover(self, h):
        self._hover = h
        self._paint()

    def _frac(self):
        return (self.value - self.lo) / (self.hi - self.lo) if self.hi != self.lo else 0

    def _paint(self):
        c = self.ui.c
        f = self._frac()
        fw = max(4, round(self.w * f))
        c.itemconfigure(self.fill, image=rrect_image(fw, 4, 2, fill=ACCENT))
        kx = self.x + self.w * f
        c.coords(self.knob, kx, self.ty)
        c.itemconfigure(self.knob, image=circle_image(16 if self._hover else 14, TEXT, outline=BG, width=2))
        c.itemconfigure(self.val_txt, text=self.fmt.format(self.value))

    def _drag(self, e):
        f = max(0.0, min(1.0, (e.x - self.x) / self.w))
        raw = self.lo + f * (self.hi - self.lo)
        v = round(raw / self.step) * self.step
        v = round(v, 4) if isinstance(self.step, float) else int(v)
        if v != self.value:
            self.value = v
            self._paint()
            if self.command:
                self.command(v, False)

    def set(self, v):
        self.value = v
        self._paint()


class Field:
    """Yuvarlak kenarlı metin alanı (gömülü tk.Entry) + isteğe bağlı sağ ikon düğmesi."""

    def __init__(self, ui: UI, x, y, w, h, var, show=None, tags=(), lead_icon=None, trail=None,
                 placeholder=""):
        self.ui, self.x, self.y, self.w, self.h = ui, x, y, w, h
        c = ui.c
        self.tags = tuple(tags)
        self.bg = c.create_image(x, y, anchor="nw", tags=self.tags)
        ex = x + 10
        if lead_icon:
            AnimatedIcon(ui, x + 18, y + h / 2, lead_icon, 16, TEXT3, tags=self.tags)
            ex = x + 32
        ew = w - (ex - x) - (34 if trail else 10)
        self.entry = tk.Entry(c, textvariable=var, show=show or "", bg=SURFACE2, fg=TEXT, insertbackground=ACCENT,
                              relief="flat", bd=0, highlightthickness=0, font=ui.f.ui(13),
                              disabledbackground=SURFACE2, selectbackground=mix(SURFACE2, ACCENT, 0.4),
                              selectforeground=TEXT)
        self.win = c.create_window(ex, y + h / 2, anchor="w", window=self.entry, width=ew, height=h - 10,
                                   tags=self.tags)
        self.entry.bind("<FocusIn>", lambda e: self._paint(True))
        self.entry.bind("<FocusOut>", lambda e: self._paint(False))
        self.trail = None
        if trail:
            name, cmd = trail
            self.trail = AnimatedIcon(ui, x + w - 18, y + h / 2, name, 18, TEXT3, tags=self.tags)
            t = ui.tag("ft")
            c.addtag_withtag(t, self.trail.item)
            c.tag_bind(t, "<Button-1>", lambda e: cmd())
            c.tag_bind(t, "<Enter>", lambda e: (c.configure(cursor="hand2"), self.trail.set(color=TEXT)))
            c.tag_bind(t, "<Leave>", lambda e: (c.configure(cursor=""), self.trail.set(color=TEXT3)))
        self._paint(False)

    def _paint(self, focus):
        img = rrect_image(self.w, self.h, 6, fill=SURFACE2, outline=ACCENT if focus else LINE)
        self.ui.c.itemconfigure(self.bg, image=img)

    def set_show(self, show):
        self.entry.configure(show=show)


class Dropdown:
    def __init__(self, ui: UI, x, y, w, h, options, value, command=None, tags=()):
        self.ui, self.x, self.y, self.w, self.h = ui, x, y, w, h
        self.options, self.value, self.command = options, value, command
        c = ui.c
        self.tag = ui.tag("dd")
        tags = tuple(tags) + (self.tag,)
        self.bg = c.create_image(x, y, anchor="nw", image=rrect_image(w, h, 6, fill=SURFACE2, outline=LINE), tags=tags)
        self.txt = c.create_text(x + 10, y + h / 2, anchor="w", text=value, font=ui.f.code(12), fill=TEXT, tags=tags)
        self.chev = AnimatedIcon(ui, x + w - 14, y + h / 2, "chevron", 14, TEXT3, tags=tags)
        self.popup = None
        c.tag_bind(self.tag, "<Button-1>", lambda e: self.open())
        c.tag_bind(self.tag, "<Enter>", lambda e: (c.configure(cursor="hand2"),
                                                   c.itemconfigure(self.bg, image=rrect_image(w, h, 6, fill=RAISED, outline=LINE_STRONG))))
        c.tag_bind(self.tag, "<Leave>", lambda e: (c.configure(cursor=""),
                                                   c.itemconfigure(self.bg, image=rrect_image(w, h, 6, fill=SURFACE2, outline=LINE))))

    def set(self, value):
        self.value = value
        self.ui.c.itemconfigure(self.txt, text=value)

    def open(self):
        if self.popup:
            self.close()
            return
        c = self.ui.c
        self.chev.animate(1.0, 180)
        row_h = 26
        ph = row_h * len(self.options) + 8
        top = tk.Toplevel(c)
        top.overrideredirect(True)
        top.attributes("-topmost", True)
        top.configure(bg=BG)
        x = c.winfo_rootx() + self.x
        y = c.winfo_rooty() + self.y + self.h + 4
        top.geometry(f"{self.w}x{ph}+{x}+{y}")
        pc = tk.Canvas(top, width=self.w, height=ph, bg=BG, highlightthickness=0, bd=0)
        pc.pack()
        pc.create_image(0, 0, anchor="nw", image=rrect_image(self.w, ph, 6, fill=RAISED, outline=LINE_STRONG))
        hl = pc.create_image(4, 4, anchor="nw", image=rrect_image(self.w - 8, row_h, 4, fill=SURFACE2), state="hidden")
        for i, opt in enumerate(self.options):
            yy = 4 + i * row_h + row_h / 2
            pc.create_text(14, yy, anchor="w", text=opt, font=self.ui.f.code(12),
                           fill=ACCENT if opt == self.value else TEXT)

        def motion(e):
            i = int((e.y - 4) // row_h)
            if 0 <= i < len(self.options):
                pc.coords(hl, 4, 4 + i * row_h)
                pc.itemconfigure(hl, state="normal")

        def click(e):
            i = int((e.y - 4) // row_h)
            if 0 <= i < len(self.options):
                self.set(self.options[i])
                if self.command:
                    self.command(self.value)
            self.close()

        pc.bind("<Motion>", motion)
        pc.bind("<Button-1>", click)
        top.bind("<FocusOut>", lambda e: self.close())
        top.bind("<Escape>", lambda e: self.close())
        top.focus_force()
        self.popup = top

    def close(self):
        if self.popup:
            self.popup.destroy()
            self.popup = None
            self.chev.animate(0.0, 180)


class Tabs:
    def __init__(self, ui: UI, x, y, w, tabs, value, command=None, tags=()):
        self.ui, self.x, self.y, self.w = ui, x, y, w
        self.tabs, self.value, self.command = tabs, value, command
        c = ui.c
        f = ui.f.ui(13, semi=True)
        fnt = tkfont.Font(font=f)
        self.items = []
        cx = x
        for key, label in tabs:
            tw = fnt.measure(label)
            t = c.create_text(cx, y + 12, text=label, anchor="w", font=f, tags=tags)
            tg = ui.tag("tab")
            c.addtag_withtag(tg, t)
            c.tag_bind(tg, "<Button-1>", lambda e, k=key: self.select(k, user=True))
            c.tag_bind(tg, "<Enter>", lambda e, t=t: (c.configure(cursor="hand2"), self._hover(t, True)))
            c.tag_bind(tg, "<Leave>", lambda e, t=t: (c.configure(cursor=""), self._hover(t, False)))
            self.items.append((key, t, cx, tw))
            cx += tw + 22
        c.create_line(x, y + 30, x + w, y + 30, fill=LINE, tags=tags)
        self.bar = c.create_image(x, y + 29, anchor="nw", tags=tags)
        self._bx, self._bw = 0.0, 0.0
        self._tw = None
        self.select(value, user=False, animate=False)

    def _hover(self, item, on):
        key = next(k for k, t, _, _ in self.items if t == item)
        if key != self.value:
            self.ui.c.itemconfigure(item, fill=TEXT2 if on else TEXT3)

    def select(self, key, user=False, animate=True):
        self.value = key
        c = self.ui.c
        tx, tw = 0, 0
        for k, t, x, w in self.items:
            c.itemconfigure(t, fill=TEXT if k == key else TEXT3)
            if k == key:
                tx, tw = x, w
        if self._tw:
            self._tw.cancel()
        if not animate:
            self._bx, self._bw = tx, tw
            self._paint()
        else:
            sx, sw = self._bx, self._bw
            self._tw = Tween(self.ui.root, 220, lambda e: (setattr(self, "_bx", sx + (tx - sx) * e),
                                                           setattr(self, "_bw", sw + (tw - sw) * e), self._paint()))
        if user and self.command:
            self.command(key)

    def _paint(self):
        self.ui.c.coords(self.bar, self._bx, self.y + 29)
        self.ui.c.itemconfigure(self.bar, image=rrect_image(max(2, round(self._bw)), 2, 1, fill=ACCENT))


def sparkline_image(points, w, h):
    """points: [(ms, kind)] — pool noktaları yeşil, api noktaları açık gri; çizgi soluk."""
    def build():
        W, H = w * _SS, h * _SS
        im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        if len(points) < 2:
            d = ImageDraw.Draw(im)
            d.line((0, H - 2 * _SS, W, H - 2 * _SS), fill=rgb(LINE) + (255,), width=_SS)
            return im.resize((w, h), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        mx = max(p[0] for p in points) or 1
        n = len(points)
        pad = 3 * _SS
        xy = [(pad + (W - 2 * pad) * i / (n - 1), H - pad - (H - 2 * pad) * min(1.0, p[0] / mx)) for i, p in enumerate(points)]
        d.line(xy, fill=rgb(LINE_STRONG) + (255,), width=round(1.5 * _SS), joint="curve")
        for (x, y), (_, kind) in zip(xy, points):
            col = rgb(VERIFIED) if kind == "pool" else rgb(TEXT2)
            r = 1.8 * _SS
            d.ellipse((x - r, y - r, x + r, y + r), fill=col + (255,))
        return im.resize((w, h), Image.LANCZOS)
    key = ("spark", w, h, tuple(points))
    return _photo(key, build)


def checker_image(w, h, radius=6):
    """Önizleme zemini: tasarım araçlarındaki saydamlık damalı deseni (yuvarlak köşeli)."""
    def build():
        im = Image.new("RGBA", (w, h), rgb("#1b1c1f") + (255,))
        d = ImageDraw.Draw(im)
        s = 10
        for yy in range(0, h, s):
            for xx in range(0, w, s):
                if (xx // s + yy // s) % 2:
                    d.rectangle((xx, yy, xx + s - 1, yy + s - 1), fill=rgb("#222327") + (255,))
        mask = Image.new("L", (w * _SS, h * _SS), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, w * _SS - 1, h * _SS - 1), radius=radius * _SS, fill=255)
        im.putalpha(mask.resize((w, h), Image.LANCZOS))
        return im
    return _photo(("checker", w, h, radius), build)
