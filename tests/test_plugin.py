"""
Makine Live — makine-live.dll C ABI duman testi.

Çalıştırma (önce derleyin):  py tests/test_plugin.py

Doğrulananlar:
  - get_info / initialize / is_ready / shutdown
  - Türkçe karakterli (UTF-8) veri klasörü
  - ayarların kalıcılığı; apiKey'in diske DPAPI ile şifreli yazılması
  - ekrana çizilen bilinen bir metnin yakalanıp OCR ile okunması (DXGI/GDI)
  - OCR çalışma dosyaları eksikken initialize'ın hata kodu + açıklama döndürmesi
"""

import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import Structure, c_bool, c_char_p, c_int, c_uint32, c_void_p

from _root import ROOT  # noqa: E402
RELEASE = os.path.join(ROOT, "build", "release")
DLL = os.path.join(RELEASE, "makine-live.dll")

# include/makine/plugin/plugin_types.h
MAKINE_OK = 0
MAKINE_ERR_INIT_FAILED = 1
MAKINE_ERR_INVALID_PARAM = 3


class MakinePluginInfo(Structure):
    _fields_ = [("id", c_char_p), ("name", c_char_p), ("version", c_char_p), ("apiVersion", c_uint32)]


def load(path):
    dll = ctypes.CDLL(path)
    dll.makine_get_info.restype = MakinePluginInfo
    dll.makine_initialize.restype = c_int
    dll.makine_initialize.argtypes = [c_char_p]
    dll.makine_is_ready.restype = c_bool
    dll.makine_get_last_error.restype = c_char_p
    dll.makine_get_setting.restype = c_char_p
    dll.makine_get_setting.argtypes = [c_char_p]
    dll.makine_set_setting.argtypes = [c_char_p, c_char_p]
    dll.makine_capture_and_ocr.restype = c_char_p
    dll.makine_capture_and_ocr.argtypes = [c_void_p, c_int, c_int, c_int, c_int]
    dll.makine_get_last_ocr_text.restype = c_char_p
    return dll


failures = 0


def check(cond, label):
    global failures
    print(("  ok    " if cond else "  HATA  ") + label)
    if not cond:
        failures += 1


def text(b):
    return (b or b"").decode("utf-8", errors="replace")


def ocr_known_text(dll):
    """Ekrana büyük, net bir satır çizip o bölgeyi OCR ile okur."""
    import tkinter as tk

    phrase = "Press X to call Jason"
    root = tk.Tk()
    root.overrideredirect(True)
    root.attributes("-topmost", True)
    root.configure(bg="black")
    w, h, x, y = 900, 120, 120, 120
    root.geometry(f"{w}x{h}+{x}+{y}")
    tk.Label(root, text=phrase, fg="white", bg="black", font=("Segoe UI", 36)).pack(expand=True)
    root.update()
    time.sleep(0.4)
    root.update()

    # Pencerenin gerçek ekran konumu (DPI ölçeklemesinden bağımsız olarak)
    rect = ctypes.wintypes.RECT()
    ctypes.windll.user32.GetWindowRect(root.winfo_id(), ctypes.byref(rect))
    region = (rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top)
    result = ""
    for _ in range(3):
        result = text(dll.makine_capture_and_ocr(None, *region))
        if "Jason" in result:
            break
        time.sleep(0.2)
    # Same pixels again → OCR is skipped and the previous text comes back fast
    t0 = time.perf_counter()
    again = text(dll.makine_capture_and_ocr(None, *region))
    fast_ms = (time.perf_counter() - t0) * 1000
    root.destroy()
    return phrase, result, again, fast_ms


def main_test():
    if not os.path.isfile(DLL):
        print("build/release/makine-live.dll yok — önce derleyin.")
        return 1
    dll = load(DLL)

    print("[1] Bilgi")
    info = dll.makine_get_info()
    check(text(info.id) == "com.makineceviri.live" and info.apiVersion == 1, f"{text(info.name)} {text(info.version)}")

    print("[2] Başlatma (Türkçe karakterli veri klasörü)")
    check(dll.makine_initialize(None) == MAKINE_ERR_INVALID_PARAM, "null dataPath → INVALID_PARAM")
    data_dir = tempfile.mkdtemp(prefix="makine-live-Çeviri-ığüş-")
    rc = dll.makine_initialize(data_dir.encode("utf-8"))
    check(rc == MAKINE_OK, f"initialize → {rc} {text(dll.makine_get_last_error())}")
    check(dll.makine_is_ready(), "is_ready")

    print("[3] Ayarlar ve DPAPI")
    dll.makine_set_setting(b"translatorEngine", b"deepl")
    dll.makine_set_setting(b"apiKey", b"test-anahtar-123:fx")
    dll.makine_set_setting(b"note", "satır1\nsatır2 = ğ".encode("utf-8"))
    cfg = os.path.join(data_dir, "settings.cfg")
    raw = open(cfg, encoding="utf-8").read()
    check("apiKey=dpapi:" in raw and "test-anahtar-123" not in raw, "apiKey diske şifreli yazıldı")
    dll.makine_shutdown()
    check(not dll.makine_is_ready(), "shutdown sonrası hazır değil")
    check(dll.makine_initialize(data_dir.encode("utf-8")) == MAKINE_OK, "yeniden başlatma")
    check(text(dll.makine_get_setting(b"apiKey")) == "test-anahtar-123:fx", "apiKey geri okundu")
    check(text(dll.makine_get_setting(b"note")) == "satır1\nsatır2 = ğ", "çok satırlı UTF-8 değer korundu")
    check(text(dll.makine_get_setting(b"translatorEngine")) == "deepl", "motor ayarı korundu")

    print("[4] Yakalama + OCR")
    phrase, result, again, fast_ms = ocr_known_text(dll)
    check("Jason" in result, f"OCR: {result!r}")
    check(again == result and fast_ms < 30, f"değişmeyen kare OCR'sız döndü ({fast_ms:.1f} ms)")
    check(text(dll.makine_get_last_ocr_text()) == result, "get_last_ocr_text aynı metni döndürdü")
    empty = text(dll.makine_capture_and_ocr(None, 0, 0, 0, 0))
    check(empty == "" and text(dll.makine_get_last_error()) != "", f"geçersiz bölge → boş metin + hata: {text(dll.makine_get_last_error())!r}")

    dll.makine_shutdown()
    shutil.rmtree(data_dir, ignore_errors=True)

    print("[5] OCR dosyaları eksik paket")
    res = subprocess.run([sys.executable, __file__, "--missing-runtime"], capture_output=True, text=True, encoding="utf-8")
    print(res.stdout.rstrip())
    check(res.returncode == 0, "eksik çalışma dosyalarında anlamlı hata")

    print("\nTüm DLL testleri geçti." if failures == 0 else f"\n{failures} test başarısız.")
    return 1 if failures else 0


def missing_runtime_test():
    """Yalnızca DLL'in kopyalandığı bir klasörde initialize çağrılır (ayrı süreçte)."""
    tmp = tempfile.mkdtemp(prefix="makine-live-bare-")
    try:
        shutil.copy(DLL, tmp)
        dll = load(os.path.join(tmp, "makine-live.dll"))
        rc = dll.makine_initialize(tmp.encode("utf-8"))
        err = text(dll.makine_get_last_error())
        print(f"        initialize → {rc}, hata: {err!r}")
        ok = rc == MAKINE_ERR_INIT_FAILED and "RapidOcrOnnx" in err and not dll.makine_is_ready()
        del dll
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    import ctypes.wintypes  # noqa: F401  (ocr_known_text)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if "--missing-runtime" in sys.argv:
        sys.exit(missing_runtime_test())
    sys.exit(main_test())
