#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Altyazı web yayını [TEST→C++]: ekrandaki çeviriyi tarayıcıya veren küçük bir yerel HTTP sunucusu.

İki sayfa, aynı akış:
  /      Telefon / tablet ekranı. Monitörde oynayanlar (Makine TV kurulamayan ekranlar) telefonu ekranın altına koyar;
         son satır büyük, öncekiler küçük görünür. Ev ağından açılır: http://<PC>:47712
  /obs   OBS "Tarayıcı" kaynağı: saydam zeminde oyun içi kutuyla aynı görünümde altyazı şeridi. OBS'nin Oyun Yakalama'sı
         yalnızca oyunu alır, kutumuz o görüntüde yoktur; bu kaynak altyazıyı yayına ayrı bir katman olarak ekler.
  /events  Server-Sent Events: {"t":"line","id","text","kind"} | {"t":"clear"} | {"t":"style","size","opacity"}

Sunucu yalnızca açıkken çalışır. Telefon ekranı kapalıyken yalnızca 127.0.0.1'i dinler (OBS aynı bilgisayardadır);
telefon ekranı açılınca ev ağına açılır (Windows ilk seferde güvenlik duvarı izni sorar). Sayfalar yalnızca altyazıyı
gösterir: hesap, jeton ya da sunucu bilgisi taşımaz. Dışarıdan bir şey yüklenmez (CSP: default-src 'self').
"""

from __future__ import annotations

import json
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable, Optional
from urllib.parse import urlsplit

PORT = 47712
PORT_TRIES = 8                 # kapı doluysa sonraki kapılar denenir (OBS adresi panelde gösterilir)
HEARTBEAT_S = 15
HISTORY = 4                    # telefon ekranında son satırın üstünde görünen önceki satırlar


def lan_ip() -> str:
    """Ev ağındaki adres (varsayılan yolun arayüzü). Paket gönderilmez: UDP 'connect' yalnızca yolu seçer."""
    for probe in ("192.168.1.1", "10.255.255.255", "8.8.8.8"):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((probe, 9))
                ip = s.getsockname()[0]
                if ip and not ip.startswith(("127.", "0.", "169.254.")):
                    return ip
        except OSError:
            continue
    try:
        return socket.gethostbyname(socket.gethostname())
    except OSError:
        return "127.0.0.1"


class SubtitleWeb:
    """
    Panel (Tk ana iş parçacığı) send_line / clear / set_style çağırır; tarayıcılar /events'e bağlıdır.
    Her olay bir sürüm numarası artırır, bekleyen bağlantılar koşul değişkeniyle uyanır.
    """

    def __init__(self, logo_path: str = "", on_clients: Optional[Callable[[int], None]] = None):
        self.logo_path = logo_path
        self.on_clients = on_clients
        self._cond = threading.Condition()
        self._version = 0
        self._line: Optional[dict] = None         # ekrandaki satır (yoksa None)
        self._history: list = []                  # telefon için önceki satırlar (en yeni sonda)
        self._style = {"size": 26, "opacity": 78}
        self._server: Optional[ThreadingHTTPServer] = None
        self._lan = False
        self.port = 0
        self.clients = 0
        self.error = ""

    # ── Yaşam döngüsü ──

    @property
    def running(self) -> bool:
        return self._server is not None

    def configure(self, enabled: bool, lan: bool) -> None:
        """Sunucuyu açar / kapatır; telefon ekranı (lan) değişince doğru arayüzde yeniden açar."""
        if not enabled:
            self.stop()
            return
        if self._server is not None and self._lan == lan:
            return
        self.stop()
        host = "0.0.0.0" if lan else "127.0.0.1"
        self.error = ""
        for port in range(PORT, PORT + PORT_TRIES):
            try:
                srv = _Server((host, port), _handler_for(self))
            except OSError as e:
                self.error = str(e)
                continue
            self._server, self._lan, self.port = srv, lan, port
            threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.5}, daemon=True,
                             name="altyazi-web").start()
            return

    def stop(self) -> None:
        srv, self._server = self._server, None
        if srv is not None:
            with self._cond:                      # bekleyen /events bağlantıları kapansın
                self._version += 1
                self._cond.notify_all()
            threading.Thread(target=lambda: (srv.shutdown(), srv.server_close()), daemon=True).start()
        self.port = 0

    # ── Adresler ──

    @property
    def obs_url(self) -> str:
        return f"http://127.0.0.1:{self.port or PORT}/obs"

    @property
    def phone_url(self) -> str:
        return f"http://{lan_ip()}:{self.port or PORT}"

    # ── Panelden gelenler ──

    def send_line(self, line_id: int, text: str, kind: str = "api") -> None:
        with self._cond:
            if self._line and self._line.get("text") and self._line["text"] != text:
                self._history = (self._history + [self._line])[-HISTORY:]
            self._line = {"t": "line", "id": int(line_id), "text": text, "kind": kind}
            self._bump()

    def clear(self) -> None:
        with self._cond:
            if self._line is None:
                return
            self._history = (self._history + [self._line])[-HISTORY:]
            self._line = None
            self._bump()

    def set_style(self, size: int, opacity: int, **extra) -> None:
        """
        Panelin kutu ayarları. extra (OBS sayfası kutuyla aynı görünsün diye): text, band (#rrggbb), font (CSS ailesi),
        outline (px), shadow (bool), width (ekranın %'si), pos ("alt" | "ust"), offset (kenardan %).
        """
        with self._cond:
            style = {"size": int(size), "opacity": int(opacity)}
            for k in ("text", "band", "font", "pos"):
                if isinstance(extra.get(k), str):
                    style[k] = extra[k][:120]
            for k in ("outline", "width", "offset"):
                if isinstance(extra.get(k), (int, float)):
                    style[k] = float(extra[k])
            if "shadow" in extra:
                style["shadow"] = bool(extra["shadow"])
            if style != self._style:
                self._style = style
                self._bump()

    def _bump(self):
        self._version += 1
        self._cond.notify_all()

    def snapshot(self) -> dict:
        with self._cond:
            return self._snapshot()

    def _snapshot(self) -> dict:
        return {"t": "state", "line": self._line, "history": list(self._history), "style": dict(self._style)}

    # ── /events ──

    def _stream(self, write: Callable[[bytes], None], alive: Callable[[], bool]) -> None:
        """Bağlantı açık kaldıkça her değişiklikte tam durumu gönderir (küçük; ayrı olay türü gerekmez)."""
        self._count(+1)
        try:
            with self._cond:
                seen = self._version
                data = self._snapshot()
            write(_sse(data))
            last = time.monotonic()
            while alive() and self._server is not None:
                with self._cond:
                    self._cond.wait_for(lambda: self._version != seen, timeout=1.0)
                    changed = self._version != seen
                    seen = self._version
                    data = self._snapshot() if changed else None
                if data is not None:
                    write(_sse(data))
                    last = time.monotonic()
                elif time.monotonic() - last >= HEARTBEAT_S:
                    write(b"event: ping\ndata: 1\n\n")   # a named event: the page sees it, a comment it would not
                    last = time.monotonic()
        except OSError:
            pass
        finally:
            self._count(-1)

    def _count(self, delta: int):
        with self._cond:
            self.clients = max(0, self.clients + delta)
            n = self.clients
        if self.on_clients:
            try:
                self.on_clients(n)
            except Exception:  # noqa: BLE001 — panel kapanırken geri çağrı hata verebilir
                pass


class _Server(ThreadingHTTPServer):
    # Windows'ta SO_REUSEADDR başka bir sürecin dinlediği kapıyı da almaya izin verir: kapalı tutulur
    allow_reuse_address = False
    daemon_threads = True


def _sse(obj: dict) -> bytes:
    return b"data: " + json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n\n"


_CSP = "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self'"


def _handler_for(web: SubtitleWeb):
    class Handler(BaseHTTPRequestHandler):
        server_version = "MakineLive"
        sys_version = ""
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args):  # noqa: A002 — panel kaydını doldurmasın
            pass

        def _send(self, status: int, body: bytes, ctype: str, extra: Optional[dict] = None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            path = urlsplit(self.path).path.rstrip("/") or "/"
            if path == "/":
                self._send(200, PHONE_HTML.encode("utf-8"), "text/html; charset=utf-8",
                           {"Content-Security-Policy": _CSP})
            elif path == "/obs":
                self._send(200, OBS_HTML.encode("utf-8"), "text/html; charset=utf-8",
                           {"Content-Security-Policy": _CSP})
            elif path == "/logo.webp" and web.logo_path and os.path.isfile(web.logo_path):
                with open(web.logo_path, "rb") as fh:
                    self._send(200, fh.read(), "image/webp", {"Cache-Control": "max-age=86400"})
            elif path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True

                def write(b: bytes):
                    self.wfile.write(b)
                    self.wfile.flush()

                web._stream(write, lambda: True)
            else:
                self._send(404, b"not found", "text/plain; charset=utf-8")

    return Handler


# ─────────────────────────────────────────────────────────────
# Sayfalar (spec/design_tokens.json: koyu zemin, tek vurgu rengi altyazı sarısı; dış kaynak yok)
# ─────────────────────────────────────────────────────────────

PHONE_HTML = r"""<!doctype html>
<html lang="tr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0f1012">
<title>Makine Live altyazı</title>
<style>
  :root { --bg:#0f1012; --surface:#16171a; --line:#2a2c31; --text:#ecebe6; --text2:#a9a8a2; --text3:#86868b;
          --accent:#f2c230; --verified:#3fae72; --fs:34px; }
  * { box-sizing:border-box; }
  html, body { margin:0; height:100%; background:var(--bg); color:var(--text);
               font-family:"Segoe UI Variable Text","Segoe UI",system-ui,-apple-system,Roboto,sans-serif; }
  body { display:flex; flex-direction:column; padding:16px max(16px, env(safe-area-inset-right)) 16px max(16px, env(safe-area-inset-left)); }
  header { display:flex; align-items:center; gap:10px; font-size:15px; color:var(--text2); }
  header img { width:22px; height:22px; }
  header b { color:var(--text); font-weight:600; }
  .state { margin-left:auto; display:flex; align-items:center; gap:8px; font-size:13px; }
  .dot { width:8px; height:8px; border-radius:50%; background:var(--accent); }
  .ok .dot { background:var(--verified); }
  main { flex:1; display:flex; flex-direction:column; justify-content:flex-end; gap:14px; padding:20px 0; overflow:hidden; }
  .old { color:var(--text3); font-size:calc(var(--fs) * .5); line-height:1.35; }
  .old p { margin:0 0 8px; white-space:pre-line; }
  .cur { min-height:calc(var(--fs) * 2.6); display:flex; align-items:flex-end; }
  .cur p { margin:0; min-width:0; overflow-wrap:anywhere; white-space:pre-line; font-size:var(--fs); line-height:1.3; font-weight:600; padding-left:14px;
           border-left:3px solid transparent; transition:opacity .16s ease; }
  .cur p.verified { border-left-color:var(--verified); }
  .cur p.pending { border-left-color:var(--accent); }
  .cur p.out { opacity:0; }
  .hint { color:var(--text3); font-size:14px; }
  footer { display:flex; gap:8px; align-items:center; }
  button { font:inherit; font-size:14px; color:var(--text); background:var(--surface); border:1px solid var(--line);
           border-radius:6px; padding:10px 14px; min-width:44px; }
  button:active { border-color:var(--accent); }
  footer .sp { flex:1; }
  @media (prefers-reduced-motion: reduce) { .cur p { transition:none; } }
</style>
</head>
<body>
<header>
  <img src="/logo.webp" alt="" onerror="this.remove()">
  <b>Makine Live</b>
  <div class="state" id="state"><span class="dot"></span><span id="stateText">Bağlanıyor…</span></div>
</header>
<main>
  <div class="old" id="old"></div>
  <div class="cur" id="cur"><p class="hint" id="hint">Oyunda bir altyazı belirince Türkçesi burada görünür.</p></div>
</main>
<footer>
  <button id="smaller" aria-label="Yazıyı küçült">A−</button>
  <button id="bigger" aria-label="Yazıyı büyüt">A+</button>
  <span class="sp"></span>
  <button id="full">Tam ekran</button>
</footer>
<script>
(function () {
  var cur = document.getElementById('cur'), old = document.getElementById('old');
  var state = document.getElementById('state'), stateText = document.getElementById('stateText');
  var fs = 34;
  try { fs = parseInt(localStorage.getItem('fs') || '34', 10) || 34; } catch (e) {}
  function applyFs() { document.documentElement.style.setProperty('--fs', fs + 'px');
                       try { localStorage.setItem('fs', String(fs)); } catch (e) {} }
  applyFs();
  document.getElementById('smaller').onclick = function () { fs = Math.max(20, fs - 4); applyFs(); };
  document.getElementById('bigger').onclick = function () { fs = Math.min(72, fs + 4); applyFs(); };
  document.getElementById('full').onclick = function () {
    var el = document.documentElement;
    (el.requestFullscreen || el.webkitRequestFullscreen || function () {}).call(el);
    if (navigator.wakeLock) navigator.wakeLock.request('screen').catch(function () {});
  };
  function para(text, cls) { var p = document.createElement('p'); p.textContent = text; if (cls) p.className = cls; return p; }
  var shown = null;
  function render(s) {
    old.textContent = '';
    (s.history || []).forEach(function (h) { old.appendChild(para(h.text)); });
    var line = s.line;
    if (line && shown === line.id) return;
    var prev = cur.firstChild;
    if (prev && !prev.classList.contains('hint')) prev.classList.add('out');
    if (!line) { shown = null; return; }
    shown = line.id;
    cur.textContent = '';
    cur.appendChild(para(line.text, line.kind === 'verified' ? 'verified' : (line.kind === 'pending' ? 'pending' : '')));
  }
  var es = null, lastSeen = 0;
  function seen() { lastSeen = Date.now(); }
  function connect() {
    if (es) es.close();
    seen();
    es = new EventSource('/events');
    es.onopen = function () { seen(); state.className = 'state ok'; stateText.textContent = 'Bağlı'; };
    es.onerror = function () { state.className = 'state'; stateText.textContent = 'Bilgisayar bekleniyor'; };
    es.onmessage = function (e) { seen(); try { render(JSON.parse(e.data)); } catch (x) {} };
    es.addEventListener('ping', seen);
  }
  // A connection that died without a word (the tablet's Wi-Fi slept, the network changed) never errors and
  // the page would stay on old lines: the computer pings every 15 s, so 40 s of silence opens a new one
  setInterval(function () {
    if (Date.now() - lastSeen > 40000) { state.className = 'state'; stateText.textContent = 'Yeniden bağlanıyor…'; connect(); }
  }, 5000);
  document.addEventListener('visibilitychange', function () { if (!document.hidden && Date.now() - lastSeen > 20000) connect(); });
  window.addEventListener('online', connect);
  connect();
})();
</script>
</body>
</html>
"""

OBS_HTML = r"""<!doctype html>
<html lang="tr">
<head>
<meta charset="utf-8">
<title>Makine Live · OBS</title>
<style>
  html, body { margin:0; height:100%; background:transparent; overflow:hidden; }
  body { font-family:"Segoe UI Variable Display","Segoe UI",system-ui,sans-serif; }
  .wrap { position:absolute; left:0; right:0; display:flex; justify-content:center; padding:0 4vw; }
  .wrap.alt { bottom:8vh; }
  .wrap.ust { top:6vh; }
  @media (prefers-reduced-motion: reduce) { .band { transition:none; } }
  .band { max-width:62vw; white-space:pre-line; padding:.46em .85em; border-radius:10px; color:#fff; font-weight:600; line-height:1.3;
          text-align:center; border-left:3px solid transparent; opacity:0; transform:translateY(2px);
          transition:opacity .16s cubic-bezier(.2,.7,.2,1), transform .16s cubic-bezier(.2,.7,.2,1);
          text-shadow:-1px -1px 0 #000, 1px -1px 0 #000, -1px 1px 0 #000, 1px 1px 0 #000;
          box-shadow:0 6px 18px rgba(0,0,0,.35); }
  .band.on { opacity:1; transform:none; }
  .band.verified { border-left-color:#3fae72; }
  .band.pending { border-left-color:#f2c230; }
</style>
</head>
<body>
<div class="wrap alt" id="wrap"><div class="band" id="band"></div></div>
<script>
(function () {
  // Adres parametreleri (isteğe bağlı): ?size=32&opacity=78&pos=ust  — verilmezse paneldeki ayarlar kullanılır
  var q = {};
  location.search.replace(/^\?/, '').split('&').forEach(function (kv) {
    var p = kv.split('='); if (p[0]) q[decodeURIComponent(p[0])] = decodeURIComponent(p[1] || '');
  });
  var band = document.getElementById('band'), wrap = document.getElementById('wrap');
  var shown = null;
  function rgba(hex, a) {
    var m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '') || [0, '0f', '10', '12'];
    return 'rgba(' + parseInt(m[1], 16) + ',' + parseInt(m[2], 16) + ',' + parseInt(m[3], 16) + ',' + a + ')';
  }
  function style(s) {
    var size = parseInt(q.size || s.size || 26, 10);
    var op = parseInt(q.opacity !== undefined ? q.opacity : (s.opacity !== undefined ? s.opacity : 78), 10) / 100;
    var pos = q.pos || s.pos || 'alt', off = s.offset || (pos === 'ust' ? 6 : 8);
    // Panelin kutusu 1080p ekrana göre ölçülür; OBS kaynağının genişliği farklıysa aynı oranda büyür
    band.style.fontSize = 'calc(' + size + 'px * 100vh / 1080)';
    band.style.background = rgba(s.band, Math.max(0, Math.min(1, op)));
    band.style.color = s.text || '#fff';
    if (s.font) band.style.fontFamily = s.font;
    band.style.maxWidth = (s.width || 62) + 'vw';
    var o = s.outline === undefined ? 1 : s.outline;
    band.style.textShadow = o > 0 ? [[-1, -1], [1, -1], [-1, 1], [1, 1]].map(function (d) {
      return (d[0] * o) + 'px ' + (d[1] * o) + 'px 0 #000'; }).join(',') : 'none';
    band.style.boxShadow = s.shadow === false || op === 0 ? 'none' : '0 6px 18px rgba(0,0,0,.35)';
    wrap.className = 'wrap ' + (pos === 'ust' ? 'ust' : 'alt');
    wrap.style.top = pos === 'ust' ? Math.max(2, off - 2) + 'vh' : '';
    wrap.style.bottom = pos === 'ust' ? '' : off + 'vh';
  }
  function render(s) {
    style(s.style || {});
    var line = s.line;
    if (!line) { band.classList.remove('on'); shown = null; return; }
    if (shown === line.id) return;
    shown = line.id;
    band.textContent = line.text;
    band.className = 'band on' + (line.kind === 'verified' ? ' verified' : (line.kind === 'pending' ? ' pending' : ''));
  }
  var es = null, lastSeen = 0;
  function seen() { lastSeen = Date.now(); }
  function connect() {
    if (es) es.close();
    seen();
    es = new EventSource('/events');
    es.onopen = seen;
    es.onmessage = function (e) { seen(); try { render(JSON.parse(e.data)); } catch (x) {} };
    es.onerror = function () { band.classList.remove('on'); shown = null; };
    es.addEventListener('ping', seen);
  }
  // A silently dead connection (no error, no lines): 40 s without even a ping opens a new one
  setInterval(function () { if (Date.now() - lastSeen > 40000) connect(); }, 5000);
  connect();
})();
</script>
</body>
</html>
"""


if __name__ == "__main__":
    import sys
    from makine_live.paths import LOGO_PATH
    web = SubtitleWeb(LOGO_PATH)
    web.configure(True, lan="--lan" in sys.argv)
    print("OBS:", web.obs_url)
    print("Telefon:", web.phone_url if "--lan" in sys.argv else "(kapalı; --lan ile açın)")
    lines = ["Örnek altyazı: onaylı çeviri yeşil şeritle gelir.", "Havuzda bekleyen çeviri sarı şeritle gelir.",
             "Yeni DeepL çevirisi şeritsiz gelir."]
    i = 0
    try:
        while True:
            web.send_line(i, lines[i % len(lines)], ("verified", "pending", "api")[i % 3])
            i += 1
            time.sleep(2.5)
            web.clear()
            time.sleep(0.8)
    except KeyboardInterrupt:
        web.stop()
