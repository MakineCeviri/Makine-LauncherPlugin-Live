"""
Makine Live — sunucu istemcisi (TEST HARNESS / kapalı beta uygulaması)

Her şey makineceviri.org'daki Makine Live API'si üzerinden yürür (/api/live/*, Makine-WEB src/live/):
  - Giriş:  Makine Çeviri hesabıyla: auth?action=device_start → kod + bağlantı (makineceviri.org/live/baglan/),
            kullanıcı tarayıcıda onaylar, device_poll uzun ömürlü jetonu bir kez verir.
            Ekip hesapları kullanıcı adı + şifreyle: auth?action=app_login. Jeton DPAPI ile şifreli saklanır.
  - Çeviri: translate → sunucu havuza bakar, yoksa KENDİ DeepL anahtarıyla bir kez çevirir ve havuza yazar.
            Kullanıcının API anahtarı gerekmez; aynı satırı ikinci kez okuyan herkes aynı çeviriyi alır.
Bağlantılar keep-alive tutulur (TLS el sıkışması istek başına değil, bir kez).

İleride aynı akış Makine Launcher'da C++ DLL'den yürüyecek (translatorEngine=makine, authToken ayarı).
"""

import ctypes
import ctypes.wintypes
import gzip
import hashlib
import http.client
import json
import os
import re
import ssl
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from typing import Dict, Optional, Tuple

SERVER_URL = os.environ.get("MAKINE_LIVE_SERVER", "https://makineceviri.org").rstrip("/")
USER_AGENT = "MakineLive-App/0.3"

# ── Canonical dialogue normalization (spec v2) ──
# Shared with the server (Makine-WEB src/live/text.ts) and the future C++ port. Test vectors: spec/normalization_vectors.json
#   1. trim  2. lowercase  3. collapse whitespace  4. strip leading quotes/dashes  5. strip trailing punctuation  6. trim
# "Whitespace" is ASCII whitespace (space \t \n \r \f \v) plus NBSP (U+00A0).
_WS = " \t\n\r\f\v "
_WS_RUN = re.compile(f"[{_WS}]+")
_LEADING = re.compile("^[" + re.escape("\"'`“”‘’«»([-–—…") + _WS + "]+")
_TRAILING = re.compile("[" + re.escape(".,!?:;\"'`~_-–—…“”‘’«»)]") + _WS + "]+$")


def normalize_dialogue_text(text: str) -> str:
    """Metni havuz anahtarı için kanonik biçime getirir (spec v2)."""
    s = text.strip(_WS).lower()
    s = _WS_RUN.sub(" ", s)
    s = _LEADING.sub("", s)
    s = _TRAILING.sub("", s)
    return s.strip(_WS)


def compact_pool_key(text: str) -> str:
    """Havuz anahtarı: normalize edilmiş metnin yalnızca harf ve rakamları (sunucudaki dialogue_key ile aynı).
    OCR'ın boşluk ve noktalama hataları ("Shaun?I" / "Shaun. I") anahtarı değiştirmez."""
    return "".join(ch for ch in normalize_dialogue_text(text) if unicodedata.category(ch)[0] in "LN")


# ─────────────────────────────────────────────────────────────
# Kalıcı HTTP(S) bağlantıları
# ─────────────────────────────────────────────────────────────

class HttpPool:
    """
    Host başına yeniden kullanılan bağlantılar (keep-alive). urllib her istekte yeni TCP + TLS açar
    (yeni TLS el sıkışması ~100–250 ms); açık bağlantıyla aynı istek ~30 ms.
    Bir bağlantıyı aynı anda tek thread kullanır. http:// yalnızca yerel testler içindir.
    """
    MAX_IDLE = 4

    def __init__(self):
        self._idle: Dict[Tuple[str, str], list] = {}
        self._lock = threading.Lock()
        self._ctx = ssl.create_default_context()

    def _new(self, scheme: str, host: str, timeout: float):
        if scheme == "http":
            return http.client.HTTPConnection(host, timeout=timeout)
        return http.client.HTTPSConnection(host, timeout=timeout, context=self._ctx)

    def _take(self, key, timeout: float):
        with self._lock:
            idle = self._idle.get(key)
            if idle:
                conn = idle.pop()
                conn.timeout = timeout
                if conn.sock is not None:
                    conn.sock.settimeout(timeout)
                return conn
        return self._new(key[0], key[1], timeout)

    def _give(self, key, conn):
        with self._lock:
            idle = self._idle.setdefault(key, [])
            if len(idle) < self.MAX_IDLE:
                idle.append(conn)
                return
        conn.close()

    def request(self, method: str, url: str, body: bytes = None, headers: dict = None,
                timeout: float = 3.0) -> Tuple[int, bytes]:
        """(status, gövde) döndürür; ağ hatasında OSError fırlatır."""
        u = urllib.parse.urlsplit(url)
        key = (u.scheme or "https", u.netloc)
        path = (u.path or "/") + (f"?{u.query}" if u.query else "")
        for attempt in (1, 2):
            conn = self._take(key, timeout)
            reused = conn.sock is not None
            try:
                conn.request(method, path, body=body, headers=headers or {})
                resp = conn.getresponse()
                data = resp.read()
                if (resp.getheader("Content-Encoding") or "").lower() == "gzip":
                    data = gzip.decompress(data)
            except (http.client.HTTPException, OSError) as e:
                conn.close()
                # Sunucunun kapattığı boşta bağlantı: bir kez yeni bağlantıyla dene
                if reused and attempt == 1:
                    continue
                if isinstance(e, OSError):
                    raise
                raise OSError(str(e)) from e
            if resp.will_close:
                conn.close()
            else:
                self._give(key, conn)
            return resp.status, data
        raise OSError("unreachable")

    def warm(self, url: str, timeout: float = 3.0):
        """Bağlantıyı (TLS el sıkışması dahil) önceden kur; ilk istek beklemesin. Arka planda çağırın."""
        u = urllib.parse.urlsplit(url)
        key = (u.scheme or "https", u.netloc)
        with self._lock:
            if self._idle.get(key):
                return
        conn = self._new(key[0], key[1], timeout)
        try:
            conn.connect()
        except OSError:
            conn.close()
            return
        self._give(key, conn)


HTTP = HttpPool()


# ─────────────────────────────────────────────────────────────
# Oturum deposu: jeton Windows DPAPI ile (geçerli kullanıcıya bağlı) şifrelenir
# ─────────────────────────────────────────────────────────────

class _Blob(ctypes.Structure):
    _fields_ = [("cbData", ctypes.wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def _dpapi(data: bytes, protect: bool) -> bytes:
    crypt32 = ctypes.windll.crypt32
    kernel32 = ctypes.windll.kernel32
    buf = ctypes.create_string_buffer(data, len(data))
    src = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    out = _Blob()
    fn = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
    # (pDataIn, szDataDescr, pOptionalEntropy, pvReserved, pPromptStruct, dwFlags=UI_FORBIDDEN, pDataOut)
    if not fn(ctypes.byref(src), "Makine Live" if protect else None, None, None, None, 0x1, ctypes.byref(out)):
        raise OSError("DPAPI işlemi başarısız")
    try:
        return ctypes.string_at(out.pbData, out.cbData)
    finally:
        kernel32.LocalFree(out.pbData)


def app_data_dir() -> str:
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, "MakineLive")


class SessionStore:
    """%APPDATA%\\MakineLive\\session.dat: {"token", "user", "server"} DPAPI ile şifreli."""

    def __init__(self, path: Optional[str] = None):
        self.path = path or os.path.join(app_data_dir(), "session.dat")

    def load(self) -> Optional[dict]:
        try:
            with open(self.path, "rb") as f:
                data = json.loads(_dpapi(f.read(), protect=False).decode("utf-8"))
        except (OSError, ValueError):
            return None
        if not isinstance(data, dict) or not re.fullmatch(r"[0-9a-f]{64}", str(data.get("token", ""))):
            return None
        return data

    def save(self, token: str, user: str, server: str):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        blob = _dpapi(json.dumps({"token": token, "user": user, "server": server}).encode("utf-8"), protect=True)
        tmp = self.path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, self.path)

    def clear(self):
        try:
            os.remove(self.path)
        except OSError:
            pass


# ─────────────────────────────────────────────────────────────
# Sunucu API'si
# ─────────────────────────────────────────────────────────────

class AuthRequired(Exception):
    """Jeton yok, süresi doldu ya da yönetici oturumu kapattı (401)."""


class NetworkError(Exception):
    """Sunucuya ulaşılamadı / zaman aşımı. Jeton silinmez."""


class ServerError(Exception):
    def __init__(self, status: int, code: str, message: str, data: dict):
        super().__init__(message)
        self.status, self.code, self.data = status, code, data


def device_id() -> str:
    """
    Bu bilgisayarın kalıcı kimliği (yöneticinin hesap paylaşımını görebilmesi için). Windows'un MachineGuid değerinin
    uygulamaya özgü SHA-256 özeti gönderilir; ham kimlik sunucuya gitmez. Okunamazsa %APPDATA% altında rastgele bir kimlik.
    """
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography", 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            guid, _ = winreg.QueryValueEx(k, "MachineGuid")
        if guid:
            return hashlib.sha256(("makine-live:" + str(guid).lower()).encode()).hexdigest()[:32]
    except OSError:
        pass
    path = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "MakineLive", "device_id")
    try:
        with open(path, encoding="utf-8") as fh:
            v = fh.read().strip()
        if re.fullmatch(r"[a-f0-9]{32}", v):
            return v
    except OSError:
        pass
    v = hashlib.sha256(os.urandom(32)).hexdigest()[:32]
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(v)
    except OSError:
        pass
    return v


class ServerClient:
    """Uygulamanın konuştuğu tek sunucu: giriş, profil, çeviri, durum (bakım / güncelleme / duyurular)."""

    def __init__(self, base_url: str = SERVER_URL, app_version: str = "", device_name: str = ""):
        self.base_url = base_url.rstrip("/")
        self.host = urllib.parse.urlsplit(self.base_url).netloc
        self.token: Optional[str] = None
        self.app_version = app_version
        self.device = device_id()
        self.device_name = device_name

    # ── yardımcılar ──

    def _headers(self, json_body=False) -> dict:
        h = {"User-Agent": USER_AGENT, "Accept": "application/json", "Accept-Encoding": "gzip",
             "X-Makine-Device": self.device, "X-Makine-Version": self.app_version}
        if self.device_name:
            h["X-Makine-Device-Name"] = self.device_name.encode("ascii", "replace").decode()
        if json_body:
            h["Content-Type"] = "application/json"
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        return h

    def _call(self, method: str, path: str, payload: dict = None, timeout: float = 4.0) -> dict:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
        try:
            status, raw = HTTP.request(method, f"{self.base_url}{path}", body=body,
                                       headers=self._headers(payload is not None), timeout=timeout)
        except OSError as e:
            raise NetworkError(str(e) or "bağlantı kurulamadı") from e
        try:
            data = json.loads(raw.decode("utf-8")) if raw else {}
        except ValueError:
            data = {}
        if status == 401:
            raise AuthRequired(data.get("error") or "Giriş gerekli")
        if status >= 400 or not isinstance(data, dict):
            raise ServerError(status, str(data.get("code", "")), str(data.get("error") or f"HTTP {status}"),
                              data if isinstance(data, dict) else {})
        return data

    def warm(self):
        HTTP.warm(self.base_url)

    # ── oturum ──

    def login(self, username: str, password: str, device: str) -> dict:
        """Ekip hesabı. Başarılıysa profil (token dahil); yanlış şifrede AuthRequired, ağ hatasında NetworkError."""
        self.token = None
        data = self._call("POST", "/api/live/auth?action=app_login",
                          {"username": username, "password": password, "device": device}, timeout=8.0)
        self.token = data.get("token")
        return data

    def device_start(self, device: str) -> dict:
        """
        Makine Çeviri hesabıyla giriş, 1. adım: {"device", "code", "url", "expires_in", "interval"}.
        url tarayıcıda açılır (makineceviri.org/live/baglan/?kod=…); kullanıcı orada giriş yapıp kodu onaylar.
        """
        self.token = None
        return self._call("POST", "/api/live/auth?action=device_start", {"device": device}, timeout=8.0)

    def device_poll(self, device_secret: str) -> Optional[dict]:
        """
        2. adım, interval saniyede bir: onaylanmadıysa None; onaylandıysa profil (token dahil, jeton bir kez verilir).
        Kodun süresi dolduysa ServerError(410).
        """
        data = self._call("GET", "/api/live/auth?action=device_poll&device=" + urllib.parse.quote(device_secret), timeout=6.0)
        if data.get("status") != "approved":
            return None
        self.token = data.get("token")
        return data

    def me(self) -> dict:
        return self._call("GET", "/api/live/auth?action=app_me", timeout=5.0)

    def logout(self):
        try:
            if self.token:
                self._call("POST", "/api/live/auth?action=app_logout", {}, timeout=3.0)
        except (AuthRequired, NetworkError, ServerError):
            pass
        self.token = None

    # ── çeviri ──

    def translate(self, game_slug: str, game_name: str, text: str, source_lang: str = "en",
                  target_lang: str = "tr", timeout: float = 12.0, steam_appid: str = "") -> dict:
        """
        Tek istekte havuz + (gerekirse) DeepL + havuza kayıt.
        200 → {"translated", "origin": "pool"|"own"|"shared"|"deepl", "status", "server_ms", "used_today", "daily_limit", ...}
        Hatalar: AuthRequired (401), NetworkError, ServerError (422 çöp / 429 kota / 503 anahtar yok / 502 DeepL).
        """
        body = {"game": game_slug, "game_name": game_name, "text": text,
                "source_lang": source_lang, "target_lang": target_lang}
        if steam_appid:
            body["steam_appid"] = steam_appid          # portaldaki oyun görseli için (yalnızca boşsa kaydedilir)
        return self._call("POST", "/api/live/translate", body, timeout=timeout)

    # ── yerel havuz (oyunun onaylı satırları) ──

    def pool_sync(self, game_slug: str, rev: str = "", head: bool = False, timeout: float = 20.0,
                  steam_appid: str = "") -> dict:
        """
        head=True → {"count", "rev", "name"} (indirmeden önce sor). Aksi halde rev aynıysa {"unchanged": True},
        değilse {"rev", "count", "items": [[id, anahtar, kaynak, çeviri], ...]}. Yalnızca ONAYLI satırlar.
        """
        q = {"action": "sync", "game": game_slug}
        if rev:
            q["rev"] = rev
        if head:
            q["head"] = "1"
        if steam_appid:
            q["steam_appid"] = steam_appid
        return self._call("GET", "/api/live/pool?" + urllib.parse.urlencode(q), timeout=timeout)

    def pool_hits(self, game_slug: str, hits: Dict[int, int], timeout: float = 5.0) -> dict:
        """Yerel havuzdan gösterilen satırların sayacı (portaldaki "En çok görülen" sıralaması)."""
        return self._call("POST", "/api/live/pool?action=hits",
                          {"game": game_slug, "hits": {str(k): int(v) for k, v in hits.items()}}, timeout=timeout)

    # ── durum: bakım, güncelleme, duyurular ──

    def status(self, timeout: float = 5.0) -> dict:
        """{maintenance: {on, message, bypass}, update: {latest, available, mandatory, download}, announcements: [...]}"""
        return self._call("GET", "/api/live/app?action=status", timeout=timeout)

    def announcement_read(self, ann_id: int, timeout: float = 5.0) -> dict:
        return self._call("POST", "/api/live/app?action=ann_read", {"id": int(ann_id)}, timeout=timeout)

    def download_release(self, path: str, dest: str, sha256: str, progress=None, timeout: float = 30.0) -> str:
        """
        Yeni sürümü indirir (herkese açık adres; jeton varsa o da gider), SHA-256'yı doğrular ve dosya yolunu döndürür. progress(alınan, toplam) çağrılır.
        Özet tutmazsa dosya silinir ve ServerError fırlatılır.
        """
        url = f"{self.base_url}/{path.lstrip('/')}"
        req = urllib.request.Request(url, headers={k: v for k, v in self._headers().items() if k != "Accept-Encoding"})
        part = dest + ".part"
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        h = hashlib.sha256()
        try:
            ctx = ssl.create_default_context() if url.startswith("https") else None
            with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r, open(part, "wb") as out:
                total = int(r.headers.get("Content-Length") or 0)
                got = 0
                while True:
                    chunk = r.read(256 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
                    h.update(chunk)
                    got += len(chunk)
                    if progress:
                        progress(got, total)
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise AuthRequired("Giriş gerekli") from e
            raise ServerError(e.code, "", f"İndirme başarısız (HTTP {e.code})", {}) from e
        except OSError as e:
            raise NetworkError(str(e) or "indirme kesildi") from e
        if sha256 and h.hexdigest() != sha256.lower():
            try:
                os.remove(part)
            except OSError:
                pass
            raise ServerError(0, "checksum", "İndirilen dosya doğrulanamadı (SHA-256 tutmuyor).", {})
        os.replace(part, dest)
        return dest

    def check_connection(self, timeout: float = 2.0) -> bool:
        """Sunucu yanıt veriyor mu (bağlantı havuzda açık kalır; ilk çeviri el sıkışma beklemez)."""
        try:
            status, _ = HTTP.request("GET", f"{self.base_url}/api/live/games?action=stats",
                                     headers={"User-Agent": USER_AGENT}, timeout=timeout)
            return status < 500
        except OSError:
            return False
