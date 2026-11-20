# -*- coding: utf-8 -*-
"""
Makine Live — oyun içi çeviri kutusu: çizim + Win32 katmanlı pencere (TEST HARNESS).

Bu modül native C++ overlay'in birebir prototipidir. Taşınacak parçalar:

  1. render_overlay(content, style, chrome) -> Rendered
       Saf fonksiyon. Kutuyu 2× süper örneklemeyle RGBA (düz alfa) bir görüntüye çizer:
       yuvarlak bant (yarı saydam) + yumuşak gölge + tam opak beyaz metin (koyu kontur) +
       sol kenarda durum şeridi. Kutuda YALNIZCA çeviri görünür (kaynak/İngilizce metin yok).
       Uzun metin en fazla 4 satıra sığsın diye yazı adım adım küçültülür (en az %74).
       Bant kutusunun görüntü içindeki yeri (band_box) ve durum şeridinin yeri (strip_box) de döner.
       C++: Direct2D/DirectWrite ile aynı katmanlar (ya da GDI+), çıktı premultiplied BGRA DIB.

  2. paint_strip(premult, rendered, kind)
       Durum şeridi (onaylı yeşil / havuzda bekleyen sarı / API: yok).

  3. LayeredWindow.present(premult_bgra, x, y, alpha)
       UpdateLayeredWindow(ULW_ALPHA) ile piksel bazında alfa. Solma (fade) için görüntü yeniden
       çizilmez; BLENDFUNCTION.SourceConstantAlpha değiştirilir. Pencere stilleri:
       WS_POPUP + WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
       kilitliyken + WS_EX_TRANSPARENT (tıklamalar oyuna geçer).
       Taşıma modunda sürükleme pencerenin kendi fare mesajlarıyla yapılır (SetCapture + SetWindowPos).
       Windows'un taşıma döngüsü (HTCAPTION) KULLANILMAZ ve pencere yordamından Tk'ya ASLA dokunulmaz:
       yordam, Tcl olay döngüsünün içinden ctypes geri çağrısı olarak çalışır; oradan Tkinter çağrısı
       yorumlayıcının iş parçacığı durumunu bozar ve süreç iz bırakmadan kapanır (v0.1.0 çökmesi).
       Bırakılan konum kuyruğa yazılır, SubtitleOverlay onu Tk zamanlayıcısıyla alır.
       SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE): OCR kutunun kendisini okumaz. Okuma pencerenin kendi
       içeriğinden yapılıyorsa (konsol modu, "yayında göster") kutu yakalamaya açılır: OBS ve ekran paylaşımı da görür.

  4. SubtitleOverlay — gösterim durum makinesi (solarak gel / çapraz geçiş / sol).
       NE ZAMAN gösterilip gizleneceğine karar vermez: bunu subtitle_timing.SubtitleScheduler verir
       (present_line / end_line). Zamanlayıcı olarak Tk after() kullanır; C++'ta SetTimer / overlay thread'i.

Tasarım değerleri: spec/design_tokens.json → "overlay".
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import time
from dataclasses import dataclass, field, replace

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# ─────────────────────────────────────────────────────────────
# Tasarım değerleri
# ─────────────────────────────────────────────────────────────

from makine_live.paths import DESIGN_TOKENS as _TOKENS_PATH  # noqa: E402


def _load_tokens() -> dict:
    try:
        with open(_TOKENS_PATH, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


TOKENS = _load_tokens()


def _hex(c: str) -> tuple:
    c = c.lstrip("#")
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


_OV = TOKENS.get("overlay", {})
_COL = TOKENS.get("color", {})
BAND_RGB = _hex(_OV.get("band", {}).get("bg", "#0f1012"))
TEXT_RGB = _hex(_OV.get("text", {}).get("color", "#ffffff"))
OUTLINE_RGB = _hex(_OV.get("text", {}).get("outline", "#000000"))
ACCENT_RGB = _hex(_COL.get("accent", "#f2c230"))
ACCENT_INK_RGB = _hex(_COL.get("accentInk", "#16171a"))
PLACEHOLDER_RGB = (118, 118, 122)

STATUS_RGB = {
    "verified": _hex(_COL.get("status", {}).get("verified", "#3fae72")),
    "pending": ACCENT_RGB,                       # havuz: henüz onaylanmamış
    "api": None,                                 # API çevirisi: şerit yok
}

SS = 2              # süper örnekleme katsayısı
MARGIN = 28         # gölge için görüntü kenar boşluğu (1×)
CHROME_H = 26       # taşıma modunda üstteki etiket şeridi (1×)
FIT_SCALES = (1.0, 0.9, 0.82, 0.74)   # uzun metin satır sınırına sığsın diye yazı en fazla %74'e küçülür

# altyazi: yuvarlak köşeli bant · yalin: bant yok, gölgeli yazı · sinema: tam genişlik şerit
# kontrast: altyazı bandı + sarı yazı + kalın kontur (parlak sahnelerde okunurluk)
STYLES = ("altyazi", "yalin", "sinema", "kontrast")
OUTLINE_PX = {"yok": 0.0, "ince": 1.5, "kalin": 3.0}

# ─────────────────────────────────────────────────────────────
# Yazı tipleri
# ─────────────────────────────────────────────────────────────

_FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
# anahtar → (paneldeki ad, CSS ailesi (OBS sayfası), [(dosya, değişken yazı tipinin örneği | None)])
# Yalnızca Windows'la gelen yazı tipleri; kalın / yarı kalın kesimler (altyazı ince yazıyla okunmaz).
FONTS = {
    "segoe": ("Segoe UI", '"Segoe UI Variable Display","Segoe UI",sans-serif',
              [("SegUIVar.ttf", "Semibold Display"), ("seguisb.ttf", None), ("segoeui.ttf", None)]),
    "bahnschrift": ("Bahnschrift", "Bahnschrift,sans-serif", [("bahnschrift.ttf", "SemiBold")]),
    "arial": ("Arial", "Arial,sans-serif", [("arialbd.ttf", None)]),
    "verdana": ("Verdana", "Verdana,sans-serif", [("verdanab.ttf", None)]),
    "trebuchet": ("Trebuchet", '"Trebuchet MS",sans-serif', [("trebucbd.ttf", None)]),
    "tahoma": ("Tahoma", "Tahoma,sans-serif", [("tahomabd.ttf", None)]),
    "calibri": ("Calibri", "Calibri,sans-serif", [("calibrib.ttf", None)]),
    "georgia": ("Georgia", "Georgia,serif", [("georgiab.ttf", None)]),
}
_font_cache: dict = {}


def available_fonts() -> list:
    """[(anahtar, ad)]: bu bilgisayarda dosyası olan yazı tipleri."""
    return [(k, v[0]) for k, v in FONTS.items() if any(os.path.isfile(os.path.join(_FONT_DIR, f)) for f, _ in v[2])]


def get_font(px: int, variation: str = "Semibold Display", family: str = "segoe") -> ImageFont.FreeTypeFont:
    key = (px, variation, family)
    if key in _font_cache:
        return _font_cache[key]
    font = None
    cands = FONTS.get(family, FONTS["segoe"])[2] + (FONTS["segoe"][2] if family != "segoe" else [])
    for name, var in cands:
        path = os.path.join(_FONT_DIR, name)
        if not os.path.isfile(path):
            continue
        try:
            font = ImageFont.truetype(path, px)
            if var:
                try:
                    # Segoe'da panel "Semibold Text" ister (taşıma etiketi); diğer değişken yazı tiplerinde kendi kesimi
                    font.set_variation_by_name(variation if name == "SegUIVar.ttf" else var)
                except (OSError, ValueError):
                    pass
            break
        except OSError:
            continue
    if font is None:
        font = ImageFont.load_default(px)
    _font_cache[key] = font
    return font


# ─────────────────────────────────────────────────────────────
# Veri tipleri
# ─────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class OverlayStyle:
    style: str = "altyazi"       # altyazi | yalin | sinema | kontrast
    font_px: int = 26
    opacity: float = 0.78        # bant opaklığı (metin her zaman tam opak)
    max_width: int = 1100        # bant için en fazla genişlik (ekranın width oranı)
    font: str = "segoe"          # FONTS anahtarı
    text_rgb: tuple = TEXT_RGB
    band_rgb: tuple = BAND_RGB
    outline: float = 1.5         # yazı konturu (px); 0 = yok
    shadow: bool = True          # bandın (yalın stilde yazının) gölgesi
    max_lines: int = 4           # uzun metin bu kadar satıra sığsın diye yazı küçültülür


@dataclass(frozen=True)
class OverlayContent:
    text: str = ""               # çeviri (kutuda yalnızca çeviri gösterilir)
    kind: str = "api"            # pending | verified | api
    placeholder: bool = False    # taşıma modunda boş kutu


@dataclass
class Rendered:
    image: Image.Image           # RGBA, düz alfa, 1×
    band_box: tuple              # (x0, y0, x1, y1) bant ya da metin bloğu, görüntü koordinatı
    strip_box: tuple | None      # durum şeridi kutusu
    kind: str = "api"
    premult: np.ndarray = field(default=None, repr=False)   # H×W×4 float32, premultiplied BGRA


# ─────────────────────────────────────────────────────────────
# 1. Yerleşim + çizim (saf fonksiyonlar)
# ─────────────────────────────────────────────────────────────

def wrap_lines(text: str, font: ImageFont.FreeTypeFont, max_w: float) -> list:
    """Kelime bazlı satır kaydırma; tek başına sığmayan kelimeyi harf bazında böler."""
    out = []
    for para in text.replace("\r", "").split("\n"):
        words = para.split()
        if not words:
            continue
        line = ""
        for w in words:
            cand = f"{line} {w}" if line else w
            if font.getlength(cand) <= max_w:
                line = cand
                continue
            if line:
                out.append(line)
            while font.getlength(w) > max_w and len(w) > 1:
                cut = len(w)
                while cut > 1 and font.getlength(w[:cut]) > max_w:
                    cut -= 1
                out.append(w[:cut])
                w = w[cut:]
            line = w
        if line:
            out.append(line)
    return out


def _metrics(style: OverlayStyle, scale: float = 1.0):
    sinema = style.style == "sinema"
    fp = max(12, round(style.font_px * (1.22 if sinema else 1.0) * scale))
    return {
        "fp": fp,
        "pad_x": 40 if sinema else 22,
        "pad_y": 16 if sinema else 12,
        "radius": 4 if sinema else 10,
        "line_h": round(fp * 1.26),
        # Yalın stilde bant yok: kontur seçilmişse biraz kalın (yazı doğrudan oyunun üstünde)
        "stroke": (max(style.outline, 2.0) if style.style == "yalin" and style.outline else style.outline) * scale ** 0.5,
    }


def fit_text(text: str, style: OverlayStyle):
    """Metni style.max_lines satıra sığdıran en büyük yazı ölçeğini seçer; (metrikler, font, satırlar)."""
    for scale in FIT_SCALES:
        m = _metrics(style, scale)
        font = get_font(m["fp"] * SS, "Semibold Display", style.font)
        lines = wrap_lines(text, font, (style.max_width - 2 * m["pad_x"]) * SS)
        if len(lines) <= max(1, style.max_lines):
            break
    return m, font, lines    # en küçük ölçekte de sığmazsa satır sayısı artar (kesilmez)


def render_overlay(content: OverlayContent, style: OverlayStyle, chrome: bool = False) -> Rendered:
    band = style.style != "yalin"
    text = content.text or ("Altyazı burada görünecek" if content.placeholder else "")
    m, main_font, main_lines = fit_text(text, style)
    if not main_lines:
        main_lines = [""]

    content_w = max([main_font.getlength(l) for l in main_lines] + [1]) / SS
    content_h = len(main_lines) * m["line_h"]

    band_w = round(content_w + 2 * m["pad_x"])
    if style.style == "sinema":
        band_w = style.max_width
    if chrome:
        band_w = max(band_w, 320)
    band_w = min(band_w, style.max_width)
    band_h = round(content_h + 2 * m["pad_y"])

    top_extra = CHROME_H if chrome else 0
    W = band_w + 2 * MARGIN
    H = band_h + 2 * MARGIN + top_extra
    bx0, by0 = MARGIN, MARGIN + top_extra
    bx1, by1 = bx0 + band_w, by0 + band_h

    # ── 2× katmanlar ──
    Ws, Hs = W * SS, H * SS
    text_layer = Image.new("RGBA", (Ws, Hs), (0, 0, 0, 0))
    d = ImageDraw.Draw(text_layer)
    stroke = round(m["stroke"] * SS)
    cx = (bx0 + bx1) / 2 * SS
    y = (by0 + m["pad_y"]) * SS

    if content.placeholder and not content.text:
        fill = PLACEHOLDER_RGB + (255,)
    else:
        fill = tuple(style.text_rgb) + (255,)
    # Semibold Display'in görsel dikey merkezi için küçük düzeltme
    y_nudge = -round(m["fp"] * 0.06 * SS)
    for line in main_lines:
        lw = main_font.getlength(line)
        if stroke > 0:
            d.text((cx - lw / 2, y + y_nudge), line, font=main_font, fill=fill,
                   stroke_width=stroke, stroke_fill=OUTLINE_RGB + (255,))
        else:
            d.text((cx - lw / 2, y + y_nudge), line, font=main_font, fill=fill)
        y += m["line_h"] * SS

    # Premultiplied alanda küçült → kenarlarda koyu saçak oluşmaz
    text_1x = text_layer.convert("RGBa").resize((W, H), Image.LANCZOS).convert("RGBA")

    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    if band and style.shadow:
        # Gölge: bant şeklinin bulanık kopyası
        shadow_cfg = _OV.get("band", {})
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).rounded_rectangle((bx0, by0 + 4, bx1, by1 + 4), radius=m["radius"], fill=255)
        blur = shadow_cfg.get("shadowBlur", 18) / 2
        mask = mask.filter(ImageFilter.GaussianBlur(blur))
        shadow_alpha = shadow_cfg.get("shadowAlpha", 0.35) * min(1.0, style.opacity / 0.78)
        mask = mask.point(lambda v: round(v * shadow_alpha))
        shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        shadow.putalpha(mask)
        canvas = Image.alpha_composite(canvas, shadow)

    if band:
        # Bant (2× çizip küçült: yumuşak köşeler)
        band_layer = Image.new("RGBA", (Ws, Hs), (0, 0, 0, 0))
        ImageDraw.Draw(band_layer).rounded_rectangle(
            (bx0 * SS, by0 * SS, bx1 * SS, by1 * SS), radius=m["radius"] * SS,
            fill=tuple(style.band_rgb) + (round(255 * style.opacity),))
        band_1x = band_layer.convert("RGBa").resize((W, H), Image.LANCZOS).convert("RGBA")
        canvas = Image.alpha_composite(canvas, band_1x)
    elif style.shadow:
        # Yalın: bant yok; metnin altına yumuşak bir gölge (okunabilirlik)
        a = text_1x.getchannel("A").filter(ImageFilter.GaussianBlur(3))
        a = a.point(lambda v: round(v * 0.55))
        sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        sh.putalpha(a)
        canvas = Image.alpha_composite(canvas, sh.transform(sh.size, Image.AFFINE, (1, 0, 0, 0, 1, -2)))

    canvas = Image.alpha_composite(canvas, text_1x)

    # Durum şeridi kutusu
    if band:
        strip_box = (bx0 + 7, by0 + 9, bx0 + 10, by1 - 9)
    else:
        tx0 = round((bx0 + bx1) / 2 - content_w / 2)
        strip_box = (tx0 - 14, by0 + m["pad_y"], tx0 - 11, by1 - m["pad_y"])

    if chrome:
        canvas = _draw_chrome(canvas, (bx0, by0, bx1, by1), m["radius"])

    r = Rendered(image=canvas, band_box=(bx0, by0, bx1, by1),
                 strip_box=strip_box if not (content.placeholder and not content.text) else None,
                 kind=content.kind)
    r.premult = to_premult_bgra(canvas)
    if r.strip_box:
        paint_strip(r.premult, r, content.kind)
    return r


def _draw_chrome(img: Image.Image, box: tuple, radius: int) -> Image.Image:
    """Taşıma modu: hafif karartılmış tutma alanı, kesikli vurgu çerçevesi, etiket."""
    x0, y0, x1, y1 = box
    # Tutma alanı: alfa > 0 olmalı ki pencere fareyi yakalasın (Yalın stilde de)
    grab = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(grab).rounded_rectangle((x0 - 6, y0 - 6, x1 + 6, y1 + 6), radius=radius + 4, fill=(0, 0, 0, 40))
    img = Image.alpha_composite(grab, img)
    d = ImageDraw.Draw(img)

    # Kesikli çerçeve (1 px, vurgu rengi)
    dash, gap = 6, 4
    ax0, ay0, ax1, ay1 = x0 - 6, y0 - 6, x1 + 6, y1 + 6
    col = ACCENT_RGB + (230,)
    for x in range(ax0, ax1, dash + gap):
        d.line((x, ay0, min(x + dash, ax1), ay0), fill=col)
        d.line((x, ay1, min(x + dash, ax1), ay1), fill=col)
    for y in range(ay0, ay1, dash + gap):
        d.line((ax0, y, ax0, min(y + dash, ay1)), fill=col)
        d.line((ax1, y, ax1, min(y + dash, ay1)), fill=col)

    # Etiket: "taşı · F8 kilitle"
    f = get_font(12 * SS, "Semibold Text", "segoe")
    label = "taşı  ·  F8 kilitle"
    lw = f.getlength(label) / SS
    lx0, ly1 = ax0, ay0 - 5
    lx1, ly0 = lx0 + lw + 16, ly1 - 20
    chip = Image.new("RGBA", (img.size[0] * SS, img.size[1] * SS), (0, 0, 0, 0))
    cd = ImageDraw.Draw(chip)
    cd.rounded_rectangle((lx0 * SS, ly0 * SS, lx1 * SS, ly1 * SS), radius=4 * SS, fill=ACCENT_RGB + (255,))
    cd.text(((lx0 + 8) * SS, (ly0 + 3) * SS), label, font=f, fill=ACCENT_INK_RGB + (255,))
    chip = chip.convert("RGBa").resize(img.size, Image.LANCZOS).convert("RGBA")
    return Image.alpha_composite(img, chip)


def to_premult_bgra(img: Image.Image) -> np.ndarray:
    """RGBA (düz alfa) → premultiplied BGRA float32 (UpdateLayeredWindow'un istediği biçim)."""
    a = np.asarray(img, dtype=np.float32)
    alpha = a[..., 3:4] / 255.0
    out = np.empty_like(a)
    out[..., 0] = a[..., 2] * alpha[..., 0]
    out[..., 1] = a[..., 1] * alpha[..., 0]
    out[..., 2] = a[..., 0] * alpha[..., 0]
    out[..., 3] = a[..., 3]
    return out


def paint_strip(premult: np.ndarray, r: Rendered, kind: str):
    """Durum şeridini premultiplied tampona çizer (verified / pending: dolu 3 px şerit, api: yok)."""
    if not r.strip_box:
        return
    x0, y0, x1, y1 = r.strip_box
    rgb = STATUS_RGB.get(kind)
    if rgb is None or y1 - y0 < 6:
        return
    region = premult[y0:y1, x0:x1]
    region[...] = 0.0
    _fill_premult(region, rgb, 1.0)
    # yuvarlatılmış uçlar
    for yy in (0, -1):
        _fill_premult(region[yy:yy + 1 if yy == 0 else None], rgb, 0.45)


def _fill_premult(region: np.ndarray, rgb: tuple, a: float):
    region[..., 0] = rgb[2] * a
    region[..., 1] = rgb[1] * a
    region[..., 2] = rgb[0] * a
    region[..., 3] = 255.0 * a


# ─────────────────────────────────────────────────────────────
# 2. Win32 katmanlı pencere
# ─────────────────────────────────────────────────────────────

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

LRESULT = ctypes.c_ssize_t
WPARAM = ctypes.c_size_t
LPARAM = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, WPARAM, LPARAM)

WS_POPUP = 0x80000000
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008
WS_EX_NOACTIVATE = 0x08000000
GWL_EXSTYLE = -20
SW_HIDE = 0
SW_SHOWNOACTIVATE = 4
WM_NCHITTEST = 0x0084
WM_MOUSEACTIVATE = 0x0021
WM_EXITSIZEMOVE = 0x0232
WM_SETCURSOR = 0x0020
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_CAPTURECHANGED = 0x0215
HTCLIENT = 1
HTCAPTION = 2
HTTRANSPARENT = -1
MA_NOACTIVATE = 3
ULW_ALPHA = 0x02
AC_SRC_OVER = 0x00
AC_SRC_ALPHA = 0x01
WDA_NONE = 0x00000000
WDA_EXCLUDEFROMCAPTURE = 0x00000011
DIB_RGB_COLORS = 0
BI_RGB = 0
IDC_SIZEALL = 32646
HWND_TOPMOST = -1
SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE, SWP_NOZORDER = 0x2, 0x1, 0x10, 0x4


class BLENDFUNCTION(ctypes.Structure):
    _fields_ = [("BlendOp", ctypes.c_ubyte), ("BlendFlags", ctypes.c_ubyte),
                ("SourceConstantAlpha", ctypes.c_ubyte), ("AlphaFormat", ctypes.c_ubyte)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int), ("hInstance", wt.HINSTANCE),
                ("hIcon", wt.HICON), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR), ("hIconSm", wt.HICON)]


user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, WPARAM, LPARAM]
user32.DefWindowProcW.restype = LRESULT
user32.RegisterClassExW.argtypes = [ctypes.POINTER(WNDCLASSEXW)]
user32.RegisterClassExW.restype = wt.ATOM
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.CreateWindowExW.restype = wt.HWND
user32.UpdateLayeredWindow.argtypes = [wt.HWND, wt.HDC, ctypes.POINTER(wt.POINT), ctypes.POINTER(wt.SIZE),
                                       wt.HDC, ctypes.POINTER(wt.POINT), wt.COLORREF,
                                       ctypes.POINTER(BLENDFUNCTION), wt.DWORD]
user32.UpdateLayeredWindow.restype = wt.BOOL
user32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.SetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_ssize_t]
user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.GetDC.argtypes = [wt.HWND]
user32.GetDC.restype = wt.HDC
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.DestroyWindow.argtypes = [wt.HWND]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.SetWindowDisplayAffinity.argtypes = [wt.HWND, wt.DWORD]
user32.ReleaseCapture.argtypes = []
user32.LoadCursorW.argtypes = [wt.HINSTANCE, wt.LPVOID]
user32.LoadCursorW.restype = wt.HANDLE
user32.SetCursor.argtypes = [wt.HANDLE]
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
user32.SetCapture.argtypes = [wt.HWND]
user32.SetCapture.restype = wt.HWND
user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateDIBSection.argtypes = [wt.HDC, ctypes.c_void_p, wt.UINT, ctypes.POINTER(ctypes.c_void_p),
                                   wt.HANDLE, wt.DWORD]
gdi32.CreateDIBSection.restype = wt.HBITMAP
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetModuleHandleW.restype = wt.HMODULE


def screen_size() -> tuple:
    return user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)


class LayeredWindow:
    """Saf Win32 katmanlı pencere. Mesajları Tk'nın olay döngüsü (aynı thread) dağıtır."""

    _class_registered = False
    _CLASS = "MakineLiveSubtitle"

    def __init__(self, on_moved=None):
        self.on_moved = on_moved
        self.draggable = True
        self._proc = WNDPROC(self._wndproc)    # referans tutulmalı (GC)
        hinst = kernel32.GetModuleHandleW(None)
        if not LayeredWindow._class_registered:
            wc = WNDCLASSEXW()
            wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
            wc.lpfnWndProc = self._proc
            wc.hInstance = hinst
            wc.lpszClassName = self._CLASS
            if not user32.RegisterClassExW(ctypes.byref(wc)):
                raise OSError("RegisterClassExW failed")
            LayeredWindow._class_registered = True
            LayeredWindow._shared_proc_owner = self
        LayeredWindow._instance = self
        ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE
        self.hwnd = user32.CreateWindowExW(ex, self._CLASS, "Makine Live altyazı", WS_POPUP,
                                           0, 0, 1, 1, None, None, hinst, None)
        if not self.hwnd:
            raise OSError("CreateWindowExW failed")
        self.capture_visible = False
        self.set_capture_visible(False)
        self._screen_dc = user32.GetDC(None)
        self._mem_dc = gdi32.CreateCompatibleDC(self._screen_dc)
        self._bitmap = None
        self._old_bitmap = None
        self._bits = ctypes.c_void_p()
        self._size = (0, 0)
        self._pos = (0, 0)
        self._visible = False
        self._click_through = False
        self._drag = None            # (imleç x, imleç y, pencere x, pencere y) sürüklerken
        self.moved = None            # bırakılan konum (left, top); Tk tarafı take_moved() ile alır

    def set_capture_visible(self, visible: bool) -> bool:
        """
        Kutu ekran yakalamada (OBS ekran yakalama, Discord / Teams ekran paylaşımı) görünsün mü?
        Görünmezken (WDA_EXCLUDEFROMCAPTURE) OCR'ın ekran kopyası da kutuyu görmez; görünür yapmak yalnızca okuma
        pencerenin kendi içeriğinden yapılırken güvenlidir (konsol modu, "yayında göster"). Ana iş parçacığından çağrılır.
        """
        visible = bool(visible)
        try:
            ok = bool(user32.SetWindowDisplayAffinity(self.hwnd, WDA_NONE if visible else WDA_EXCLUDEFROMCAPTURE))
        except OSError:
            ok = False
        if ok:
            self.capture_visible = visible
        return ok

    @property
    def dragging(self) -> bool:
        return self._drag is not None

    def take_moved(self):
        """Sürükleme bittiyse bırakılan konumu bir kez döndürür (Tk zamanlayıcısından çağrılır)."""
        m, self.moved = self.moved, None
        return m

    # Tek pencere sınıfı kullanıldığı için mesajlar etkin örneğe yönlendirilir.
    @staticmethod
    def _dispatch(hwnd, msg, wparam, lparam):
        inst = getattr(LayeredWindow, "_instance", None)
        if inst is not None and inst.hwnd == hwnd:
            return inst._handle(hwnd, msg, wparam, lparam)
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _wndproc(self, hwnd, msg, wparam, lparam):
        try:
            return LayeredWindow._dispatch(hwnd, msg, wparam, lparam)
        except Exception:   # Python istisnası pencere yordamından dışarı sızmamalı
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _handle(self, hwnd, msg, wparam, lparam):
        # DİKKAT: burada Tk / Tkinter çağrısı yapılmaz (modül açıklamasına bakın).
        if msg == WM_NCHITTEST:
            return HTCLIENT if self.draggable else HTTRANSPARENT
        if msg == WM_MOUSEACTIVATE:
            return MA_NOACTIVATE
        if msg == WM_SETCURSOR and self.draggable:
            user32.SetCursor(user32.LoadCursorW(None, ctypes.c_void_p(IDC_SIZEALL)))
            return 1
        if msg == WM_LBUTTONDOWN and self.draggable:
            pt = wt.POINT()
            user32.GetCursorPos(ctypes.byref(pt))
            self._drag = (pt.x, pt.y, self._pos[0], self._pos[1])
            user32.SetCapture(hwnd)
            return 0
        if msg == WM_MOUSEMOVE and self._drag is not None:
            pt = wt.POINT()
            user32.GetCursorPos(ctypes.byref(pt))
            cx, cy, wx, wy = self._drag
            x, y = wx + pt.x - cx, wy + pt.y - cy
            user32.SetWindowPos(hwnd, None, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
            self._pos = (x, y)
            return 0
        if msg in (WM_LBUTTONUP, WM_CAPTURECHANGED) and self._drag is not None:
            self._drag = None
            if msg == WM_LBUTTONUP:
                user32.ReleaseCapture()
            self.moved = self._pos      # Tk tarafı alır; buradan geri çağrı yok
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _ensure_bitmap(self, w: int, h: int):
        if self._bitmap and self._size == (w, h):
            return
        if self._bitmap:
            gdi32.SelectObject(self._mem_dc, self._old_bitmap)
            gdi32.DeleteObject(self._bitmap)
        bmi = BITMAPINFOHEADER()
        bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.biWidth = w
        bmi.biHeight = -h       # yukarıdan aşağı
        bmi.biPlanes = 1
        bmi.biBitCount = 32
        bmi.biCompression = BI_RGB
        self._bitmap = gdi32.CreateDIBSection(self._mem_dc, ctypes.byref(bmi), DIB_RGB_COLORS,
                                              ctypes.byref(self._bits), None, 0)
        self._old_bitmap = gdi32.SelectObject(self._mem_dc, self._bitmap)
        self._size = (w, h)

    def upload(self, premult: np.ndarray):
        """Premultiplied BGRA tamponu DIB'e kopyalar (pencere bir sonraki present'te gösterir)."""
        h, w = premult.shape[:2]
        self._ensure_bitmap(w, h)
        data = np.clip(premult + 0.5, 0, 255).astype(np.uint8).tobytes()
        ctypes.memmove(self._bits, data, len(data))

    def present(self, x: int, y: int, alpha: float = 1.0):
        """Son yüklenen görüntüyü (x, y) konumunda, sabit alfa ile gösterir."""
        if not self._bitmap:
            return
        if self._drag is not None:
            x, y = self._pos             # kullanıcı sürüklerken kutu yerinden zıplamasın
        w, h = self._size
        pt_dst = wt.POINT(int(x), int(y))
        size = wt.SIZE(w, h)
        pt_src = wt.POINT(0, 0)
        blend = BLENDFUNCTION(AC_SRC_OVER, 0, max(0, min(255, round(alpha * 255))), AC_SRC_ALPHA)
        user32.UpdateLayeredWindow(self.hwnd, self._screen_dc, ctypes.byref(pt_dst), ctypes.byref(size),
                                   self._mem_dc, ctypes.byref(pt_src), 0, ctypes.byref(blend), ULW_ALPHA)
        self._pos = (int(x), int(y))
        if not self._visible:
            user32.ShowWindow(self.hwnd, SW_SHOWNOACTIVATE)
            user32.SetWindowPos(self.hwnd, ctypes.c_void_p(HWND_TOPMOST), 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            self._visible = True

    def hide(self):
        if self._visible:
            user32.ShowWindow(self.hwnd, SW_HIDE)
            self._visible = False

    @property
    def visible(self):
        return self._visible

    @property
    def size(self):
        return self._size

    def set_click_through(self, enable: bool):
        self._click_through = enable
        self.draggable = not enable
        style = user32.GetWindowLongPtrW(self.hwnd, GWL_EXSTYLE)
        if enable:
            style |= WS_EX_TRANSPARENT
        else:
            style &= ~WS_EX_TRANSPARENT
        user32.SetWindowLongPtrW(self.hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED | WS_EX_NOACTIVATE)

    def ex_style(self) -> int:
        return user32.GetWindowLongPtrW(self.hwnd, GWL_EXSTYLE)

    def destroy(self):
        if self._bitmap:
            gdi32.SelectObject(self._mem_dc, self._old_bitmap)
            gdi32.DeleteObject(self._bitmap)
            self._bitmap = None
        if self._mem_dc:
            gdi32.DeleteDC(self._mem_dc)
            self._mem_dc = None
        if self._screen_dc:
            user32.ReleaseDC(None, self._screen_dc)
            self._screen_dc = None
        if self.hwnd:
            user32.DestroyWindow(self.hwnd)
            self.hwnd = None


# ─────────────────────────────────────────────────────────────
# 3. Durum makinesi
# ─────────────────────────────────────────────────────────────

def _ease_out(t: float) -> float:
    return 1 - (1 - t) ** 3


class SubtitleOverlay:
    """
    Oyun içi çeviri kutusu (yalnızca gösterim).

    Davranış
      - present_line(text, kind): kutu görünmüyorsa 160 ms solarak ve 2 px yükselerek gelir; görünüyorsa
        eski satırla 160 ms çapraz geçiş yapılır (premultiplied karışım). Kutuda yalnızca çeviri vardır.
      - end_line(): 220 ms'de solar (taşıma modunda yer tutucuya döner).
      - Ne zaman gösterilip gizleneceğine subtitle_timing.SubtitleScheduler karar verir; kutunun
        kendi zamanlayıcısı yoktur.
      - Konum: "alt" (alt orta, ekranın %8'i yukarıda), "üst", ya da sürüklenen serbest nokta.
        Kutu her zaman ekranın içinde kalır (uzun metin üst kenardan taşmaz).
    """

    FADE_MS = int(_OV.get("fadeMs", 160))
    CROSS_MS = 130        # satırdan satıra: kısa tut, yeni satır beklemesin
    FADE_OUT_MS = 220
    FRAME_MS = 16
    RISE_PX = 6           # belirirken bu kadar aşağıdan gelir

    def __init__(self, tk_root, on_moved=None):
        self.root = tk_root
        self.on_moved_cb = on_moved
        self.win = LayeredWindow(on_moved=self._on_moved)
        sw, sh = screen_size()
        self.screen = (sw, sh)
        self.width_ratio = float(_OV.get("maxWidthRatio", 0.62))
        self.offset = 0.08                 # alt / üst kenardan uzaklık (görüntü yüksekliğinin oranı)
        self.style = OverlayStyle(max_width=round(sw * self.width_ratio))
        self.position = "alt"
        self.custom_anchor = None        # (cx, bottom)
        self.frame = None                # (sol, üst, sağ, alt): konsol görüntüsünün ekrandaki alanı; None = ekran
        self.locked = False

        self._content = OverlayContent()
        self._rendered: Rendered | None = None
        self._prev: Rendered | None = None
        self._anim_job = None
        self._anim = None                 # ("in"|"cross"|"out", t0, duration)
        self._shown = False
        self._render_cache = None
        self._poll_moved()

    def _poll_moved(self):
        """Sürüklenip bırakılan konumu pencere yordamı yerine Tk'nın kendi döngüsünde işler."""
        try:
            m = self.win.take_moved() if self.win.hwnd else None
            if m:
                self._on_moved(*m)
        finally:
            if self.win.hwnd:
                self.root.after(120, self._poll_moved)

    # ── Ayarlar ──

    def configure_look(self, style=None, font_px=None, opacity=None, position=None, custom_anchor=None,
                       font=None, text_rgb=None, band_rgb=None, outline=None, shadow=None, max_lines=None,
                       width_ratio=None, offset=None):
        kw = {}
        if style in STYLES:
            kw["style"] = style
        if font_px is not None:
            kw["font_px"] = int(font_px)
        if opacity is not None:
            kw["opacity"] = float(opacity)
        if font in FONTS:
            kw["font"] = font
        if text_rgb is not None:
            kw["text_rgb"] = tuple(text_rgb)
        if band_rgb is not None:
            kw["band_rgb"] = tuple(band_rgb)
        if outline is not None:
            kw["outline"] = float(outline)
        if shadow is not None:
            kw["shadow"] = bool(shadow)
        if max_lines is not None:
            kw["max_lines"] = int(max_lines)
        if width_ratio is not None:
            self.width_ratio = float(width_ratio)
            width = (self.frame[2] - self.frame[0]) if self.frame else self.screen[0]
            kw["max_width"] = round(width * self.width_ratio)
        if offset is not None:
            self.offset = float(offset)
        if kw:
            self.style = replace(self.style, **kw)
        if position in ("alt", "ust", "serbest"):
            self.position = position
        if custom_anchor is not None:
            self.custom_anchor = tuple(custom_anchor)
        if self._shown:
            self._rerender(keep_visible=True)

    def set_frame(self, frame):
        """
        Alt / üst konumu ekran yerine bu dikdörtgene göre hesaplanır (konsol modunda Remote Play / yakalama penceresinin
        16:9 görüntüsü: pencere tam ekran değilse de altyazı görüntünün üstünde durur). None → ekran.
        """
        frame = tuple(int(v) for v in frame) if frame else None
        if frame and (frame[2] - frame[0] < 200 or frame[3] - frame[1] < 120):
            frame = None
        if frame == self.frame:
            return
        self.frame = frame
        width = (frame[2] - frame[0]) if frame else self.screen[0]
        self.style = replace(self.style, max_width=round(width * self.width_ratio))
        if self._shown:
            self._rerender(keep_visible=True)

    def set_capture_visible(self, visible: bool) -> bool:
        return self.win.set_capture_visible(visible)

    def anchor(self) -> tuple:
        if self.position == "serbest" and self.custom_anchor:
            return self.custom_anchor[0], self.custom_anchor[1], "bottom"
        l, t, r, b = self.frame or (0, 0, *self.screen)
        cx, h = (l + r) // 2, b - t
        if self.position == "ust":
            return cx, t + round(h * max(0.02, self.offset - 0.02)), "top"
        return cx, b - round(h * self.offset), "bottom"

    def _origin_for(self, r: Rendered) -> tuple:
        cx, y, mode = self.anchor()
        bx0, by0, bx1, by1 = r.band_box
        x = cx - (bx0 + bx1) // 2
        y = y - (by1 if mode == "bottom" else by0)
        # Ekranın içinde tut (gölge kenar boşluğu taşabilir, bant taşmaz)
        sw, sh = self.screen
        x = min(max(x, -bx0), sw - bx1)
        y = min(max(y, -by0), sh - by1)
        return x, y

    # ── Dışarıya açık API ──

    def present_line(self, text: str, kind: str = "api"):
        """Çeviriyi göster. kind: pending (havuz, onaysız) | verified | api"""
        if not text:
            return
        self._set_content(OverlayContent(text=text, kind=kind))

    def end_line(self):
        """Satırın süresi doldu: sol (taşıma modunda yer tutucu kalır)."""
        if self._content.placeholder or not self._content.text:
            return
        if self.locked:
            self._content = OverlayContent()
            self.hide()
        else:
            self._set_content(OverlayContent(placeholder=True))

    @property
    def showing_line(self) -> bool:
        return self._shown and bool(self._content.text)

    def show(self):
        """Taşıma modunda boş yer tutucuyu gösterir."""
        if not self._shown:
            self._set_content(self._content if self._content.text else OverlayContent(placeholder=True))

    def hide(self):
        if not self._shown:
            return
        self._start_anim("out", self.FADE_OUT_MS)

    def clear(self):
        self._content = OverlayContent()
        self.hide()

    def set_click_through(self, enable: bool):
        self.locked = enable
        self.win.set_click_through(enable)
        if enable and (self._content.placeholder or not self._content.text):
            self.hide()
        elif self._shown:
            self._rerender(keep_visible=True)

    def destroy(self):
        if self._anim_job:
            try:
                self.root.after_cancel(self._anim_job)
            except Exception:
                pass
        self.win.destroy()

    def render_preview(self, content: OverlayContent, max_width: int) -> Image.Image:
        """Panel önizlemesi için aynı çizici (kenar boşlukları kırpılmış)."""
        style = replace(self.style, max_width=max_width)
        return render_overlay(content, style).image

    # ── İç işleyiş ──

    def _on_moved(self, left, top):
        if not self._rendered:
            return
        bx0, by0, bx1, by1 = self._rendered.band_box
        self.custom_anchor = (left + (bx0 + bx1) // 2, top + by1)
        self.position = "serbest"
        if self.on_moved_cb:
            self.on_moved_cb(self.custom_anchor)

    def _render(self, content: OverlayContent) -> Rendered:
        chrome = not self.locked
        key = (content, self.style, chrome)
        if self._render_cache and self._render_cache[0] == key:
            return self._render_cache[1]
        r = render_overlay(content, self.style, chrome=chrome)
        self._render_cache = (key, r)
        return r

    def _set_content(self, content: OverlayContent):
        if self.locked and content.placeholder:
            self._content = content
            self.hide()
            return
        self._content = content
        new = self._render(content)
        fading_out = self._anim is not None and self._anim[0] == "out"
        if self._shown and self._rendered is not None and not fading_out:
            if new is self._rendered:
                return
            self._prev = self._rendered
            self._rendered = new
            self._start_anim("cross", self.CROSS_MS)
        else:
            self._prev = None
            self._rendered = new
            self._start_anim("in", self.FADE_MS)

    def _rerender(self, keep_visible=False):
        self._rendered = self._render(self._content)
        if self._anim is None and keep_visible:
            self._present_static()

    def _present_static(self, alpha: float = 1.0, dy: int = 0):
        r = self._rendered
        self.win.upload(r.premult)
        x, y = self._origin_for(r)
        self.win.present(x, y + dy, alpha)

    def _start_anim(self, kind: str, duration: int):
        if self._anim_job:
            self.root.after_cancel(self._anim_job)
            self._anim_job = None
        if kind == "out" and not self._shown:
            return
        self._anim = (kind, time.perf_counter(), duration)
        if kind == "cross" and self._prev is not None:
            self._cross_frames = self._prepare_cross(self._prev, self._rendered)
        elif kind == "in":
            # Belirme yalnızca pencerenin sabit alfası ve konumuyla yapılır: görüntü bir kez yüklenir
            # (eskiden her karede bütün tampon numpy'den yeniden kopyalanıyordu)
            self.win.upload(self._rendered.premult)
        self._shown = True
        self._tick()

    def _prepare_cross(self, a: Rendered, b: Rendered):
        """İki görüntüyü bant çapasına göre hizalayıp aynı boyutlu tamponlara yerleştirir."""
        ax, ay = self._origin_for(a)
        bxo, byo = self._origin_for(b)
        x0, y0 = min(ax, bxo), min(ay, byo)
        x1 = max(ax + a.image.width, bxo + b.image.width)
        y1 = max(ay + a.image.height, byo + b.image.height)
        W, H = x1 - x0, y1 - y0
        A = np.zeros((H, W, 4), np.float32)
        B = np.zeros((H, W, 4), np.float32)
        A[ay - y0:ay - y0 + a.image.height, ax - x0:ax - x0 + a.image.width] = a.premult
        B[byo - y0:byo - y0 + b.image.height, bxo - x0:bxo - x0 + b.image.width] = b.premult
        return A, B, (x0, y0), (bxo, byo)

    def _tick(self):
        self._anim_job = None
        if not self._anim:
            return
        kind, t0, dur = self._anim
        t = min(1.0, (time.perf_counter() - t0) * 1000 / dur)
        e = _ease_out(t)
        if kind == "in":
            x, y = self._origin_for(self._rendered)
            self.win.present(x, y + round(self.RISE_PX * (1 - e)), e)
        elif kind == "out":
            x, y = self.win._pos
            self.win.present(x, y, alpha=1 - e)
        elif kind == "cross":
            A, B, (x0, y0), _ = self._cross_frames
            self.win.upload(A + (B - A) * e)
            self.win.present(x0, y0, 1.0)
        if t < 1.0:
            self._anim_job = self.root.after(self.FRAME_MS, self._tick)
            return
        # animasyon bitti
        self._anim = None
        self._prev = None
        if kind == "out":
            self.win.hide()
            self._shown = False
        else:
            self._present_static()
