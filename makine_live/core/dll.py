# -*- coding: utf-8 -*-
"""makine-live.dll'in C ABI'si (docs/MIMARI.md §3.1) için ctypes sarmalayıcısı."""

import os
import threading
from ctypes import CDLL, Structure, c_bool, c_char_p, c_int, c_uint32, c_void_p


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
        self._lock = threading.Lock()
        self.load()

    def load(self):
        if not os.path.exists(self.dll_path):
            return False
        try:
            self.dll = CDLL(self.dll_path)
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
        except Exception:
            self.is_loaded = False
            return False

    def initialize(self, data_path: str) -> int:
        with self._lock:
            if not self.is_loaded:
                return -2
            os.makedirs(data_path, exist_ok=True)
            return self.dll.makine_initialize(data_path.encode('utf-8'))

    def last_error(self) -> str:
        with self._lock:
            if not self.is_loaded:
                return "DLL yüklenemedi"
            res = self.dll.makine_get_last_error()
            return res.decode('utf-8', errors='replace') if res else ""

    def shutdown(self):
        with self._lock:
            if self.is_loaded and self.dll:
                self.dll.makine_shutdown()

    def set_setting(self, key: str, val: str):
        with self._lock:
            if self.is_loaded:
                self.dll.makine_set_setting(key.encode('utf-8'), val.encode('utf-8'))

    def get_setting(self, key: str) -> str:
        with self._lock:
            if not self.is_loaded:
                return ""
            res = self.dll.makine_get_setting(key.encode('utf-8'))
            return res.decode('utf-8', errors='replace') if res else ""

    def capture_and_ocr(self, x: int, y: int, w: int, h: int, hwnd=None) -> str:
        """hwnd yoksa ekran koordinatı (DXGI); varsa o pencerenin istemci koordinatı ve pencerenin kendi içeriği
        (üstü kapalı olsa da: konsol modunda Remote Play / yakalama penceresi)."""
        with self._lock:
            if not self.is_loaded:
                return ""
            res = self.dll.makine_capture_and_ocr(hwnd, x, y, w, h)
            return res.decode('utf-8', errors='replace') if res else ""
