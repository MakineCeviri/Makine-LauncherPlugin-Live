#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Konsol modu [TEST→C++]: PS5 / Xbox oyunları için okuma kaynağı.

Konsol HDMI ile TV'ye bağlı kalır, oyuncu orada oynar. Görüntünün bir kopyası PC'de bir pencerede açılır:
  - PS Remote Play / chiaki (PS5; ikinci, ücretsiz bir PSN hesabıyla bağlanılması önerilir: kendi hesabınla
    bağlanırsan kolun konsoldan kopar),
  - Xbox uygulamasının uzaktan oynatma penceresi / Greenlight (Xbox),
  - yakalama kartının önizlemesi (Elgato 4K Capture Utility, OBS pencereli projektör, Windows Kamera).
Bu modül o pencereyi bulur ve game_detector.TargetWindow olarak verir; böylece okuma, havuz ve çeviri akışı PC
oyunlarındakiyle aynıdır. "Yalnızca oyunlar" kuralı bozulmaz: bu kaynaklar yalnızca kullanıcı konsol modunu açınca
hedef olur.

Bölge, pencerenin 16:9 video alanına göre ORANSAL tutulur: pencere taşınsa ya da büyüse de altyazı alanı kaymaz.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import os
import re
from dataclasses import dataclass
from typing import Iterable, List, Optional, Tuple

from makine_live.games.game_detector import (TargetWindow, _top_level_windows, _is_cloaked, _class_name, window_pid, process_path,
                                             window_title, _u32)
from makine_live.games.game_library import GameEntry

# ─────────────────────────────────────────────────────────────
# Kaynak uygulamalar
# ─────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class SourceApp:
    key: str
    label: str
    console: str                     # ps5 | xbox | capture
    exes: Tuple[str, ...]            # küçük harf exe adları
    title: str = ""                  # pencere başlığı deseni (regex, büyük/küçük harf duyarsız); boş = her başlık


SOURCES: Tuple[SourceApp, ...] = (
    SourceApp("ps-remote-play", "PS Remote Play", "ps5", ("remoteplay.exe",)),
    SourceApp("chiaki", "chiaki", "ps5", ("chiaki.exe", "chiaki-ng.exe")),
    SourceApp("xbox-app", "Xbox uygulaması", "xbox", ("xboxpcapp.exe",)),
    SourceApp("greenlight", "Greenlight", "xbox", ("greenlight.exe",)),
    SourceApp("elgato", "Elgato 4K Capture Utility", "capture", ("4kcaptureutility.exe", "gamecapture.exe")),
    SourceApp("obs-projector", "OBS projektör", "capture", ("obs64.exe", "obs32.exe"), r"project|projekt"),
    SourceApp("windows-camera", "Windows Kamera", "capture", ("applicationframehost.exe", "windowscamera.exe"),
              r"^(camera|kamera)$"),
)
SOURCE_BY_KEY = {s.key: s for s in SOURCES}
CONSOLE_LABELS = {"ps5": "PS5", "xbox": "Xbox", "capture": "yakalama kartı", "custom": "seçilen pencere"}

# Okunan pencere en az bu boyutta olmalı: daha küçük görüntüde altyazı OCR için fazla küçülür
MIN_SOURCE_SIZE = (960, 540)

# ─────────────────────────────────────────────────────────────
# Oyun: konsol modu yalnızca GTA VI içindir
# ─────────────────────────────────────────────────────────────

# Konsolda exe olmadığı için oyun pencereden anlaşılamaz. Kullanıcı kararı: konsol modundaki her satır GTA VI havuzuna
# gider (oyun seçimi yok). Böylece sunucunun DeepL kotası tek bir oyunun havuzunu doldurmaya harcanır ve oyuncular
# birbirinin onayladığı satırlardan yararlanır.
DEFAULT_GAME = "Grand Theft Auto VI"
CONSOLE_SLUG = "grand-theft-auto-vi"


def console_game() -> GameEntry:
    return GameEntry(DEFAULT_GAME, "console", slug=CONSOLE_SLUG)


# ─────────────────────────────────────────────────────────────
# Pencere geometrisi
# ─────────────────────────────────────────────────────────────

_u32.GetClientRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.wintypes.RECT)]
_u32.ClientToScreen.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.wintypes.POINT)]


def client_rect(hwnd) -> Tuple[int, int, int, int]:
    """İstemci alanı ekran koordinatlarında (sol, üst, sağ, alt): başlık çubuğu ve kenarlık hariç."""
    r = ctypes.wintypes.RECT()
    if not _u32.GetClientRect(hwnd, ctypes.byref(r)):
        return 0, 0, 0, 0
    p = ctypes.wintypes.POINT(0, 0)
    _u32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y, p.x + r.right, p.y + r.bottom


def video_rect(client: Tuple[int, int, int, int], aspect: float = 16 / 9) -> Tuple[int, int, int, int]:
    """İstemci alanına ortalanmış en büyük 16:9 dikdörtgen (siyah bantlar dışarıda kalır)."""
    l, t, r, b = client
    w, h = r - l, b - t
    if w <= 0 or h <= 0:
        return l, t, l, t
    if w / h > aspect:                  # yanlarda bant
        vw = round(h * aspect)
        x = l + (w - vw) // 2
        return x, t, x + vw, b
    vh = round(w / aspect)              # üstte / altta bant
    y = t + (h - vh) // 2
    return l, y, r, y + vh


_gdi = ctypes.windll.gdi32
_u32.PrintWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
_u32.GetDC.argtypes = [ctypes.c_void_p]
_u32.GetDC.restype = ctypes.c_void_p
_u32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_gdi.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
_gdi.CreateCompatibleDC.restype = ctypes.c_void_p
_gdi.CreateDIBSection.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.c_void_p, ctypes.c_uint]
_gdi.CreateDIBSection.restype = ctypes.c_void_p
_gdi.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_gdi.SelectObject.restype = ctypes.c_void_p
_gdi.DeleteObject.argtypes = [ctypes.c_void_p]
_gdi.DeleteDC.argtypes = [ctypes.c_void_p]
PW_CLIENTONLY_FULL = 0x1 | 0x2          # PW_CLIENTONLY | PW_RENDERFULLCONTENT (DLL'in captureWindow'u ile aynı)


def window_has_image(hwnd) -> bool:
    """
    Pencerenin kendi içeriği (PrintWindow) okunabiliyor mu? DLL hwnd verilince bu yolla okur. Kenarlıksız / pencereli
    DirectX oyunlarının çoğunda çalışır; özel tam ekran ya da bazı Vulkan / OpenGL oyunlarında görüntü siyah gelir.
    Siyah değilse (seyrek örneklemede belirgin parlaklık ve çeşitlilik varsa) True.
    """
    l, t, r, b = client_rect(hwnd)
    w, h = r - l, b - t
    if w < 64 or h < 64:
        return False

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32), ("biHeight", ctypes.c_int32),
                    ("biPlanes", ctypes.c_uint16), ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32), ("biClrImportant", ctypes.c_uint32)]

    bmi = BMIH(ctypes.sizeof(BMIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    screen = _u32.GetDC(None)
    mem = _gdi.CreateCompatibleDC(screen)
    bits = ctypes.c_void_p()
    bmp = _gdi.CreateDIBSection(screen, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    try:
        if not mem or not bmp or not bits.value:
            return False
        old = _gdi.SelectObject(mem, bmp)
        ok = _u32.PrintWindow(hwnd, mem, PW_CLIENTONLY_FULL)
        _gdi.SelectObject(mem, old)
        if not ok:
            return False
        buf = (ctypes.c_uint8 * (w * h * 4)).from_address(bits.value)
        step = max(4, (w * h) // 4096) * 4                 # ~4000 piksel örneği
        sample = memoryview(buf).cast("B")[::step].tobytes()   # B, G, R, A karışık gelir: yalnızca istatistik
        bright = sum(1 for v in sample if v > 24)
        return bright > len(sample) * 0.02
    finally:
        if bmp:
            _gdi.DeleteObject(bmp)
        if mem:
            _gdi.DeleteDC(mem)
        _u32.ReleaseDC(None, screen)


# Varsayılan altyazı alanı: alt orta (16:9 görüntünün oranı olarak x, y, genişlik, yükseklik)
DEFAULT_REGION = (0.12, 0.70, 0.76, 0.26)


def region_to_screen(video: Tuple[int, int, int, int], frac) -> Tuple[int, int, int, int]:
    """Oransal bölge → ekran (x, y, w, h)."""
    l, t, r, b = video
    fx, fy, fw, fh = frac
    vw, vh = r - l, b - t
    x, y = l + round(fx * vw), t + round(fy * vh)
    return x, y, max(1, round(fw * vw)), max(1, round(fh * vh))


def screen_to_region(video: Tuple[int, int, int, int], rect) -> Tuple[float, float, float, float]:
    """Ekran (x, y, w, h) → video alanına göre oran; video dışına taşan kısım kırpılır."""
    l, t, r, b = video
    vw, vh = max(1, r - l), max(1, b - t)
    x, y, w, h = rect
    x0, y0 = min(max(x, l), r), min(max(y, t), b)
    x1, y1 = min(max(x + w, l), r), min(max(y + h, t), b)
    return (round((x0 - l) / vw, 4), round((y0 - t) / vh, 4),
            round(max(0, x1 - x0) / vw, 4), round(max(0, y1 - y0) / vh, 4))


def valid_region(frac) -> bool:
    try:
        fx, fy, fw, fh = (float(v) for v in frac)
    except (TypeError, ValueError):
        return False
    return 0 <= fx < 1 and 0 <= fy < 1 and 0.02 <= fw <= 1 and 0.02 <= fh <= 1 and fx + fw <= 1.001 and fy + fh <= 1.001


# ─────────────────────────────────────────────────────────────
# Kaynak bulma
# ─────────────────────────────────────────────────────────────


class ConsoleTarget(TargetWindow):
    """Konsol görüntüsünün açık olduğu pencere + kullanıcının seçtiği oyun."""

    def __init__(self, hwnd, pid: int, path: str, game: GameEntry, source: SourceApp):
        super().__init__(hwnd, pid, path, game)
        self.source = source
        self.store = "console"

    def video(self) -> Tuple[int, int, int, int]:
        """16:9 video alanı, ekran koordinatı (bölge seçimi ve çerçeve çizimi için)."""
        return video_rect(client_rect(self.hwnd))

    def client_region(self, frac) -> Tuple[int, int, int, int]:
        """Oransal bölge → pencerenin istemci koordinatı (x, y, w, h). DLL pencerenin kendi içeriğini okur."""
        l, t, r, b = client_rect(self.hwnd)
        return region_to_screen(video_rect((0, 0, r - l, b - t)), frac)

    def size(self) -> Tuple[int, int]:
        l, t, r, b = client_rect(self.hwnd)
        return r - l, b - t

    def __repr__(self):
        return f"ConsoleTarget({self.name!r}, {self.source.label}, {self.exe}, pid={self.pid})"


def match_source(exe: str, title: str, custom_exes: Iterable[str] = ()) -> Optional[SourceApp]:
    """Pencerenin exe adı ve başlığından kaynak uygulaması (yoksa None)."""
    exe = (exe or "").lower()
    for s in SOURCES:
        if exe in s.exes and (not s.title or re.search(s.title, title or "", re.IGNORECASE)):
            return s
    if exe and exe in {e.lower() for e in custom_exes}:
        return SourceApp("custom", os.path.splitext(exe)[0], "custom", (exe,))
    return None


def _source_window(hwnd) -> bool:
    if not _u32.IsWindowVisible(hwnd) or _is_cloaked(hwnd):
        return False
    if _class_name(hwnd) in ("Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Progman", "WorkerW"):
        return False
    l, t, r, b = client_rect(hwnd)
    # Küçültülmüş pencere de kaynak sayılır (kullanıcıya "pencereyi açın" denir); küçük açılır pencereler sayılmaz
    return bool(_u32.IsIconic(hwnd)) or (r - l >= 320 and b - t >= 200)


def find_console_source(ignore_pids=(), prefer: str = "",
                        custom_exes: Iterable[str] = ()) -> Optional[ConsoleTarget]:
    """
    Açık konsol kaynağını bulur. Birden fazla varsa: kullanıcının son kullandığı (prefer) > z-sırasında en üstteki.
    Oyun her zaman GTA VI'dır (console_game).
    """
    ignore = set(ignore_pids)
    found: List[Tuple[int, ConsoleTarget]] = []
    seen = set()
    custom = tuple(custom_exes)
    for hwnd in _top_level_windows():
        if not _source_window(hwnd):
            continue
        pid = window_pid(hwnd)
        if not pid or pid in ignore or pid in seen:
            continue
        path = process_path(pid)
        src = match_source(os.path.basename(path), window_title(hwnd), custom)
        if src is None:
            continue
        seen.add(pid)
        found.append((0 if prefer and src.key == prefer else 1, ConsoleTarget(hwnd, pid, path, console_game(), src)))
    if not found:
        return None
    found.sort(key=lambda kv: kv[0])     # sabit sıralama: aynı öncelikte z-sırası korunur
    return found[0][1]


def console_block_reason(target: Optional[ConsoleTarget]) -> Optional[str]:
    """Konsol kaynağına özgü bekleme sebepleri; None → game_detector.capture_block_reason ile devam."""
    if target is None:
        return "konsol görüntüsü bulunamadı · Remote Play ya da yakalama penceresini açın"
    if not target.alive():
        return f"{target.source.label} kapandı"
    if _u32.IsIconic(target.hwnd):
        return f"{target.source.label} penceresi küçültülmüş · okunabilmesi için ekranda açık kalmalı"
    w, h = target.size()
    if w < MIN_SOURCE_SIZE[0] or h < MIN_SOURCE_SIZE[1]:
        return f"{target.source.label} penceresi küçük ({w}×{h}) · en az {MIN_SOURCE_SIZE[0]}×{MIN_SOURCE_SIZE[1]} yapın"
    return None


if __name__ == "__main__":
    t = find_console_source({os.getpid()})
    print("kaynak:", t or "yok")
    if t:
        print("istemci:", client_rect(t.hwnd), "video:", t.video(), "bölge:", region_to_screen(t.video(), DEFAULT_REGION))
        print("durum:", console_block_reason(t) or "okunabilir")
