"""
Makine Live'ı sunucusuz dener: bilgisayarda küçük bir sahte sunucu açar ve uygulamayı ona bağlar.
makineceviri.org'a hiçbir istek gitmez; arayüz, altyazı ayarları, Discord'da gösterme, bağış penceresi, duyurular
ve yerel havuz sunucu kapalıyken de denenebilir.

    test.cmd                      çift tıklayın (kökte)
    py tools/offline.py           en yeni kod: dist/MakineLive.exe kaynaklardan yeniyse o, değilse py -m makine_live
    py tools/offline.py --exe     dist/MakineLive.exe ile (yüklemeden önce son kontrol)
    py tools/offline.py --kaynak  py -m makine_live ile

Sahte sunucu:
- Giriş: "Makine Çeviri hesabıyla giriş yap" tarayıcıda yerel bir onay sayfası açar ve kendiliğinden onaylanır.
  "Ekip hesabıyla giriş" her kullanıcı adı / şifreyi kabul eder (yönetici).
- Çeviri: örnek repliklerden (spec/sample_quotes.json) biri okunursa Türkçesi, değilse "[Test] <satır>" döner.
  Bilgisayarınıza daha önce indirdiğiniz onaylı çeviriler (%APPDATA%\\MakineLive\\pool) test klasörüne kopyalanır,
  onlar gerçek Türkçeleriyle gelir.
- Açılışta bir test duyurusu gösterilir (kapatınca bir daha gelmez).

Test, gerçek oturumunuza ve ayarlarınıza dokunmaz: veriler build/offline/ altında durur (her açılışta bağış
penceresinin yeniden görünmesi için --temiz).
"""

import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from makine_live.net.pool_client import compact_pool_key  # noqa: E402  (havuz anahtarıyla aynı kural)

OFFLINE = os.path.join(ROOT, "build", "offline")
APPDATA = os.path.join(OFFLINE, "appdata")
POOL = os.path.join(APPDATA, "MakineLive", "pool")
EXE = os.path.join(ROOT, "dist", "MakineLive.exe")
REAL_POOL = os.path.join(os.environ.get("APPDATA", ""), "MakineLive", "pool")

TOKEN = "0" * 64
ANNOUNCEMENT = {
    "id": 1, "kind": "info", "kind_label": "Test", "title": "Sunucusuz test modu",
    "author": "Makine Live", "created_at": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()),
    "body": "Bu oturum bilgisayardaki sahte sunucuya bağlı; makineceviri.org'a istek gitmiyor. Çeviriler örnek "
            "repliklerden ya da bilgisayarınızdaki onaylı çevirilerden gelir, diğer satırlar \"[Test]\" ile görünür.",
}


def load_quotes() -> dict:
    with open(os.path.join(ROOT, "spec", "sample_quotes.json"), encoding="utf-8") as fh:
        return {compact_pool_key(q["source"]): q["tr"] for q in json.load(fh)["quotes"]}


def say(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


class State:
    def __init__(self):
        self.quotes = load_quotes()
        self.approved = set()        # onay sayfası açılan giriş kodları
        self.started = {}            # cihaz → kodun verildiği an
        self.read = set()            # kapatılan duyurular
        self.lines = 0


def pool_file(slug):
    path = os.path.join(POOL, f"{slug}.json")
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def make_handler(st: State, port_ref: list):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"        # uygulama bağlantıyı açık tutar (keep-alive)

        def log_message(self, format, *args):  # noqa: A002 — istek başına satır yazılmaz
            pass

        def _send(self, code, data, ctype="application/json; charset=utf-8"):
            body = data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                return json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                return {}

        def _authed(self):
            return self.headers.get("Authorization", "") == f"Bearer {TOKEN}"

        def do_GET(self):
            self._route("GET", {})

        def do_POST(self):
            self._route("POST", self._body())

        def _route(self, method, body):
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            path, action = u.path.rstrip("/"), q.get("action", "")

            if path == "/onay":                       # tarayıcıda açılan giriş sayfası
                st.approved.add(q.get("kod", ""))
                say(f"giriş  kod {q.get('kod', '')} onaylandı")
                page = ("<!doctype html><meta charset=utf-8><title>Makine Live test</title>"
                        "<body style='background:#111214;color:#e8e8ea;font:16px Segoe UI,sans-serif;"
                        "display:grid;place-items:center;height:90vh'><div><h2 style='color:#FCCD66'>"
                        "Test girişi onaylandı</h2><p>Uygulamaya dönebilirsiniz; bu sayfayı kapatın.</p></div>")
                return self._send(200, page.encode("utf-8"), "text/html; charset=utf-8")

            if path == "/api/live/auth":
                if action == "device_start":
                    code = f"{random.randint(1000, 9999)}-{random.randint(1000, 9999)}"
                    dev = f"dev-{code}"
                    st.started[dev] = (code, time.time())
                    port = port_ref[0]
                    return self._send(200, {"success": True, "device": dev, "code": code, "expires_in": 600,
                                            "interval": 2, "url": f"http://127.0.0.1:{port}/onay?kod={code}"})
                if action == "device_poll":
                    code, t0 = st.started.get(q.get("device", ""), ("", 0))
                    if code in st.approved or (t0 and time.time() - t0 > 8):   # sayfa açılmasa da 8 sn sonra
                        return self._send(200, {"success": True, "status": "approved", "token": TOKEN,
                                                "user": "test-oyuncu", "role": "player"})
                    return self._send(200, {"success": True, "status": "pending"})
                if action == "app_login":
                    say(f"giriş  ekip hesabı: {body.get('username', '')}")
                    return self._send(200, {"success": True, "token": TOKEN,
                                            "user": body.get("username") or "test", "role": "admin"})
                if action == "app_me":
                    if not self._authed():
                        return self._send(401, {"success": False, "error": "Giriş gerekli"})
                    return self._send(200, {"success": True, "user": "test-oyuncu", "role": "player"})
                if action == "app_logout":
                    return self._send(200, {"success": True})

            if not self._authed() and not path.startswith("/api/live/releases"):
                return self._send(401, {"success": False, "error": "Giriş gerekli"})

            if path == "/api/live/app":
                if action == "status":
                    anns = [ANNOUNCEMENT] if ANNOUNCEMENT["id"] not in st.read else []
                    return self._send(200, {"success": True, "maintenance": {"on": False},
                                            "update": {"available": False, "mandatory": False},
                                            "announcements": anns})
                if action == "ann_read":
                    st.read.add(int(body.get("id") or 0))
                    return self._send(200, {"success": True})

            if path == "/api/live/translate":
                time.sleep(0.08)                      # gerçek sunucunun havuz yanıtı kadar
                text = str(body.get("text", ""))
                tr = st.quotes.get(compact_pool_key(text))
                st.lines += 1
                say(f"çeviri {'örnek  ' if tr else 'test   '} {text[:70]}")
                return self._send(200, {"success": True, "translated": tr or f"[Test] {text}",
                                        "origin": "pool" if tr else "deepl", "status": "verified" if tr else "pending",
                                        "server_ms": 3, "votes": 1, "votes_needed": 3})

            if path == "/api/live/pool":
                if action == "hits":
                    return self._send(200, {"success": True})
                if action == "sync":
                    slug = q.get("game", "")
                    data = pool_file(slug) or {"rev": "r0", "name": "", "items": []}
                    rev, items = data.get("rev") or "r0", data.get("items") or []
                    if q.get("head"):
                        return self._send(200, {"success": True, "count": len(items), "rev": rev,
                                                "name": data.get("name", "")})
                    if q.get("rev") == rev:
                        return self._send(200, {"success": True, "unchanged": True, "rev": rev})
                    return self._send(200, {"success": True, "rev": rev, "count": len(items),
                                            "name": data.get("name", ""), "items": items})

            if path == "/api/live/releases":
                return self._send(200, {"success": True, "latest": None})

            say(f"bilinmeyen istek: {method} {self.path}")
            return self._send(404, {"success": False, "error": "Test sunucusunda yok"})

    return Handler


def copy_real_pool():
    """Daha önce indirilen onaylı çeviriler (gerçek klasöre dokunulmaz, yalnızca kopyalanır)."""
    if not os.path.isdir(REAL_POOL):
        return 0
    os.makedirs(POOL, exist_ok=True)
    n = 0
    for name in os.listdir(REAL_POOL):
        if name.endswith(".json"):
            shutil.copy2(os.path.join(REAL_POOL, name), os.path.join(POOL, name))
            n += 1
    return n


def newest_source() -> float:
    return max(os.path.getmtime(os.path.join(d, f)) for d, _, fs in os.walk(os.path.join(ROOT, "makine_live"))
               for f in fs if f.endswith(".py"))


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = sys.argv[1:]
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0
    if "--temiz" in args:
        shutil.rmtree(OFFLINE, ignore_errors=True)
        dev_prefs = os.path.join(ROOT, "build", "test_data", "hud.json")   # py -m makine_live'ın ayarları
        try:
            with open(dev_prefs, encoding="utf-8") as fh:
                p = json.load(fh)
            p["donate_seen"] = False
            with open(dev_prefs, "w", encoding="utf-8") as fh:
                json.dump(p, fh)
        except (OSError, ValueError):
            pass
    os.makedirs(APPDATA, exist_ok=True)
    pools = copy_real_pool()

    st, port_ref = State(), [0]
    server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(st, port_ref))
    port = port_ref[0] = int(server.server_address[1])
    threading.Thread(target=server.serve_forever, daemon=True).start()

    use_exe = "--exe" in args or ("--kaynak" not in args and os.path.isfile(EXE)
                                  and os.path.getmtime(EXE) > newest_source())
    if use_exe and not os.path.isfile(EXE):
        print("dist\\MakineLive.exe yok. Önce build.cmd ile derleyin ya da --kaynak kullanın.")
        return 1
    cmd = [EXE] if use_exe else [sys.executable, "-m", "makine_live"]
    env = dict(os.environ, MAKINE_LIVE_SERVER=f"http://127.0.0.1:{port}", APPDATA=APPDATA)

    print("Makine Live · sunucusuz test")
    print(f"  sahte sunucu   http://127.0.0.1:{port}  (makineceviri.org'a istek gitmez)")
    print(f"  uygulama       {'dist/MakineLive.exe' if use_exe else 'py -m makine_live (kaynak kod)'}")
    print(f"  veriler        {os.path.relpath(APPDATA, ROOT)}  (gerçek oturumunuza dokunulmaz)")
    print(f"  onaylı havuz   {pools} oyun kopyalandı" if pools else "  onaylı havuz   bilgisayarda indirilmiş havuz yok")
    print("  Giriş: \"Makine Çeviri hesabıyla giriş yap\" (kendiliğinden onaylanır) ya da ekip hesabıyla herhangi bir ad.")
    print("  Discord için: Discord açıkken bir oyun başlatın, F9 ile okumayı açın.")
    print("  Uygulama açıksa önce kapatın (aynı anda tek Makine Live çalışır). Uygulamayı kapatınca test biter.\n")
    rc = subprocess.call(cmd, cwd=ROOT, env=env)
    server.shutdown()
    say(f"test bitti · sahte sunucuya giden çeviri isteği: {st.lines}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
