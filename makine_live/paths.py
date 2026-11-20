"""
Makine Live'ın bütün dosya yolları tek yerde.

Geliştirmede kök deponun kendisidir; tek dosyalık exe'de PyInstaller'ın açtığı geçici klasördür (sys._MEIPASS).
İkisinde de aynı alt klasörler bulunur: spec/, assets/, resources/ (exe'de DLL ve OCR dosyaları runtime/ altında).
"""

import os
import sys

FROZEN = bool(getattr(sys, "frozen", False))
PACKAGE_DIR = os.path.dirname(os.path.abspath(__file__))
# Kaynak kök: geliştirmede depo, tek dosya exe'de PyInstaller'ın açtığı geçici klasör
ROOT = getattr(sys, "_MEIPASS", os.path.dirname(PACKAGE_DIR))
# makine-live.dll + RapidOcrOnnx.dll + onnxruntime.dll + models/ + resources/
RUNTIME_DIR = os.path.join(ROOT, "runtime") if FROZEN else os.path.join(ROOT, "build", "release")
SPEC_DIR = os.path.join(ROOT, "spec")
ASSETS_DIR = os.path.join(ROOT, "assets")
RESOURCES_DIR = os.path.join(ROOT, "resources")
# Makine Çeviri logosu: her logo / ikonun tek kaynağı (tools/brand_assets.py bundan üretir)
LOGO_PATH = os.path.join(ASSETS_DIR, "logo_white-96.webp")
DESIGN_TOKENS = os.path.join(SPEC_DIR, "design_tokens.json")


def app_data_dir() -> str:
    """%APPDATA%\\MakineLive: oturum, ayarlar, yerel havuz, günlükler."""
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, "MakineLive")


# Eklentinin veri klasörü (settings.cfg, hud.json): exe'de kullanıcı profili, geliştirmede build/test_data
DATA_DIR = app_data_dir() if FROZEN else os.path.join(ROOT, "build", "test_data")


def bundled(*parts: str) -> str:
    """Pakete gömülü (exe) ya da geliştirmede depodaki bir dosya: ilk bulunan; yoksa ''."""
    for p in parts:
        path = os.path.join(ROOT, p)
        if os.path.isfile(path):
            return path
    return ""
