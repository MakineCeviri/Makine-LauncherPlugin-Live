"""
Makine Live — aktif oyun algılama ve hedef pencere kilidi.

[TEST→C++] Yalnızca OYUNLAR algılanır. Bir pencere ancak süreci kurulu oyun kütüphanesinde (game_library.GameLibrary:
Steam + mağaza türleri, Epic, GOG, Ubisoft, EA, Xbox, Windows oyun listesi, bilinen exe adları, kullanıcı işaretleri)
oyun olarak tanınıyorsa hedef olur. Tahmin ya da "öndeki pencere" yedeği yoktur: tarayıcı, müzik çalar, IDE asla
algılanmaz. Oyun bulunamazsa hedef boştur ve hiçbir şey okunmaz.
"""

import ctypes
import ctypes.wintypes
import os
import re
import threading
from typing import Dict, List, Optional, Tuple

from game_library import GameEntry, GameLibrary, steam_running_appid

# Bilinen oyun exe adları (mağaza bilgisi bulunamazsa son kaynak) ve havuzdaki sabit slug'ları
KNOWN_GAMES: Dict[str, Tuple[str, str]] = {
    # exe_name_lower: (Friendly Name, Slug)
    "heavyrain.exe": ("Heavy Rain", "heavy-rain"),
    "detroitbecomehuman.exe": ("Detroit: Become Human", "detroit-become-human"),
    "beyondtwosouls.exe": ("Beyond: Two Souls", "beyond-two-souls"),
    "cyberpunk2077.exe": ("Cyberpunk 2077", "cyberpunk-2077"),
    "witcher3.exe": ("The Witcher 3: Wild Hunt", "witcher-3"),
    "rdr2.exe": ("Red Dead Redemption 2", "red-dead-redemption-2"),
    "starfield.exe": ("Starfield", "starfield"),
    "eldenring.exe": ("Elden Ring", "elden-ring"),
    "bg3.exe": ("Baldur's Gate 3", "baldurs-gate-3"),
    "bg3_dx11.exe": ("Baldur's Gate 3", "baldurs-gate-3"),
    "hogwartslegacy.exe": ("Hogwarts Legacy", "hogwarts-legacy"),
    "godofwar.exe": ("God of War", "god-of-war"),
    "spiderman.exe": ("Marvel's Spider-Man", "marvels-spider-man"),
    "ghostoftsushima.exe": ("Ghost of Tsushima", "ghost-of-tsushima"),
    "alanwake2.exe": ("Alan Wake 2", "alan-wake-2"),
    "deathstranding.exe": ("Death Stranding", "death-stranding"),
    "skyrimse.exe": ("Skyrim Special Edition", "skyrim"),
    "tesv.exe": ("The Elder Scrolls V: Skyrim", "skyrim"),
    "fallout4.exe": ("Fallout 4", "fallout-4"),
    "gta5.exe": ("Grand Theft Auto V", "gta-5"),
    "tlou-i.exe": ("The Last of Us Part I", "the-last-of-us"),
    "re4.exe": ("Resident Evil 4", "resident-evil-4"),
    "re2.exe": ("Resident Evil 2", "resident-evil-2"),
    "re3.exe": ("Resident Evil 3", "resident-evil-3"),
    "re8.exe": ("Resident Evil Village", "resident-evil-village"),
    "masseffect1.exe": ("Mass Effect", "mass-effect"),
    "masseffect2.exe": ("Mass Effect 2", "mass-effect-2"),
    "masseffect3.exe": ("Mass Effect 3", "mass-effect-3"),
    "disco.exe": ("Disco Elysium", "disco-elysium"),
    "p5r.exe": ("Persona 5 Royal", "persona-5-royal"),
    "p3r.exe": ("Persona 3 Reload", "persona-3-reload"),
    "sandfall-win64-shipping.exe": ("Clair Obscur: Expedition 33", "expedition-33"),
    "avowed.exe": ("Avowed", "avowed"),
    "stalker2-win64-shipping.exe": ("S.T.A.L.K.E.R. 2: Heart of Chornobyl", "stalker-2"),
}


# ─────────────────────────────────────────────────────────────
# Kütüphane (tek örnek; ilk kullanımda arka planda taranır)
# ─────────────────────────────────────────────────────────────

def data_dir() -> str:
    return os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "MakineLive")


_library: Optional[GameLibrary] = None
_library_lock = threading.Lock()


def get_library() -> GameLibrary:
    global _library
    with _library_lock:
        if _library is None:
            _library = GameLibrary(data_dir(), known_exes=KNOWN_GAMES)
    return _library


def set_library(lib: GameLibrary):
    """Testler ve gömülü kullanım için kütüphaneyi değiştir."""
    global _library
    with _library_lock:
        _library = lib


# ─────────────────────────────────────────────────────────────
# Win32
# ─────────────────────────────────────────────────────────────

_u32 = ctypes.windll.user32
_k32 = ctypes.windll.kernel32
_dwm = ctypes.windll.dwmapi

GW_HWNDNEXT = 2
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
DWMWA_EXTENDED_FRAME_BOUNDS = 9
DWMWA_CLOAKED = 14

_HWND = ctypes.wintypes.HWND
_u32.GetWindow.restype = _HWND
_u32.GetWindow.argtypes = [_HWND, ctypes.c_uint]
_u32.GetTopWindow.restype = _HWND
_u32.GetTopWindow.argtypes = [_HWND]
_u32.GetForegroundWindow.restype = _HWND
_u32.GetWindowLongW.restype = ctypes.c_long
_u32.GetWindowLongW.argtypes = [_HWND, ctypes.c_int]
_u32.IsWindowVisible.argtypes = [_HWND]
_u32.IsIconic.argtypes = [_HWND]
_u32.IsWindow.argtypes = [_HWND]
_u32.GetWindowThreadProcessId.argtypes = [_HWND, ctypes.POINTER(ctypes.wintypes.DWORD)]
_u32.GetWindowRect.argtypes = [_HWND, ctypes.POINTER(ctypes.wintypes.RECT)]
_u32.GetClassNameW.argtypes = [_HWND, ctypes.c_wchar_p, ctypes.c_int]
_u32.GetWindowTextW.argtypes = [_HWND, ctypes.c_wchar_p, ctypes.c_int]
_dwm.DwmGetWindowAttribute.argtypes = [_HWND, ctypes.wintypes.DWORD, ctypes.c_void_p, ctypes.wintypes.DWORD]

# Tam ekran oyun öndeyken görev çubuğu görünmez ama z-sırasında üstte kalabilir
_TASKBAR_CLASSES = {"Shell_TrayWnd", "Shell_SecondaryTrayWnd"}
# Oyun penceresi sayılmayacak kadar küçük pencereler (bildirim, açılır pencere, simge)
MIN_GAME_WINDOW = (320, 200)


def window_pid(hwnd) -> int:
    pid = ctypes.wintypes.DWORD()
    _u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def process_path(pid: int) -> str:
    """Sürecin tam exe yolu ('' = erişilemedi)."""
    h = _k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(1024)
    size = ctypes.wintypes.DWORD(1024)
    path = buf.value if _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)) else ""
    _k32.CloseHandle(h)
    return path


def process_exe(pid: int) -> str:
    return os.path.basename(process_path(pid))


def window_rect(hwnd):
    """Görünen çerçeve (DWM gölgesi hariç); olmazsa GetWindowRect."""
    r = ctypes.wintypes.RECT()
    if _dwm.DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        _u32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def window_title(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(256)
    _u32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def _is_cloaked(hwnd) -> bool:
    v = ctypes.c_int(0)
    if _dwm.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(v), ctypes.sizeof(v)) != 0:
        return False
    return v.value != 0


def _class_name(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(128)
    _u32.GetClassNameW(hwnd, buf, 128)
    return buf.value


def _intersects(a, b) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def _top_level_windows():
    """Üst düzey pencereler, z-sırasında yukarıdan aşağıya."""
    h = _u32.GetTopWindow(None)
    while h:
        yield h
        h = _u32.GetWindow(h, GW_HWNDNEXT)


def _occluder_candidate(hwnd, ignore_pids) -> bool:
    """Bölgeyi gerçekten kapatabilecek, görünür bir üst düzey pencere mi?"""
    if not _u32.IsWindowVisible(hwnd) or _u32.IsIconic(hwnd):
        return False
    if window_pid(hwnd) in ignore_pids:
        return False
    if _u32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT:
        return False                     # tıklama geçirgen katman (ör. başka overlay'ler)
    if _is_cloaked(hwnd):
        return False                     # başka sanal masaüstü / gizlenmiş UWP penceresi
    l, t, r, b = window_rect(hwnd)
    return r - l > 1 and b - t > 1


def _game_window_candidate(hwnd) -> bool:
    if not _u32.IsWindowVisible(hwnd) or _u32.IsIconic(hwnd) or _is_cloaked(hwnd):
        return False
    if _u32.GetWindowLongW(hwnd, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW):
        return False
    l, t, r, b = window_rect(hwnd)
    return r - l >= MIN_GAME_WINDOW[0] and b - t >= MIN_GAME_WINDOW[1]


# ─────────────────────────────────────────────────────────────
# Algılama
# ─────────────────────────────────────────────────────────────

class TargetWindow:
    """Kilitlenilen hedef: oyun süreci + ana penceresi."""

    def __init__(self, hwnd, pid: int, path: str, game: GameEntry):
        self.hwnd, self.pid, self.path = hwnd, pid, path
        self.exe = os.path.basename(path)
        self.game = game
        self.name, self.slug, self.store = game.name, game.slug, game.store

    def alive(self) -> bool:
        return bool(self.hwnd) and bool(_u32.IsWindow(self.hwnd)) and window_pid(self.hwnd) == self.pid

    def __repr__(self):
        return f"TargetWindow({self.name!r}, {self.exe}, pid={self.pid}, {self.store})"


def running_games(ignore_pids=(), library: Optional[GameLibrary] = None) -> List[TargetWindow]:
    """Görünür penceresi olan ve kütüphanede oyun olarak tanınan süreçler (en büyük pencere önce)."""
    lib = library or get_library()
    ignore = set(ignore_pids)
    verdicts: Dict[int, Tuple[str, Optional[GameEntry]]] = {}
    best: Dict[int, Tuple[int, object]] = {}
    for h in _top_level_windows():
        if not _game_window_candidate(h):
            continue
        pid = window_pid(h)
        if not pid or pid in ignore:
            continue
        if pid not in verdicts:
            path = process_path(pid)
            game = lib.match(path) if path else None
            verdicts[pid] = (path, game)
        if verdicts[pid][1] is None:
            continue
        l, t, r, b = window_rect(h)
        area = (r - l) * (b - t)
        if pid not in best or area > best[pid][0]:
            best[pid] = (area, h)
    out = []
    for pid, (area, h) in sorted(best.items(), key=lambda kv: -kv[1][0]):
        path, game = verdicts[pid]
        if game is not None:
            out.append(TargetWindow(h, pid, path, _host_named(game, path, h)))
    return out


# Oyunu barındıran ortak çalışma ortamları: ad, exe'den değil pencere başlığından gelir
_JAVA_HOSTS = {"javaw.exe", "java.exe"}
_MINECRAFT = re.compile(r"minecraft|lunar client|badlion|feather client|labymod", re.IGNORECASE)


def _host_named(game: GameEntry, path: str, hwnd) -> GameEntry:
    if os.path.basename(path).lower() not in _JAVA_HOSTS or game.store in ("custom", "steam"):
        return game
    title = window_title(hwnd)
    name = "Minecraft" if _MINECRAFT.search(title) else (re.sub(r"\s*[\d.]+.*$", "", title).strip() or game.name)
    return GameEntry(name, game.store, exe=game.exe, appid=game.appid)


def detect_game(ignore_pids=(), library: Optional[GameLibrary] = None) -> Optional[TargetWindow]:
    """
    Çalışan oyunu bulur; oyun yoksa None.
    Birden fazla oyun varsa: öndeki oyun > Steam'in o an çalıştırdığı oyun > en büyük pencere.
    """
    games = running_games(ignore_pids, library)
    if not games:
        return None
    fg = _u32.GetForegroundWindow()
    fg_pid = window_pid(fg) if fg else 0
    for g in games:
        if g.pid == fg_pid:
            return g
    running = steam_running_appid()
    if running:
        for g in games:
            if g.game.store == "steam" and g.game.appid == running:
                return g
    return games[0]


def foreground_window_info(ignore_pids=()) -> Optional[Tuple[int, str, str]]:
    """Öndeki uygulama penceresi: (pid, exe yolu, başlık). Kullanıcının elle oyun eklemesi için."""
    fg = _u32.GetForegroundWindow()
    if not fg:
        return None
    pid = window_pid(fg)
    if not pid or pid in set(ignore_pids) or _class_name(fg) in _TASKBAR_CLASSES | {"Progman", "WorkerW"}:
        return None
    path = process_path(pid)
    if not path or os.path.basename(path).lower() in ("explorer.exe", "searchhost.exe", "shellexperiencehost.exe"):
        return None
    return pid, path, window_title(fg)


def detect_active_game() -> Tuple[str, str, str]:
    """Eski arayüz: (oyun adı, slug, exe). Oyun yoksa ("", "", "")."""
    t = detect_game({os.getpid()})
    return (t.name, t.slug, t.exe) if t else ("", "", "")


def pick_target(region=None, ignore_pids=(), exe_hint: str = "") -> Optional[TargetWindow]:  # noqa: ARG001
    """Eski arayüz: hedef artık yalnızca algılanan oyundur (bölgenin altındaki pencereye bakılmaz)."""
    return detect_game(ignore_pids)


def capture_block_reason(target: Optional[TargetWindow], region, ignore_pids=(),
                         require_foreground: bool = True, own_windows=()) -> Optional[str]:
    """
    Bölge okunabilir mi? None → evet; aksi halde kısa bir sebep.
      - hedef ön planda olmalı (require_foreground; panelimiz öndeyse sayılmaz),
      - z-sırasında hedefin üstünde, bölgeyle kesişen görünür başka pencere olmamalı.
    own_windows: uygulamanın ekran yakalamaya görünen kendi pencereleri (kontrol paneli). ignore_pids içindeki süreçten
    olsalar da bölgenin üstündeyse okuma yapılmaz; yoksa panelin kendi yazısı okunurdu. Çeviri kutusu yakalamadan
    hariç tutulduğu (WDA_EXCLUDEFROMCAPTURE) için buraya verilmez.
    """
    if target is None:
        return "oyun algılanmadı"
    if not target.alive():
        return "oyun kapandı"
    ignore = set(ignore_pids)
    fg = _u32.GetForegroundWindow()
    fg_pid = window_pid(fg) if fg else 0
    if require_foreground and fg_pid != target.pid and fg_pid not in ignore:
        return "oyun önde değil"

    x, y, w, h = region
    reg = (x, y, x + w, y + h)
    tl, tt, tr, tb = window_rect(target.hwnd)
    sw, sh = _u32.GetSystemMetrics(0), _u32.GetSystemMetrics(1)
    fullscreen = tl <= 0 and tt <= 0 and tr >= sw and tb >= sh

    own = {int(h) for h in own_windows if h}
    for hwnd in _top_level_windows():
        if own and int(hwnd) in own:
            if _u32.IsWindowVisible(hwnd) and not _u32.IsIconic(hwnd) and _intersects(window_rect(hwnd), reg):
                return "Makine Live paneli okunan bölgenin üstünde"
            continue
        pid = window_pid(hwnd)
        if pid == target.pid:
            if _u32.IsWindowVisible(hwnd) and not _u32.IsIconic(hwnd) and _intersects(window_rect(hwnd), reg):
                return None              # bölgeyi kaplayan ilk pencere oyunun kendisi
            continue
        if not _occluder_candidate(hwnd, ignore):
            continue
        if fullscreen and _class_name(hwnd) in _TASKBAR_CLASSES:
            continue
        if _intersects(window_rect(hwnd), reg):
            return f"üstte başka pencere: {process_exe(pid) or _class_name(hwnd)}"
    return "oyun penceresi bölgede değil"


if __name__ == "__main__":
    import sys
    import time as _t

    lib = get_library()
    t0 = _t.perf_counter()
    lib.refresh()
    print(f"kütüphane taraması {(_t.perf_counter() - t0) * 1000:.0f} ms · {lib.summary()}")
    if "--steam" in sys.argv:
        lib.wait_idle(120)
        for appid, name, verdict, src in lib.steam_report():
            print(f"  {appid:>8}  {verdict:<8} {src:<8} {name}")
    else:
        lib.wait_idle(15)
    games = running_games({os.getpid()})
    print("çalışan oyunlar:", games or "yok")
    print("seçilen:", detect_game({os.getpid()}))
