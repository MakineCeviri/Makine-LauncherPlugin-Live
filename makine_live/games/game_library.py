"""
Makine Live — kurulu oyun kütüphanesi ve "bu süreç bir oyun mu?" kararı.

[TEST→C++] Kaynak okuyucular ve karar mantığı ayrıdır; kaynaklar testte sahte verilebilir. İleride native çekirdeğe taşınacak.

İlke: yalnızca OYUNLARI algıla, tahmin etme. Bir süreç ancak aşağıdaki kaynaklardan biri onu oyun olarak tanıyorsa
oyundur. Hiçbiri tanımıyorsa algılanmaz: tarayıcı, müzik, IDE, Discord… asla.

Kaynaklar:
  1. Kullanıcı işaretleri   %APPDATA%\\MakineLive\\games_custom.json: elle eklenen oyunlar ve "oyun değil" işaretleri
  2. Steam                  libraryfolders.vdf + appmanifest_*.acf (ad, appid, klasör). Steam kütüphanesinde oyun olmayan
                            uygulamalar da bulunur (Soundpad, Lossless Scaling, Crosshair X, SteamVR, sürücü paketleri).
                            Uygulama türü SteamDB'nin de kullandığı yerel önbellekten okunur: appcache\\appinfo.vdf →
                            common.type (Game / Application / Tool / Config / DLC…) ve primary_genre. Çevrimdışı çalışır,
                            mağazadan kaldırılmış oyunları da bilir (~20 ms). Önbellekte olmayan uygulama için Steam mağazası
                            türlerine bakılır (yazılım türü ya da türsüz = oyun değil) ve sonuç steam_app_types.json içinde
                            30 gün saklanır. Türü bilinmeyen Steam uygulaması klasör eşleşmesiyle oyun sayılmaz.
  3. Windows oyun listesi   HKCU\\System\\GameConfigStore\\Children: Windows Oyun Çubuğu'nun oyun olarak tanıdığı exe'ler.
  4. Epic (AppCategories "games"), GOG, Ubisoft Connect, EA, Xbox / Game Pass (MicrosoftGame.config) kurulumları.
  5. Bilinen oyun exe adları (game_detector.KNOWN_GAMES).

Klasör eşleşmelerinde (2, 4) oyunun klasöründeki yardımcı programlar (çökme raporlayıcı, başlatıcı, hile koruması,
kurulum/kaldırma, yeniden dağıtılabilir paketler) oyun sayılmaz. Windows listesi exe düzeyinde olduğu için bu kurala
takılmaz, ama Steam'in "oyun değil" dediği bir uygulamanın klasöründeki exe Windows listesinde olsa bile reddedilir.
"""

from __future__ import annotations

import collections
import ctypes
import glob
import json
import os
import re
import string
import struct
import threading
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Tuple

try:
    import winreg
except ImportError:  # pragma: no cover - yalnızca Windows
    winreg = None


# ─────────────────────────────────────────────────────────────
# Veri tipleri ve yardımcılar
# ─────────────────────────────────────────────────────────────

@dataclass
class GameEntry:
    name: str
    store: str                       # steam | epic | gog | ubisoft | ea | xbox | windows | known | custom
    root: str = ""                   # kurulum klasörü (klasör eşleşmesi)
    exe: str = ""                    # tam exe yolu (exe eşleşmesi)
    appid: str = ""                  # mağazanın kendi kimliği (Steam appid, Epic katalog, GOG…)
    slug: str = ""
    steam_appid: str = ""            # Steam uygulama numarası (portaldaki oyun görseli); Steam dışı oyunlarda ada göre bulunur

    def __post_init__(self):
        self.name = clean_name(self.name)
        if not self.slug:
            self.slug = slugify(self.name)


# Konsol görüntüsünü PC'ye getiren uygulamalar (PS Remote Play, Xbox uygulaması, Asobi, chiaki, Greenlight). Bunlarda yalnızca
# konsol oyunu görünür ve konsol havuzu GTA VI'nındır (console_mode.console_game): PC modunda da oyun olarak GTA VI sayılırlar.
# 0.3.0 ve öncesinde bu pencereler kendi adlarıyla ayrı havuz açıyordu; sunucu da aynı adları GTA VI'ya yönlendirir (text.ts poolSlug).
CONSOLE_GAME_NAME = "Grand Theft Auto VI"
CONSOLE_GAME_SLUG = "grand-theft-auto-vi"
CONSOLE_STREAM_EXES = frozenset({"remoteplay.exe", "chiaki.exe", "chiaki-ng.exe", "xboxpcapp.exe", "greenlight.exe"})
CONSOLE_STREAM_STEM = re.compile(r"asobi", re.IGNORECASE)          # Asobi: Remote Play (exe adı sürüme göre değişebilir)
CONSOLE_STREAM_APPIDS = frozenset({"4505440", "4534740"})            # Asobi'nin Steam sayfaları


def is_console_stream_exe(path: str) -> bool:
    base = os.path.basename(path or "").lower()
    return base in CONSOLE_STREAM_EXES or bool(CONSOLE_STREAM_STEM.search(os.path.splitext(base)[0]))


def clean_name(name: str) -> str:
    name = re.sub(r"[™®©]", "", name or "").strip()
    return re.sub(r"\s+", " ", name)


def slugify(text: str) -> str:
    """Sunucudaki slugify ile uyumlu; kesme işaretleri atılır ("Baldur's" → "baldurs")."""
    slug = re.sub(r"[^a-z0-9]+", "-", re.sub(r"['’`]", "", (text or "").lower())).strip("-")
    return slug[:100] or "game"


def norm(path: str) -> str:
    return os.path.normcase(os.path.normpath(path)) if path else ""


# Oyun klasöründeki yardımcı programlar (klasör eşleşmesinde oyun sayılmaz)
HELPER_EXE = re.compile(
    r"(crash|report|unins|uninst|setup|install|redist|vcredist|vc_redist|dxsetup|directx|dotnet|prereq|"
    r"launcher|bootstrap|updater|patcher|helper|easyanticheat|anticheat|battleye|beservice|eac_|"
    r"cefprocess|webhelper|overlay|service|server|benchmark|config|settings|editor|modmanager)",
    re.IGNORECASE,
)

# Steam mağazası yazılım türleri: 51 Animation & Modeling, 52 Audio Production, 53 Design & Illustration,
# 55 Photo Editing, 56 Software Training, 57 Utilities, 58 Video Production, 59 Web Publishing, 60 Game Development.
# (54 Education eğitici oyunlarda da kullanıldığı için listede değil.)
STEAM_SOFTWARE_GENRES = {"51", "52", "53", "55", "56", "57", "58", "59", "60"}
# Ağ yokken bile oyun olmadığı bilinen Steam uygulamaları (Steamworks ortak paketleri, SteamVR, Linux çalışma ortamları)
STEAM_NOT_GAMES = {"228980", "250820", "1070560", "1391110", "1628350", "1493710", "1826330", "2180100"}
STEAM_NOT_GAME_NAME = re.compile(
    r"redistributable|steamvr|steam linux runtime|proton|dedicated server|\bsdk\b|soundtrack|wallpaper engine|"
    r"steamworks|benchmark", re.IGNORECASE)


# ─────────────────────────────────────────────────────────────
# Kaynak okuyucular (her biri GameEntry listesi döndürür; hata → boş liste)
# ─────────────────────────────────────────────────────────────

def _reg_subkeys(root, path) -> Iterable[Tuple[str, Dict[str, object]]]:
    """Bir kayıt defteri anahtarının alt anahtarları ve değerleri (64 bit, yoksa 32 bit görünüm)."""
    if winreg is None:
        return
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            key = winreg.OpenKey(root, path, 0, winreg.KEY_READ | view)
        except OSError:
            continue
        with key:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(key, i)
                except OSError:
                    break
                i += 1
                vals: Dict[str, object] = {}
                try:
                    with winreg.OpenKey(key, sub, 0, winreg.KEY_READ | view) as sk:
                        j = 0
                        while True:
                            try:
                                n, v, _ = winreg.EnumValue(sk, j)
                            except OSError:
                                break
                            vals[n] = v
                            j += 1
                except OSError:
                    continue
                yield sub, vals
        return


def parse_vdf(text: str) -> dict:
    """Valve'ın metin VDF/ACF biçimi için küçük ayrıştırıcı ("anahtar" "değer" ve { } blokları)."""
    root: dict = {}
    stack = [root]
    key = None
    for quoted, brace in re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text):
        if brace == "{":
            new: dict = {}
            stack[-1][key if key is not None else ""] = new
            stack.append(new)
            key = None
        elif brace == "}":
            if len(stack) > 1:
                stack.pop()
            key = None
        else:
            value = re.sub(r"\\(.)", r"\1", quoted)
            if key is None:
                key = value
            else:
                stack[-1][key] = value
                key = None
    return root


def steam_root() -> str:
    if winreg is None:
        return ""
    for hive, path, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                             (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                             (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
        try:
            with winreg.OpenKey(hive, path) as k:
                v, _ = winreg.QueryValueEx(k, name)
                if v and os.path.isdir(v):
                    return os.path.normpath(v)
        except OSError:
            continue
    return ""


def steam_running_appid() -> str:
    """Steam'in o an çalıştırdığı oyunun appid'si ("" = yok)."""
    if winreg is None:
        return ""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            v, _ = winreg.QueryValueEx(k, "RunningAppID")
            return str(v) if v and str(v) != "0" else ""
    except OSError:
        return ""


def read_steam(steam_path: Optional[str] = None) -> List[GameEntry]:
    steam_path = steam_path if steam_path is not None else steam_root()
    if not steam_path:
        return []
    libraries = [steam_path]
    try:
        with open(os.path.join(steam_path, "steamapps", "libraryfolders.vdf"), encoding="utf-8", errors="replace") as fh:
            data = parse_vdf(fh.read()).get("libraryfolders", {})
        for v in data.values():
            if isinstance(v, dict) and v.get("path"):
                libraries.append(v["path"])
    except OSError:
        pass
    out: List[GameEntry] = []
    seen = set()
    for lib in libraries:
        apps = os.path.join(lib, "steamapps")
        for acf in glob.glob(os.path.join(apps, "appmanifest_*.acf")):
            try:
                with open(acf, encoding="utf-8", errors="replace") as fh:
                    st = parse_vdf(fh.read()).get("AppState", {})
            except OSError:
                continue
            appid, name, installdir = st.get("appid", ""), st.get("name", ""), st.get("installdir", "")
            if not appid or not installdir or appid in seen:
                continue
            seen.add(appid)
            out.append(GameEntry(name or installdir, "steam", root=os.path.join(apps, "common", installdir), appid=appid,
                                 steam_appid=appid))
    return out


def read_epic(manifest_dir: Optional[str] = None) -> List[GameEntry]:
    manifest_dir = manifest_dir or os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"),
                                                "Epic", "EpicGamesLauncher", "Data", "Manifests")
    out = []
    for item in glob.glob(os.path.join(manifest_dir, "*.item")):
        try:
            with open(item, encoding="utf-8", errors="replace") as fh:
                m = json.load(fh)
        except (OSError, ValueError):
            continue
        cats = [str(c).lower() for c in m.get("AppCategories", [])]
        if "games" not in cats or not m.get("InstallLocation"):
            continue   # Unreal Engine gibi uygulamalar "applications" kategorisindedir
        out.append(GameEntry(m.get("DisplayName") or m.get("AppName", ""), "epic", root=m["InstallLocation"],
                             appid=str(m.get("CatalogItemId", ""))))
    return out


def read_gog() -> List[GameEntry]:
    if winreg is None:
        return []
    out = []
    for gid, v in _reg_subkeys(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\GOG.com\Games"):
        name = v.get("gameName") or v.get("GAMENAME")
        if v.get("path") and name:
            out.append(GameEntry(str(name), "gog", root=str(v["path"]), appid=gid))
    return out


def read_ubisoft() -> List[GameEntry]:
    if winreg is None:
        return []
    out = []
    for gid, v in _reg_subkeys(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Ubisoft\Launcher\Installs"):
        d = str(v.get("InstallDir", ""))
        if d:
            out.append(GameEntry(os.path.basename(os.path.normpath(d)), "ubisoft", root=d, appid=gid))
    return out


def read_ea() -> List[GameEntry]:
    if winreg is None:
        return []
    out = []
    for base in (r"SOFTWARE\EA Games", r"SOFTWARE\Electronic Arts"):
        for name, v in _reg_subkeys(winreg.HKEY_LOCAL_MACHINE, base):
            d = str(v.get("Install Dir") or v.get("InstallDir") or "")
            if d and os.path.isdir(d) and not re.search(r"ea desktop|^origin$|ea app", name, re.IGNORECASE):
                out.append(GameEntry(str(v.get("DisplayName") or name), "ea", root=d))
    return out


def fixed_drives() -> List[str]:
    try:
        k32 = ctypes.windll.kernel32
        mask = k32.GetLogicalDrives()
    except (AttributeError, OSError):
        return []
    out = []
    for i, c in enumerate(string.ascii_uppercase):
        if mask & (1 << i) and k32.GetDriveTypeW(f"{c}:\\") == 3:   # DRIVE_FIXED: ağ/CD sürücüsünde beklemeyelim
            out.append(f"{c}:\\")
    return out


def read_xbox(drives: Optional[Iterable[str]] = None) -> List[GameEntry]:
    """Xbox uygulaması / Game Pass: <sürücü>:\\XboxGames\\<Oyun>\\Content\\MicrosoftGame.config"""
    out = []
    for d in (drives if drives is not None else fixed_drives()):
        for cfg in glob.glob(os.path.join(d, "XboxGames", "*", "Content", "MicrosoftGame.config")):
            content = os.path.dirname(cfg)
            name = os.path.basename(os.path.dirname(content))
            try:
                with open(cfg, encoding="utf-8", errors="replace") as fh:
                    m = re.search(r'DefaultDisplayName="([^"]+)"', fh.read())
                if m and not m.group(1).startswith("ms-resource"):
                    name = m.group(1)
            except OSError:
                pass
            out.append(GameEntry(name, "xbox", root=content))
    return out


def read_windows_games() -> List[GameEntry]:
    """Windows Oyun Çubuğu'nun oyun olarak tanıdığı exe'ler (exe düzeyinde)."""
    if winreg is None:
        return []
    out = []
    try:
        root = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"System\GameConfigStore\Children")
    except OSError:
        return []
    with root:
        i = 0
        while True:
            try:
                sub = winreg.EnumKey(root, i)
            except OSError:
                break
            i += 1
            try:
                with winreg.OpenKey(root, sub) as k:
                    exe, _ = winreg.QueryValueEx(k, "MatchedExeFullPath")
            except OSError:
                continue
            if exe:
                out.append(GameEntry(stem_name(exe), "windows", exe=exe))
    return out


def stem_name(path: str) -> str:
    stem = os.path.splitext(os.path.basename(path))[0]
    stem = re.sub(r"(-win64-shipping|-win32-shipping|[-_.](win64|win32|shipping|x64|dx11|dx12))+$", "", stem,
                  flags=re.IGNORECASE)
    stem = re.sub(r"(?<=[a-z])(64|32)$", "", stem)
    if stem.islower():
        stem = stem.title()
    return re.sub(r"([a-z])([A-Z])", r"\1 \2", stem).replace("_", " ").strip() or stem


# Sürüm bilgisindeki ürün adı oyunun değil motorun / çalışma ortamının adıysa kullanılmaz
_GENERIC_PRODUCT = re.compile(r"unreal|unity|launcher|java|openjdk|zulu|temurin|runtime|platform se binary|bootstrap",
                              re.IGNORECASE)


def exe_display_name(path: str) -> str:
    """Mağazasız oyunun adı: Steam klasör adı > exe'nin ürün adı > dosya adı."""
    m = re.search(r"[\\/]steamapps[\\/]common[\\/]([^\\/]+)[\\/]", path, re.IGNORECASE)
    if m:
        return m.group(1)
    name = _version_string(path, "ProductName") or _version_string(path, "FileDescription")
    if name and not _GENERIC_PRODUCT.search(name):
        return name
    return stem_name(path)


def _version_string(path: str, key: str) -> str:
    try:
        ver = ctypes.windll.version
        size = ver.GetFileVersionInfoSizeW(path, None)
        if not size:
            return ""
        buf = ctypes.create_string_buffer(size)
        if not ver.GetFileVersionInfoW(path, 0, size, buf):
            return ""
        lp = ctypes.c_void_p()
        n = ctypes.c_uint()
        if not ver.VerQueryValueW(buf, r"\VarFileInfo\Translation", ctypes.byref(lp), ctypes.byref(n)) or n.value < 4:
            return ""
        lang, cp = ctypes.cast(lp, ctypes.POINTER(ctypes.c_ushort * 2)).contents
        q = f"\\StringFileInfo\\{lang:04x}{cp:04x}\\{key}"
        if not ver.VerQueryValueW(buf, q, ctypes.byref(lp), ctypes.byref(n)) or not n.value:
            return ""
        return ctypes.wstring_at(lp, n.value).strip("\x00 ").strip()
    except (OSError, AttributeError, ValueError):
        return ""


# ─────────────────────────────────────────────────────────────
# Steam türü sınıflandırması
# ─────────────────────────────────────────────────────────────

GAME = "game"
NOT_GAME = "not"
UNKNOWN = "unknown"      # mağazada sayfası yok, yerel türü yok: yalnızca exe düzeyindeki kanıt (Windows listesi) kabul edilir
PENDING = "pending"      # henüz sorulmadı

APPINFO_GAME_TYPES = {"game", "demo", "mod"}


def appinfo_verdict(app_type: str, primary_genre: str = "") -> Optional[str]:
    """appinfo.vdf'teki tür → GAME / NOT_GAME; tür yoksa None."""
    t = (app_type or "").strip().lower()
    if not t:
        return None
    if t not in APPINFO_GAME_TYPES:
        return NOT_GAME                      # application, tool, config, dlc, music, video…
    return NOT_GAME if str(primary_genre) in STEAM_SOFTWARE_GENRES else GAME


def read_steam_appinfo(steam_path: str, wanted: Iterable[str]) -> Dict[str, Tuple[str, str]]:
    """
    Steam'in yerel uygulama bilgisi önbelleği (appcache\\appinfo.vdf, ikili KeyValues; sürüm 27, 28, 29).
    Yalnızca istenen appid'ler ayrıştırılır. Dönüş: {appid: (common.type, common.primary_genre)}. Hata → {}.
    """
    want = {int(a) for a in wanted if str(a).isdigit()}
    if not steam_path or not want:
        return {}
    try:
        with open(os.path.join(steam_path, "appcache", "appinfo.vdf"), "rb") as fh:
            data = fh.read()
    except OSError:
        return {}
    try:
        return _parse_appinfo(data, want)
    except (struct.error, ValueError, IndexError, RecursionError):
        return {}


def _parse_appinfo(data: bytes, want: set) -> Dict[str, Tuple[str, str]]:
    magic, _universe = struct.unpack_from("<II", data, 0)
    if magic not in (0x07564427, 0x07564428, 0x07564429):
        return {}
    off, strings = 8, None
    if magic == 0x07564429:                  # v29: anahtar adları dosya sonundaki dizgi tablosunda
        table, = struct.unpack_from("<q", data, 8)
        off = 16
        count, = struct.unpack_from("<I", data, table)
        p, strings = table + 4, []
        for _ in range(count):
            e = data.index(b"\0", p)
            strings.append(data[p:e].decode("utf-8", "replace"))
            p = e + 1
    header = 40 + (20 if magic >= 0x07564428 else 0)

    def cstr(p):
        e = data.index(b"\0", p)
        return data[p:e].decode("utf-8", "replace"), e + 1

    def kv(p, end):
        out = {}
        while p < end:
            t = data[p]
            p += 1
            if t == 8:
                return out, p
            if strings is not None:
                idx, = struct.unpack_from("<i", data, p)
                name, p = strings[idx], p + 4
            else:
                name, p = cstr(p)
            if t == 0:
                v, p = kv(p, end)
            elif t == 1:
                v, p = cstr(p)
            elif t in (2, 3):
                v, p = struct.unpack_from("<i" if t == 2 else "<f", data, p)[0], p + 4
            elif t in (7, 10):
                v, p = struct.unpack_from("<Q" if t == 7 else "<q", data, p)[0], p + 8
            else:
                raise ValueError(f"appinfo: bilinmeyen tür {t}")
            out[name] = v
        return out, p

    out: Dict[str, Tuple[str, str]] = {}
    while off + 8 <= len(data):
        appid, size = struct.unpack_from("<II", data, off)
        if appid == 0:
            break
        nxt = off + 8 + size
        if appid in want:
            d, _ = kv(off + 8 + header, nxt)
            common = (d.get("appinfo") or {}).get("common") or {}
            out[str(appid)] = (str(common.get("type", "")), str(common.get("primary_genre", "")))
            if len(out) == len(want):
                break
        off = nxt
    return out


def steam_store_is_game(appid: str, timeout: float = 8.0) -> Optional[str]:
    """Steam mağazasına göre GAME / NOT_GAME / UNKNOWN; ağ hatasında None (sonra yeniden denenir)."""
    url = f"https://store.steampowered.com/api/appdetails?appids={appid}&l=english"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "MakineLive/0.2"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8")).get(str(appid), {})
    except (OSError, ValueError):
        return None
    if not d.get("success"):
        return UNKNOWN                       # mağazadan kaldırılmış oyun da olabilir, araç da
    data = d.get("data") or {}
    if data.get("type") not in ("game", "demo"):
        return NOT_GAME                      # dlc, music, video…
    genres = {str(g.get("id")) for g in data.get("genres", [])}
    if not genres or genres & STEAM_SOFTWARE_GENRES:
        return NOT_GAME                      # türsüz araçlar (SteamVR) ve yazılım türleri
    return GAME


def _name_key(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", clean_name(name).lower())


def steam_search_appid(name: str, timeout: float = 6.0) -> Optional[str]:
    """Steam mağazasında adı BİREBİR aynı olan oyunun numarası; yoksa "", ağ hatasında None."""
    url = "https://store.steampowered.com/api/storesearch/?" + urllib.parse.urlencode(
        {"term": name, "l": "english", "cc": "US"})
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "MakineLive/0.2"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            items = json.loads(r.read().decode("utf-8")).get("items") or []
    except (OSError, ValueError):
        return None
    want = _name_key(name)
    for it in items:
        if _name_key(str(it.get("name", ""))) == want and it.get("id"):
            return str(it["id"])
    return ""


# ─────────────────────────────────────────────────────────────
# Kütüphane
# ─────────────────────────────────────────────────────────────

class GameLibrary:
    """
    Kurulu oyunlar. match(exe_path) → GameEntry ya da None (belirsizlik = oyun değil).
    refresh() kaynakları yeniden okur. Steam uygulamaları arka planda sınıflandırılır; bir karar değişince on_change çağrılır.
    """

    TYPE_TTL_S = 30 * 86400

    def __init__(self, data_dir: str, sources: Optional[Dict[str, Callable[[], List[GameEntry]]]] = None,
                 steam_classifier: Optional[Callable[[str], Optional[str]]] = steam_store_is_game,
                 appinfo_reader: Optional[Callable[[List[str]], Dict[str, Tuple[str, str]]]] = None,
                 known_exes: Optional[Dict[str, Tuple[str, str]]] = None,
                 on_change: Optional[Callable[[], None]] = None, request_gap_s: float = 0.35):
        self.data_dir = data_dir
        self.sources = sources if sources is not None else {
            "steam": read_steam, "epic": read_epic, "gog": read_gog, "ubisoft": read_ubisoft,
            "ea": read_ea, "xbox": read_xbox, "windows": read_windows_games,
        }
        self.classifier = steam_classifier
        self.appinfo_reader = appinfo_reader if appinfo_reader is not None else (
            lambda ids: read_steam_appinfo(steam_root(), ids))
        self.known_exes = {k.lower(): v for k, v in (known_exes or {}).items()}
        self.on_change = on_change
        self.request_gap_s = request_gap_s
        self._lock = threading.Lock()
        self._dirs: List[Tuple[str, GameEntry]] = []
        self._exes: Dict[str, GameEntry] = {}
        self._steam_names: Dict[str, str] = {}
        self._custom: Dict[str, str] = {}
        self._ignored: set = set()
        self._types: Dict[str, dict] = {}
        self._offline: Dict[str, str] = {}
        self._queue: collections.deque = collections.deque()
        self._worker: Optional[threading.Thread] = None
        self._named: set = set()
        self.loaded = False
        self._load_user_files()

    # ── kalıcı dosyalar ──
    def _path(self, name):
        return os.path.join(self.data_dir, name)

    def _load_user_files(self):
        try:
            with open(self._path("games_custom.json"), encoding="utf-8") as fh:
                d = json.load(fh)
            self._custom = {norm(k): str(v) for k, v in d.get("games", {}).items()}
            self._ignored = {norm(p) for p in d.get("not_games", [])}
        except (OSError, ValueError, AttributeError):
            pass
        try:
            with open(self._path("steam_app_types.json"), encoding="utf-8") as fh:
                types = json.load(fh)
            self._types = types if isinstance(types, dict) else {}
        except (OSError, ValueError):
            self._types = {}

    def _write_json(self, name, data):
        try:
            os.makedirs(self.data_dir, exist_ok=True)
            tmp = self._path(name + ".tmp")
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(data, fh, ensure_ascii=False, indent=1)
            os.replace(tmp, self._path(name))
        except OSError:
            pass

    def _save_user_files(self):
        with self._lock:
            data = {"games": dict(self._custom), "not_games": sorted(self._ignored)}
        self._write_json("games_custom.json", data)

    # ── kullanıcı işaretleri ──
    def add_custom(self, exe_path: str, name: str = "") -> GameEntry:
        p = norm(exe_path)
        with self._lock:
            store_hit = next((e for root, e in self._dirs if p.startswith(root)), None)
        # Ad önce mağazadan (Steam'deki "Heavy Rain"), yoksa exe'den; klasör adı ("HEAVY RAIN") son çare
        name = clean_name(name) or (store_hit.name if store_hit else "") or exe_display_name(exe_path)
        with self._lock:
            self._custom[norm(exe_path)] = name
            self._ignored.discard(norm(exe_path))
        self._save_user_files()
        return GameEntry(name, "custom", exe=exe_path)

    def mark_not_game(self, exe_path: str):
        with self._lock:
            self._ignored.add(norm(exe_path))
            self._custom.pop(norm(exe_path), None)
        self._save_user_files()

    # ── tarama ──
    def refresh(self):
        dirs, exes, steam_names = [], {}, {}
        for reader in self.sources.values():
            try:
                entries = reader()
            except Exception:  # noqa: BLE001 — bir kaynağın bozulması diğerlerini durdurmasın
                entries = []
            for e in entries:
                if e.exe:
                    exes[norm(e.exe)] = e
                elif e.root:
                    dirs.append((norm(e.root).rstrip("\\/") + os.sep, e))
                    if e.store == "steam":
                        steam_names[e.appid] = e.name
        dirs.sort(key=lambda t: len(t[0]), reverse=True)   # en özel (en uzun) klasör önce
        offline: Dict[str, str] = {}
        if steam_names:
            try:
                info = self.appinfo_reader(list(steam_names))
            except Exception:  # noqa: BLE001
                info = {}
            for appid, (app_type, genre) in info.items():
                v = appinfo_verdict(app_type, genre)
                if v:
                    offline[appid] = v
        with self._lock:
            self._dirs, self._exes, self._steam_names, self._offline = dirs, exes, steam_names, offline
            self._named = set()
            self.loaded = True
            now = time.time()
            stale = [a for a in steam_names if a not in offline and a not in STEAM_NOT_GAMES
                     and not STEAM_NOT_GAME_NAME.search(steam_names[a])
                     and now - (self._types.get(a) or {}).get("t", 0) > self.TYPE_TTL_S]
        for a in stale:
            self._enqueue(a)

    def _enqueue(self, appid: str, urgent: bool = False):
        if not self.classifier:
            return
        with self._lock:
            if appid in self._queue:
                if not urgent:
                    return
                self._queue.remove(appid)
            if urgent:
                self._queue.appendleft(appid)
            else:
                self._queue.append(appid)
            if self._worker is None or not self._worker.is_alive():
                self._worker = threading.Thread(target=self._classify_worker, daemon=True, name="steam-classify")
                self._worker.start()

    def _classify_worker(self):
        classifier = self.classifier
        if classifier is None:
            return
        changed = False
        failures = 0
        while True:
            with self._lock:
                if not self._queue:
                    self._worker = None
                    break
                appid = self._queue.popleft()
                name = self._steam_names.get(appid, "")
            verdict = classifier(appid)
            time.sleep(self.request_gap_s)           # mağaza servisine nazik ol
            if verdict is None:
                failures += 1
                if failures >= 3:                    # ağ yok: kalanları sonraki refresh'e bırak
                    with self._lock:
                        self._queue.clear()
                        self._worker = None
                    break
                continue
            with self._lock:
                old = (self._types.get(appid) or {}).get("v")
                self._types[appid] = {"v": verdict, "t": time.time(), "name": name}
            changed = changed or old != verdict
            if old != verdict and self.on_change:
                self._save_types()
                self.on_change()
        if changed:
            self._save_types()

    def _save_types(self):
        with self._lock:
            data = dict(self._types)
        self._write_json("steam_app_types.json", data)

    def wait_idle(self, timeout: float = 30.0) -> bool:
        """Testler için: sınıflandırma kuyruğu boşalana kadar bekle."""
        end = time.time() + timeout
        while time.time() < end:
            w = self._worker
            if w is None or not w.is_alive():
                return True
            w.join(0.05)
        return False

    def steam_verdict(self, appid: str) -> str:
        """GAME / NOT_GAME / UNKNOWN / PENDING."""
        if appid in STEAM_NOT_GAMES or STEAM_NOT_GAME_NAME.search(self._steam_names.get(appid, "")):
            return NOT_GAME
        if appid in self._offline:
            return self._offline[appid]
        v = (self._types.get(appid) or {}).get("v")
        return v if v in (GAME, NOT_GAME, UNKNOWN) else PENDING

    # ── karar ──
    def match(self, exe_path: str) -> Optional[GameEntry]:
        """exe_path bir oyunsa GameEntry, değilse None."""
        p = norm(exe_path)
        if not p:
            return None
        base = os.path.basename(p)
        stem = os.path.splitext(base)[0]
        pending = ""
        steam_ok = True
        with self._lock:
            if p in self._ignored:
                return None
            dir_hit = next((e for root, e in self._dirs if p.startswith(root)), None)
            if is_console_stream_exe(p) or (dir_hit is not None and dir_hit.store == "steam" and (
                    dir_hit.appid in CONSOLE_STREAM_APPIDS or CONSOLE_STREAM_STEM.search(dir_hit.name))):
                return GameEntry(CONSOLE_GAME_NAME, "console", exe=exe_path, slug=CONSOLE_GAME_SLUG)
            if p in self._custom:
                # Elle eklenen oyun: "oyun mu" kararı kullanıcının; ad ve slug mağazada kayıtlıysa oradan gelir
                if dir_hit is not None:
                    return self._with_known(GameEntry(dir_hit.name, "custom", exe=exe_path, appid=dir_hit.appid,
                                                      slug=dir_hit.slug, steam_appid=dir_hit.steam_appid), base)
                return self._with_known(GameEntry(self._custom[p], "custom", exe=exe_path), base)
            if dir_hit is not None and dir_hit.store == "steam":
                verdict = self.steam_verdict(dir_hit.appid)
                if verdict == NOT_GAME:
                    return None                  # Steam'de oyun olmayan uygulama (Soundpad, SteamVR…)
                steam_ok = verdict == GAME
                if verdict == PENDING:
                    pending = dir_hit.appid
            exe_hit = self._exes.get(p)
            name_it = exe_hit is not None and dir_hit is None and p not in self._named
            if name_it:
                self._named.add(p)
        if pending:
            self._enqueue(pending, urgent=True)
        if exe_hit is not None:                  # Windows'un oyun listesi: exe düzeyinde
            if dir_hit is not None:
                return self._with_known(GameEntry(dir_hit.name, dir_hit.store, exe=exe_path, appid=dir_hit.appid,
                                                  slug=dir_hit.slug, steam_appid=dir_hit.steam_appid), base)
            if name_it:                          # mağazasız oyun: adı exe'nin sürüm bilgisinden, bir kez
                exe_hit.name = clean_name(exe_display_name(exe_path))
                exe_hit.slug = slugify(exe_hit.name)
            return self._with_known(exe_hit, base)
        if dir_hit is not None and steam_ok and not HELPER_EXE.search(stem):
            return self._with_known(GameEntry(dir_hit.name, dir_hit.store, exe=exe_path, appid=dir_hit.appid,
                                              root=dir_hit.root, slug=dir_hit.slug, steam_appid=dir_hit.steam_appid), base)
        known = self.known_exes.get(base)
        if known:
            return GameEntry(known[0], "known", exe=exe_path, slug=known[1])
        return None

    def _with_known(self, e: GameEntry, base: str) -> GameEntry:
        """Bilinen oyunlarda havuzdaki slug sabit kalsın (mağaza adı farklı yazılsa bile)."""
        known = self.known_exes.get(base)
        if known and e.slug != known[1]:
            return GameEntry(e.name, e.store, exe=e.exe, root=e.root, appid=e.appid, slug=known[1], steam_appid=e.steam_appid)
        return e

    def summary(self) -> Dict[str, int]:
        with self._lock:
            counts: Dict[str, int] = {}
            for _, e in self._dirs:
                if e.store == "steam" and self.steam_verdict(e.appid) != GAME:
                    continue
                counts[e.store] = counts.get(e.store, 0) + 1
            counts["windows"] = len(self._exes)
            if self._custom:
                counts["custom"] = len(self._custom)
            return counts

    def steam_appid_for(self, entry: GameEntry, search: Callable[[str], Optional[str]] = steam_search_appid) -> str:
        """
        Oyunun Steam numarası (portaldaki görsel için). Steam oyunlarında hazırdır; diğerlerinde Steam mağazasında adı
        birebir aynı oyun aranır ve sonuç steam_names.json içinde saklanır. Ağ çağrısı yapabilir: arka planda çağırın.
        """
        if entry.steam_appid:
            return entry.steam_appid
        key = _name_key(entry.name)
        if not key:
            return ""
        cache = self._read_cache("steam_names.json")
        hit = cache.get(key)
        if isinstance(hit, list) and len(hit) == 2 and time.time() - float(hit[1]) < self.TYPE_TTL_S:
            return str(hit[0])
        found = search(entry.name)
        if found is None:
            return ""                        # ağ yok: sonra yeniden denenir
        cache[key] = [found, time.time()]
        self._write_json("steam_names.json", cache)
        return found

    def _read_cache(self, name: str) -> dict:
        try:
            with open(self._path(name), encoding="utf-8") as fh:
                d = json.load(fh)
            return d if isinstance(d, dict) else {}
        except (OSError, ValueError):
            return {}

    def steam_report(self) -> List[Tuple[str, str, str, str]]:
        """(appid, ad, karar, kaynak); kaynak: appinfo | mağaza | kural"""
        with self._lock:
            items = sorted(self._steam_names.items(), key=lambda t: t[1].lower())
            out = []
            for a, n in items:
                src = "appinfo" if a in self._offline else ("mağaza" if a in self._types else "kural")
                out.append((a, n, self.steam_verdict(a), src))
            return out
