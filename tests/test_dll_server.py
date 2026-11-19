"""
Makine Live — DLL'in sunucu üzerinden çeviri motoru ("makine") için uçtan uca test.

Makine Launcher entegrasyonunda kullanılacak yol: host uygulama kullanıcının jetonunu DLL'e verir
(makine_set_setting("authToken", ...)), DLL ekranı yakalar, OCR yapar ve çeviriyi
{serverUrl}/api/live/translate'ten alır (makineceviri.org, Makine-WEB src/live/translate.ts).

Sunucu burada aynı isteği karşılayan küçük bir sahte sunucudur: jeton (Authorization: Bearer), gövde
({game, text, source_lang, target_lang}) ve yanıt ({success, translated, origin}) gerçek API'yle aynı.
Sunucunun kendisi Makine-WEB'de test edilir (tests/live-api.test.ts).

Çalıştırma (önce derleyin):  py tests/test_dll_server.py
Gerekenler: build/release (DLL + OCR dosyaları).
"""

import ctypes
import ctypes.wintypes
import json
import os
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from _root import ROOT  # noqa: E402
from test_plugin import DLL, load, text  # noqa: E402

TOKEN = "ab" * 32
failures = 0


def check(cond, label):
    global failures
    print(("  ok    " if cond else "  HATA  ") + label)
    if not cond:
        failures += 1


class FakeLive(BaseHTTPRequestHandler):
    """POST /api/live/translate — geçerli jetonla TR[metin], iptal edilmiş jetonla 401."""
    revoked = False
    seen = []

    def log_message(self, format, *args):
        pass

    def _reply(self, status, data):
        raw = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        FakeLive.seen.append((self.path, dict(self.headers), body))
        if self.path != "/api/live/translate":
            return self._reply(404, {"success": False, "error": "Böyle bir uç nokta yok."})
        if FakeLive.revoked or self.headers.get("Authorization") != f"Bearer {TOKEN}":
            return self._reply(401, {"success": False, "error": "Giriş yapmanız gerekiyor.", "code": "auth_required"})
        return self._reply(200, {"success": True, "translated": f"TR[{body.get('text', '')}]", "origin": "deepl",
                                 "status": "pending", "votes": 1, "votes_needed": 3})


def ocr_translate(dll, phrase):
    import tkinter as tk
    root = tk.Tk()
    root.overrideredirect(True)
    root.attributes("-topmost", True)
    root.configure(bg="black")
    root.geometry("1000x110+120+140")
    tk.Label(root, text=phrase, fg="white", bg="black", font=("Segoe UI", 32)).pack(expand=True)
    root.update(); time.sleep(0.4); root.update()
    r = ctypes.wintypes.RECT()
    ctypes.windll.user32.GetWindowRect(root.winfo_id(), ctypes.byref(r))
    dll.makine_capture_ocr_translate.restype = ctypes.c_char_p
    dll.makine_capture_ocr_translate.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    out = ""
    for _ in range(3):
        out = text(dll.makine_capture_ocr_translate(None, r.left, r.top, r.right - r.left, r.bottom - r.top))
        if out.startswith("TR["):
            break
        time.sleep(0.2)
    root.destroy()
    return out


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    server = ThreadingHTTPServer(("127.0.0.1", 0), FakeLive)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        print("[1] DLL, hesap jetonuyla sunucu üzerinden çeviri yapar (kullanıcıda API anahtarı yok)")
        dll = load(DLL)
        data = tempfile.mkdtemp(prefix="makine-live-dll-")
        check(dll.makine_initialize(data.encode()) == 0, "initialize")
        for k, v in {"ocrEnabled": "true", "serverUrl": base, "authToken": TOKEN,
                     "gameSlug": "heavy-rain", "gameName": "Heavy Rain"}.items():
            dll.makine_set_setting(k.encode(), v.encode())
        check(text(dll.makine_get_setting(b"translatorEngine")) == "", "varsayılan motor (makine) kullanılıyor")
        raw = open(os.path.join(data, "settings.cfg"), encoding="utf-8").read()
        check("authToken=dpapi:" in raw and TOKEN not in raw, "jeton diske şifreli yazıldı")
        out = ocr_translate(dll, "Press X to call Jason")
        check(out == "TR[Press X to call Jason]", f"çeviri sunucudan geldi: {out!r}")
        if FakeLive.seen:
            path, _headers, body = FakeLive.seen[-1]
            check(path == "/api/live/translate", f"adres /api/live/translate: {path}")
            check(body.get("game") == "heavy-rain" and body.get("target_lang") == "tr", f"gövde: {body}")

        print("[2] Jeton iptal edilince DLL çeviri yapmaz, ham metni döndürür ve sebebi bildirir")
        FakeLive.revoked = True
        out = ocr_translate(dll, "Where were you last night, Ethan?")
        err = text(dll.makine_get_last_error())
        check(not out.startswith("TR[") and "401" in err, f"iptal sonrası: {out!r} / {err!r}")
        dll.makine_shutdown()
    finally:
        server.shutdown()
    print("\nDLL sunucu testleri geçti." if failures == 0 else f"\n{failures} test başarısız.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
