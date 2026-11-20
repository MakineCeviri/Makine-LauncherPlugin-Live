#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Makine Live — oyun içi test koşucusu (TEST HARNESS).

Nihai ürün makine-live.dll'dir. Bu betik; ekran yakalama + OCR'ı DLL üzerinden,
havuz sorgusunu ve yedek çeviriyi Python tarafında çalıştırarak özellikleri
gerçek bir oyunda denemek için kullanılır.

Arayüz (makine_live/ui/)
  - hud_ui.py         panel bileşenleri ve animasyonlu ikonlar (Canvas + Pillow)
  - overlay_render.py oyun içi çeviri kutusu (piksel bazında alfa, UpdateLayeredWindow)
  - login.py, region.py, widgets.py  giriş ekranı, F10 bölge seçimi, geçmiş listesi
  - Tasarım dili      spec/design_tokens.json ("altyazı stüdyosu")

Konsol / TV modu (PS5, Xbox): Remote Play ya da yakalama kartı penceresi okunur (games/console_mode.py), çeviri ev
ağından TV'deki Makine TV uygulamasına gider (tv/tv_link.py) ve HDMI görüntüsünün üstünde gösterilir.

Çalıştırma: py -m makine_live

Kısayollar: F8 kilitle/taşı · F9 başlat/durdur · F10 bölge seç
"""

import os
import sys
import json
import time
import queue
import webbrowser
import random
import ctypes
import ctypes.wintypes
import threading
from collections import deque, OrderedDict
from concurrent.futures import ThreadPoolExecutor
import tkinter as tk
import tkinter.font as tkfont

from PIL import Image, ImageTk

from makine_live.games.game_detector import detect_game, capture_block_reason, foreground_window_info, get_library
from makine_live.games.console_mode import (ConsoleTarget, find_console_source, console_block_reason, region_to_screen,
                                            screen_to_region, valid_region, client_rect, window_has_image, DEFAULT_REGION,
                                            DEFAULT_GAME, CONSOLE_LABELS)
from makine_live.tv.subtitle_web import SubtitleWeb
from makine_live.tv.tv_link import TvLink, TvInfo, discover as discover_tvs, TCP_PORT as TV_TCP_PORT
from makine_live.tv import tv_install
import socket
from makine_live.net.local_pool import LocalPool
from makine_live.net.pool_client import HTTP, HttpPool
from makine_live.net import app_update
from makine_live.net.discord_presence import DiscordPresence, build_activity
from makine_live.net.pool_client import (ServerClient, SessionStore, AuthRequired, NetworkError, ServerError, SERVER_URL,
                                         compact_pool_key, app_data_dir)
from makine_live.core.subtitle_timing import LineTracker, SubtitleScheduler

from makine_live.paths import FROZEN, RUNTIME_DIR, DATA_DIR, SPEC_DIR, LOGO_PATH
from makine_live.version import APP_VERSION, APP_VERSION_LABEL

# DPI farkındalığı: kutu ve bölge seçimi fiziksel piksellerle çalışır.
# (Tk penceresi oluşturulmadan önce ayarlanmalı.)
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

from makine_live.ui import hud_ui as ui                        # noqa: E402
from makine_live.ui.layout import W, PAD, INNER, H, TAB_Y, CONTENT_Y, GAME_BTN_W  # noqa: E402
from makine_live.ui.brand import app_mark_image                # noqa: E402
from makine_live.ui.region import RegionSelector               # noqa: E402
from makine_live.ui.widgets import HistoryList                 # noqa: E402
from makine_live.ui.login import LoginView                     # noqa: E402
from makine_live.core.dll import PluginWrapper                 # noqa: E402
from makine_live.core.garbage import filter_ocr                # noqa: E402
from makine_live.ui.hud_ui import (BG, SURFACE, RAISED, LINE, LINE_STRONG, TEXT, TEXT2, TEXT3,  # noqa: E402
                                   ACCENT, VERIFIED, mix)
from makine_live.ui.overlay_render import (SubtitleOverlay, OverlayContent, STYLES, FONTS, OUTLINE_PX,  # noqa: E402
                                           available_fonts)


user32 = ctypes.windll.user32

# Virtual Key Codes
VK_F8 = 0x77
VK_F9 = 0x78
VK_F10 = 0x79


# ─────────────────────────────────────────────────────────────
# 4. Kontrol paneli ve arka plan pipeline'ı
# ─────────────────────────────────────────────────────────────

SOURCE_LANG = "en"                       # şimdilik yalnızca İngilizceden Türkçeye
TARGET_LANG = "tr"                      # sunucu şimdilik yalnızca Türkçeye çeviriyor
ROLE_LABELS = {"admin": "yönetici", "translator": "çevirmen", "tester": "tester", "player": "oyuncu"}
STYLE_LABELS = [("altyazi", "Altyazı"), ("yalin", "Yalın"), ("sinema", "Sinema"), ("kontrast", "Kontrast")]
POSITION_LABELS = [("alt", "Alt"), ("ust", "Üst"), ("serbest", "Serbest")]
LOOK_SECTIONS = [("yazi", "Yazı"), ("kutu", "Kutu"), ("konum", "Konum ve yayın")]
OUTLINE_LABELS = [("yok", "Yok"), ("ince", "İnce"), ("kalin", "Kalın")]
LINES_LABELS = [(2, "2"), (3, "3"), (4, "4")]
# Renkler tasarım dilinin içinde: neon / mor yok; yazı rengi koyu bantta, bant rengi beyaz yazıyla okunur
TEXT_COLORS = [("#ffffff", "Beyaz"), ("#f5ecd7", "Krem"), ("#f2c230", "Sarı"), ("#a9dcef", "Açık mavi"),
               ("#b9e3b0", "Açık yeşil")]
BAND_COLORS = [("#0f1012", "Mürekkep"), ("#000000", "Siyah"), ("#24262b", "Grafit"), ("#142033", "Lacivert"),
               ("#2b2115", "Kahve")]
# Hazır stiller: seçilince renkler / opaklık / kontur bununla başlar, sonra tek tek değiştirilebilir
STYLE_PRESETS = {
    "altyazi": {"text_color": "#ffffff", "band_color": "#0f1012", "opacity": 78, "outline": "ince", "shadow": True},
    "yalin": {"text_color": "#ffffff", "outline": "kalin", "shadow": True},
    "sinema": {"text_color": "#ffffff", "band_color": "#000000", "opacity": 85, "outline": "ince", "shadow": False},
    "kontrast": {"text_color": "#f2c230", "band_color": "#000000", "opacity": 92, "outline": "kalin", "shadow": True},
}
LOOK_DEFAULTS = {"style": "altyazi", "font": "segoe", "font_px": 26, "opacity": 78, "text_color": "#ffffff",
                 "band_color": "#0f1012", "outline": "ince", "shadow": True, "max_lines": 4, "width": 62, "offset": 8,
                 "position": "alt", "min_show_ms": 1200}

POOL_SYNC_S = 180         # yerel havuzun sunucuyla karşılaştırılma aralığı (okuma açıkken)
POOL_IDLE_SYNC_S = 600    # okuma kapalıyken (oyun açık) aynı karşılaştırma
POOL_TICK_S = 60          # eşitleme zamanı geldi mi diye bakma aralığı
TR_CACHE_TTL_S = 900      # onaylanmamış satırın çevirisi oturumda bu kadar tutulur (oy sunucuda zaten kayıtlı)
STATUS_POLL_S = 300       # bakım / yeni sürüm / duyuru kontrolü (oturum açıkken)


def _local_time(utc: str) -> str:
    """Sunucunun UTC zamanı ("2026-10-22 12:00:00") → yerel "22.10.2026 15:00"."""
    import datetime as _dt
    try:
        t = _dt.datetime.strptime(utc[:19], "%Y-%m-%d %H:%M:%S").replace(tzinfo=_dt.timezone.utc).astimezone()
        return t.strftime("%d.%m.%Y %H:%M")
    except (ValueError, TypeError):
        return utc

STORE_LABELS = {"steam": "Steam", "epic": "Epic Games", "gog": "GOG", "ubisoft": "Ubisoft Connect", "ea": "EA app",
                "xbox": "Xbox", "windows": "Windows oyun listesi", "known": "bilinen oyun", "custom": "elle eklendi",
                "console": "Konsol"}
TABS = [("canli", "Canlı"), ("ceviri", "Çeviri"), ("gorunum", "Görünüm"), ("konsol", "Konsol"), ("hesap", "Hesap")]
TV_STATE_TEXT = {"idle": "TV seçilmedi", "connecting": "Bağlanıyor", "connected": "Bağlı",
                 "bad_code": "Kod kabul edilmedi · TV bul ile yeniden girin", "offline": "TV'ye ulaşılamıyor · yeniden deneniyor"}


def _same_target(a, b) -> bool:
    if a is None or b is None:
        return a is b
    return a.pid == b.pid and a.hwnd == b.hwnd

def _load_samples():
    """Örnek altyazılar (spec/sample_quotes.json): oyunlarda gerçekten geçen replikler, her seferinde biri rastgele."""
    try:
        with open(os.path.join(SPEC_DIR, "sample_quotes.json"), encoding="utf-8") as fh:
            out = [(q["game"], q["source"], q["tr"]) for q in json.load(fh)["quotes"] if q.get("tr")]
        if out:
            return out
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return [("Fallout", "War. War never changes.", "Savaş. Savaş asla değişmez.")]


SAMPLES = _load_samples()


def _short(s: str, n: int = 28) -> str:
    s = " ".join(s.split())
    return s if len(s) <= n else s[:n - 1] + "…"


def _fmt_int(n: int) -> str:
    return f"{n:,}".replace(",", " ")


class MakineGameLiveApp(tk.Tk):
    def __init__(self, plugin=None, server=None, session_store=None):
        super().__init__()
        self.title("Makine Live")
        self.configure(bg=BG)
        self.geometry(f"{W}x{H}")
        self.resizable(False, False)
        self._set_window_icon()

        self.dll_path = os.path.join(RUNTIME_DIR, "makine-live.dll")
        self.data_path = DATA_DIR
        self.hud_prefs_path = os.path.join(self.data_path, "hud.json")

        self._ui_queue = queue.Queue()
        self._cfg_lock = threading.Lock()
        self._cfg = {}

        self.plugin = plugin or PluginWrapper(self.dll_path)
        init_rc = self.plugin.initialize(self.data_path)
        # Giriş + çeviri makineceviri.org üzerinden (kullanıcıda API anahtarı yok)
        self.server = server or ServerClient(SERVER_URL, app_version=APP_VERSION, device_name=self._device_name())
        self._status = {}                     # son /api/live/app?action=status yanıtı
        self._gate = None                     # "maintenance" | "update" | None (giriş tuvalinde kapı ekranı)
        self._popups = []                     # sırayla gösterilecek duyurular / güncelleme önerisi
        self._popup_open = False
        self._ann_seen = set()
        self._update_offered = False
        self._status_tick_started = False
        self.session = session_store or SessionStore()
        self.profile = None                   # app_me / app_login yanıtı; None = giriş yapılmamış
        self._notice = None                   # (metin, tür) — kota / anahtar yok / bağlantı
        self._skip_keys = set()               # 422: sunucunun çöp saydığı satırlar (yeniden sorulmaz)
        self._gate_poll_armed = False         # bakım ekranının tek yoklama zinciri
        self.discord = DiscordPresence()      # Discord'da gösterme (yerel IPC; Discord kapalıysa bekler)
        self._session_lines = 0               # bu oturumda (F9'dan beri) çevrilen diyalog

        # Yalnızca oyunlar algılanır (game_library): oyun yoksa hedef boştur ve hiçbir şey okunmaz
        self.detected_game_name = ""
        self.detected_game_slug = ""
        self.detected_exe = ""
        self.detected_appid = ""              # Steam numarası (portaldaki oyun görseli)
        self.library = get_library()
        self.library.on_change = lambda: self._post(self._apply_detection)
        self._adding_game = False

        self.is_running = False
        self.is_locked = False
        self.region = (0, 0, 800, 400)
        self.worker_thread = None
        self._started_at = None

        # Altyazı zamanlaması (subtitle_timing.py): takip worker'da, zamanlayıcı ana thread'de
        self.tracker = LineTracker()
        self.scheduler = SubtitleScheduler()
        self._shown_disp = None
        self._flash_id = 10 ** 9
        self._line_t0 = {}                    # satır id -> ilk görüldüğü an (algılama→gösterim süresi)
        self._executor = ThreadPoolExecutor(max_workers=3, thread_name_prefix="ceviri")
        self._tr_cache = OrderedDict()        # boşluksuz anahtar -> (çeviri, tür, an); oturum içi
        self._tr_lock = threading.Lock()
        self._inflight = {}                   # boşluksuz anahtar -> threading.Event (aynı satır iki kez gönderilmez)
        self._dispatch_seq = {}               # satır id -> son gönderim no (büyüyen satırın eski işi atlanır)
        self._gone_at = {}                    # satır id -> ekrandan kalktığı an (bayat iş gönderilmez)
        # Yerel havuz: algılanan oyunun ONAYLI çevirileri bilgisayara iner (kullanıcı onaylayınca), satırlar önce oradan
        self.local_pool = LocalPool(app_data_dir())
        self._pool_slug = ""                  # yerel havuzu etkin oyun
        self._pool_prompt_slug = ""           # "oynayacağınız oyun bu mu?" sorusu açık olan oyun
        self._pool_busy = False
        self._pool_synced_at = 0.0
        self._pool_offline_logged = False
        self._pool_retry_slug = ""            # soru sorulamadı (ağ / eski sunucu): bir sonraki turda yeniden denenir
        self.target = None                    # game_detector.TargetWindow
        self._block_reason = None
        self._last_block_log = 0.0

        self.stats = {"lines": 0, "pool": 0, "verified": 0, "api": 0, "chars_saved": 0,
                      "lat": deque(maxlen=30)}
        self.stage_ms = {"ocr": deque(maxlen=50), "pool": deque(maxlen=50), "api": deque(maxlen=50)}
        self.prefs = {**LOOK_DEFAULTS, "custom_anchor": None, "tab": "canli", "look_sec": "yazi",
                      # Discord profilinde "Makine Live ile Türkçe oynuyor" (Hesap sekmesi; kimlik yoksa gizli)
                      "discord_presence": True,
                      # Konsol / TV modu: oyun konsolda, görüntü Remote Play / yakalama penceresinden okunur
                      # Konsol modu yalnızca GTA VI içindir (console_mode.console_game). Altyazı nerede görünür:
                      # tv = Makine TV, pc = bu bilgisayarın ekranı (monitörde oynayanlar), web = telefon / tablet
                      "mode": "pc", "console_region": list(DEFAULT_REGION), "console_out": "tv",
                      "console_source": "", "console_exes": [], "tv": None, "tv_code": "",
                      "tv_kind": "android",
                      # Yayın: kutu OBS ekran yakalamada / ekran paylaşımında görünsün; OBS tarayıcı kaynağı
                      "stream_visible": False, "obs_source": False}
        # TV bağlantısı (Makine TV, ev ağı). Durum değişiklikleri ana thread'e taşınır.
        self.tv_link = TvLink(os.environ.get("COMPUTERNAME", "PC"),
                              on_state=lambda s, d: self._post(self._on_tv_state, s, d))
        self._tv_line_id = 0
        self._sheet_top = None
        # Telefon / tablet ekranı ve OBS tarayıcı kaynağı (yerel HTTP, yalnızca açıkken)
        self.web = SubtitleWeb(LOGO_PATH, on_clients=lambda n: self._post(self._on_web_clients, n))
        self._web_clients = 0
        # "Yayında göster" (PC oyunu): kutu yakalamaya açıkken okuma oyun penceresinin kendi içeriğinden yapılır.
        # Pencereden okunamayan oyunlarda (tamamen siyah) kutu yakalamadan gizli kalır ve ekran kopyası okunur.
        self._wincap = {"key": None, "ok": None, "next": 0.0, "fails": 0}

        self._sample = random.choice(SAMPLES)    # önizleme / "Örnek göster" repliği
        self.fonts = ui.Fonts(self)
        self.c = tk.Canvas(self, width=W, height=H, bg=BG, highlightthickness=0, bd=0)
        self.c.pack(fill="both", expand=True)
        self.u = ui.UI(self, self.c, self.fonts)

        self.overlay = SubtitleOverlay(self, on_moved=self._on_overlay_moved)
        self._load_prefs()
        self._build_ui()
        self._load_settings()
        self._snapshot_cfg()
        self._apply_mode(initial=True)

        if init_rc != 0:
            reason = self.plugin.last_error() or f"kod {init_rc}"
            self.log(f"DLL başlatılamadı: {reason}")
            self.after(600, lambda: self.toast("DLL başlatılamadı · ayrıntı Kayıt'ta", warn=True))
        self._scan_library(first=True)

        # Giriş ekranı: panel yalnızca geçerli bir oturumla görünür
        self.c.pack_forget()
        self.login = LoginView(self, self.fonts, self._login_submit, self._validate_session,
                               self._switch_account, on_device=self._device_login, on_device_cancel=self._device_cancel,
                               on_device_reopen=self._device_reopen)
        self._device_gen = 0                  # Makine Çeviri hesabıyla girişin sırası (Vazgeç eskisini durdurur)
        self._device_url = ""
        self.login.c.pack(fill="both", expand=True)
        saved = self.session.load()
        if saved and saved.get("server", self.server.base_url) == self.server.base_url:
            self.server.token = saved["token"]
            self._saved_user = saved.get("user", "")
        else:
            self._saved_user = ""
        # Önce sunucu durumu: bakım ve zorunlu güncelleme girişten önce kontrol edilir
        self.login.show_checking(self._saved_user or "Makine Live")
        self.login.c.itemconfigure(self.login.chk_title, text="Sunucuya bağlanılıyor")
        self.login.c.itemconfigure(self.login.chk_sub, text="Sürüm ve bakım durumu kontrol ediliyor…")
        self._fetch_status(self._on_startup_status)

        self._style_title_bar()
        self._start_hotkey_listener()
        self.after(30, self._drain_ui_queue)
        self.after(50, self._tick_scheduler)
        self.after(1000, self._tick_clock)
        self.protocol("WM_DELETE_WINDOW", self.destroy)

    def _set_window_icon(self):
        try:
            self._icon_photo = ImageTk.PhotoImage(app_mark_image(64))
            self.iconphoto(True, self._icon_photo)
        except Exception:
            pass

    # ─────────────────────────────────────────────────────────
    # Oturum
    # ─────────────────────────────────────────────────────────

    @staticmethod
    def _device_name() -> str:
        return f"{os.environ.get('COMPUTERNAME', 'PC')} · Makine Live {APP_VERSION_LABEL}"

    def _login_submit(self, username, password):
        def worker():
            try:
                profile = self.server.login(username, password, self._device_name())
                self._post(self._on_login_ok, profile)
            except AuthRequired as e:
                self._post(self.login.show_form, str(e) or "Kullanıcı adı veya şifre hatalı.", username)
            except NetworkError:
                self._post(self.login.show_form, "Sunucuya ulaşılamıyor. İnternet bağlantınızı kontrol edin.", username)
            except ServerError as e:
                self._post(self.login.show_form, str(e), username)

        threading.Thread(target=worker, daemon=True).start()

    def _on_login_ok(self, profile):
        try:
            self.session.save(self.server.token, profile.get("user", ""), self.server.base_url)
        except OSError as e:
            self.log(f"oturum kaydedilemedi: {e}")
        self.log(f"giriş  {profile.get('user')} ({profile.get('role')})")
        self._on_signed_in(profile)

    def _validate_session(self):
        """Kayıtlı jetonu sunucuda doğrula. Ağ hatasında jeton silinmez (çevrimdışı ekranı), 401'de silinir."""
        user = getattr(self, "_saved_user", "") or "Hesap"
        self.login.show_checking(user)

        def worker():
            try:
                profile = self.server.me()
                self._post(self._on_signed_in, profile)
            except AuthRequired:
                self._post(self._on_auth_lost, "Oturumun süresi doldu. Lütfen yeniden giriş yapın.")
            except ServerError as e:
                if e.code == "banned":
                    self._post(self._on_auth_lost, str(e))
                elif e.code == "maintenance":
                    self._post(self._fetch_status, self._on_startup_status)
                else:
                    self._post(self.login.show_offline, user, "İnternet bağlantısı gelince tekrar deneyin.")
            except NetworkError:
                self._post(self.login.show_offline, user, "İnternet bağlantısı gelince tekrar deneyin.")

        threading.Thread(target=worker, daemon=True).start()

    def _switch_account(self):
        self.server.token = None
        self.session.clear()
        self.login.show_choice()

    # Makine Çeviri hesabıyla giriş: kod + tarayıcıda onay (makineceviri.org/live/baglan/), uygulama bekler.

    def _device_login(self):
        self._device_gen += 1
        gen = self._device_gen

        def worker():
            try:
                start = self.server.device_start(self._device_name())
            except NetworkError:
                self._post(self.login.show_choice, "Sunucuya ulaşılamıyor. İnternet bağlantınızı kontrol edin.")
                return
            except (ServerError, AuthRequired) as e:
                self._post(self.login.show_choice, str(e) or "Giriş başlatılamadı. Biraz sonra tekrar deneyin.")
                return
            if gen != self._device_gen:
                return
            self._device_url = start.get("url", "")
            self._post(self.login.show_device, start.get("code", ""))
            self._post(self._device_reopen)
            deadline = time.time() + float(start.get("expires_in", 600))
            interval = max(2.0, float(start.get("interval", 3)))
            offline = False
            while gen == self._device_gen and time.time() < deadline:
                time.sleep(interval)
                if gen != self._device_gen:
                    return
                try:
                    profile = self.server.device_poll(start["device"])
                except NetworkError:
                    if not offline:
                        offline = True
                        self._post(self.login.set_device_note, "Bağlantı koptu · yeniden deneniyor…")
                    continue
                except (ServerError, AuthRequired) as e:
                    if gen == self._device_gen:
                        expired = isinstance(e, ServerError) and e.status == 410
                        self._post(self.login.show_choice, "Kodun süresi doldu. Tekrar deneyin." if expired else str(e))
                    return
                if offline:
                    offline = False
                    self._post(self.login.set_device_note, "Onaylayınca uygulama kendiliğinden giriş yapar.")
                if profile:
                    if gen == self._device_gen:
                        self._device_gen += 1
                        self._post(self._on_login_ok, profile)
                    return
            if gen == self._device_gen:
                self._post(self.login.show_choice, "Kodun süresi doldu. Tekrar deneyin.")

        threading.Thread(target=worker, daemon=True).start()

    def _device_reopen(self):
        if self._device_url:
            webbrowser.open(self._device_url)

    def _device_cancel(self):
        self._device_gen += 1
        self.login.show_choice()

    def _on_signed_in(self, profile):
        self.profile = profile
        if self.prefs["discord_presence"]:
            self.discord.start()
        self.login.c.pack_forget()
        self.c.pack(fill="both", expand=True)
        self._update_account()
        self._executor.submit(self.server.warm)
        self._check_server_async()
        self._pool_on_game(self.target)
        self._refresh_status()
        if not self._status_tick_started:
            self._status_tick_started = True
            self.after(STATUS_POLL_S * 1000, self._status_tick)
        if not getattr(self, "_pool_tick_started", False):
            self._pool_tick_started = True
            self.after(POOL_TICK_S * 1000, self._pool_tick)
        if not profile.get("translation_ready", True):
            self._set_notice("Sunucuda çeviri anahtarı yok · yalnızca havuzdaki satırlar gelir", "no_key")

    def _on_auth_lost(self, message="Oturumunuz kapatıldı. Lütfen yeniden giriş yapın."):
        """401: jeton geçersiz (süre, iptal ya da hesap silindi). Pipeline durur, giriş ekranına dönülür."""
        if self.is_running:
            self.toggle_running()
        user = (self.profile or {}).get("user") or getattr(self, "_saved_user", "")
        self.profile = None
        self.server.token = None
        self.session.clear()
        self.c.pack_forget()
        self.login.c.pack(fill="both", expand=True)
        self.login.show_choice(message)
        self.log("oturum  kapandı")

    def logout(self):
        if self.is_running:
            self.toggle_running()
        threading.Thread(target=self.server.logout, daemon=True).start()
        user = (self.profile or {}).get("user", "")
        self.session.clear()
        self.profile = None
        self.c.pack_forget()
        self.login.c.pack(fill="both", expand=True)
        self.login.show_choice()
        self.log("çıkış yapıldı")

    def _update_account(self):
        p = self.profile or {}
        user = p.get("user", "")
        role = ROLE_LABELS.get(p.get("role", ""), p.get("role", ""))
        self.c.itemconfigure(self.hdr_sub, text=f"{user} · {role}" if user else "")
        self.c.itemconfigure(self.acc_user, text=user or "—")
        self.c.itemconfigure(self.acc_kind, text="Makine Çeviri hesabı" if p.get("role") == "player" else "Ekip hesabı" if user else "")
        chip_f = self.fonts.ui(11, semi=True)
        cw = tkfont.Font(font=chip_f).measure(role.upper()) + 16
        ux = PAD + 16 + tkfont.Font(font=self.fonts.disp(18)).measure(user or "—") + 12
        color = ACCENT if p.get("role") == "tester" else (VERIFIED if p.get("role") == "admin" else TEXT2)
        self.c.coords(self.acc_chip_bg, ux, self.acc_y + 15)
        self.c.itemconfigure(self.acc_chip_bg, image=ui.rrect_image(round(cw), 20, 10, fill=mix(SURFACE, color, 0.14),
                                                                    outline=mix(SURFACE, color, 0.5)))
        self.c.coords(self.acc_chip, ux + cw / 2, self.acc_y + 25)
        self.c.itemconfigure(self.acc_chip, text=role.upper(), fill=color)

    def _set_notice(self, text, kind=None):
        self._notice = (text, kind) if text else None
        self._tick_clock(reschedule=False)

    def _clear_notice(self, *kinds):
        if self._notice and (not kinds or self._notice[1] in kinds):
            self._set_notice(None)

    # ─────────────────────────────────────────────────────────
    # Pencere
    # ─────────────────────────────────────────────────────────

    def _toplevel_hwnd(self):
        """Panelin üst düzey pencere tanıtıcısı (Tk'nın iç penceresinin ebeveyni)."""
        try:
            return user32.GetParent(self.winfo_id()) or self.winfo_id()
        except Exception:  # noqa: BLE001
            return 0

    def _style_title_bar(self):
        """Windows 11: koyu başlık çubuğu; başlık ve kenar rengi zeminle aynı."""
        try:
            self.update_idletasks()
            hwnd = user32.GetParent(self.winfo_id()) or self.winfo_id()
            dwm = ctypes.windll.dwmapi

            def set_attr(attr, value):
                v = ctypes.c_int(value)
                dwm.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(v), ctypes.sizeof(v))

            def colorref(hexc):
                r, g, b = ui.rgb(hexc)
                return r | (g << 8) | (b << 16)

            set_attr(20, 1)                    # DWMWA_USE_IMMERSIVE_DARK_MODE
            set_attr(35, colorref(BG))         # DWMWA_CAPTION_COLOR
            set_attr(34, colorref(LINE))       # DWMWA_BORDER_COLOR
            set_attr(36, colorref(TEXT2))      # DWMWA_TEXT_COLOR
        except Exception:
            pass

    # ─────────────────────────────────────────────────────────
    # Thread-safe UI köprüsü
    # ─────────────────────────────────────────────────────────

    def _post(self, fn, *args):
        """Arka plan thread'lerinden ana thread'e iş gönderir."""
        self._ui_queue.put((fn, args))

    def _drain_ui_queue(self):
        # Bir işin hatası köprüyü durdurmasın: hata günlüğe yazılır, sıradaki işler ve sonraki turlar devam eder
        try:
            while True:
                try:
                    fn, args = self._ui_queue.get_nowait()
                except queue.Empty:
                    break
                try:
                    fn(*args)
                except Exception:  # noqa: BLE001
                    _FILE_LOG.error("arayüz işi başarısız: %s\n%s", getattr(fn, "__name__", fn), traceback.format_exc())
        finally:
            self.after(30, self._drain_ui_queue)

    def _snapshot_cfg(self, *_):
        with self._cfg_lock:
            p = self.prefs
            self._cfg = {
                "src": SOURCE_LANG,
                "tgt": TARGET_LANG,
                "only_focus": True,          # her zaman: yalnızca oyun öndeyken okunur (konsol modunda pencere açık olmalı)
                "mode": p["mode"],
                "console_region": tuple(p["console_region"]),
                "stream_visible": p["stream_visible"],
            }

    def _cfg_copy(self):
        with self._cfg_lock:
            return dict(self._cfg)

    # ─────────────────────────────────────────────────────────
    # Arayüz
    # ─────────────────────────────────────────────────────────

    def _build_ui(self):
        c, u, f = self.c, self.u, self.fonts

        # ── Başlık ──
        c.create_text(PAD, 30, anchor="w", text="Makine Live", font=f.disp(18), fill=TEXT)
        wm = tkfont.Font(font=f.disp(18)).measure("Makine Live")
        self.hdr_sub = c.create_text(PAD + wm + 8, 32, anchor="w", text="", font=f.ui(12), fill=TEXT3)

        # ── Oyun kartı ──
        y = 56
        c.create_image(PAD, y, anchor="nw", image=ui.rrect_image(INNER, 58, 8, fill=SURFACE, outline=LINE))
        self.game_icon = ui.AnimatedIcon(u, PAD + 26, y + 29, "game", 22, TEXT2)
        self.game_name = c.create_text(PAD + 50, y + 21, anchor="w", text="Oyunlar taranıyor…",
                                       font=f.ui(14, semi=True), fill=TEXT)
        self.game_meta = c.create_text(PAD + 50, y + 40, anchor="w", text="", font=f.code(11), fill=TEXT3)
        self.btn_refresh = ui.Button(u, W - PAD - 46, y + 11, 36, 36, icon="refresh", kind="ghost",
                                     command=lambda: self.refresh_active_game(animate=True), parent_bg=SURFACE)
        # Algılanan oyun yoksa: öndeki pencereyi oyun olarak ekle. Varsa: "bu bir oyun değil" işareti.
        self.btn_game = ui.Button(u, W - PAD - 46 - 6 - GAME_BTN_W, y + 11, GAME_BTN_W, 36, text="Oyun ekle",
                                  kind="ghost", command=self._game_action, parent_bg=SURFACE, font=f.ui(12))
        self._game_text_w = W - PAD - 46 - 6 - GAME_BTN_W - 8 - (PAD + 50)

        # ── Ana düğme ──
        y = 128
        self.btn_main = ui.Button(u, PAD, y, INNER, 46, text="Başlat", icon="play_stop", kind="primary",
                                  command=self.toggle_running, keycap="F9", font=f.ui(14, semi=True), icon_size=18)
        self.status_icon = ui.AnimatedIcon(u, PAD + 7, y + 64, "link", 14, TEXT3)
        self.status_txt = c.create_text(PAD + 20, y + 64, anchor="w", text="", font=f.ui(12), fill=TEXT3)

        # ── Canlı önizleme ──
        y = 212
        c.create_text(PAD, y + 13, anchor="w", text="Önizleme", font=f.ui(12, semi=True), fill=TEXT2)
        self.btn_lock = ui.Button(u, W - PAD - 132, y, 132, 26, text="Kilitle", icon="lock", kind="ghost",
                                  command=self.toggle_lock, keycap="F8", font=f.ui(12), icon_size=16)
        self.btn_lock.icon.set(t=1.0)   # başlangıçta taşıma modu: kilit açık
        self.btn_sample = ui.Button(u, W - PAD - 132 - 120, y, 116, 26, text="Örnek göster", icon="subtitle",
                                    kind="ghost", command=self.flash_sample, font=f.ui(12), icon_size=16,
                                    on_hover=lambda on: on and self.btn_sample.icon.animate(1.0, 320, frm=0.0))
        self.btn_sample.icon.set(t=1.0)
        self.preview_y = y + 34
        self.preview_h = 112
        c.create_image(PAD, self.preview_y, anchor="nw", image=ui.checker_image(INNER, self.preview_h, 8))
        self.preview_item = c.create_image(W / 2, self.preview_y + self.preview_h / 2)
        self.preview_scale = c.create_text(W - PAD - 10, self.preview_y + self.preview_h - 10, anchor="e",
                                           font=f.code(10), fill=TEXT3)
        self._preview_job = None

        # ── Sekmeler ──
        self.tabs = ui.Tabs(u, PAD, TAB_Y, INNER, TABS, self.prefs.get("tab", "canli"), command=self._switch_tab)

        self._build_tab_live("tab_canli")
        self._build_tab_translate("tab_ceviri")
        self._build_tab_look("tab_gorunum")
        self._build_tab_console("tab_konsol")
        self._build_tab_account("tab_hesap")
        self._show_tab(self.tabs.value, animate=False)

        # ── Alt bilgi: kısayollar ──
        fy = H - 26
        c.create_line(PAD, fy - 20, W - PAD, fy - 20, fill=LINE)
        x = PAD
        for key, label in (("F8", "kilitle"), ("F9", "başlat / durdur"), ("F10", "bölge seç")):
            kc = ui.Keycap(u, x, fy - 9, key)
            x += kc.w + 6
            c.create_text(x, fy, anchor="w", text=label, font=f.ui(12), fill=TEXT3)
            x += tkfont.Font(font=f.ui(12)).measure(label) + 18

        self._toast_items = []
        self._tick_clock(reschedule=False)
        self._render_preview()


    # ── Canlı sekmesi ──

    def _build_tab_live(self, tag):
        c, u, f = self.c, self.u, self.fonts
        y = CONTENT_Y
        tiles = [("lines", "Satır"), ("hit", "Havuz isabeti"), ("lat", "Ort. gecikme"), ("saved", "DeepL tasarrufu")]
        tw = (INNER - 3 * 8) / 4
        self.stat_items = {}
        for i, (key, label) in enumerate(tiles):
            x = PAD + i * (tw + 8)
            c.create_image(x, y, anchor="nw", image=ui.rrect_image(round(tw), 64, 6, fill=SURFACE, outline=LINE), tags=tag)
            v = c.create_text(x + 12, y + 24, anchor="w", text="0", font=f.disp(19), fill=TEXT, tags=tag)
            s = c.create_text(x + 12, y + 47, anchor="w", text=label, font=f.ui(11), fill=TEXT3, tags=tag)
            self.stat_items[key] = (v, s)

        y += 78
        c.create_text(PAD, y + 7, anchor="w", text="Gecikme · son 30 satır", font=f.ui(11), fill=TEXT3, tags=tag)
        c.create_oval(W - PAD - 106, y + 4, W - PAD - 100, y + 10, fill=VERIFIED, outline="", tags=tag)
        c.create_text(W - PAD - 94, y + 7, anchor="w", text="havuz", font=f.ui(11), fill=TEXT3, tags=tag)
        c.create_oval(W - PAD - 46, y + 4, W - PAD - 40, y + 10, fill=TEXT2, outline="", tags=tag)
        c.create_text(W - PAD, y + 7, anchor="e", text="DeepL", font=f.ui(11), fill=TEXT3, tags=tag)
        self.spark = c.create_image(PAD, y + 18, anchor="nw", tags=tag)

        y += 60
        c.create_text(PAD, y + 13, anchor="w", text="Geçmiş", font=f.ui(13, semi=True), fill=TEXT, tags=tag)
        self.hist_count = c.create_text(PAD + 60, y + 13, anchor="w", text="", font=f.code(11), fill=TEXT3, tags=tag)
        self.btn_log = ui.Button(u, W - PAD - 136, y, 72, 26, text="Kayıt", kind="ghost", command=self._toggle_log,
                                 font=f.ui(12), tags=(tag,))
        self.btn_clear = ui.Button(u, W - PAD - 60, y, 60, 26, text="Sil", icon="trash", kind="ghost",
                                   command=self._clear_history, font=f.ui(12), icon_size=16, tags=(tag,),
                                   on_hover=lambda on: on and self.btn_clear.icon.play(360))
        list_y = y + 34
        list_h = H - 58 - list_y
        self.history = HistoryList(c, self.fonts, INNER, list_h, self._copy_row, self._replay_row)
        self.history_win = c.create_window(PAD, list_y, anchor="nw", window=self.history, tags=tag)
        self.log_box = tk.Text(c, bg=SURFACE, fg=TEXT2, insertbackground=TEXT, relief="flat", bd=0,
                               font=f.code(11), wrap="word", highlightthickness=0, padx=10, pady=8)
        self.log_box.configure(state="disabled")
        self.log_win = c.create_window(PAD, list_y, anchor="nw", window=self.log_box, width=INNER, height=list_h,
                                       state="hidden")
        self._log_visible = False
        self._refresh_stats()

    # ── Çeviri sekmesi ──

    def _build_tab_translate(self, tag):
        c, u, f = self.c, self.u, self.fonts
        y = CONTENT_Y
        c.create_text(PAD, y + 8, anchor="w", text="Dil", font=f.ui(12, semi=True), fill=TEXT2, tags=tag)
        for x, code, name in ((PAD, "en", "İngilizce"), (PAD + 164, "tr", "Türkçe")):
            c.create_image(x, y + 22, anchor="nw", tags=tag, image=ui.rrect_image(120, 34, 6, fill=SURFACE, outline=LINE))
            c.create_text(x + 12, y + 39, anchor="w", text=code, font=f.code(12), fill=TEXT3, tags=tag)
            c.create_text(x + 38, y + 39, anchor="w", text=name, font=f.ui(13), fill=TEXT2, tags=tag)
        ui.AnimatedIcon(u, PAD + 142, y + 39, "arrow", 18, TEXT3, tags=(tag,))
        c.create_text(PAD, y + 74, anchor="nw", font=f.ui(12), fill=TEXT3, tags=tag, width=INNER,
                      text="Çeviri sunucuda yapılır, API anahtarı gerekmez. Bir satır 3 farklı oyuncudan uyuşan "
                           "çeviri alınca onaylanır ve herkese havuzdan anında gelir.")
        y += 124
        c.create_line(PAD, y, W - PAD, y, fill=LINE, tags=tag)
        y += 16
        c.create_text(PAD, y + 8, anchor="w", text="Okunacak bölge", font=f.ui(12, semi=True), fill=TEXT2, tags=tag)
        self.region_txt = c.create_text(PAD, y + 38, anchor="w", font=f.code(14), fill=TEXT, tags=tag)
        self.region_hint = c.create_text(PAD, y + 58, anchor="w", font=f.ui(11), fill=TEXT3, tags=tag,
                                         text="x, y   ·   genişlik × yükseklik")
        self.btn_region = ui.Button(u, W - PAD - 128, y + 22, 128, 34, text="Seç", icon="region", kind="secondary",
                                    command=self.start_region_selection, keycap="F10", font=f.ui(13, semi=True),
                                    tags=(tag,), on_hover=lambda on: on and self.btn_region.icon.play(400))
        self._update_region_label()
        y += 84
        c.create_text(PAD, y, anchor="nw", font=f.ui(11), fill=TEXT3, tags=tag, width=INNER,
                      text="Yalnızca algılanan oyun öndeyken ve bölgenin üstünde başka pencere yokken okunur.")

    # ── Görünüm sekmesi ──

    def _build_tab_look(self, tag):
        """
        Üç bölüm (sekme dolu olduğu için): Yazı (stil, yazı tipi, boyut, satır, süre), Kutu (renkler, opaklık,
        kontur, gölge, genişlik), Konum ve yayın. Her değişiklik önizlemede ve oyundaki kutuda anında görünür.
        """
        c, u, f = self.c, self.u, self.fonts
        p = self.prefs
        y = CONTENT_Y
        self.seg_look = ui.Segmented(u, PAD, y, INNER, 30, LOOK_SECTIONS, p["look_sec"],
                                     command=self._switch_look_section, tags=(tag,))
        y0 = y + 44

        # ── Yazı ──
        t = (tag, "look_yazi")
        y = y0
        c.create_text(PAD, y + 8, anchor="w", text="Hazır stil", font=f.ui(12, semi=True), fill=TEXT2, tags=t)
        self.seg_style = ui.Segmented(u, PAD, y + 22, INNER, 32, STYLE_LABELS, p["style"],
                                      command=self._apply_preset, tags=t)
        y += 66
        c.create_text(PAD, y + 15, anchor="w", text="Yazı tipi", font=f.ui(13), fill=TEXT, tags=t)
        self._font_opts = available_fonts()
        # Her seçenek kendi yazı tipiyle görünür (Tk'nın aile adları; Segoe paneldeki yazı tipi)
        tk_family = {"segoe": f.display, "trebuchet": "Trebuchet MS"}
        self.dd_font = ui.Dropdown(u, W - PAD - 190, y, 190, 30, [lab for _, lab in self._font_opts],
                                   FONTS.get(p["font"], FONTS["segoe"])[0],
                                   command=lambda lab: self._set_pref("font", next(k for k, v in self._font_opts if v == lab)),
                                   tags=t, fonts={lab: (tk_family.get(k, lab), -14, "bold") for k, lab in self._font_opts})
        y += 42
        self.sl_size = ui.Slider(u, PAD, y, INNER, "Yazı boyutu", 16, 48, p["font_px"], "{} px",
                                 command=lambda v, fin: self._set_pref("font_px", v, fin), tags=t)
        y += 48
        c.create_text(PAD, y + 15, anchor="w", text="En fazla satır", font=f.ui(13), fill=TEXT, tags=t)
        self.seg_lines = ui.Segmented(u, W - PAD - 150, y, 150, 30, LINES_LABELS, p["max_lines"],
                                      command=lambda v: self._set_pref("max_lines", v), tags=t)
        y += 44
        self.sl_hold = ui.Slider(u, PAD, y, INNER, "En kısa gösterim", 0.6, 3.0, p["min_show_ms"] / 1000,
                                 "{:.1f} sn", step=0.2,
                                 command=lambda v, fin: self._set_pref("min_show_ms", int(v * 1000), fin), tags=t)

        # ── Kutu ──
        t = (tag, "look_kutu")
        y = y0
        self.sw_text = ui.Swatches(u, PAD, y, INNER, "Yazı rengi", TEXT_COLORS, p["text_color"],
                                   command=lambda v: self._set_pref("text_color", v), tags=t)
        y += 40
        self.sw_band = ui.Swatches(u, PAD, y, INNER, "Bant rengi", BAND_COLORS, p["band_color"],
                                   command=lambda v: self._set_pref("band_color", v), tags=t)
        y += 44
        self.sl_opacity = ui.Slider(u, PAD, y, INNER, "Bant opaklığı", 40, 100, p["opacity"], "%{}",
                                    command=lambda v, fin: self._set_pref("opacity", v, fin), tags=t)
        y += 50
        c.create_text(PAD, y + 15, anchor="w", text="Yazı konturu", font=f.ui(13), fill=TEXT, tags=t)
        self.seg_outline = ui.Segmented(u, W - PAD - 200, y, 200, 30, OUTLINE_LABELS, p["outline"],
                                        command=lambda v: self._set_pref("outline", v), tags=t)
        y += 44
        self.tg_shadow = ui.Toggle(u, PAD, y, INNER, "Gölge", p["shadow"],
                                   command=lambda v: self._set_pref("shadow", v), tags=t)
        y += 32
        self.sl_width = ui.Slider(u, PAD, y, INNER, "En fazla genişlik (ekranın)", 40, 90, p["width"], "%{}",
                                  command=lambda v, fin: self._set_pref("width", v, fin), tags=t)

        # ── Konum ve yayın ──
        t = (tag, "look_konum")
        y = y0
        c.create_text(PAD, y + 8, anchor="w", text="Konum", font=f.ui(12, semi=True), fill=TEXT2, tags=t)
        self.seg_pos = ui.Segmented(u, PAD, y + 22, INNER, 32, POSITION_LABELS, p["position"],
                                    command=lambda v: self._set_pref("position", v), tags=t)
        y += 64
        self.sl_offset = ui.Slider(u, PAD, y, INNER, "Kenardan uzaklık", 2, 30, p["offset"], "%{}",
                                   command=lambda v, fin: self._set_pref("offset", v, fin), tags=t)
        y += 46
        c.create_text(PAD, y + 4, anchor="w", font=f.ui(11), fill=TEXT3, tags=t,
                      text="Serbest: kilit açıkken kutuyu oyunda sürükleyin.")
        y += 22
        c.create_line(PAD, y, W - PAD, y, fill=LINE, tags=t)
        y += 12
        # Yayın: OBS'nin ekran yakalaması ve Discord / Teams ekran paylaşımı kutuyu ancak bu açıkken görür.
        self.tg_stream = ui.Toggle(u, PAD, y, INNER, "Yayında ve ekran paylaşımında göster", p["stream_visible"],
                                   command=self._set_stream_visible, tags=t)
        y += 30
        self.tg_obs = ui.Toggle(u, PAD, y, INNER, "OBS tarayıcı kaynağı", p["obs_source"],
                                command=self._set_obs_source, tags=t)
        y += 28
        self.obs_url_txt = c.create_text(PAD, y + 12, anchor="w", text="", font=f.code(11), fill=TEXT3,
                                         tags=t + ("obs_row",))
        self.btn_obs_copy = ui.Button(u, W - PAD - 76, y, 76, 24, text="Kopyala", kind="ghost", font=f.ui(12),
                                      command=lambda: self._copy_text(self.web.obs_url, "OBS adresi kopyalandı"),
                                      tags=t + ("obs_row",))
        y += 38
        ui.Button(u, PAD, y, INNER, 32, text="Görünümü varsayılana döndür", icon="refresh", kind="ghost",
                  command=self._reset_look, font=f.ui(12), icon_size=16, tags=t)

    def _switch_look_section(self, key, animate=True):
        self.prefs["look_sec"] = key
        self._show_look_section(animate=animate)
        self._save_hud_prefs()

    def _show_look_section(self, animate=False):
        cur = self.prefs["look_sec"]
        visible = self._tab_now == "gorunum"
        for k, _ in LOOK_SECTIONS:
            self.c.itemconfigure(f"look_{k}", state="normal" if visible and k == cur else "hidden")
        if visible and cur == "konum":
            self._show_items("obs_row", self.prefs["obs_source"] and self.web.running)
        else:
            self._show_items("obs_row", False)
        if animate and visible:
            tag = f"look_{cur}"
            moved = [8]
            self.c.move(tag, 0, 8)

            def step(e):
                target = round(8 * (1 - e))
                self.c.move(tag, 0, target - moved[0])
                moved[0] = target
            ui.Tween(self, 180, step)

    def _apply_preset(self, style):
        """Hazır stil: biçim (bant / yalın / sinema) ve onunla gelen renk, opaklık, kontur, gölge."""
        self.prefs["style"] = style
        self.prefs.update(STYLE_PRESETS.get(style, {}))
        self._sync_look_controls()
        self._set_pref("style", style)

    def _reset_look(self):
        anchor = self.prefs.get("custom_anchor")
        self.prefs.update(LOOK_DEFAULTS)
        self.prefs["custom_anchor"] = anchor
        self._sync_look_controls()
        self._set_pref("style", self.prefs["style"])
        self.toast("Görünüm varsayılana döndü")

    def _sync_look_controls(self):
        p = self.prefs
        self.seg_style.set(p["style"], animate=True)
        self.dd_font.set(FONTS.get(p["font"], FONTS["segoe"])[0])
        self.sl_size.set(p["font_px"])
        self.seg_lines.set(p["max_lines"], animate=True)
        self.sl_hold.set(p["min_show_ms"] / 1000)
        self.sw_text.set(p["text_color"], animate=True)
        self.sw_band.set(p["band_color"], animate=True)
        self.sl_opacity.set(p["opacity"])
        self.seg_outline.set(p["outline"], animate=True)
        self.tg_shadow.set(p["shadow"], animate=True)
        self.sl_width.set(p["width"])
        self.seg_pos.set(p["position"], animate=True)
        self.sl_offset.set(p["offset"])

    # ── Konsol / TV sekmesi ──

    def _build_tab_console(self, tag):
        """
        PS5 / Xbox: PC Remote Play ya da yakalama penceresini okur. Çeviri nerede görünür: TV'deki Makine TV,
        bu bilgisayarın ekranı (monitörde oynayanlar: konsol görüntüsü PC'de tam ekran) ya da telefon / tablet.
        Konsol modu yalnızca GTA VI içindir; oyun seçimi yoktur.
        """
        c, u, f = self.c, self.u, self.fonts
        p = self.prefs
        y = CONTENT_Y
        c.create_text(PAD, y + 8, anchor="w", text="Oyun nerede?", font=f.ui(12, semi=True), fill=TEXT2, tags=tag)
        self.seg_mode = ui.Segmented(u, PAD, y + 22, INNER, 34, [("pc", "Bu bilgisayarda"), ("konsol", "Konsolda · PS5 / Xbox")],
                                     p["mode"], command=self._set_mode, tags=(tag,))
        y += 68
        self.con_cards = {"src": self._console_card(tag, y, "game", "Konsol görüntüsü",
                                                    [("Pencere seç", self._pick_console_window, 104)])}
        y += 70
        c.create_text(PAD, y + 8, anchor="w", text="Altyazı nerede görünsün?", font=f.ui(12, semi=True), fill=TEXT2,
                      tags=tag)
        self.seg_out = ui.Segmented(u, PAD, y + 22, INNER, 34, [("tv", "TV'de"), ("pc", "Bu ekranda"), ("web", "Telefonda")],
                                    p["console_out"], command=self._set_console_out, tags=(tag,))
        y += 68
        card = self._console_card(tag, y, "link", "TV", [("TV bul", self._tv_search, 70), ("Kur", self._tv_install_sheet, 52)])
        card["web_btn"] = ui.Button(u, W - PAD - 10 - 84, y + 14, 84, 32, text="Kopyala", kind="secondary",
                                    command=lambda: self._copy_text(self.web.phone_url, "Telefon adresi kopyalandı"),
                                    font=f.ui(12), tags=(tag,), parent_bg=SURFACE)
        self.con_cards["out"] = card
        y += 70
        c.create_text(PAD, y, anchor="nw", font=f.ui(11), fill=TEXT3, tags=tag, width=INNER,
                      text=f"Konsol modu yalnızca {DEFAULT_GAME} içindir; bütün çeviriler onun havuzundan gelir. "
                           "Monitörde oynuyorsanız konsol görüntüsünü bu bilgisayarda tam ekran açıp \"Bu ekranda\"yı "
                           "ya da telefonu ekranın altına koyup \"Telefonda\"yı seçin.")
        self._refresh_console_cards()

    def _console_card(self, tag, y, icon, title, buttons):
        c, u, f = self.c, self.u, self.fonts
        h = 60
        c.create_image(PAD, y, anchor="nw", tags=tag, image=ui.rrect_image(INNER, h, 8, fill=SURFACE, outline=LINE))
        ic = ui.AnimatedIcon(u, PAD + 24, y + h / 2, icon, 20, TEXT3, tags=(tag,))
        if icon == "link":
            ic.set(t=0.7)          # dalgalar görünsün (t=0 yalnızca nokta)
        bx = W - PAD - 10
        btns = []
        for label, cmd, bw in reversed(buttons):
            bx -= bw
            btns.append(ui.Button(u, bx, y + 14, bw, 32, text=label, kind="secondary", command=cmd,
                                  font=f.ui(12), tags=(tag,), parent_bg=SURFACE))
            bx -= 6
        text_w = bx - (PAD + 46) - 4
        t = c.create_text(PAD + 46, y + 20, anchor="w", text=title, font=f.ui(13, semi=True), fill=TEXT, tags=tag)
        m = c.create_text(PAD + 46, y + 40, anchor="w", text="", font=f.code(11), fill=TEXT3, tags=tag)
        return {"icon": ic, "title": t, "meta": m, "w": text_w, "full_w": W - PAD - 10 - (PAD + 46) - 4,
                "buttons": btns[::-1]}

    def _set_card(self, key, title=None, meta=None, ok=None, width=None):
        card = self.con_cards[key]
        fit = self._fit_w
        w = width or card["w"]
        if title is not None:
            self.c.itemconfigure(card["title"], text=fit(title, self.fonts.ui(13, semi=True), w))
        if meta is not None:
            self.c.itemconfigure(card["meta"], text=fit(meta, self.fonts.code(11), w))
        if ok is not None:
            card["icon"].set(color=VERIFIED if ok is True else (ACCENT if ok == "wait" else TEXT3))

    def _show_items(self, tag_or_id, visible):
        """Sekmenin içinde duruma göre görünen öğeler (sekme gizliyken dokunulmaz: sekme geçişi hepsini açar)."""
        on = getattr(self, "_tab_now", None) == ("konsol" if tag_or_id != "obs_row" else "gorunum")
        self.c.itemconfigure(tag_or_id, state="normal" if (visible and on) else "hidden")

    def _refresh_console_cards(self):
        if not hasattr(self, "con_cards"):
            return
        p = self.prefs
        konsol = p["mode"] == "konsol"
        t = self.target if isinstance(self.target, ConsoleTarget) else None
        if not konsol:
            self._set_card("src", "Konsol görüntüsü", "konsol modu kapalı", ok=None)
        elif t is None:
            self._set_card("src", "Konsol görüntüsü bulunamadı", "PS Remote Play / Xbox / yakalama penceresini açın",
                           ok="wait")
        else:
            w, h = t.size()
            self._set_card("src", t.source.label, f"{CONSOLE_LABELS.get(t.source.console, '')} · {w}×{h}", ok=True)

        out = p["console_out"]
        card = self.con_cards["out"]
        tv_btns, web_btn = card["buttons"], card["web_btn"]
        for b in tv_btns:
            self._show_items(b.tag, out == "tv")
        self._show_items(web_btn.tag, out == "web")
        card["icon"].set(name={"pc": "subtitle", "web": "phone"}.get(out, "link"), t=0.7 if out == "tv" else 1.0)
        if out == "pc":
            self._set_card("out", "Bu bilgisayarın ekranı",
                           "konsol görüntüsünün üstünde · tam ekran açın" if konsol else "konsol modu kapalı",
                           ok=True if konsol else None, width=card["full_w"])
        elif out == "web":
            if not konsol:
                self._set_card("out", "Telefon / tablet", "konsol modu kapalı", ok=None)
            elif not self.web.running:
                self._set_card("out", "Telefon / tablet", f"açılamadı · {self.web.error or 'kapı dolu'}", ok="wait")
            else:
                n = self._web_clients
                self._set_card("out", self.web.phone_url.replace("http://", ""),
                               f"{n} cihaz bağlı" if n else "telefonun tarayıcısına yazın", ok=True if n else "wait")
        else:
            state, detail = self.tv_link.state, self.tv_link.detail
            tv = TvInfo.from_json(p["tv"] or {})
            if state == "connected":
                self._set_card("out", detail or (tv.name if tv else "TV"), "bağlı · altyazılar TV'de gösteriliyor", ok=True)
            elif tv is None:
                self._set_card("out", "TV seçilmedi", "TV'de Makine TV'yi açıp TV bul'a basın", ok="wait" if konsol else None)
            else:
                meta = TV_STATE_TEXT.get(state, state) if konsol else "konsol modu kapalı"
                self._set_card("out", tv.name, meta, ok="wait" if konsol else None)

    def _copy_text(self, text, message):
        self.clipboard_clear()
        self.clipboard_append(text)
        self.toast(message)

    def _fit_w(self, text, font, width):
        fnt = tkfont.Font(font=font)
        if fnt.measure(text) <= width:
            return text
        while text and fnt.measure(text + "…") > width:
            text = text[:-1]
        return text.rstrip() + "…"

    # Mod

    def _set_mode(self, mode):
        if mode == self.prefs["mode"]:
            return
        self.prefs["mode"] = mode
        self._save_hud_prefs()
        self._snapshot_cfg()
        self._apply_mode()
        self.log("mod    " + ("konsol · Remote Play / yakalama penceresi okunur, çeviri TV'ye gider" if mode == "konsol"
                              else "bilgisayar · algılanan PC oyunu okunur"))

    def _apply_mode(self, initial=False):
        """Mod / altyazı yeri değişince: hedef yeniden aranır, TV bağlantısı ve web yayını açılır / kapanır."""
        self._apply_outputs()
        if not initial:
            self.tracker = LineTracker()
            self.scheduler.clear()
            self._shown_disp = None
            self.overlay.clear()
            self._set_target(None)
            self._apply_detection()
        self._update_region_label()
        self._refresh_console_cards()

    def _apply_outputs(self):
        """TV bağlantısı, telefon / OBS sunucusu, kutunun konumu ve yakalamadaki görünürlüğü prefs'e göre."""
        p = self.prefs
        konsol = p["mode"] == "konsol"
        tv = TvInfo.from_json(p["tv"] or {}) if konsol and p["console_out"] == "tv" else None
        if tv and p["tv_code"]:
            self.tv_link.pair(tv, p["tv_code"])
        else:
            self.tv_link.pair(None)
        phone = konsol and p["console_out"] == "web"
        was = self.web.running
        self.web.configure(phone or p["obs_source"], lan=phone)
        self.web.set_style(p["font_px"], p["opacity"])
        if self.web.running and not was:
            self.log("web    altyazı yayını açık · " + (f"telefon: {self.web.phone_url} · " if phone else "")
                     + f"OBS: {self.web.obs_url}")
        elif not self.web.running and (phone or p["obs_source"]):
            self.log(f"web    altyazı yayını açılamadı: {self.web.error}")
        if not (konsol and p["console_out"] == "pc"):
            self.overlay.set_frame(None)
        self._update_capture_visibility()
        if hasattr(self, "obs_url_txt"):
            self.c.itemconfigure(self.obs_url_txt, text=self.web.obs_url.replace("http://", ""))
            self._show_items("obs_row", p["obs_source"] and self.web.running)

    def _set_console_out(self, out):
        if out == self.prefs["console_out"]:
            return
        self.prefs["console_out"] = out
        self._save_hud_prefs()
        self.overlay.clear()
        self.tv_link.clear()
        self.web.clear()
        self._shown_disp = None
        self._apply_outputs()
        self._refresh_console_cards()
        self.log("çıkış  altyazı " + {"tv": "TV'de (Makine TV)", "pc": "bu bilgisayarın ekranında",
                                      "web": f"telefonda / tablette · {self.web.phone_url}"}[out])
        if out == "web" and self.web.running:
            self.toast("Telefonda tarayıcıya yazın: " + self.web.phone_url.replace("http://", ""))

    def _pc_box_on(self) -> bool:
        """Çeviri kutusu bu bilgisayarda gösterilsin mi? Konsol modunda yalnızca "Bu ekranda" seçiliyse."""
        return self.prefs["mode"] != "konsol" or self.prefs["console_out"] == "pc"

    # Yayın: OBS ekran yakalama, Discord / Teams ekran paylaşımı, OBS tarayıcı kaynağı

    def _set_stream_visible(self, value):
        self.prefs["stream_visible"] = bool(value)
        self._save_hud_prefs()
        self._snapshot_cfg()
        self._wincap = {"key": None, "ok": None, "next": 0.0, "fails": 0}
        self._update_capture_visibility()
        if value:
            self.log("yayın  kutu yayında ve ekran paylaşımında görünür · oyun penceresinin kendi görüntüsü okunur")
            self.toast("Kutu artık OBS'de ve ekran paylaşımında görünür")
        else:
            self.log("yayın  kutu ekran yakalamadan gizli (yalnızca sizin ekranınızda)")

    def _set_obs_source(self, value):
        self.prefs["obs_source"] = bool(value)
        self._save_hud_prefs()
        self._apply_outputs()
        if value and self.web.running:
            self._copy_text(self.web.obs_url, "OBS adresi kopyalandı · OBS'de Kaynak ekle › Tarayıcı")

    def _capture_visible_wanted(self) -> bool:
        """
        Kutu ekran yakalamaya açık olabilir mi? Yalnızca okuma ekranın kopyasından değil pencerenin kendi içeriğinden
        yapılıyorsa: konsol modunda her zaman; PC oyununda "yayında göster" açık ve oyun penceresi okunabiliyorsa.
        """
        if self.prefs["mode"] == "konsol":
            return True
        if not self.prefs["stream_visible"]:
            return False
        return not self.is_running or self._wincap["ok"] is True

    def _update_capture_visibility(self):
        want = self._capture_visible_wanted()
        if self.overlay.win.capture_visible != want:
            self.overlay.set_capture_visible(want)

    def _on_web_clients(self, n):
        prev, self._web_clients = self._web_clients, n
        if n > prev:
            self.log(f"web    bir cihaz bağlandı ({n})")
        self._refresh_console_cards()

    def _detect_target(self):
        if self.prefs["mode"] == "konsol":
            p = self.prefs
            return find_console_source({os.getpid()}, prefer=p["console_source"], custom_exes=p["console_exes"])
        return detect_game({os.getpid()})

    # Konsol kaynağı: listede olmayan bir pencereyi elle seç

    def _pick_console_window(self):
        if self._adding_game:
            return
        if self.prefs["mode"] != "konsol":
            self.seg_mode.set("konsol", animate=True)
            self._set_mode("konsol")
        self._adding_game = True

        def tick(n):
            if n > 0:
                self._set_card("src", f"Konsol penceresine geçin · {n}", "öndeki pencere okunacak", ok="wait")
                self.after(1000, lambda: tick(n - 1))
                return
            self._adding_game = False
            info = foreground_window_info({os.getpid()})
            if info is None:
                self.toast("Öndeki pencere seçilemedi", warn=True)
            else:
                exe = os.path.basename(info[1]).lower()
                exes = self.prefs["console_exes"]
                if exe not in exes:
                    exes.insert(0, exe)
                    del exes[10:]
                self.prefs["console_source"] = "custom"
                self._save_hud_prefs()
                self.log(f"konsol kaynak olarak seçildi: {exe} · \"{info[2]}\"")
                self.toast(f"{os.path.splitext(exe)[0]} konsol görüntüsü olarak okunacak")
            self._set_target(None)
            self._apply_detection()
        tick(3)

    # Açılır pencere

    def _sheet(self, height, title):
        """Panelin üstünde açılan koyu pencere (oyun seçme, TV bulma, kurulum). Esc ya da Kapat kapatır."""
        self._close_sheet()
        top = tk.Toplevel(self)
        top.overrideredirect(True)
        top.configure(bg=BG)
        top.attributes("-topmost", True)
        w = INNER
        top.geometry(f"{w}x{height}+{self.winfo_rootx() + PAD}+{self.winfo_rooty() + CONTENT_Y - 200}")
        cv = tk.Canvas(top, width=w, height=height, bg=BG, highlightthickness=0, bd=0)
        cv.pack()
        cv.create_image(0, 0, anchor="nw", image=ui.rrect_image(w, height, 10, fill=RAISED, outline=LINE_STRONG))
        cv.create_rectangle(1, 16, 4, 44, fill=ACCENT, outline="")
        cv.create_text(20, 30, anchor="w", text=title, font=self.fonts.ui(15, semi=True), fill=TEXT)
        su = ui.UI(top, cv, self.fonts)
        ui.Button(su, w - 78, 14, 64, 30, text="Kapat", kind="ghost", command=self._close_sheet, parent_bg=RAISED,
                  font=self.fonts.ui(12))
        top.bind("<Escape>", lambda e: self._close_sheet())
        top.focus_force()
        self._sheet_top = top
        # Açılış: saydamdan belirir, 8 px aşağıdan yerine oturur (pencerenin kendi alfası; içerik yeniden çizilmez)
        gx, gy = self.winfo_rootx() + PAD, self.winfo_rooty() + CONTENT_Y - 200
        top.attributes("-alpha", 0.0)

        def step(e):
            try:
                top.attributes("-alpha", e)
                top.geometry(f"+{gx}+{gy + round(8 * (1 - e))}")
            except tk.TclError:
                pass
        ui.Tween(self, 180, step)
        return top, cv, su, w

    def _close_sheet(self):
        top, self._sheet_top = self._sheet_top, None
        if top is None:
            return

        def gone():
            try:
                top.destroy()
            except tk.TclError:
                pass

        def step(e):
            try:
                top.attributes("-alpha", 1 - e)
            except tk.TclError:
                pass
        ui.Tween(self, 120, step, done=gone)

    # TV: bul, eşleş, kur

    def _on_tv_state(self, state, detail):
        prev = getattr(self, "_tv_prev_state", "idle")
        self._tv_prev_state = state
        if state == "connected" and prev != "connected":
            self.log(f"tv     bağlandı · {detail}")
            self.toast(f"TV bağlandı · {detail}")
            if self._shown_disp is not None:          # bağlanınca ekrandaki satır TV'de de görünsün
                self._tv_line_id += 1
                self.tv_link.send_line(self._tv_line_id, self._shown_disp.text, self._shown_disp.kind)
        elif state == "bad_code":
            self.log(f"tv     {detail}: eşleşme kodu kabul edilmedi")
            self.toast("TV kodu kabul etmedi · TV bul ile yeniden girin", warn=True)
        elif state == "offline" and prev == "connected":
            self.log(f"tv     {detail}: bağlantı koptu · yeniden deneniyor")
        self._refresh_console_cards()

    def _tv_search(self):
        if self.prefs["mode"] != "konsol":
            self.seg_mode.set("konsol", animate=True)
            self._set_mode("konsol")
        if self.prefs["console_out"] != "tv":
            self.seg_out.set("tv", animate=True)
            self._set_console_out("tv")
        self._set_card("out", "TV aranıyor…", "ev ağında Makine TV aranıyor", ok="wait")

        def worker():
            try:
                found = discover_tvs(1.6)
            except OSError as e:
                self._post(self.log, f"tv     arama başarısız: {e}")
                found = []
            self._post(self._show_tv_list, found)

        threading.Thread(target=worker, daemon=True, name="tv-ara").start()

    def _show_tv_list(self, tvs):
        self._refresh_console_cards()
        f = self.fonts
        if not tvs:
            h = 318
            top, cv, su, w = self._sheet(h, "TV bulunamadı")
            cv.create_text(20, 58, anchor="nw", width=w - 40, font=f.ui(13), fill=TEXT2,
                           text="TV'de Makine TV uygulaması açık mı? TV ile bu bilgisayar aynı ağa (aynı modeme) bağlı olmalı.\n\n"
                                "Bazı modemler ve LG TV'ler otomatik bulmayı engeller: Makine TV ekranında yazan adresle "
                                "bağlanabilirsiniz. Makine TV henüz kurulu değilse \"TV'ye kur\".")
            ui.Button(su, 20, h - 54, 116, 36, text="Tekrar ara", kind="primary", command=lambda: (self._close_sheet(),
                      self._tv_search()), parent_bg=RAISED)
            ui.Button(su, 144, h - 54, 140, 36, text="Adresle bağlan", kind="secondary", command=self._tv_address_sheet,
                      parent_bg=RAISED)
            ui.Button(su, 292, h - 54, 88, 36, text="TV'ye kur", kind="ghost", command=self._tv_install_sheet,
                      parent_bg=RAISED, font=f.ui(12))
            return
        if len(tvs) == 1:
            self._tv_code_sheet(tvs[0])
            return
        row_h = 52
        h = 70 + row_h * len(tvs) + 56
        top, cv, su, w = self._sheet(h, "Hangi TV?")
        for i, tv in enumerate(tvs):
            ui.Button(su, 16, 60 + i * row_h, w - 32, row_h - 8, text=f"{tv.name}   ·   {tv.host}", kind="secondary",
                      align="left", command=lambda t=tv: self._tv_code_sheet(t), parent_bg=RAISED)
        ui.Button(su, 16, h - 48, 150, 32, text="Adresle bağlan", kind="ghost", command=self._tv_address_sheet,
                  parent_bg=RAISED, font=f.ui(12))

    def _tv_address_sheet(self):
        """Otomatik bulma çalışmazsa (yayın paketlerini engelleyen modem / TV): TV ekranındaki adresle bağlan."""
        h = 236
        top, cv, su, w = self._sheet(h, "Adresle bağlan")
        f = self.fonts
        cv.create_text(20, 58, anchor="nw", width=w - 40, font=f.ui(13), fill=TEXT2,
                       text="Makine TV ekranında yazan adresi girin (ör. 192.168.1.40).")
        var = tk.StringVar()
        old = TvInfo.from_json(self.prefs["tv"] or {})
        if old:
            var.set(old.host)
        field = ui.Field(su, 20, 100, w - 40, 40, var, placeholder="192.168.1.40")
        status = cv.create_text(20, 160, anchor="w", text="", font=f.ui(12), fill=ui.DISPUTED)

        def submit(*_):
            host = var.get().strip()
            try:
                socket.inet_aton(host)
            except OSError:
                cv.itemconfigure(status, text="Adres 192.168.1.40 biçiminde olmalı.")
                return
            self._tv_code_sheet(TvInfo(f"ip:{host}", host, host, TV_TCP_PORT))

        ui.Button(su, 20, h - 54, w - 40, 38, text="Devam", kind="primary", command=submit, parent_bg=RAISED)
        field.entry.bind("<Return>", submit)
        field.entry.focus_set()

    def _tv_code_sheet(self, tv):
        top, cv, su, w = self._sheet(236, tv.name)
        f = self.fonts
        cv.create_text(20, 58, anchor="nw", width=w - 40, font=f.ui(13), fill=TEXT2,
                       text="TV ekranındaki 4 haneli eşleşme kodunu girin.")
        var = tk.StringVar()
        field = ui.Field(su, 20, 100, w - 40, 44, var, placeholder="0000")
        field.entry.configure(font=f.code(20), justify="center")
        status = cv.create_text(20, 164, anchor="w", text="", font=f.ui(12), fill=ui.DISPUTED)

        def submit(*_):
            code = "".join(ch for ch in var.get() if ch.isdigit())
            if len(code) != 4:
                cv.itemconfigure(status, text="Kod 4 rakamdan oluşur.")
                return
            self._close_sheet()
            self.prefs["tv"] = tv.to_json()
            self.prefs["tv_code"] = code
            self._save_hud_prefs()
            self.tv_link.pair(tv, code)
            self.log(f"tv     {tv.name} ({tv.host}) ile eşleşiliyor")
            self._refresh_console_cards()

        ui.Button(su, 20, 236 - 54, w - 40, 38, text="Bağlan", kind="primary", command=submit, parent_bg=RAISED)
        field.entry.bind("<Return>", submit)
        field.entry.focus_set()

    INSTALL_STEPS = {
        "android": ("TV'de Ayarlar › Cihaz tercihleri › Hakkında › Yapı satırına 7 kez basın (geliştirici seçenekleri açılır).",
                    "Ayarlar › Cihaz tercihleri › Geliştirici seçenekleri'nde USB hata ayıklama'yı açın. Varsa Ağ üzerinden "
                    "hata ayıklama'yı da açın.",
                    "TV'nin adresi aşağıda kendiliğinden bulunur (bulunmazsa Ayarlar › Ağ'da yazar). Kur'a basın; TV'de "
                    "izin sorusu çıkarsa İzin ver'i seçin."),
        "lg": ("TV'de LG Content Store'dan \"Developer Mode\" uygulamasını kurup ücretsiz LG geliştirici hesabıyla "
               "(developer.lge.com) giriş yapın.",
               "Developer Mode'da Dev Mode Status'u açın (TV yeniden başlar), sonra Key Server'ı açın.",
               "Developer Mode ekranında yazan 6 karakterlik Passphrase'i aşağıya girip Kur'a basın. Makine TV kurulunca "
               "konsolun girişini uygulamadan seçin (1–4)."),
    }

    def _tv_install_sheet(self, kind=None):
        kind = kind if kind in self.INSTALL_STEPS else self.prefs.get("tv_kind", "android")
        lg = kind == "lg"
        sheet_h = 478 if lg else 448
        top, cv, su, w = self._sheet(sheet_h, "Makine TV'yi kur")
        f = self.fonts

        def switch(k):
            self.prefs["tv_kind"] = k
            self._save_hud_prefs()
            self.after(0, lambda: self._tv_install_sheet(k))

        ui.Segmented(su, 20, 56, w - 40, 32, [("android", "Android / Google TV"), ("lg", "LG (webOS)")], kind,
                     command=switch)
        y = 104
        for i, s in enumerate(self.INSTALL_STEPS[kind]):
            cv.create_text(20, y, anchor="nw", text=str(i + 1), font=f.code(13), fill=ACCENT)
            item = cv.create_text(40, y, anchor="nw", width=w - 60, text=s, font=f.ui(12), fill=TEXT2)
            bb = cv.bbox(item)
            y = (bb[3] if bb else y + 34) + 10
        var = tk.StringVar()
        cv.create_text(20, y + 16, anchor="w", text="TV adresi", font=f.ui(12), fill=TEXT3)
        af = ui.Field(su, 120, y + 4, w - 140, 36, var, placeholder="192.168.1.40")
        af.entry.configure(font=f.code(14))
        y += 44
        pw = tk.StringVar()
        if lg:
            cv.create_text(20, y + 14, anchor="w", text="Passphrase", font=f.ui(12), fill=TEXT3)
            pf = ui.Field(su, 120, y + 2, w - 140, 36, pw, placeholder="ör. A1B2C3")
            pf.entry.configure(font=f.code(14))
            y += 44
        status = cv.create_text(20, y + 14, anchor="nw", width=w - 40, text="Ağda TV aranıyor…", font=f.ui(12), fill=TEXT3)
        busy = [False]

        def set_status(text, color=TEXT3):
            try:
                cv.itemconfigure(status, text=text, fill=color)
            except tk.TclError:
                pass

        def found(hosts):
            if not var.get().strip() and hosts:
                var.set(hosts[0])
            if hosts:
                set_status("TV bulundu, " + ("Passphrase'i girip " if lg else "") + "Kur'a basın.")
            else:
                set_status("Ağda " + ("geliştirici modu açık bir LG TV" if lg else "hata ayıklaması açık bir TV")
                           + " bulunamadı. Adresi elle girin.")

        scan = tv_install.find_webos_hosts if lg else tv_install.find_adb_hosts
        threading.Thread(target=lambda: self._post(found, scan()), daemon=True).start()

        def run():
            host = var.get().strip()
            if busy[0] or not host:
                return
            if lg and len(pw.get().strip()) < 4:
                set_status("Developer Mode ekranındaki Passphrase'i girin.", ui.DISPUTED)
                return
            busy[0] = True
            set_status("Başlıyor…", TEXT2)

            def worker():
                try:
                    progress = lambda m: self._post(set_status, m, TEXT2)  # noqa: E731
                    if lg:
                        ver = tv_install.install_webos(host, pw.get(), progress=progress)
                    else:
                        ver = tv_install.install(host, progress=progress)
                    self._post(set_status, f"Kuruldu{f' · sürüm {ver}' if ver else ''}. Şimdi TV bul ile eşleşin.", VERIFIED)
                    self._post(self.log, f"tv     Makine TV kuruldu ({host}){f' · {ver}' if ver else ''}")
                except tv_install.InstallError as e:
                    self._post(set_status, str(e), ui.DISPUTED)
                    self._post(self.log, f"tv     kurulum başarısız ({host}): {e}")
                finally:
                    busy[0] = False

            threading.Thread(target=worker, daemon=True, name="tv-kur").start()

        by = sheet_h - 54
        ui.Button(su, 20, by, 110, 38, text="Kur", kind="primary", command=run, parent_bg=RAISED)
        if not lg:
            ui.Button(su, 138, by, 170, 38, text="APK'yı kaydet", kind="ghost", command=self._save_apk, parent_bg=RAISED)

    def _save_apk(self):
        from tkinter import filedialog
        import shutil
        src = tv_install.apk_path()
        if not src:
            self.toast("Makine TV paketi bulunamadı", warn=True)
            return
        dest = filedialog.asksaveasfilename(parent=self._sheet_top or self, title="Makine TV paketini kaydet",
                                            initialfile="makine-tv.apk", defaultextension=".apk",
                                            filetypes=[("Android paketi", "*.apk")])
        if dest:
            shutil.copyfile(src, dest)
            self.toast("APK kaydedildi · USB bellekle TV'ye kurabilirsiniz")

    # ── Hesap sekmesi ──

    def _set_discord(self, value):
        self.prefs["discord_presence"] = bool(value)
        self._save_hud_prefs()
        if value:
            self.discord.start()
            self._discord_update()
            self.toast("Discord'da gösterilecek" + ("" if self.is_running else " · okumayı başlatınca"))
        else:
            self.discord.stop()
            self.toast("Discord'da gösterilmiyor")

    def _discord_update(self):
        """Okuma açıkken oyun, oturum süresi ve çevrilen diyalog sayısı; değilse temizle (hız sınırı bileşende)."""
        if not self.discord.available or not self.prefs.get("discord_presence"):
            return
        if not (self.is_running and self.profile is not None):
            self.discord.clear()
            return
        konsol = self.prefs["mode"] == "konsol"
        name = self.detected_game_name or (DEFAULT_GAME if konsol else "")
        if not name:
            self.discord.clear()
            return
        self.discord.set(build_activity(name, self._started_at or time.time(), self._session_lines, console=konsol,
                                        steam_appid=self.detected_appid))

    def _build_tab_account(self, tag):
        c, u, f = self.c, self.u, self.fonts
        y = CONTENT_Y
        self.acc_y = y
        c.create_image(PAD, y, anchor="nw", tags=tag, image=ui.rrect_image(INNER, 76, 8, fill=SURFACE, outline=LINE))
        self.acc_user = c.create_text(PAD + 16, y + 25, anchor="w", text="—", font=f.disp(18), fill=TEXT, tags=tag)
        self.acc_chip_bg = c.create_image(PAD + 120, y + 15, anchor="nw", tags=tag)
        self.acc_chip = c.create_text(PAD + 140, y + 25, text="", font=f.ui(11, semi=True), fill=ACCENT, tags=tag)
        self.acc_kind = c.create_text(PAD + 16, y + 54, anchor="w", text="", font=f.ui(12), fill=TEXT3, tags=tag)
        c.create_text(W - PAD - 16, y + 54, anchor="e", text=f"sürüm {APP_VERSION_LABEL}", font=f.code(12), fill=TEXT3,
                      tags=tag)
        y += 96
        c.create_text(PAD, y, anchor="nw", font=f.ui(11), fill=TEXT3, tags=tag, width=INNER,
                      text="Oturum bu bilgisayarda Windows hesabınıza bağlı olarak şifreli saklanır. Çıkış yapınca "
                           "silinir; yönetici oturumu uzaktan da kapatabilir.")
        y += 56
        self.btn_logout = ui.Button(u, PAD, y, INNER, 40, text="Çıkış yap", icon="lock", kind="secondary",
                                    command=self.logout, font=f.ui(13, semi=True), tags=(tag,))
        if self.discord.available:
            y += 62
            c.create_line(PAD, y - 14, W - PAD, y - 14, fill=LINE, tags=tag)
            self.tg_discord = ui.Toggle(u, PAD, y, INNER, "Discord'da göster", self.prefs["discord_presence"],
                                        command=self._set_discord, tags=(tag,), icon="game")
            c.create_text(PAD, y + 30, anchor="nw", font=f.ui(11), fill=TEXT3, tags=tag, width=INNER,
                          text="Okuma açıkken Discord profilinizde hangi oyunu Makine Live ile Türkçe oynadığınız, "
                               "ne zamandır oynadığınız ve bu oturumda kaç diyalog çevrildiği görünür.")

    # ── Sekme geçişi ──

    def _switch_tab(self, key):
        self._show_tab(key, animate=True)
        self.prefs["tab"] = key
        self._save_hud_prefs()

    def _show_tab(self, key, animate=True):
        for k, _ in TABS:
            self.c.itemconfigure(f"tab_{k}", state="normal" if k == key else "hidden")
        self._tab_now = key
        if key != "gorunum":
            for k, _ in LOOK_SECTIONS:
                self.c.itemconfigure(f"look_{k}", state="hidden")
        if key == "konsol":
            self._refresh_console_cards()           # duruma göre gizli düğmeler
        elif key == "gorunum":
            self._show_look_section()
        log_on = key == "canli" and self._log_visible
        self.c.itemconfigure(self.log_win, state="normal" if log_on else "hidden")
        if log_on:
            self.c.itemconfigure(self.history_win, state="hidden")
        if not animate:
            return
        tag = f"tab_{key}"
        moved = [10]
        self.c.move(tag, 0, 10)

        def step(e):
            target = round(10 * (1 - e))
            self.c.move(tag, 0, target - moved[0])
            moved[0] = target

        ui.Tween(self, 200, step)

    # ── Önizleme ──

    def _render_preview(self):
        self._preview_job = None
        _, _, tr = self._sample
        content = OverlayContent(text=tr, kind="verified")
        # Oyundaki gerçek genişlikle çiz, sonra küçült: satır kaydırma oyundakiyle aynı olur
        img = self.overlay.render_preview(content, max_width=self.overlay.style.max_width)
        # gölge için bırakılan boş kenarı kırp, önizleme kutusuna sığdır
        bbox = img.getbbox() or (0, 0, img.width, img.height)
        img = img.crop((max(0, bbox[0] - 6), max(0, bbox[1] - 6),
                        min(img.width, bbox[2] + 6), min(img.height, bbox[3] + 6)))
        max_w, max_h = INNER - 12, self.preview_h - 12
        k = min(1.0, max_w / img.width, max_h / img.height)
        if k < 1.0:
            img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
        self._preview_photo = ImageTk.PhotoImage(img)
        self.c.itemconfigure(self.preview_item, image=self._preview_photo)
        self.c.itemconfigure(self.preview_scale, text=f"ölçek %{round(k * 100)}")

    def _schedule_preview(self):
        if self._preview_job:
            self.after_cancel(self._preview_job)
        self._preview_job = self.after(30, self._render_preview)

    # ── Görünüm tercihleri ──

    def _set_pref(self, key, value, final=True):
        self.prefs[key] = value
        self._apply_look()
        if final:
            self._save_hud_prefs()

    def _apply_look(self):
        """Görünüm tercihleri → oyundaki kutu, panel önizlemesi, telefon / OBS sayfası ve Makine TV."""
        p = self.prefs
        self.overlay.configure_look(style=p["style"], font_px=p["font_px"], opacity=p["opacity"] / 100.0,
                                    position=p["position"], custom_anchor=p.get("custom_anchor"), font=p["font"],
                                    text_rgb=ui.rgb(p["text_color"]), band_rgb=ui.rgb(p["band_color"]),
                                    outline=OUTLINE_PX[p["outline"]], shadow=p["shadow"], max_lines=p["max_lines"],
                                    width_ratio=p["width"] / 100.0, offset=p["offset"] / 100.0)
        self.scheduler.min_base = p["min_show_ms"] / 1000.0
        band = 0 if p["style"] == "yalin" else p["opacity"]
        self.web.set_style(p["font_px"], band, text=p["text_color"], band=p["band_color"],
                           font=FONTS.get(p["font"], FONTS["segoe"])[1], outline=OUTLINE_PX[p["outline"]],
                           shadow=p["shadow"], width=p["width"], pos="ust" if p["position"] == "ust" else "alt",
                           offset=p["offset"])
        self.tv_link.set_style(p["font_px"], "ust" if p["position"] == "ust" else "alt", band)
        self._schedule_preview()

    def _on_overlay_moved(self, anchor):
        self.prefs["custom_anchor"] = list(anchor)
        self.prefs["position"] = "serbest"
        self.seg_pos.set("serbest", animate=True)
        self._save_hud_prefs()

    # ─────────────────────────────────────────────────────────
    # Kayıt, geçmiş, istatistik, bildirim
    # ─────────────────────────────────────────────────────────

    def log(self, text: str):
        _FILE_LOG.info(text)
        self.log_box.configure(state="normal")
        self.log_box.insert("end", f"{time.strftime('%H:%M:%S')}  {text}\n")
        lines = int(self.log_box.index("end-1c").split(".")[0])
        if lines > 500:
            self.log_box.delete("1.0", f"{lines - 500}.0")
        self.log_box.see("end")
        self.log_box.configure(state="disabled")

    def _toggle_log(self):
        self._log_visible = not self._log_visible
        self.c.itemconfigure(self.log_win, state="normal" if self._log_visible else "hidden")
        self.c.itemconfigure(self.history_win, state="hidden" if self._log_visible else "normal")
        self.btn_log.set_text("Geçmiş" if self._log_visible else "Kayıt")

    def _clear_history(self):
        self.history.clear()
        self._refresh_stats()

    def _add_history(self, raw, translated, source, status, ms):
        if source == "havuz":
            badge = ("onaylı", VERIFIED) if status == "verified" else ("havuz", ACCENT)
            kind = "verified" if status == "verified" else "pending"
        else:
            badge = (source, TEXT2)
            kind = "api"
        self.history.add({"time": time.strftime("%H:%M:%S"), "raw": raw, "translated": translated,
                          "badge": badge, "ms": ms, "kind": kind})
        self._refresh_stats()

    def _copy_row(self, row):
        self.clipboard_clear()
        self.clipboard_append(row["translated"])
        self.toast("Çeviri panoya kopyalandı")

    def _replay_row(self, row):
        self._flash(row["translated"], row["kind"] if row["kind"] in ("verified", "pending") else "api")
        self.toast("Oyunda yeniden gösterildi")

    def _refresh_stats(self):
        s = self.stats
        looked = s["pool"] + s["api"]
        lat = list(s["lat"])
        pool_ms = [m for m, k in lat if k == "pool"]
        api_ms = [m for m, k in lat if k == "api"]

        def avg(xs):
            return round(sum(xs) / len(xs)) if xs else None

        lat_sub = "Ort. gecikme"
        if pool_ms and api_ms:
            lat_sub = f"havuz {avg(pool_ms)} · deepl {avg(api_ms)}"
        chars = s["chars_saved"]
        values = {
            "lines": (str(s["lines"]), "Satır"),
            "hit": (f"%{round(100 * s['pool'] / looked)}" if looked else "—", "Havuz isabeti"),
            "lat": (f"{avg([m for m, _ in lat])} ms" if lat else "—", lat_sub),
            "saved": (str(s["pool"]), (f"{chars / 1000:.1f}k karakter" if chars >= 1000 else f"{chars} karakter")
                      if s["pool"] else "DeepL tasarrufu"),
        }
        for key, (v, sub) in values.items():
            vi, si = self.stat_items[key]
            self.c.itemconfigure(vi, text=v)
            self.c.itemconfigure(si, text=sub)
        self._spark_img = ui.sparkline_image(lat, INNER, 30)      # tuval görseli yalnızca referans tutuldukça yaşar
        self.c.itemconfigure(self.spark, image=self._spark_img)
        n = len(self.history.rows)
        self.c.itemconfigure(self.hist_count, text=str(n) if n else "")

    def toast(self, text, warn=False):
        for it in self._toast_items:
            self.c.delete(it)
        f = self.fonts.ui(12, semi=True)
        tw = tkfont.Font(font=f).measure(text) + 28
        x = (W - tw) / 2
        y0 = H - 96
        bg = self.c.create_image(x, y0 + 8, anchor="nw",
                                 image=ui.rrect_image(round(tw), 32, 6, fill=RAISED,
                                                      outline=ACCENT if warn else LINE_STRONG))
        tx = self.c.create_text(W / 2, y0 + 24 + 8, text=text, font=f, fill=TEXT)
        self._toast_items = [bg, tx]
        moved = [8]

        def step(e):
            target = round(8 * (1 - e))
            for it in (bg, tx):
                self.c.move(it, 0, target - moved[0])
            moved[0] = target

        ui.Tween(self, 180, step)

        def leave():
            # Kayboluş: yazı zemine doğru solar, 6 px aşağı kayar (tuval öğelerinin alfası yok: renk karıştırılır)
            if self._toast_items != [bg, tx]:
                return
            fill0 = RAISED

            def out(e):
                q = round(e * 6) / 6                   # renk adımları önbellekte sınırlı kalsın
                self.c.itemconfigure(tx, fill=mix(TEXT, BG, q))
                self.c.itemconfigure(bg, image=ui.rrect_image(round(tw), 32, 6, fill=mix(fill0, BG, q),
                                                              outline=mix(ACCENT if warn else LINE_STRONG, BG, q)))
                for it in (bg, tx):
                    self.c.move(it, 0, 1 if e < 0.99 else 0)

            ui.Tween(self, 200, out, done=lambda: [self.c.delete(i) for i in (bg, tx)])
        self.after(1800, leave)

    # ─────────────────────────────────────────────────────────
    # Ayarlar
    # ─────────────────────────────────────────────────────────

    def _load_prefs(self):
        try:
            with open(self.hud_prefs_path, encoding="utf-8") as fh:
                prefs = json.load(fh)
        except (OSError, ValueError):
            return
        p = self.prefs
        if prefs.get("style") in STYLES:
            p["style"] = prefs["style"]
        try:
            p["font_px"] = min(48, max(16, int(prefs.get("font_px", 26))))
            p["opacity"] = min(100, max(40, int(prefs.get("opacity", 78))))
            p["min_show_ms"] = min(3000, max(600, int(prefs.get("min_show_ms", 1200))))
            p["width"] = min(90, max(40, int(prefs.get("width", 62))))
            p["offset"] = min(30, max(2, int(prefs.get("offset", 8))))
            if int(prefs.get("max_lines", 4)) in (2, 3, 4):
                p["max_lines"] = int(prefs.get("max_lines", 4))
        except (TypeError, ValueError):
            pass
        if prefs.get("font") in FONTS:
            p["font"] = prefs["font"]
        if prefs.get("text_color") in dict(TEXT_COLORS):
            p["text_color"] = prefs["text_color"]
        if prefs.get("band_color") in dict(BAND_COLORS):
            p["band_color"] = prefs["band_color"]
        if prefs.get("outline") in OUTLINE_PX:
            p["outline"] = prefs["outline"]
        p["shadow"] = bool(prefs.get("shadow", True))
        if prefs.get("look_sec") in dict(LOOK_SECTIONS):
            p["look_sec"] = prefs["look_sec"]
        if prefs.get("position") in ("alt", "ust", "serbest"):
            p["position"] = prefs["position"]
        ca = prefs.get("custom_anchor")
        if isinstance(ca, list) and len(ca) == 2:
            p["custom_anchor"] = ca
        if prefs.get("tab") in dict(TABS):
            p["tab"] = prefs["tab"]
        # Konsol / TV
        if prefs.get("mode") in ("pc", "konsol"):
            p["mode"] = prefs["mode"]
        if valid_region(prefs.get("console_region")):
            p["console_region"] = [float(v) for v in prefs["console_region"]]
        if isinstance(prefs.get("console_source"), str):
            p["console_source"] = prefs["console_source"]
        if isinstance(prefs.get("console_exes"), list):
            p["console_exes"] = [str(e).lower() for e in prefs["console_exes"] if isinstance(e, str)][:10]
        if TvInfo.from_json(prefs.get("tv") or {}) is not None:
            p["tv"] = prefs["tv"]
            p["tv_code"] = str(prefs.get("tv_code") or "")[:8]
        if prefs.get("console_out") in ("tv", "pc", "web"):
            p["console_out"] = prefs["console_out"]
        elif prefs.get("pc_box") and not p["tv"]:         # 0.2.0 öncesi: "bu bilgisayarda da göster"
            p["console_out"] = "pc"
        p["stream_visible"] = bool(prefs.get("stream_visible", False))
        p["discord_presence"] = bool(prefs.get("discord_presence", True))
        p["obs_source"] = bool(prefs.get("obs_source", False))
        if prefs.get("tv_kind") in ("android", "lg"):
            p["tv_kind"] = prefs["tv_kind"]

    def _load_settings(self):
        self._apply_look()

    def _save_settings(self):
        # Çeviri Python tarafında /api/live/translate ile yapılır; DLL'den yalnızca yakalama + OCR kullanılır.
        # (translatorEngine / apiKey DLL'e yazılmaz: DLL'in varsayılan motoru zaten sunucu motoru.)
        cfg = self._cfg_copy()
        self.plugin.set_setting("ocrEnabled", "true")
        self.plugin.set_setting("sourceLang", cfg["src"])
        self.plugin.set_setting("targetLang", cfg["tgt"])
        # DXGI first (works for DirectX / fullscreen games where GDI returns black); the DLL falls back to GDI itself
        self.plugin.set_setting("captureMethod", "auto")
        self._save_hud_prefs()

    def _save_hud_prefs(self):
        """Atomik: yazarken kapanan uygulama hud.json'ı bozmaz (geçici dosya + os.replace)."""
        try:
            os.makedirs(self.data_path, exist_ok=True)
            tmp = self.hud_prefs_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(self.prefs, fh)
            os.replace(tmp, self.hud_prefs_path)
        except OSError:
            pass

    # ─────────────────────────────────────────────────────────
    # Durum
    # ─────────────────────────────────────────────────────────

    def _set_state(self, running):
        if running:
            self.btn_main.set_kind("running")
            self.btn_main.set_text("Durdur")
            self.btn_main.icon.animate(1.0, 240)
            self._started_at = time.time()
            self.status_icon.set(name="link", t=0.0, color=VERIFIED)
        else:
            self.btn_main.set_kind("primary")
            self.btn_main.set_text("Başlat")
            self.btn_main.icon.animate(0.0, 240)
            self._started_at = None
            self.status_icon.stop()
            self.status_icon.set(name="link", t=0.0, color=TEXT3)
        self._tick_clock(reschedule=False)

    def _tick_clock(self, reschedule=True):
        if self._started_at and self._block_reason:
            self.c.itemconfigure(self.status_txt, text=f"Bekliyor  ·  {self._block_reason}", fill=ACCENT)
        elif self._notice:
            self.c.itemconfigure(self.status_txt, text=self._notice[0], fill=ACCENT)
        elif self._started_at:
            el = int(time.time() - self._started_at)
            text = f"Çalışıyor  ·  {el // 3600:02d}:{el % 3600 // 60:02d}:{el % 60:02d}  ·  {self.stats['lines']} satır"
            self.c.itemconfigure(self.status_txt, text=text, fill=TEXT2)
        else:
            self.c.itemconfigure(self.status_txt, text="Durduruldu  ·  F9 ile başlatın", fill=TEXT3)
        if reschedule:
            self.after(1000, self._tick_clock)

    def _on_flight(self, active):
        """Çeviri işi sürüyor: durum ikonu döner (aynı anda birden fazla iş olabilir)."""
        self._inflight_n = max(0, getattr(self, "_inflight_n", 0) + (1 if active else -1))
        if active:
            if self._inflight_n == 1:
                self.status_icon.set(name="spinner", color=ACCENT)
                self.status_icon.loop(800)
        elif self._inflight_n == 0 and self.status_icon.name == "spinner":
            self.status_icon.stop()
            self.status_icon.set(name="link", t=0.0, color=VERIFIED if self.is_running else TEXT3)

    def _flash_check(self):
        """Havuzdan onaylı çeviri geldi: onay işareti çizilerek gelir."""
        ic = self.status_icon
        ic.stop()
        ic.set(name="check", t=0.0, color=VERIFIED)

        def back():
            if ic.name == "check":
                ic.set(name="link", t=0.0, color=VERIFIED if self.is_running else TEXT3)

        ic.animate(1.0, 320, done=lambda: self.after(900, back))

    def _update_region_label(self):
        if self.prefs["mode"] == "konsol":
            fx, fy, fw, fh = self.prefs["console_region"]
            self.c.itemconfigure(self.region_txt, text=f"%{fx * 100:.0f}, %{fy * 100:.0f}   ·   %{fw * 100:.0f} × %{fh * 100:.0f}")
            self.c.itemconfigure(self.region_hint, text="konsol görüntüsüne göre   ·   x, y   ·   genişlik × yükseklik")
            return
        x, y, w, h = self.region
        self.c.itemconfigure(self.region_txt, text=f"{x}, {y}   ·   {w} × {h}")
        self.c.itemconfigure(self.region_hint, text="x, y   ·   genişlik × yükseklik")

    def _check_server_async(self):
        def worker():
            ok = self.server.check_connection()
            self._post(self._on_server_status, ok)

        threading.Thread(target=worker, daemon=True).start()

    def _on_server_status(self, ok):
        # Bağlantı başlıkta gösterilmez; yalnızca sorun varsa durum satırında uyarı çıkar
        if ok:
            self._clear_notice("network")
        else:
            self.log("hata   sunucuya ulaşılamadı")
            self._set_notice("Sunucuya ulaşılamıyor · bağlantınızı kontrol edin", "network")

    def refresh_active_game(self, animate=True):
        """Yenile düğmesi: kütüphaneyi yeniden tara (yeni kurulan oyunlar) ve oyunu yeniden algıla."""
        if animate:
            self.btn_refresh.icon.animate(1.0, 520, frm=0.0, done=lambda: self.btn_refresh.icon.set(t=0.0))
            self.game_icon.play(420)
            self._scan_library()
            if self.target is not None:
                self.local_pool.declined.discard(self.target.slug)
                self._pool_on_game(self.target)
        else:
            self._apply_detection()

    def _scan_library(self, first=False):
        def job():
            t0 = time.monotonic()
            self.library.refresh()
            ms = (time.monotonic() - t0) * 1000
            counts = "  ".join(f"{STORE_LABELS.get(k, k)} {v}" for k, v in self.library.summary().items() if v)
            self._post(self.log, f"oyunlar {counts or 'bulunamadı'}  ·  {ms:.0f} ms")
            self._post(self._apply_detection)
            if first:
                self._post(self._detect_tick)
        threading.Thread(target=job, daemon=True, name="oyun-tarama").start()

    def _apply_detection(self):
        if self._adding_game or (not self.library.loaded and self.prefs["mode"] != "konsol"):
            return
        target = self._detect_target()
        first = not hasattr(self, "_shown_target")
        if first or not _same_target(target, self.target) or (target and target.name != self.detected_game_name):
            self._set_target(target)

    def _detect_tick(self):
        """Durdurulmuşken de kart güncel kalsın: oyun açılınca / kapanınca birkaç saniye içinde görünür."""
        if not self.is_running:
            self._apply_detection()
        self.after(3000, self._detect_tick)

    def _game_action(self):
        if self._adding_game:
            return
        if self.prefs["mode"] == "konsol":            # konsolda oyun ve kaynak Konsol sekmesinden seçilir
            self.tabs.select("konsol", user=True)
            return
        t = self.target
        if t is not None:
            self.library.mark_not_game(t.path)
            if self._pool_slug == t.slug:
                self._pool_slug = ""
            self.local_pool.forget(t.slug)
            self.log(f"oyun değil  {t.exe} · bir daha algılanmayacak ({t.path})")
            self.toast(f"{t.name} artık oyun sayılmayacak")
            self._set_target(None)
            self._apply_detection()
            return
        # Öndeki pencereyi oyun olarak ekle: kullanıcıya oyuna geçmesi için süre ver
        self._adding_game = True
        self.btn_game.set_enabled(False)

        def tick(n):
            if n > 0:
                self.c.itemconfigure(self.game_name, text=f"Oyuna geçin · {n}")
                self.c.itemconfigure(self.game_meta, text="öndeki pencere oyun olarak eklenecek")
                self.after(1000, lambda: tick(n - 1))
                return
            self._adding_game = False
            self.btn_game.set_enabled(True)
            info = foreground_window_info({os.getpid()})
            if info is None:
                self.toast("Öndeki pencere bir oyun penceresi değil", warn=True)
                self._set_target(None)
                self._apply_detection()
                return
            _pid, path, _title = info
            entry = self.library.add_custom(path)
            self.log(f"oyun eklendi  {entry.name} · {path}")
            self.toast(f"{entry.name} oyun olarak eklendi")
            self._set_target(None)
            self._apply_detection()
        tick(3)

    def toggle_running(self):
        if not self.is_running and self.profile is None:
            return   # giriş yapılmadan pipeline çalışmaz (F9 giriş ekranında yok sayılır)
        if not self.is_running:
            self._snapshot_cfg()
            self._save_settings()
            self.refresh_active_game(animate=False)
            self.tracker = LineTracker()
            self.scheduler.clear()
            self._shown_disp = None
            self._block_reason = None
            self.is_running = True
            self._session_lines = 0
            self._set_state(True)
            if not self.is_locked and self._pc_box_on():
                self.overlay.show()
            self.log("başladı" + (f" · konsol: {DEFAULT_GAME}" if self.prefs["mode"] == "konsol" else ""))
            self._wincap = {"key": None, "ok": None, "next": 0.0, "fails": 0}
            self._update_capture_visibility()
            self._pool_sync_async()
            # TLS el sıkışmasını önceden yap: ilk satır bağlantı kurulmasını beklemesin
            self._executor.submit(self.server.warm)
            self.worker_thread = threading.Thread(target=self._pipeline_loop, daemon=True)
            self.worker_thread.start()
        else:
            self.is_running = False
            self._set_state(False)
            self.scheduler.clear()
            self._shown_disp = None
            self.overlay.clear()
            self.tv_link.clear()
            self.web.clear()
            self._update_capture_visibility()
            self.log(f"durdu  ·  {self._stage_summary()}")
            self._pool_sync_async(hits_only=True)
        self._discord_update()

    def toggle_lock(self):
        self.is_locked = not self.is_locked
        self.overlay.set_click_through(self.is_locked)
        if self.is_locked:
            self.btn_lock.set_text("Kilidi aç")
            self.btn_lock.icon.animate(0.0, 260)
            self.log("kutu kilitlendi, tıklamalar oyuna geçiyor")
            self.toast("Kutu kilitlendi · tıklamalar oyuna geçer")
        else:
            self.btn_lock.set_text("Kilitle")
            self.btn_lock.icon.animate(1.0, 260)
            self.overlay.show()
            self.log("kutu taşınabilir")
            self.toast("Kutuyu oyunda sürükleyebilirsiniz")

    def flash_sample(self):
        self.btn_sample.icon.animate(1.0, 320, frm=0.0)
        # Her basışta farklı bir oyundan başka bir replik
        others = [s for s in SAMPLES if s is not self._sample] or SAMPLES
        self._sample = random.choice(others)
        self._schedule_preview()
        self._flash(self._sample[2], "verified")
        self.log(f"örnek  {self._sample[0]}: \"{self._sample[1]}\"")

    def start_region_selection(self):
        self.btn_region.icon.play(400)
        t = self.target if self.prefs["mode"] == "konsol" and isinstance(self.target, ConsoleTarget) else None
        if self.prefs["mode"] == "konsol" and t is None:
            self.toast("Önce konsol görüntüsünü (Remote Play) açın", warn=True)
            return

        def on_selected(x, y, w, h):
            if t is not None:
                # Konsolda bölge Remote Play / yakalama penceresinin video alanına göre orandır (pencere taşınsa da kalır)
                frac = screen_to_region(t.video(), (x, y, w, h))
                if not valid_region(frac):
                    self.toast(f"Bölge {t.source.label} görüntüsünün içinde olmalı", warn=True)
                    return
                self.prefs["console_region"] = list(frac)
                self._save_hud_prefs()
                self._snapshot_cfg()
                self._update_region_label()
                self.log(f"bölge  konsol görüntüsünün %{frac[0] * 100:.0f}, %{frac[1] * 100:.0f} · "
                         f"%{frac[2] * 100:.0f} × %{frac[3] * 100:.0f}")
                self.toast("Altyazı alanı seçildi")
                return
            self.region = (x, y, w, h)
            self._update_region_label()
            self.log(f"bölge  {x}, {y}  {w} × {h}")
            self.toast(f"Bölge seçildi · {w} × {h}")

        current = region_to_screen(t.video(), self.prefs["console_region"]) if t is not None else self.region
        self.after(150, lambda: RegionSelector(self, self.fonts, on_selected, current=current))

    # ─────────────────────────────────────────────────────────
    # Pipeline (arka plan thread'i)
    # ─────────────────────────────────────────────────────────

    def _pipeline_loop(self):
        """
        Yakalama döngüsü (~100 ms). Çeviri burada BEKLENMEZ: iş havuzuna verilir, böylece uzun bir
        DeepL isteği sürerken yeni satırlar ve satırın ekrandan kalkışı kaçırılmaz.
        DLL değişmemiş karede OCR yapmadan önceki metni ~1 ms'de döndürür.
        """
        our_pids = {os.getpid()}
        next_pick = 0.0
        last_raw, last_text = None, ""            # değişmeyen karede DLL aynı metni döndürür: süzgeç bir kez çalışır
        self._panel_hwnd = self._toplevel_hwnd()
        while self.is_running:
            loop_t0 = time.monotonic()
            cfg = self._cfg_copy()
            region = self.region

            # 0. Hedef kilidi: yalnızca algılanan OYUNUN penceresi okunur. Oyun yoksa / kapandıysa saniyede bir aranır.
            #    Konsol modunda hedef Remote Play / yakalama penceresidir; oyuncu TV'ye baktığı için önde olması
            #    gerekmez, ama ekranda açık ve üstü kapalı olmamalıdır. Bölge pencerenin video alanına göre orandır.
            konsol = cfg["mode"] == "konsol"
            cur = self.target
            if konsol and cur is not None and not isinstance(cur, ConsoleTarget):
                cur = None
            if (cur is None or not cur.alive()) and loop_t0 >= next_pick and not self._adding_game:
                next_pick = loop_t0 + 1.0
                target = self._detect_target()
                if not _same_target(target, self.target):
                    self.target = target
                    self._post(self._set_target, target)
            hwnd = None
            if konsol:
                # DLL pencerenin kendi içeriğini okur (PrintWindow): üstünde başka pencere olsa da okunur,
                # yalnızca küçültülmemiş ve yeterince büyük olmalı. Bölge pencerenin istemci koordinatıdır.
                t = self.target if isinstance(self.target, ConsoleTarget) else None
                reason = console_block_reason(t)
                if t is not None and not reason:
                    region = t.client_region(cfg["console_region"])
                    hwnd = t.hwnd
            else:
                reason = capture_block_reason(self.target, region, our_pids, require_foreground=cfg["only_focus"],
                                              own_windows=(self._panel_hwnd,))
                if not reason and cfg["stream_visible"]:
                    local = self._window_region(self.target, region, loop_t0)
                    if local is not None:
                        region, hwnd = local, self.target.hwnd
                if not reason and hwnd is None and self.overlay.win.capture_visible:
                    # Kutu yakalamaya açıkken ekranın kopyası okunmaz (OCR kendi çevirimizi okurdu):
                    # ana iş parçacığı kutuyu gizleyene kadar bu yoklama atlanır.
                    self._post(self._update_capture_visibility)
                    time.sleep(0.1)
                    continue
            if reason:
                self._emit(self.tracker.reset(loop_t0))
                self._post(self._set_block_reason, reason)
                time.sleep(0.2)
                continue
            self._post(self._set_block_reason, None)

            # 1. Ekran yakala + OCR (makine-live.dll)
            rx, ry, rw, rh = region
            t0 = time.monotonic()
            raw = self.plugin.capture_and_ocr(rx, ry, rw, rh, hwnd)
            ocr_ms = (time.monotonic() - t0) * 1000
            # Satır satır süzgeç: gürültü, oyun arayüzü (HUD, araç adı, konuşan adı), tuş simgeleri, sunucunun
            # reddedeceği satırlar (core/garbage.py). Geriye bir şey kalmazsa ekranda satır yok sayılır.
            if raw != last_raw:
                last_raw, last_text = raw, filter_ocr(raw, cfg["src"])
            text = last_text
            now = time.monotonic()
            if ocr_ms > 40:
                self.stage_ms["ocr"].append(ocr_ms)

            # 2. Satır takibi → yeni / kalktı / çevir olayları
            self._emit(self.tracker.update(text, now), cfg)

            time.sleep(max(0.0, self.POLL_S - (time.monotonic() - loop_t0)))

    POLL_S = 0.10
    WINCAP_RECHECK_S = 30.0       # pencereden okuma çalışırken arada bir yeniden yoklanır (ör. tam ekrana geçildi)
    WINCAP_RETRY_S = 5.0          # siyah geldiyse (yükleme ekranı olabilir) yeniden deneme aralığı
    WINCAP_MAX_FAILS = 6

    def _window_region(self, target, region, now):
        """
        "Yayında göster" (PC oyunu): bölge oyun penceresinin istemci koordinatına çevrilir ve DLL pencerenin kendi
        içeriğini okur; böylece yakalamaya açık kutu OCR'a karışmaz. Pencere okunamıyorsa (siyah) None: ekranın
        kopyası okunur ve kutu yakalamadan gizli kalır. Sonuç pipeline iş parçacığında tutulur, ana iş parçacığına
        yalnızca değişince bildirilir.
        """
        st = self._wincap
        key = (target.pid, int(target.hwnd or 0))
        if st["key"] != key:
            st.update(key=key, ok=None, next=0.0, fails=0)
        prev = st["ok"]
        if now >= st["next"]:
            if window_has_image(target.hwnd):
                st.update(ok=True, fails=0, next=now + self.WINCAP_RECHECK_S)
            else:
                st["fails"] += 1
                gave_up = st["fails"] >= self.WINCAP_MAX_FAILS
                st.update(ok=False, next=now + (60.0 if gave_up else self.WINCAP_RETRY_S))
                if st["fails"] == self.WINCAP_MAX_FAILS:
                    self._post(self.log, f"yayın  {target.exe} penceresinden okunamıyor (görüntü siyah) · kutu "
                                         "yayında gizli kalır. Oyunu kenarlıksız / pencereli modda açmayı deneyin.")
        local = None
        if st["ok"]:
            cl, ct, cr, cb = client_rect(target.hwnd)
            x, y, w, h = region
            if cl <= x and ct <= y and x + w <= cr and y + h <= cb:
                local = (x - cl, y - ct, w, h)
            else:                                                       # bölge pencereden taşıyor: ekran kopyası
                st.update(ok=False, next=now + self.WINCAP_RETRY_S)
                if prev is not False:
                    self._post(self.log, "yayın  okunan bölge oyun penceresinin dışına taşıyor · kutu yayında gizli "
                                         "(F10 ile bölgeyi pencerenin içinde seçin)")
        if prev != st["ok"]:
            if st["ok"]:
                self._post(self.log, f"yayın  {target.exe} penceresinden okunuyor · kutu yayında görünür")
            self._post(self._update_capture_visibility)
        return local

    STALE_JOB_S = 2.0      # kaynağı bu kadar önce ekrandan kalkmış satırın çevirisi artık istenmez

    def _emit(self, events, cfg=None):
        """Takip olaylarını işle: zamanlayıcı ana thread'de, çeviri iş havuzunda."""
        for ev in events:
            kind, line_id, arg = ev
            if kind == "new":
                self._post(self._on_line_new, line_id, arg)
                # Satır sabitlenene kadar (~200–600 ms) bağlantıyı hazırla: sessiz bir aradan sonra Cloudflare boşta
                # bağlantıyı kapatmış olur; TLS el sıkışması çeviri isteğini beklemesin. Sunucuya istek gitmez.
                if HTTP.idle_age(self.server.base_url) > HttpPool.IDLE_MAX_S * 0.75:
                    threading.Thread(target=self.server.warm, daemon=True, name="baglanti").start()
            elif kind == "gone":
                self._gone_at[line_id] = arg
                if len(self._gone_at) > 400:
                    for k in list(self._gone_at)[:200]:
                        self._gone_at.pop(k, None)
                self._post(self.scheduler.source_gone, line_id, arg)
            elif kind == "dispatch" and cfg is not None:
                seq = self._dispatch_seq.get(line_id, 0) + 1
                self._dispatch_seq[line_id] = seq
                if len(self._dispatch_seq) > 400:
                    for k in list(self._dispatch_seq)[:200]:
                        self._dispatch_seq.pop(k, None)
                self._executor.submit(self._translate_job, line_id, arg, cfg, self.detected_game_slug,
                                      self.detected_game_name, seq)

    def _on_line_new(self, line_id, t):
        self.scheduler.source_new(line_id, t)
        self._line_t0[line_id] = t
        if len(self._line_t0) > 200:
            for k in list(self._line_t0)[:100]:
                self._line_t0.pop(k, None)
        self._count_line()

    # ── Çeviri işi (iş havuzu thread'i) ──

    def _tr_cache_get(self, key):
        """(çeviri, tür) ya da None. Onaylı satır oturum boyunca, diğerleri TR_CACHE_TTL_S boyunca tutulur."""
        with self._tr_lock:
            v = self._tr_cache.get(key)
            if not v:
                return None
            if v[1] != "verified" and time.monotonic() - v[2] > TR_CACHE_TTL_S:
                del self._tr_cache[key]
                return None
            self._tr_cache.move_to_end(key)
            return v[0], v[1]

    def _tr_cache_put(self, key, translated, kind):
        with self._tr_lock:
            self._tr_cache[key] = (translated, kind, time.monotonic())
            self._tr_cache.move_to_end(key)
            while len(self._tr_cache) > 1000:
                self._tr_cache.popitem(last=False)

    def _translate_job(self, line_id, text, cfg, game_slug, game_name="", seq=0):
        """
        Tek sunucu isteği: /api/live/translate havuza bakar, yoksa sunucunun DeepL anahtarıyla çevirir ve havuza yazar.
        Önce yerel havuz ve oturum önbelleği; aynı satır zaten soruluyorsa o yanıt beklenir (ikinci istek gitmez).
        Satırın daha yeni bir hâli gönderildiyse ya da satır çoktan ekrandan kalktıysa iş atlanır.
        Yalnızca geçici hatalarda (ağ, 502 / 504) bir kez hemen yeniden denenir.
        """
        if seq and self._dispatch_seq.get(line_id, seq) != seq:
            return                                     # büyüyen satırın yeni hâli zaten kuyrukta
        gone = self._gone_at.get(line_id)
        if gone is not None and time.monotonic() - gone > self.STALE_JOB_S:
            self._post(self.scheduler.translation_failed, line_id)
            return
        key = compact_pool_key(text)
        self._post(self._on_flight, True)
        owner = None
        try:
            # 0. Yerel havuz: bu oyunun onaylı çevirileri bilgisayarda (ağa çıkılmaz)
            t0 = time.monotonic()
            local = self.local_pool.lookup(game_slug, text) if game_slug == self._pool_slug else None
            if local:
                ms = (time.monotonic() - t0) * 1000
                self._post(self._on_translation, line_id, text, local[1], "verified", "yerel", ms,
                           {"exact": local[2]})
                return
            if key in self._skip_keys:
                self._post(self.scheduler.translation_failed, line_id)
                return
            # 1. Bu oturumda geldi: onaylıysa her zaman, değilse bir süre (oy sunucuda zaten kayıtlı; satır bu arada
            #    onaylanırsa yerel havuz eşitlemesi getirir). Aynı satır şu an soruluyorsa o yanıt beklenir.
            for _ in (1, 2):
                hit = self._tr_cache_get(key)
                if hit:
                    self._post(self._on_translation, line_id, text, hit[0], hit[1], "önbellek", 0, {})
                    return
                with self._tr_lock:
                    pending = self._inflight.get(key)
                    if pending is None:
                        owner = self._inflight[key] = threading.Event()
                        break
                pending.wait(timeout=12.0)
            if owner is None:
                self._post(self.scheduler.translation_failed, line_id)
                return

            res = None
            for attempt in (1, 2):
                t0 = time.monotonic()
                retry = False
                try:
                    res = self.server.translate(game_slug, game_name, text, cfg["src"], cfg["tgt"],
                                                steam_appid=self.detected_appid if game_slug == self.detected_game_slug else "")
                    break
                except AuthRequired:
                    self._post(self._on_auth_lost, "Oturumunuz kapatıldı. Lütfen yeniden giriş yapın.")
                    break
                except NetworkError as e:
                    self._post(self.log, f"hata   sunucuya ulaşılamadı ({e}) · deneme {attempt}")
                    self._post(self._set_notice, "Sunucuya ulaşılamıyor · yeniden deneniyor", "network")
                    retry = True
                except ServerError as e:
                    if e.status == 422:
                        self._skip_keys.add(key)
                        self._post(self.log, f'atla   sunucu çöp saydı ({e.data.get("reason", "")}): "{_short(text)}"')
                        break
                    if e.status == 429:
                        self._post(self._set_notice, "Günlük çeviri sınırı doldu — havuzdaki çeviriler gelmeye devam ediyor",
                                   "quota")
                        break
                    if e.code == "maintenance":
                        self._post(self._refresh_status)
                        break
                    if e.code == "banned":
                        self._post(self._on_auth_lost, str(e))
                        break
                    if e.status == 503:
                        self._post(self._set_notice, "Sunucuda çeviri anahtarı yok · yalnızca havuzdaki satırlar gelir",
                                   "no_key")
                        break
                    self._post(self.log, f"hata   sunucu {e.status}: {e} · deneme {attempt}")
                    retry = e.status in (502, 504)
                    if retry:
                        self._post(self._set_notice, "Çeviri servisi yanıt vermedi · yeniden deneniyor", "upstream")
                if not retry:
                    break
                if attempt == 1:
                    # Satır bu arada ekrandan kalktıysa yeniden denemeye değmez
                    gone = self._gone_at.get(line_id)
                    if gone is not None and time.monotonic() - gone > self.STALE_JOB_S:
                        break
                    time.sleep(0.3)
            if not res or not res.get("translated"):
                self._post(self.scheduler.translation_failed, line_id)
                return

            req_ms = (time.monotonic() - t0) * 1000
            origin = res.get("origin", "deepl")
            status = res.get("status", "pending")
            if origin == "pool":
                kind = "verified" if status == "verified" else "pending"
                self.stage_ms["pool"].append(req_ms)
            else:
                kind = "api"
                self.stage_ms["api"].append(req_ms)
            self._tr_cache_put(key, res["translated"], kind)
            self._post(self._on_translation, line_id, text, res["translated"], kind, origin, req_ms, res)
        finally:
            if owner is not None:
                with self._tr_lock:
                    self._inflight.pop(key, None)
                owner.set()
            self._post(self._on_flight, False)

    # ── Sonuçlar (ana thread) ──

    def _on_translation(self, line_id, raw, translated, kind, source, stage_ms, res):
        now = time.monotonic()
        self._session_lines += 1
        self._discord_update()
        self.scheduler.translation_ready(line_id, translated, kind, now)
        t0 = self._line_t0.get(line_id)
        total_ms = round((now - t0) * 1000) if t0 else round(stage_ms)
        server_ms = res.get("server_ms") if res else None
        srv = f"sunucu {server_ms} ms · " if server_ms is not None else ""
        if source == "yerel":
            self.stats["pool"] += 1
            self.stats["verified"] += 1
            self.stats["chars_saved"] += len(raw)
            self.stats["lat"].append((total_ms, "pool"))
            self._flash_check()
            self._add_history(raw, translated, "havuz", "verified", total_ms)
            self.log(f"yerel  onaylı{'' if res.get('exact') else ' (benzer satır)'}  arama {stage_ms:.0f} ms · "
                     f"algılamadan {total_ms} ms  \"{_short(raw)}\"")
            res = None
        elif source == "pool":
            self.stats["pool"] += 1
            self.stats["chars_saved"] += len(raw)
            self.stats["lat"].append((total_ms, "pool"))
            if kind == "verified":
                self.stats["verified"] += 1
                self._flash_check()
            self._add_history(raw, translated, "havuz", "verified" if kind == "verified" else "pending", total_ms)
            self.log(f"havuz  {'onaylı' if kind == 'verified' else 'bekliyor'}  {srv}istek {round(stage_ms)} ms · "
                     f"algılamadan {total_ms} ms  \"{_short(raw)}\"")
            self._clear_notice("network", "upstream")
        elif source == "önbellek":
            self._add_history(raw, translated, "önbellek", "api", total_ms)
            self.log(f"önbellek  algılamadan {total_ms} ms  \"{_short(raw)}\"")
        elif source == "own":
            # Satır henüz onaylanmadı; bu hesabın daha önce verdiği çeviri (DeepL çağrısı ve yeni oy yok)
            votes = f"oy {res.get('votes', 0)}/{res.get('votes_needed', 3)} · " if res else ""
            self._add_history(raw, translated, "önceki", "api", total_ms)
            self.log(f"önceki  {votes}{srv}istek {round(stage_ms)} ms  \"{_short(raw)}\"")
            self._clear_notice("network", "upstream")
        elif source == "shared":
            # Satırı başka bir oyuncu daha önce okudu: sunucu aynı makine çevirisini verdi (DeepL çağrısı yok,
            # günlük sınırdan düşmez), bu hesabın oyu sayıldı
            votes = f"oy {res.get('votes', 0)}/{res.get('votes_needed', 3)} · " if res else ""
            self._add_history(raw, translated, "ortak", "api", total_ms)
            self.log(f"ortak  {votes}{srv}istek {round(stage_ms)} ms · algılamadan {total_ms} ms  \"{_short(raw)}\"")
            self._clear_notice("network", "upstream", "quota")
        else:
            self.stats["api"] += 1
            self.stats["lat"].append((total_ms, "api"))
            deepl = f"deepl {res.get('deepl_ms')} ms · " if res and res.get("deepl_ms") is not None else ""
            votes = f"oy {res.get('votes', 0)}/{res.get('votes_needed', 3)} · " if res and "votes" in res else ""
            self._add_history(raw, translated, "deepl", "api", total_ms)
            self.log(f"deepl  yeni  {votes}{deepl}{srv}istek {round(stage_ms)} ms · algılamadan {total_ms} ms  \"{_short(raw)}\"")
            self._clear_notice("network", "upstream", "no_key", "quota")
        self._refresh_stats()

    def _tick_scheduler(self):
        """Kutunun ne göstereceğine zamanlayıcı karar verir (50 ms)."""
        try:
            d = self.scheduler.tick(time.monotonic())
            if d != self._shown_disp:
                konsol = self.prefs["mode"] == "konsol"
                out = self.prefs["console_out"]
                if d is not None:
                    self._tv_line_id += 1
                    if self._pc_box_on():
                        if konsol and isinstance(self.target, ConsoleTarget) and self.target.alive():
                            self.overlay.set_frame(self.target.video())   # konsol görüntüsünün üstünde
                        self.overlay.present_line(d.text, d.kind)
                    if konsol and out == "tv":
                        self.tv_link.send_line(self._tv_line_id, d.text, d.kind)
                    if self.web.running:                                   # telefon / OBS tarayıcı kaynağı
                        self.web.send_line(self._tv_line_id, d.text, d.kind)
                else:
                    self.overlay.end_line()
                    if konsol and out == "tv":
                        self.tv_link.clear()
                    self.web.clear()
                self._shown_disp = d
        finally:
            self.after(50, self._tick_scheduler)

    def _flash(self, text, kind):
        """Örnek / geçmişten yeniden göster: kaynaksız satır, okuma süresi kadar."""
        self._flash_id += 1
        self.scheduler.flash(self._flash_id, text, kind, time.monotonic())

    # ── Hedef pencere ──

    def _set_target(self, target):
        prev = getattr(self, "_shown_target", None)
        self.target = target
        self._shown_target = target
        if target:
            self.detected_game_name, self.detected_game_slug = target.name, target.slug
            self.detected_exe = target.exe
            self.c.itemconfigure(self.game_name, text=self._fit(target.name, self.fonts.ui(14, semi=True)))
            meta = f"{self._store_label(target)}  ·  {target.exe}"
            self.c.itemconfigure(self.game_meta, text=self._fit(meta, self.fonts.code(11)))
            self.game_icon.set(color=ACCENT)
            self.btn_game.set_text("Konsol" if isinstance(target, ConsoleTarget) else "Oyun değil")
            if not _same_target(target, prev):
                self.log(f"oyun   {target.name} ({target.slug}) · {self._store_label(target)} · "
                         f"{target.exe} pid {target.pid} · yalnızca bu pencere okunur")
            self._refresh_game_meta()
            if prev is None or prev.slug != target.slug:
                self._resolve_appid(target)
                self._pool_on_game(target)
        else:
            konsol = self.prefs["mode"] == "konsol"
            self.detected_game_name = self.detected_game_slug = self.detected_exe = self.detected_appid = ""
            self.c.itemconfigure(self.game_name, text="Konsol görüntüsü bulunamadı" if konsol else "Oyun algılanmadı")
            self.c.itemconfigure(self.game_meta, text=self._fit("Remote Play ya da yakalama penceresini açın" if konsol
                                                                else "oyunu açın ya da ekleyin", self.fonts.code(11)))
            self.game_icon.set(color=TEXT2)
            self.btn_game.set_text("Konsol" if konsol else "Oyun ekle")
            self._hide_pool_prompt()
            if prev is not None:
                self.log("oyun   " + ("konsol görüntüsü yok" if konsol else "algılanan oyun yok") + " · okuma bekliyor")
        self._refresh_console_cards()
        self._discord_update()

    def _store_label(self, t) -> str:
        if isinstance(t, ConsoleTarget):
            return f"{CONSOLE_LABELS.get(t.source.console, 'Konsol')} · {t.source.label}"
        return STORE_LABELS.get(t.store, t.store)

    def _resolve_appid(self, target):
        """Steam numarası: Steam oyunlarında hazır, diğerlerinde Steam mağazasında ada göre (bir kez, önbellekli)."""
        self.detected_appid = target.game.steam_appid or ""
        if self.detected_appid or isinstance(target, ConsoleTarget):   # konsol oyununda Steam araması yapılmaz
            return

        def worker():
            appid = self.library.steam_appid_for(target.game)
            if appid:
                self._post(self._set_appid, target.slug, appid)

        threading.Thread(target=worker, daemon=True, name="steam-numara").start()

    def _set_appid(self, slug, appid):
        if self.target is not None and self.target.slug == slug:
            self.detected_appid = appid

    def _refresh_game_meta(self):
        t = self.target
        if t is None:
            return
        store = self._store_label(t)
        p = self.local_pool.get(t.slug) if self._pool_slug == t.slug else None
        meta = f"{store}  ·  {_fmt_int(len(p))} hazır çeviri" if p is not None else f"{store}  ·  {t.exe}"
        self.c.itemconfigure(self.game_meta, text=self._fit(meta, self.fonts.code(11)))

    # ── Yerel havuz: "hazır çeviriler bulundu, oynayacağınız oyun bu mu?" ──

    def _pool_on_game(self, target):
        """Oyun algılandı: önceden onaylandıysa sormadan eşitle; değilse sunucuya kaç onaylı satır olduğunu sor."""
        if target is None or self.profile is None:
            return
        slug, name = target.slug, target.name
        if self.local_pool.is_confirmed(slug):
            if self._pool_slug != slug:
                self._pool_slug = slug
                p = self.local_pool.get(slug)
                if p is not None:
                    self.log(f"yerel  {name}: {_fmt_int(len(p))} onaylı çeviri yüklendi")
                self._refresh_game_meta()
            self._pool_sync_async()
            return
        self._pool_slug = ""
        if slug in self.local_pool.declined or self._pool_prompt_slug == slug:
            return

        def worker():
            try:
                head = self.server.pool_sync(slug, head=True, timeout=6.0, steam_appid=target.game.steam_appid)
            except AuthRequired:
                self._post(self._on_auth_lost, "Oturumunuz kapatıldı. Lütfen yeniden giriş yapın.")
                return
            except (NetworkError, ServerError) as e:
                first = self._pool_retry_slug != slug
                self._pool_retry_slug = slug
                if first:
                    old = isinstance(e, ServerError) and e.status == 400
                    self._post(self.log, f"yerel  {name}: hazır çeviriler sorulamadı ("
                               + ("sunucu güncel değil, yeni sunucu paketi yüklenmeli" if old else str(e))
                               + f") · {POOL_SYNC_S} sn'de bir yeniden denenecek")
                return
            self._pool_retry_slug = ""
            count = int(head.get("count", 0) or 0)
            if count > 0:
                self._post(self._show_pool_prompt, slug, name, count)
            else:
                self._post(self.log, f"yerel  {name}: henüz onaylı çeviri yok · satırlar sunucudan gelir")

        threading.Thread(target=worker, daemon=True, name="havuz-sor").start()

    def _show_pool_prompt(self, slug, name, count):
        t = self.target
        if t is None or t.slug != slug or self.local_pool.is_confirmed(slug) or slug in self.local_pool.declined:
            return
        self._hide_pool_prompt()
        self._pool_prompt_slug = slug
        c, u, f = self.c, self.u, self.fonts
        tag = "pool_prompt"
        y = 212
        hgt = 146
        c.create_image(PAD, y, anchor="nw", tags=(tag,),
                       image=ui.rrect_image(INNER, hgt, 8, fill=RAISED, outline=mix(LINE_STRONG, ACCENT, 0.35)))
        icon = ui.AnimatedIcon(u, PAD + 28, y + 28, "subtitle", 20, ACCENT, tags=(tag,))
        icon.play(420)
        c.create_text(PAD + 48, y + 28, anchor="w", text="Hazır çeviriler bulundu", font=f.ui(14, semi=True),
                      fill=TEXT, tags=(tag,))
        c.create_text(PAD + 18, y + 50, anchor="nw", width=INNER - 36, font=f.ui(13), fill=TEXT2, tags=(tag,),
                      text=f"{name} için {_fmt_int(count)} onaylı çeviri var. Oynayacağınız oyun bu mu?")
        by = y + hgt - 44
        ui.Button(u, PAD + 18, by, 132, 32, text="Evet, indir", kind="primary", font=f.ui(12, semi=True),
                  command=lambda: self._pool_accept(slug, name), tags=(tag,), parent_bg=RAISED)
        ui.Button(u, PAD + 18 + 140, by, 84, 32, text="Hayır", kind="ghost", font=f.ui(12),
                  command=lambda: self._pool_decline(slug, name), tags=(tag,), parent_bg=RAISED)
        self.log(f"yerel  {name}: {_fmt_int(count)} onaylı çeviri bulundu · onay bekleniyor")

    def _hide_pool_prompt(self):
        if self._pool_prompt_slug:
            self.c.delete("pool_prompt")
            self._pool_prompt_slug = ""

    def _pool_accept(self, slug, name):
        self._hide_pool_prompt()
        self.local_pool.confirm(slug, name)
        self._pool_slug = slug
        self.toast(f"{name} çevirileri indiriliyor")
        self._pool_sync_async()

    def _pool_decline(self, slug, name):
        self._hide_pool_prompt()
        self.local_pool.declined.add(slug)
        self.log(f"yerel  {name}: indirilmedi · satırlar sunucudan gelir")
        self.toast("Yanlış oyun algılandıysa kartta Oyun değil'e basın")

    def _pool_tick(self):
        """
        Oyun açıkken yerel havuzu sunucuyla karşılaştır (onay / düzenleme / silme): okuma açıkken POOL_SYNC_S'de,
        kapalıyken POOL_IDLE_SYNC_S'de bir. Aynı sürümdeyse sunucu tek satır okur ve "değişmedi" der.
        """
        try:
            t = self.target
            due = POOL_SYNC_S if self.is_running else POOL_IDLE_SYNC_S
            if self._pool_slug and t is not None and t.slug == self._pool_slug:
                if time.monotonic() - self._pool_synced_at >= due:
                    self._pool_sync_async()
            elif self._pool_retry_slug and t is not None and t.slug == self._pool_retry_slug:
                self._pool_on_game(t)
        finally:
            self.after(POOL_TICK_S * 1000, self._pool_tick)

    def _pool_sync_async(self, hits_only=False):
        slug = self._pool_slug
        if not slug or self._pool_busy or self.profile is None:
            return
        self._pool_busy = True
        if not hits_only:
            self._pool_synced_at = time.monotonic()

        def worker():
            try:
                hits = self.local_pool.take_hits(slug)
                if hits:
                    try:
                        self.server.pool_hits(slug, hits)
                    except (NetworkError, ServerError, AuthRequired):
                        self.local_pool.return_hits(slug, hits)
                if hits_only:
                    return
                t0 = time.monotonic()
                try:
                    resp = self.server.pool_sync(slug, rev=self.local_pool.rev(slug), steam_appid=self.detected_appid)
                except AuthRequired:
                    self._post(self._on_auth_lost, "Oturumunuz kapatıldı. Lütfen yeniden giriş yapın.")
                    return
                except (NetworkError, ServerError) as e:
                    if not self._pool_offline_logged:
                        self._pool_offline_logged = True
                        have = self.local_pool.get(slug)
                        self._post(self.log, f"yerel  eşitlenemedi ({e})"
                                   + (f" · bilgisayardaki {_fmt_int(len(have))} satır kullanılıyor" if have else ""))
                    return
                self._pool_offline_logged = False
                d = self.local_pool.apply_sync(slug, resp)
                ms = (time.monotonic() - t0) * 1000
                self._post(self._on_pool_synced, slug, d, ms)
            finally:
                self._pool_busy = False

        threading.Thread(target=worker, daemon=True, name="havuz-esitle").start()

    def _on_pool_synced(self, slug, d, ms):
        if d.get("unchanged"):
            return
        p = self.local_pool.get(slug)
        name = (p.name if p and p.name else slug)
        parts = [f"+{d['added']}"] if d["added"] else []
        if d["changed"]:
            parts.append(f"~{d['changed']} değişti")
        if d["removed"]:
            parts.append(f"−{d['removed']}")
        self.log(f"yerel  {name}: {_fmt_int(d['total'])} onaylı çeviri"
                 + (f" ({' · '.join(parts)})" if parts else "") + f" · {ms:.0f} ms")
        self._refresh_game_meta()


    # ─────────────────────────────────────────────────────────
    # Sunucu durumu: bakım, sürüm kontrolü, duyurular
    # ─────────────────────────────────────────────────────────

    def _fetch_status(self, then):
        def worker():
            try:
                st = self.server.status()
            except (AuthRequired, NetworkError, ServerError):
                st = None
            self._post(then, st)
        threading.Thread(target=worker, daemon=True, name="durum").start()

    def _on_startup_status(self, st):
        self._status = st or {}
        if st and self._gate_for(st):
            return
        if self.server.token:
            self._validate_session()
        else:
            self.login.show_choice()

    def _refresh_status(self):
        self._fetch_status(self._apply_status)

    def _status_tick(self):
        try:
            if self.profile is not None and not self._gate:
                self._refresh_status()
        finally:
            self.after(STATUS_POLL_S * 1000, self._status_tick)

    def _gate_for(self, st) -> bool:
        """Bakım ya da zorunlu güncelleme varsa kapı ekranını gösterir (True)."""
        m = st.get("maintenance") or {}
        if m.get("on") and not m.get("bypass"):
            self._show_gate_maintenance(m.get("message") or "")
            return True
        upd = st.get("update") or {}
        if upd.get("mandatory") and upd.get("latest"):
            # İndirme herkese açık: giriş yapmadan da güncellenir.
            self._show_gate_update(upd)
            return True
        return False

    def _enter_gate(self, kind):
        """Giriş tuvaline geç (çalışıyorsa durdur)."""
        if self.is_running:
            self.toggle_running()
        self._close_popup(mark=False)
        self._gate = kind
        self.c.pack_forget()
        self.login.c.pack(fill="both", expand=True)

    def _leave_gate(self, st):
        self._gate = None
        if self.profile is not None:
            self.login.c.pack_forget()
            self.c.pack(fill="both", expand=True)
            self._apply_status(st)
        elif self.server.token:
            self._validate_session()
        else:
            self.login.show_choice()

    def _apply_status(self, st):
        if not st:
            return
        self._status = st
        if self._gate_for(st):
            return
        if self._gate:
            self._leave_gate(st)
            return
        if self.profile is None:
            return
        upd = st.get("update") or {}
        if upd.get("available") and upd.get("latest") and not self._update_offered:
            self._update_offered = True
            self._popups.append({"_type": "update", "update": upd})
            self.log(f"sürüm  yeni sürüm var: v{upd['latest']['version']} (bu: {APP_VERSION_LABEL})")
        new = [a for a in st.get("announcements") or [] if a.get("id") not in self._ann_seen]
        for i, a in enumerate(new):
            self._ann_seen.add(a.get("id"))
            self._popups.append({"_type": "ann", "ann": a, "_pos": (i + 1, len(new))})
        self._show_next_popup()

    # ── Kapı ekranları ──

    def _portal_url(self):
        return self.server.base_url + "/live/"

    def _show_gate_maintenance(self, message):
        self._enter_gate("maintenance")
        self.login.show_gate("lock", ACCENT, "Bakım çalışması",
                             (message or "Makine Live şu an bakımda.") + "\n\nBakım bitince uygulama kendiliğinden açılır.",
                             primary=("Tekrar dene", lambda: self._fetch_status(self._on_gate_status)))
        if not self._gate_poll_armed:
            # Tek zincir: bakım ekranı her yeniden çizildiğinde (Tekrar dene, çeviri isteğinin bakım yanıtı) yeni bir
            # yoklama zinciri başlamaz; eskiden her biri ayrı zincir açıyor, istekler 30 sn'de bir ikiye katlanıyordu.
            self._gate_poll_armed = True
            self.log("bakım  sunucu bakımda · uygulama bekliyor")
            self.after(30000, self._gate_poll)

    def _gate_poll(self):
        self._gate_poll_armed = False
        if self._gate == "maintenance":
            self._gate_poll_armed = True
            self._fetch_status(self._on_gate_status)
            self.after(30000, self._gate_poll)

    def _on_gate_status(self, st):
        if st is None:
            return
        self._status = st
        if not self._gate_for(st):
            self._leave_gate(st)

    def _show_gate_update(self, upd, error=""):
        latest = upd["latest"]
        self._enter_gate("update")
        notes = (latest.get("notes") or "").strip()
        msg = (f"Bu sürüm ({APP_VERSION_LABEL}) artık kullanılamıyor. Devam etmek için v{latest['version']} sürümüne "
               f"güncelleyin ({latest.get('size', 0) / 1048576:.1f} MB).")
        if notes:
            msg += "\n\n" + (notes if len(notes) <= 280 else notes[:277] + "…")
        if error:
            msg += "\n\n" + error
        self.login.show_gate("arrow", ACCENT, "Güncelleme gerekli", msg,
                             primary=("Şimdi güncelle", lambda: self._start_update(upd)),
                             secondary=("İndirme sayfasını aç", lambda: webbrowser.open(self._portal_url())))

    def _start_update(self, upd, optional=False):
        latest = upd["latest"]
        if not app_update.can_self_update():
            webbrowser.open(self._portal_url())
            self.toast("Geliştirme sürümü: yeni sürümü indirme sayfasından alın")
            return
        if optional:
            self._enter_gate("update")
        dest = os.path.join(app_update.update_dir(), f"MakineLive-{latest['version']}.exe")
        last = [0.0]

        def progress(got, total):
            now = time.monotonic()
            if now - last[0] > 0.25 or (total and got >= total):
                last[0] = now
                self._post(self._update_progress, latest, got, total)

        def worker():
            try:
                path = self.server.download_release(upd["download"], dest, latest.get("sha256", ""), progress)
                self._post(self._finish_update, path, latest)
            except AuthRequired:
                self._post(self._on_auth_lost, "Güncellemek için yeniden giriş yapın.")
            except (NetworkError, ServerError) as e:
                self._post(self._show_gate_update, upd, f"İndirme başarısız: {e}")

        self._update_progress(latest, 0, latest.get("size", 0))
        self.log(f"sürüm  v{latest['version']} indiriliyor")
        threading.Thread(target=worker, daemon=True, name="guncelleme").start()

    def _update_progress(self, latest, got, total):
        frac = got / total if total else None
        mb = f"{got / 1048576:.1f} / {total / 1048576:.1f} MB" if total else f"{got / 1048576:.1f} MB"
        self.login.show_gate("spinner", ACCENT, f"v{latest['version']} indiriliyor", mb + "\n\nİndirme bitince uygulama kapanıp "
                             "yeni sürümle yeniden açılır.", progress=frac if frac is not None else 0.0)

    def _finish_update(self, path, latest):
        self.login.show_gate("check", VERIFIED, "Güncelleniyor",
                             f"v{latest['version']} doğrulandı. Uygulama yeniden açılıyor…")
        self.log(f"sürüm  v{latest['version']} doğrulandı · yeniden başlatılıyor")
        try:
            app_update.swap_and_restart(path)
        except OSError as e:
            self.login.show_gate("arrow", ui.DISPUTED, "Güncelleme başlatılamadı", f"{e}\n\nYeni sürüm: {path}",
                                 primary=("Klasörü aç", lambda: os.startfile(os.path.dirname(path))))
            return
        self.after(600, self._quit_for_update)

    def _quit_for_update(self):
        try:
            self.overlay.clear()
            self.destroy()
        finally:
            os._exit(0)

    # ── Duyuru ve güncelleme pencereleri (panelin üstünde, birer birer) ──

    ANN_COLORS = {"important": "DISPUTED", "maintenance": "ACCENT", "release": "VERIFIED", "info": "TEXT2"}

    def _show_next_popup(self):
        if self._popup_open or not self._popups or self.profile is None or self._gate:
            return
        item = self._popups.pop(0)
        self._popup_open = item
        c, u, f = self.c, self.u, self.fonts
        tag = "popup"
        # Gömülü pencereler (geçmiş / kayıt listesi) tuvalin üstünde çizildiği için gizlenir
        c.itemconfigure(self.history_win, state="hidden")
        c.itemconfigure(self.log_win, state="hidden")
        c.create_rectangle(0, 0, W, H, fill="#000000", stipple="gray50", outline="", tags=tag)
        x0, y0, cw = PAD, 96, INNER
        if item["_type"] == "update":
            latest = item["update"]["latest"]
            kind, label = "release", "Yeni sürüm"
            title = f"Makine Live v{latest['version']} çıktı"
            meta = f"Şu an {APP_VERSION_LABEL} kullanıyorsunuz · {latest.get('size', 0) / 1048576:.1f} MB"
            body = (latest.get("notes") or "").strip() or "Yeni sürümle iyileştirmeler ve düzeltmeler geliyor."
            pos = None
        else:
            a = item["ann"]
            kind, label = a.get("kind", "info"), a.get("kind_label") or "Duyuru"
            title = a.get("title", "")
            meta = f"{a.get('author') or 'Yönetici'} · {_local_time(a.get('created_at', ''))}"
            body = (a.get("body") or "").strip()
            pos = item.get("_pos")
        color = {"DISPUTED": ui.DISPUTED, "ACCENT": ACCENT, "VERIFIED": VERIFIED, "TEXT2": TEXT2}[self.ANN_COLORS.get(kind, "TEXT2")]
        if len(body) > 700:
            body = body[:697] + "…"
        bf = f.ui(13)
        # yükseklik: gövdeyi ölç
        tmp = c.create_text(0, -1000, anchor="nw", text=body, font=bf, width=cw - 40)
        bb = c.bbox(tmp)
        c.delete(tmp)
        body_h = (bb[3] - bb[1]) if (bb and body) else 0
        ch = min(H - y0 - 70, (116 + body_h + 78) if body else 176)   # gövde + düğme payı
        c.create_image(x0, y0, anchor="nw", tags=tag, image=ui.rrect_image(cw, ch, 10, fill=RAISED, outline=mix(LINE_STRONG, color, 0.45)))
        c.create_rectangle(x0 + 1, y0 + 14, x0 + 4, y0 + 46, fill=color, outline="", tags=tag)
        chip_f = f.ui(11, semi=True)
        chip_w = tkfont.Font(font=chip_f).measure(label) + 18
        c.create_image(x0 + 20, y0 + 18, anchor="nw", tags=tag,
                       image=ui.rrect_image(round(chip_w), 22, 11, fill=mix(RAISED, color, 0.16), outline=mix(RAISED, color, 0.55)))
        c.create_text(x0 + 20 + chip_w / 2, y0 + 29, text=label, font=chip_f, fill=color, tags=tag)
        if pos and pos[1] > 1:
            c.create_text(x0 + cw - 20, y0 + 29, anchor="e", text=f"{pos[0]} / {pos[1]}", font=f.code(11), fill=TEXT3, tags=tag)
        c.create_text(x0 + 20, y0 + 62, anchor="nw", text=title, font=f.ui(16, semi=True), fill=TEXT, width=cw - 40, tags=tag)
        c.create_text(x0 + 20, y0 + 90, anchor="nw", text=meta, font=f.ui(11), fill=TEXT3, width=cw - 40, tags=tag)
        if body:
            c.create_text(x0 + 20, y0 + 116, anchor="nw", text=body, font=bf, fill=TEXT2, width=cw - 40, tags=tag)
        by = y0 + ch - 54
        if item["_type"] == "update":
            ui.Button(u, x0 + 20, by, 160, 36, text="Şimdi güncelle", kind="primary", font=f.ui(13, semi=True),
                      command=lambda: self._popup_update(item), tags=(tag,), parent_bg=RAISED)
            ui.Button(u, x0 + 188, by, 90, 36, text="Sonra", kind="ghost", font=f.ui(13),
                      command=lambda: self._close_popup(), tags=(tag,), parent_bg=RAISED)
        else:
            more = bool(self._popups) and self._popups[0].get("_type") == "ann"
            ui.Button(u, x0 + 20, by, 150, 36, text="Sonraki" if more else "Anladım", kind="primary",
                      font=f.ui(13, semi=True), command=lambda: self._close_popup(), tags=(tag,), parent_bg=RAISED)
        self.log(f"duyuru {label.lower()}: {title}" if item["_type"] == "ann" else f"sürüm  v{item['update']['latest']['version']} önerildi")
        try:
            self.deiconify()
            self.lift()
        except tk.TclError:
            pass

    def _close_popup(self, mark=True):
        item = self._popup_open
        if not item:
            return
        self._popup_open = False
        self.c.delete("popup")
        self._show_tab(self.tabs.value, animate=False)
        if mark and item.get("_type") == "ann":
            ann_id = item["ann"].get("id")

            def worker():
                try:
                    self.server.announcement_read(ann_id)
                except (AuthRequired, NetworkError, ServerError):
                    pass
            threading.Thread(target=worker, daemon=True).start()
        self.after(180, self._show_next_popup)

    def _popup_update(self, item):
        self._close_popup()
        self._start_update(item["update"], optional=True)

    def _fit(self, text, font):
        """Metni oyun kartındaki alana sığdır (sonu … ile kısaltılır)."""
        fnt = tkfont.Font(font=font)
        if fnt.measure(text) <= self._game_text_w:
            return text
        while text and fnt.measure(text + "…") > self._game_text_w:
            text = text[:-1]
        return text.rstrip() + "…"

    def _set_block_reason(self, reason):
        if reason == self._block_reason:
            return
        self._block_reason = reason
        if reason and time.monotonic() - self._last_block_log > 2.0:
            self._last_block_log = time.monotonic()
            self.log(f"bekle  {reason}")
        self._tick_clock(reschedule=False)

    def _stage_summary(self):
        def med(xs):
            xs = sorted(xs)
            return round(xs[len(xs) // 2]) if xs else None
        s = self.stage_ms
        return f"ocr {med(s['ocr'])} ms · havuzdan {med(s['pool'])} ms · deepl ile {med(s['api'])} ms (istek medyanı)"

    def _count_line(self):
        self.stats["lines"] += 1
        self._refresh_stats()

    # ─────────────────────────────────────────────────────────
    # Global kısayollar
    # ─────────────────────────────────────────────────────────

    def _start_hotkey_listener(self):
        def listener():
            prev = {VK_F8: False, VK_F9: False, VK_F10: False}
            actions = {VK_F8: self.toggle_lock, VK_F9: self.toggle_running, VK_F10: self.start_region_selection}
            while True:
                for vk, action in actions.items():
                    down = bool(user32.GetAsyncKeyState(vk) & 0x8000)
                    if down and not prev[vk]:
                        self._post(action)
                    prev[vk] = down
                time.sleep(0.06)

        threading.Thread(target=listener, daemon=True).start()

    def destroy(self):
        self.is_running = False
        self.discord.stop()
        self._executor.shutdown(wait=False, cancel_futures=True)
        self._close_sheet()
        self.tv_link.clear()
        self.tv_link.close()
        self.web.stop()
        self._save_hud_prefs()
        try:
            self.overlay.destroy()
        except Exception:
            pass
        self.plugin.shutdown()
        super().destroy()


# ─────────────────────────────────────────────────────────────
# Dosya günlüğü ve hata yakalama (kullanıcılar sorun bildirirken bu dosyayı gönderir)
#   %APPDATA%\MakineLive\logs\makine-live.log  — paneldeki kayıtlar + yakalanmayan hataların tam izi
# ─────────────────────────────────────────────────────────────

import logging
import logging.handlers
import traceback

_FILE_LOG = logging.getLogger("makine-live")
_FILE_LOG.propagate = False


def _setup_file_log() -> str:
    log_dir = os.path.join(app_data_dir(), "logs")
    path = os.path.join(log_dir, "makine-live.log")
    try:
        os.makedirs(log_dir, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(path, maxBytes=1_000_000, backupCount=2, encoding="utf-8")
        handler.setFormatter(logging.Formatter("%(asctime)s  %(message)s"))
        _FILE_LOG.addHandler(handler)
        _FILE_LOG.setLevel(logging.INFO)
        _FILE_LOG.info("──── Makine Live %s başladı (%s)", APP_VERSION_LABEL, "exe" if FROZEN else "geliştirme")
    except OSError:
        pass
    return path


_CRASH_FILE = None


def _enable_native_crash_log(log_path: str) -> None:
    """
    Yerel çökmeler (erişim ihlali, "Fatal Python error") Python istisnası üretmez ve makine-live.log'a düşmez.
    faulthandler bütün iş parçacıklarının yığınını logs/crash.log dosyasına yazar.
    """
    global _CRASH_FILE
    try:
        import faulthandler
        if _CRASH_FILE is None:
            _CRASH_FILE = open(os.path.join(os.path.dirname(log_path), "crash.log"), "a", encoding="utf-8")
            _CRASH_FILE.write(f"\n──── {time.strftime('%Y-%m-%d %H:%M:%S')} · Makine Live {APP_VERSION_LABEL}\n")
            _CRASH_FILE.flush()
        faulthandler.enable(file=_CRASH_FILE, all_threads=True)
    except (OSError, ValueError, RuntimeError):
        pass


def _install_crash_handlers(log_path: str, app=None) -> None:
    def report(kind, exc_type, exc, tb):
        _FILE_LOG.error("YAKALANMAYAN HATA (%s)\n%s", kind, "".join(traceback.format_exception(exc_type, exc, tb)))
        if app is not None:
            try:
                app.after(0, app.log, f"hata   beklenmeyen bir sorun oluştu · ayrıntı: {log_path}")
            except Exception:  # noqa: BLE001 — pencere kapanmış olabilir
                pass

    _enable_native_crash_log(log_path)
    sys.excepthook = lambda t, e, tb: report("ana", t, e, tb)
    threading.excepthook = lambda a: report(f"iş parçacığı {a.thread.name if a.thread else '?'}", a.exc_type, a.exc_value, a.exc_traceback)
    if app is not None:
        app.report_callback_exception = lambda t, e, tb: report("arayüz", t, e, tb)


# ─────────────────────────────────────────────────────────────
# Giriş noktası: tek örnek, --selftest
# ─────────────────────────────────────────────────────────────

_MUTEX_NAME = r"Local\MakineLive.App.SingleInstance"


def _acquire_single_instance():
    """İkinci açılışta mevcut pencereyi öne getirip çıkar. Mutex tutamağı süreç boyunca açık kalmalı."""
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateMutexW.restype = ctypes.c_void_p
    handle = kernel32.CreateMutexW(None, False, _MUTEX_NAME)
    if kernel32.GetLastError() == 183:   # ERROR_ALREADY_EXISTS
        hwnd = user32.FindWindowW("TkTopLevel", "Makine Live")
        if hwnd:
            user32.ShowWindow(hwnd, 9)   # SW_RESTORE
            user32.SetForegroundWindow(hwnd)
        return None
    return handle


def run_selftest() -> int:
    """
    Arayüzsüz doğrulama (paketlenmiş exe için): DLL'i paketten yükler, ekrana kısa süre bilinen bir satır çizip
    OCR ile okur, sonucu %APPDATA%/MakineLive/selftest.json dosyasına yazar. 0 = başarılı.
    """
    from PIL import ImageDraw, ImageFont
    expected = "Press X to call Jason"
    result = {"frozen": FROZEN, "runtime_dir": RUNTIME_DIR, "version": APP_VERSION, "ok": False}
    t_start = time.perf_counter()
    try:
        plugin = PluginWrapper(os.path.join(RUNTIME_DIR, "makine-live.dll"))
        result["dll_loaded"] = plugin.is_loaded
        rc = plugin.initialize(DATA_DIR)
        result["init_rc"] = rc
        if rc != 0:
            result["error"] = plugin.last_error()
        else:
            root = tk.Tk()
            root.overrideredirect(True)
            root.attributes("-topmost", True)
            w, h = 760, 110
            root.geometry(f"{w}x{h}+80+80")
            img = Image.new("RGB", (w, h), (16, 16, 18))
            try:
                fnt = ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "segoeui.ttf"), 44)
            except OSError:
                fnt = ImageFont.load_default()
            ImageDraw.Draw(img).text((30, 22), expected, font=fnt, fill=(255, 255, 255))
            photo = ImageTk.PhotoImage(img)
            tk.Label(root, image=photo, bd=0).pack()
            root.update()
            time.sleep(0.4)
            root.update()
            r = ctypes.wintypes.RECT()
            user32.GetWindowRect(root.winfo_id(), ctypes.byref(r))
            text = ""
            t0 = time.perf_counter()
            for _ in range(3):
                text = plugin.capture_and_ocr(r.left, r.top, r.right - r.left, r.bottom - r.top)
                if "Jason" in text:
                    break
                time.sleep(0.2)
            result["ocr_ms"] = round((time.perf_counter() - t0) * 1000)
            result["ocr_text"] = text
            result["ocr_model"] = plugin.get_setting("_ocrModel")
            result["ok"] = "Jason" in text
            root.destroy()
        plugin.shutdown()
        # Oyun kütüphanesi paketten de okunabiliyor mu (kayıt defteri, Steam appinfo.vdf); ağa çıkılmaz
        lib = get_library()
        lib.classifier = None
        t0 = time.perf_counter()
        lib.refresh()
        result["games"] = lib.summary()
        result["games_ms"] = round((time.perf_counter() - t0) * 1000)
        # Konsol / TV modu: "TV'ye kur" için gömülü APK ve ADB kütüphanesi pakette mi
        result["tv_apk"] = bool(tv_install.apk_path())
        result["tv_ipk"] = bool(tv_install.ipk_path())
        try:
            from adb_shell.adb_device import AdbDeviceTcp  # noqa: F401
            from adb_shell.auth.sign_pythonrsa import PythonRSASigner  # noqa: F401
            result["tv_adb"] = True
        except ImportError as e:
            result["tv_adb"] = f"yok: {e}"
        try:
            import paramiko  # noqa: F401
            result["tv_ssh"] = True
        except ImportError as e:
            result["tv_ssh"] = f"yok: {e}"
    except Exception as e:  # noqa: BLE001 — selftest her hatayı rapora yazar
        result["error"] = f"{type(e).__name__}: {e}"
    result["total_ms"] = round((time.perf_counter() - t_start) * 1000)
    os.makedirs(app_data_dir(), exist_ok=True)
    with open(os.path.join(app_data_dir(), "selftest.json"), "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False, indent=2)
    return 0 if result["ok"] else 1


def main() -> int:
    log_path = _setup_file_log()
    _install_crash_handlers(log_path)
    if "--selftest" in sys.argv:
        return run_selftest()
    mutex = _acquire_single_instance()
    if mutex is None:
        return 0
    try:
        app = MakineGameLiveApp()
    except Exception:  # noqa: BLE001 — açılış hatası dosyaya yazılır, sonra PyInstaller penceresi gösterilir
        _FILE_LOG.error("AÇILIŞ HATASI\n%s", traceback.format_exc())
        raise
    _install_crash_handlers(log_path, app)
    # Windows zamanlayıcısı varsayılan 15,6 ms: Tk'nın after(16) animasyonları takılır. Uygulama açıkken 1 ms
    # (oyunlar da aynısını yapar); kapanınca eski hâline döner.
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)
    except (AttributeError, OSError):
        pass
    try:
        app.mainloop()
    finally:
        try:
            ctypes.windll.winmm.timeEndPeriod(1)
        except (AttributeError, OSError):
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
