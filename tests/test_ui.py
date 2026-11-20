#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Makine Live Plugin — Geliştirici Test & Simülasyon Arayüzü
Makine Launcher'dan bağımsız olarak makine-live.dll'in tüm işlevlerini canlı test eder.
"""

import os
import sys
import time
import json
import urllib.request
import ctypes
from ctypes import c_char_p, c_uint32, c_int, c_void_p, c_bool, Structure
import threading
import tkinter as tk
from tkinter import messagebox
import customtkinter as ctk

from _root import ROOT  # noqa: E402

# Windows DPI Awareness (Ekran koordinatlarının %125/%150 ölçeklemede kaymasını önler)
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2) # Per-Monitor DPI Aware
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

# ─────────────────────────────────────────────────────────────
# 1. DLL C ABI Yapıları ve Sarmalayıcısı
# ─────────────────────────────────────────────────────────────

class MakinePluginInfo(Structure):
    _fields_ = [
        ("id", c_char_p),
        ("name", c_char_p),
        ("version", c_char_p),
        ("apiVersion", c_uint32),
    ]

class PluginWrapper:
    def __init__(self, dll_path: str):
        self.dll_path = os.path.abspath(dll_path)
        self.dll = None
        self.is_loaded = False
        self.error_msg = ""
        self._lock = threading.Lock()
        self.load()

    def load(self):
        if not os.path.exists(self.dll_path):
            self.error_msg = f"DLL bulunamadı: {self.dll_path}"
            return False

        try:
            self.dll = ctypes.CDLL(self.dll_path)

            self.dll.makine_get_info.restype = MakinePluginInfo
            self.dll.makine_get_info.argtypes = []

            self.dll.makine_initialize.restype = c_int
            self.dll.makine_initialize.argtypes = [c_char_p]

            self.dll.makine_shutdown.restype = None
            self.dll.makine_shutdown.argtypes = []

            self.dll.makine_is_ready.restype = c_bool
            self.dll.makine_is_ready.argtypes = []

            self.dll.makine_get_last_error.restype = c_char_p
            self.dll.makine_get_last_error.argtypes = []

            self.dll.makine_get_setting.restype = c_char_p
            self.dll.makine_get_setting.argtypes = [c_char_p]

            self.dll.makine_set_setting.restype = None
            self.dll.makine_set_setting.argtypes = [c_char_p, c_char_p]

            self.dll.makine_capture_and_ocr.restype = c_char_p
            self.dll.makine_capture_and_ocr.argtypes = [c_void_p, c_int, c_int, c_int, c_int]

            self.dll.makine_capture_ocr_translate.restype = c_char_p
            self.dll.makine_capture_ocr_translate.argtypes = [c_void_p, c_int, c_int, c_int, c_int]

            self.dll.makine_get_last_ocr_text.restype = c_char_p
            self.dll.makine_get_last_ocr_text.argtypes = []

            self.is_loaded = True
            return True
        except Exception as e:
            self.error_msg = str(e)
            self.is_loaded = False
            return False

    def initialize(self, data_path: str) -> int:
        with self._lock:
            if not self.is_loaded: return -2
            os.makedirs(data_path, exist_ok=True)
            return self.dll.makine_initialize(data_path.encode('utf-8'))

    def shutdown(self):
        with self._lock:
            if self.is_loaded and self.dll:
                self.dll.makine_shutdown()

    def is_ready(self) -> bool:
        with self._lock:
            if not self.is_loaded: return False
            return self.dll.makine_is_ready()

    def get_info(self):
        with self._lock:
            if not self.is_loaded: return None
            info = self.dll.makine_get_info()
            return {
                "id": info.id.decode('utf-8', errors='replace'),
                "name": info.name.decode('utf-8', errors='replace'),
                "version": info.version.decode('utf-8', errors='replace'),
                "apiVersion": info.apiVersion,
            }

    def get_setting(self, key: str) -> str:
        with self._lock:
            if not self.is_loaded: return ""
            res = self.dll.makine_get_setting(key.encode('utf-8'))
            return res.decode('utf-8', errors='replace') if res else ""

    def set_setting(self, key: str, val: str):
        with self._lock:
            if not self.is_loaded: return
            self.dll.makine_set_setting(key.encode('utf-8'), val.encode('utf-8'))

    def capture_and_ocr(self, x: int, y: int, w: int, h: int) -> str:
        with self._lock:
            if not self.is_loaded: return ""
            res = self.dll.makine_capture_and_ocr(None, x, y, w, h)
            return res.decode('utf-8', errors='replace') if res else ""

    def capture_ocr_translate(self, x: int, y: int, w: int, h: int) -> str:
        with self._lock:
            if not self.is_loaded: return ""
            res = self.dll.makine_capture_ocr_translate(None, x, y, w, h)
            return res.decode('utf-8', errors='replace') if res else ""

    def get_last_ocr_text(self) -> str:
        with self._lock:
            if not self.is_loaded: return ""
            res = self.dll.makine_get_last_ocr_text()
            return res.decode('utf-8', errors='replace') if res else ""

    def get_last_error(self) -> str:
        with self._lock:
            if not self.is_loaded: return ""
            res = self.dll.makine_get_last_error()
            return res.decode('utf-8', errors='replace') if res else ""


# ─────────────────────────────────────────────────────────────
# 2. Şeffaf Oyun İçi Overlay Penceresi (Subtitle Simülatörü)
# ─────────────────────────────────────────────────────────────

class InGameOverlay(ctk.CTkToplevel):
    def __init__(self, master):
        super().__init__(master)
        self.title("Makine Live — In-Game Overlay")
        self.geometry("700x120+400+750")
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.attributes("-alpha", 0.88)
        self.configure(fg_color="#0a0c10")

        # Taşınabilirlik (Drag & Drop)
        self.bind("<ButtonPress-1>", self._start_move)
        self.bind("<B1-Motion>", self._on_move)

        # Windows DWM: Ekran yakalamaya karşı görünmez yap (Arkasını okumasını sağlar)
        self.update_idletasks()
        try:
            root_hwnd = user32.GetAncestor(self.winfo_id(), 2) or self.winfo_id()
            user32.SetWindowDisplayAffinity(root_hwnd, 0x00000011) # WDA_EXCLUDEFROMCAPTURE
            user32.SetWindowDisplayAffinity(self.winfo_id(), 0x00000011)
        except Exception:
            pass

        # Çerçeve
        self.frame = ctk.CTkFrame(self, fg_color="#12161f", corner_radius=12, border_width=1, border_color="#2b3346")
        self.frame.pack(fill="both", expand=True, padx=4, pady=4)

        # Üst bar
        top_bar = ctk.CTkFrame(self.frame, fg_color="transparent")
        top_bar.pack(fill="x", padx=10, pady=(6, 2))

        self.badge = ctk.CTkLabel(top_bar, text="🎮 OYUN İÇİ CANLI ÇEVİRİ", font=ctk.CTkFont(size=10, weight="bold"), text_color="#38bdf8")
        self.badge.pack(side="left")

        close_btn = ctk.CTkButton(top_bar, text="✕", width=22, height=22, fg_color="#ef4444", hover_color="#dc2626",
                                  command=self.hide_overlay, corner_radius=11)
        close_btn.pack(side="right")

        # Çeviri Metni
        self.label_translation = ctk.CTkLabel(
            self.frame,
            text="(Ekrandan metin algılandığında çeviri burada görünecektir)",
            font=ctk.CTkFont(size=15, weight="bold"),
            text_color="#f8fafc",
            wraplength=660,
            justify="center"
        )
        self.label_translation.pack(fill="both", expand=True, padx=12, pady=(0, 6))

    def _start_move(self, event):
        self._x = event.x
        self._y = event.y

    def _on_move(self, event):
        deltax = event.x - self._x
        deltay = event.y - self._y
        new_x = self.winfo_x() + deltax
        new_y = self.winfo_y() + deltay
        self.geometry(f"+{new_x}+{new_y}")

    def update_text(self, text: str):
        if text.strip():
            self.label_translation.configure(text=text, text_color="#38bdf8")
        else:
            self.label_translation.configure(text="[Metin bekleniyor...]", text_color="#64748b")

    def hide_overlay(self):
        self.withdraw()


# ─────────────────────────────────────────────────────────────
# 3. Ekrandan Bölge Seçme Aracı (Snipping Overlay)
# ─────────────────────────────────────────────────────────────

class RegionSelector(tk.Toplevel):
    def __init__(self, callback):
        super().__init__()
        self.callback = callback
        self.attributes("-fullscreen", True)
        self.attributes("-alpha", 0.30)
        self.attributes("-topmost", True)
        self.config(cursor="cross")

        self.canvas = tk.Canvas(self, cursor="cross", bg="#000000", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)

        self.start_x = None
        self.start_y = None
        self.rect_id = None

        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.bind("<Escape>", lambda e: self.destroy())

    def on_press(self, event):
        self.start_x = event.x
        self.start_y = event.y
        self.rect_id = self.canvas.create_rectangle(self.start_x, self.start_y, self.start_x, self.start_y,
                                                   outline="#ef4444", width=3, fill="#38bdf8", stipple="gray25")

    def on_drag(self, event):
        cur_x, cur_y = event.x, event.y
        self.canvas.coords(self.rect_id, self.start_x, self.start_y, cur_x, cur_y)

    def on_release(self, event):
        end_x, end_y = event.x, event.y
        x = min(self.start_x, end_x)
        y = min(self.start_y, end_y)
        w = abs(end_x - self.start_x)
        h = abs(end_y - self.start_y)
        self.destroy()
        if w > 10 and h > 10:
            self.callback(x, y, w, h)


# ─────────────────────────────────────────────────────────────
# 4. Ana Geliştirici Test Arayüzü (Main Window)
# ─────────────────────────────────────────────────────────────

class MakineLiveDevUI(ctk.CTk):
    def __init__(self):
        super().__init__()

        # Pencere Ayarları
        self.title("Makine Live — Geliştirici Test & Simülasyon Arayüzü")
        self.geometry("1100x720")
        self.minsize(980, 640)
        ctk.set_appearance_mode("Dark")
        ctk.set_default_color_theme("blue")

        # Plugin Yolu
        self.root_dir = ROOT
        self.dll_path = os.path.join(self.root_dir, "build", "release", "makine-live.dll")
        self.data_path = os.path.join(self.root_dir, "build", "test_data")

        # Plugin Yükle
        self.plugin = PluginWrapper(self.dll_path)
        self.overlay = None
        self.is_live_running = False
        self.live_thread = None

        # Arayüz Bileşenlerini İnşa Et
        self._build_ui()
        self._init_plugin()

    def _build_ui(self):
        # Üst Başlık Barı
        header = ctk.CTkFrame(self, fg_color="#181e29", corner_radius=0, height=65)
        header.pack(fill="x", side="top")

        title_box = ctk.CTkFrame(header, fg_color="transparent")
        title_box.pack(side="left", padx=20, pady=12)

        ctk.CTkLabel(title_box, text="⚡ MAKINE LIVE", font=ctk.CTkFont(size=20, weight="bold"), text_color="#38bdf8").pack(side="left")
        ctk.CTkLabel(title_box, text="  Plugin Developer Suite (C++23 Shared Library Test)", font=ctk.CTkFont(size=13), text_color="#94a3b8").pack(side="left")

        # Durum Rozetleri
        self.status_box = ctk.CTkFrame(header, fg_color="transparent")
        self.status_box.pack(side="right", padx=20)

        self.badge_dll = ctk.CTkLabel(self.status_box, text="● DLL: Kontrol...", font=ctk.CTkFont(size=11, weight="bold"),
                                      text_color="#e2e8f0", fg_color="#334155", corner_radius=6, padx=10, pady=4)
        self.badge_dll.pack(side="left", padx=4)

        self.badge_ready = ctk.CTkLabel(self.status_box, text="● Hazır: Hayır", font=ctk.CTkFont(size=11, weight="bold"),
                                        text_color="#e2e8f0", fg_color="#334155", corner_radius=6, padx=10, pady=4)
        self.badge_ready.pack(side="left", padx=4)

        # Ana Gövde (2 Sütun)
        body = ctk.CTkFrame(self, fg_color="transparent")
        body.pack(fill="both", expand=True, padx=16, pady=16)

        # Sol Kontrol Paneli (Genişlik: ~380px)
        left_panel = ctk.CTkScrollableFrame(body, width=390, fg_color="#131720", corner_radius=12)
        left_panel.pack(side="left", fill="y", padx=(0, 12))

        # Sağ Çıktı & Monitör Paneli
        right_panel = ctk.CTkFrame(body, fg_color="#131720", corner_radius=12)
        right_panel.pack(side="right", fill="both", expand=True)

        # ── SOL PANEL: 1. Alan & Yakalama Seçenekleri ──
        ctk.CTkLabel(left_panel, text="🎯 Yakalanacak Bölge (Screen Region)", font=ctk.CTkFont(size=14, weight="bold"), text_color="#f1f5f9").pack(anchor="w", padx=10, pady=(10, 6))

        coord_grid = ctk.CTkFrame(left_panel, fg_color="transparent")
        coord_grid.pack(fill="x", padx=10, pady=4)

        # X, Y
        ctk.CTkLabel(coord_grid, text="X:", font=ctk.CTkFont(weight="bold")).grid(row=0, column=0, sticky="w", pady=2)
        self.entry_x = ctk.CTkEntry(coord_grid, width=70)
        self.entry_x.insert(0, "0")
        self.entry_x.grid(row=0, column=1, padx=(4, 12), pady=2)

        ctk.CTkLabel(coord_grid, text="Y:", font=ctk.CTkFont(weight="bold")).grid(row=0, column=2, sticky="w", pady=2)
        self.entry_y = ctk.CTkEntry(coord_grid, width=70)
        self.entry_y.insert(0, "0")
        self.entry_y.grid(row=0, column=3, padx=4, pady=2)

        # W, H
        ctk.CTkLabel(coord_grid, text="W:", font=ctk.CTkFont(weight="bold")).grid(row=1, column=0, sticky="w", pady=2)
        self.entry_w = ctk.CTkEntry(coord_grid, width=70)
        self.entry_w.insert(0, "800")
        self.entry_w.grid(row=1, column=1, padx=(4, 12), pady=2)

        ctk.CTkLabel(coord_grid, text="H:", font=ctk.CTkFont(weight="bold")).grid(row=1, column=2, sticky="w", pady=2)
        self.entry_h = ctk.CTkEntry(coord_grid, width=70)
        self.entry_h.insert(0, "400")
        self.entry_h.grid(row=1, column=3, padx=4, pady=2)

        # Alan Seçim Butonları
        btn_box = ctk.CTkFrame(left_panel, fg_color="transparent")
        btn_box.pack(fill="x", padx=10, pady=(6, 12))

        ctk.CTkButton(btn_box, text="🎯 Ekranda Çiz", fg_color="#2563eb", hover_color="#1d4ed8",
                      command=self._open_region_picker, width=160).pack(side="left", padx=(0, 6))

        ctk.CTkButton(btn_box, text="🖥️ Tüm Ekran", fg_color="#334155", hover_color="#475569",
                      command=self._set_fullscreen, width=160).pack(side="right")

        # Ayarlar API
        ctk.CTkLabel(left_panel, text="⚙️ Eklenti Parametreleri", font=ctk.CTkFont(size=14, weight="bold"), text_color="#f1f5f9").pack(anchor="w", padx=10, pady=(10, 6))

        # OCR Aktif Toggle (CRITICAL: C++ live_core.cpp line 320 checks ocrEnabled == true)
        self.switch_ocr_enabled = ctk.CTkSwitch(left_panel, text="⚡ OCR Etkinleştir (ocrEnabled)", command=self._apply_settings_to_dll,
                                                font=ctk.CTkFont(weight="bold"), text_color="#38bdf8")
        self.switch_ocr_enabled.select()
        self.switch_ocr_enabled.pack(anchor="w", padx=10, pady=(2, 6))

        # Çeviri Motoru
        ctk.CTkLabel(left_panel, text="Çeviri Motoru:", font=ctk.CTkFont(size=12)).pack(anchor="w", padx=10, pady=(2, 0))
        self.combo_engine = ctk.CTkOptionMenu(left_panel, values=["google", "gpt", "deepl"])
        self.combo_engine.set("google")
        self.combo_engine.pack(fill="x", padx=10, pady=2)

        # API Key
        ctk.CTkLabel(left_panel, text="API Anahtarı (Google/GPT/DeepL):", font=ctk.CTkFont(size=12)).pack(anchor="w", padx=10, pady=(6, 0))
        self.entry_apikey = ctk.CTkEntry(left_panel, placeholder_text="API anahtarınızı girin...", show="*")
        self.entry_apikey.pack(fill="x", padx=10, pady=2)

        # Diller (Kaynak -> Hedef)
        lang_box = ctk.CTkFrame(left_panel, fg_color="transparent")
        lang_box.pack(fill="x", padx=10, pady=6)

        ctk.CTkLabel(lang_box, text="Kaynak:").grid(row=0, column=0, sticky="w")
        self.combo_srclang = ctk.CTkOptionMenu(lang_box, values=["en", "ja", "ko", "zh", "de", "fr", "es", "ru"], width=130)
        self.combo_srclang.set("en")
        self.combo_srclang.grid(row=0, column=1, padx=(6, 12))

        ctk.CTkLabel(lang_box, text="Hedef:").grid(row=0, column=2, sticky="w")
        self.combo_tgtlang = ctk.CTkOptionMenu(lang_box, values=["tr", "en", "de", "fr", "es", "ru"], width=130)
        self.combo_tgtlang.set("tr")
        self.combo_tgtlang.grid(row=0, column=3, padx=6)

        # Yakalama Yöntemi & Aralık
        cap_box = ctk.CTkFrame(left_panel, fg_color="transparent")
        cap_box.pack(fill="x", padx=10, pady=4)

        ctk.CTkLabel(cap_box, text="Yakalama:").grid(row=0, column=0, sticky="w")
        self.combo_capture = ctk.CTkOptionMenu(cap_box, values=["auto", "dxgi", "gdi"], width=130)
        self.combo_capture.set("auto")
        self.combo_capture.grid(row=0, column=1, padx=(6, 12))

        ctk.CTkLabel(cap_box, text="Aralık (ms):").grid(row=0, column=2, sticky="w")
        self.combo_interval = ctk.CTkOptionMenu(cap_box, values=["500", "1000", "2000", "3000"], width=130)
        self.combo_interval.set("1000")
        self.combo_interval.grid(row=0, column=3, padx=6)

        # Ayarları Kaydet Butonu
        ctk.CTkButton(left_panel, text="💾 Ayarları DLL'e Uygula (set_setting)", fg_color="#334155", hover_color="#475569",
                      command=self._apply_settings_to_dll).pack(fill="x", padx=10, pady=(8, 14))

        # ── SOL PANEL: 3. Tetikleyiciler ──
        ctk.CTkLabel(left_panel, text="🚀 Test Komutları", font=ctk.CTkFont(size=14, weight="bold"), text_color="#f1f5f9").pack(anchor="w", padx=10, pady=(6, 6))

        ctk.CTkButton(left_panel, text="📷 Tek Seferlik Yakala & OCR", fg_color="#0284c7", hover_color="#0369a1",
                      command=self._trigger_ocr_only).pack(fill="x", padx=10, pady=4)

        ctk.CTkButton(left_panel, text="🌐 Tek Seferlik Yakala & Çevir", fg_color="#10b981", hover_color="#059669",
                      command=self._trigger_ocr_translate).pack(fill="x", padx=10, pady=4)

        self.btn_live = ctk.CTkButton(left_panel, text="▶️ Canlı OCR/Çeviri Döngüsünü Başlat", fg_color="#7c3aed", hover_color="#6d28d9",
                                      command=self._toggle_live_loop, height=38, font=ctk.CTkFont(weight="bold"))
        self.btn_live.pack(fill="x", padx=10, pady=(8, 8))

        ctk.CTkButton(left_panel, text="🎮 OYUN İÇİ HUD'U BAŞLAT (py -m makine_live)", fg_color="#ea580c", hover_color="#c2410c",
                      height=38, font=ctk.CTkFont(weight="bold"),
                      command=self._launch_game_overlay).pack(fill="x", padx=10, pady=(0, 8))

        self.switch_overlay = ctk.CTkSwitch(left_panel, text="🪟 Şeffaf Oyun İçi Overlay'i Göster", command=self._toggle_overlay)
        self.switch_overlay.pack(anchor="w", padx=10, pady=(6, 12))

        # ── SAĞ PANEL: Çıktı & İzleme ──
        # 1. OCR Metni Başlığı + Doğrudan Çeviri Butonu
        ocr_header = ctk.CTkFrame(right_panel, fg_color="transparent")
        ocr_header.pack(fill="x", padx=16, pady=(14, 4))

        ctk.CTkLabel(ocr_header, text="📝 Orijinal Metin (OCR Çıktısı)",
                     font=ctk.CTkFont(size=13, weight="bold"), text_color="#cbd5e1").pack(side="left")

        ctk.CTkButton(ocr_header, text="🌐 Bu Metni Çevir (Doğrudan API Testi)", fg_color="#059669", hover_color="#047857",
                      command=self._translate_current_ocr_text, height=26, font=ctk.CTkFont(size=12, weight="bold")).pack(side="right")

        self.txt_ocr = ctk.CTkTextbox(right_panel, height=130, font=ctk.CTkFont(size=13))
        self.txt_ocr.pack(fill="x", padx=16, pady=(0, 10))

        # 2. Çeviri Metni
        ctk.CTkLabel(right_panel, text="🌍 Çevrilmiş Metin (Translation - makine_capture_ocr_translate)",
                     font=ctk.CTkFont(size=13, weight="bold"), text_color="#38bdf8").pack(anchor="w", padx=16, pady=(4, 4))

        self.txt_trans = ctk.CTkTextbox(right_panel, height=150, font=ctk.CTkFont(size=14, weight="bold"))
        self.txt_trans.pack(fill="x", padx=16, pady=(0, 10))

        # 3. Canlı Log ve İstatistikler
        ctk.CTkLabel(right_panel, text="📊 Pipeline Durumu & Sistem Günlüğü",
                     font=ctk.CTkFont(size=13, weight="bold"), text_color="#94a3b8").pack(anchor="w", padx=16, pady=(4, 4))

        self.txt_logs = ctk.CTkTextbox(right_panel, height=140, font=ctk.CTkFont(family="Consolas", size=11))
        self.txt_logs.pack(fill="both", expand=True, padx=16, pady=(0, 14))

    # ─────────────────────────────────────────────────────────
    # Mantıksal Fonksiyonlar & DLL Entegrasyonu
    # ─────────────────────────────────────────────────────────

    def log(self, msg: str):
        timestamp = time.strftime("%H:%M:%S")
        entry = f"[{timestamp}] {msg}\n"
        self.txt_logs.insert("end", entry)
        self.txt_logs.see("end")

    def _init_plugin(self):
        if not self.plugin.is_loaded:
            self.badge_dll.configure(text="● DLL: HATA", fg_color="#ef4444")
            self.log(f"HATA: DLL yüklenemedi: {self.plugin.error_msg}")
            return

        self.badge_dll.configure(text="● DLL: Yüklendi (v0.1.0)", fg_color="#10b981")
        self.log(f"DLL başarıyla yüklendi: {self.plugin.dll_path}")

        # Bilgileri al
        info = self.plugin.get_info()
        if info:
            self.log(f"Eklenti Bilgisi: {info['name']} (v{info['version']}) [ID: {info['id']}, API: {info['apiVersion']}]")

        # Initialize
        res = self.plugin.initialize(self.data_path)
        if res == 0:
            self.log(f"makine_initialize başarılı. (Veri dizini: {self.data_path})")
            ready = self.plugin.is_ready()
            if ready:
                self.badge_ready.configure(text="● Hazır: Evet", fg_color="#10b981")
                self.log("makine_is_ready: True (Capture pipeline hazır)")
            else:
                self.badge_ready.configure(text="● Hazır: Hayır", fg_color="#f59e0b")
        else:
            self.log(f"makine_initialize başarısız (Kod: {res})")

        # Bağımlılık kontrolü (RapidOcrOnnx.dll, onnxruntime.dll, models/)
        release_dir = os.path.dirname(self.plugin.dll_path)
        rapid_dll = os.path.join(release_dir, "RapidOcrOnnx.dll")
        onnx_dll = os.path.join(release_dir, "onnxruntime.dll")
        models_dir = os.path.join(release_dir, "models")

        has_rapid = os.path.isfile(rapid_dll)
        has_onnx = os.path.isfile(onnx_dll)
        has_models = os.path.isdir(models_dir)

        if not has_rapid or not has_models:
            self.log("⚠️ [KRİTİK UYARI] 'RapidOcrOnnx.dll' veya 'models/' klasörü 'build/release/' dizininde bulunamadı!")
            self.log("   -> Eklentinin C++ kodu (s_ocr->isReady() == false) olduğu için OCR adımını 0ms içinde boş geçer.")
            self.log("   -> Gerçek metin tanıma için lütfen bu dosyaları 'build/release/' içine kopyalayın.")
        else:
            self.log("✅ Tüm OCR runtime bağımlılıkları (RapidOcrOnnx.dll, models/) mevcut.")

        # Mevcut ayarları DLL'den oku
        self._load_settings_from_dll()
        # Varsayılan olarak ocrEnabled = true yap
        self._apply_settings_to_dll()

    def _load_settings_from_dll(self):
        try:
            ocr_en = self.plugin.get_setting("ocrEnabled")
            if ocr_en == "false":
                self.switch_ocr_enabled.deselect()
            else:
                self.switch_ocr_enabled.select()

            engine = self.plugin.get_setting("translatorEngine")
            if engine in ["google", "gpt", "deepl"]: self.combo_engine.set(engine)

            src = self.plugin.get_setting("sourceLang")
            if src: self.combo_srclang.set(src)

            tgt = self.plugin.get_setting("targetLang")
            if tgt: self.combo_tgtlang.set(tgt)

            cap = self.plugin.get_setting("captureMethod")
            if cap in ["auto", "dxgi", "gdi"]: self.combo_capture.set(cap)

            interval = self.plugin.get_setting("ocrInterval")
            if interval in ["500", "1000", "2000", "3000"]: self.combo_interval.set(interval)

            api_key = self.plugin.get_setting("apiKey")
            if api_key:
                self.entry_apikey.delete(0, "end")
                self.entry_apikey.insert(0, api_key)

            self.log("Mevcut ayarlar DLL'den senkronize edildi.")
        except Exception as e:
            self.log(f"Ayar yükleme uyarısı: {e}")

    def _apply_settings_to_dll(self):
        try:
            ocr_val = "true" if self.switch_ocr_enabled.get() else "false"
            self.plugin.set_setting("ocrEnabled", ocr_val)
            self.plugin.set_setting("translatorEngine", self.combo_engine.get())
            self.plugin.set_setting("apiKey", self.entry_apikey.get().strip())
            self.plugin.set_setting("sourceLang", self.combo_srclang.get())
            self.plugin.set_setting("targetLang", self.combo_tgtlang.get())
            self.plugin.set_setting("captureMethod", self.combo_capture.get())
            self.plugin.set_setting("ocrInterval", self.combo_interval.get())
            self.log(f"Ayarlar DLL'e uygulandı (ocrEnabled={ocr_val}, motor={self.combo_engine.get()}).")
        except Exception as e:
            self.log(f"Ayar uygulama hatası: {e}")

    def _get_coords(self):
        try:
            x = int(self.entry_x.get())
            y = int(self.entry_y.get())
            w = int(self.entry_w.get())
            h = int(self.entry_h.get())
            return x, y, w, h
        except ValueError:
            return 0, 0, 800, 400

    def _set_fullscreen(self):
        sw = self.winfo_screenwidth()
        sh = self.winfo_screenheight()
        self.entry_x.delete(0, "end"); self.entry_x.insert(0, "0")
        self.entry_y.delete(0, "end"); self.entry_y.insert(0, "0")
        self.entry_w.delete(0, "end"); self.entry_w.insert(0, str(sw))
        self.entry_h.delete(0, "end"); self.entry_h.insert(0, str(sh))
        self.log(f"Bölge tüm ekran olarak ayarlandı ({sw}x{sh}).")

    def _open_region_picker(self):
        self.iconify() # Ana pencereyi simge durumuna küçült
        def on_selected(x, y, w, h):
            self.deiconify()
            self.entry_x.delete(0, "end"); self.entry_x.insert(0, str(x))
            self.entry_y.delete(0, "end"); self.entry_y.insert(0, str(y))
            self.entry_w.delete(0, "end"); self.entry_w.insert(0, str(w))
            self.entry_h.delete(0, "end"); self.entry_h.insert(0, str(h))
            self.log(f"Alan seçildi -> X:{x}, Y:{y}, W:{w}, H:{h}")

        self.after(300, lambda: RegionSelector(on_selected))

    def _toggle_overlay(self):
        if self.switch_overlay.get():
            if not self.overlay:
                self.overlay = InGameOverlay(self)
            else:
                self.overlay.deiconify()
            self.log("Şeffaf oyun içi overlay gösterildi.")
        else:
            if self.overlay:
                self.overlay.withdraw()
            self.log("Oyun içi overlay gizlendi.")

    def _launch_game_overlay(self):
        import subprocess
        subprocess.Popen([sys.executable, "-m", "makine_live"], cwd=self.root_dir)
        self.log("🎮 Oyun İçi Overlay Paketi (py -m makine_live) başlatıldı!")

    def _trigger_ocr_only(self):
        x, y, w, h = self._get_coords()
        threading.Thread(target=self._run_ocr_worker, args=(x, y, w, h), daemon=True).start()

    def _run_ocr_worker(self, x, y, w, h):
        t0 = time.time()
        res = self.plugin.capture_and_ocr(x, y, w, h)
        duration = int((time.time() - t0) * 1000)

        self.after(0, lambda: self._update_ocr_display(res, duration))

    def _update_ocr_display(self, text, duration):
        self.txt_ocr.delete("1.0", "end")
        self.txt_ocr.insert("1.0", text)
        if text.strip():
            self.log(f"OCR tamamlandı ({duration}ms) -> {len(text)} karakter okundu.")
        else:
            err = self.plugin.get_last_error()
            msg = f" (Hata: {err})" if err else " (Metin bulunamadı / Değişiklik yok veya RapidOCR DLL eksik)"
            self.log(f"OCR tamamlandı ({duration}ms){msg}")

    def _trigger_ocr_translate(self):
        self._apply_settings_to_dll()
        x, y, w, h = self._get_coords()
        threading.Thread(target=self._run_translate_worker, args=(x, y, w, h), daemon=True).start()

    def _run_translate_worker(self, x, y, w, h):
        t0 = time.time()
        trans = self.plugin.capture_ocr_translate(x, y, w, h)
        raw_ocr = self.plugin.get_last_ocr_text()
        duration = int((time.time() - t0) * 1000)

        self.after(0, lambda: self._update_translate_display(raw_ocr, trans, duration))

    def _update_translate_display(self, raw_ocr, trans, duration):
        if raw_ocr and raw_ocr.strip():
            self.txt_ocr.delete("1.0", "end")
            self.txt_ocr.insert("1.0", raw_ocr)
            self.log(f"📸 [OCR Algılandı] ({len(raw_ocr)} kr.): '{raw_ocr[:45]}...'")
        else:
            self.log(f"⚠️ [OCR] Seçilen alanda metin algılanamadı.")

        if trans and trans.strip():
            self.txt_trans.delete("1.0", "end")
            self.txt_trans.insert("1.0", trans)
            if self.overlay and self.switch_overlay.get():
                self.overlay.update_text(trans)
            self.log(f"🌐 [Çeviri Başarılı] ({duration}ms): '{trans[:60]}...'")
        else:
            if not raw_ocr or not raw_ocr.strip():
                self.log(f"   -> OCR metin bulamadığı için çeviri tetiklenmedi ({duration}ms).")
            else:
                self.log(f"   -> Çeviri yanıtı boş döndü ({duration}ms). Sağdaki 'Bu Metni Çevir' butonuyla API'yi test edin.")

    def _translate_current_ocr_text(self):
        text = self.txt_ocr.get("1.0", "end").strip()
        if not text:
            self.log("⚠️ [Uyarı] Çevrilecek metin yok! Lütfen önce OCR yapın veya kutuya metin yazın.")
            return

        api_key = self.entry_apikey.get().strip()
        engine = self.combo_engine.get()
        src = self.combo_srclang.get()
        tgt = self.combo_tgtlang.get()

        if not api_key:
            self.log("❌ [Hata] API Anahtarı girilmemiş!")
            return

        self.log(f"🚀 [{engine.upper()}] '{text[:30]}...' metni {src}->{tgt} çevriliyor...")

        def worker():
            t0 = time.time()
            try:
                if engine == "deepl":
                    host = "api-free.deepl.com" if ":fx" in api_key else "api.deepl.com"
                    url = f"https://{host}/v2/translate"
                    body = json.dumps({"text": [text], "source_lang": src, "target_lang": tgt}).encode('utf-8')
                    req = urllib.request.Request(url, data=body, headers={
                        "Content-Type": "application/json",
                        "Authorization": f"DeepL-Auth-Key {api_key}"
                    })
                    with urllib.request.urlopen(req) as resp:
                        res = json.loads(resp.read().decode('utf-8'))
                        translated = res.get("translations", [{}])[0].get("text", "")
                elif engine == "google":
                    url = f"https://translation.googleapis.com/language/translate/v2?key={api_key}"
                    body = json.dumps({"q": text, "source": src, "target": tgt, "format": "text"}).encode('utf-8')
                    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
                    with urllib.request.urlopen(req) as resp:
                        res = json.loads(resp.read().decode('utf-8'))
                        translated = res.get("data", {}).get("translations", [{}])[0].get("translatedText", "")
                else: # gpt
                    url = "https://api.openai.com/v1/chat/completions"
                    body = json.dumps({
                        "model": "gpt-4o-mini",
                        "messages": [
                            {"role": "system", "content": f"You are a translator. Translate from {src} to {tgt}. Return ONLY translated text."},
                            {"role": "user", "content": text}
                        ]
                    }).encode('utf-8')
                    req = urllib.request.Request(url, data=body, headers={
                        "Content-Type": "application/json",
                        "Authorization": f"Bearer {api_key}"
                    })
                    with urllib.request.urlopen(req) as resp:
                        res = json.loads(resp.read().decode('utf-8'))
                        translated = res.get("choices", [{}])[0].get("message", {}).get("content", "")

                dur = int((time.time() - t0) * 1000)
                def on_done():
                    self.txt_trans.delete("1.0", "end")
                    self.txt_trans.insert("1.0", translated)
                    if self.overlay and self.switch_overlay.get():
                        self.overlay.update_text(translated)
                    self.log(f"✅ [{engine.upper()} Yanıtı] ({dur}ms): '{translated[:60]}...'")
                self.after(0, on_done)
            except urllib.error.HTTPError as e:
                err_body = e.read().decode('utf-8', errors='replace')
                self.after(0, lambda: self.log(f"❌ [{engine.upper()} HTTP HATA {e.code}] {err_body}"))
            except Exception as e:
                self.after(0, lambda: self.log(f"❌ [{engine.upper()} Hata] {e}"))

        threading.Thread(target=worker, daemon=True).start()

    def _toggle_live_loop(self):
        if not self.is_live_running:
            self.is_live_running = True
            self.btn_live.configure(text="⏹️ Canlı Döngüyü Durdur", fg_color="#ef4444", hover_color="#dc2626")
            self.log("Canlı OCR/Çeviri döngüsü başlatıldı.")
            self.live_thread = threading.Thread(target=self._live_loop_worker, daemon=True)
            self.live_thread.start()
        else:
            self.is_live_running = False
            self.btn_live.configure(text="▶️ Canlı OCR/Çeviri Döngüsünü Başlat", fg_color="#7c3aed", hover_color="#6d28d9")
            self.log("Canlı döngü durduruldu.")

    def _live_loop_worker(self):
        while self.is_live_running:
            x, y, w, h = self._get_coords()
            t0 = time.time()
            trans = self.plugin.capture_ocr_translate(x, y, w, h)
            raw_ocr = self.plugin.get_last_ocr_text()
            duration = int((time.time() - t0) * 1000)

            self.after(0, lambda r=raw_ocr, t=trans, d=duration: self._update_translate_display(r, t, d))

            try:
                interval_ms = int(self.combo_interval.get())
            except ValueError:
                interval_ms = 1000

            time.sleep(max(0.2, interval_ms / 1000.0))

    def destroy(self):
        self.is_live_running = False
        if self.overlay:
            try: self.overlay.destroy()
            except Exception: pass
        self.plugin.shutdown()
        super().destroy()


if __name__ == "__main__":
    app = MakineLiveDevUI()
    app.mainloop()
