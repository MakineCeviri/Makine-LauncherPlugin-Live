# -*- coding: utf-8 -*-
"""
Discord'da "Makine Live ile Türkçe oynuyor" (Rich Presence). Bağımlılık yok: Discord masaüstü uygulamasının yerel
IPC kanalı (\\\\.\\pipe\\discord-ipc-0..9) ile konuşur; ağa çıkmaz, Discord'a kullanıcı adı / jeton gitmez.

Görünen:  büyük görsel  oyunun Steam kapağı (Steam numarası varsa) ya da Makine Live logosu
          ayrıntı       "Grand Theft Auto VI · Türkçe oynuyor"
          durum         "Bu oturumda 124 diyalog çevrildi"
          süre          oturumun başından beri (Discord "01:23:45 geçti" gösterir)
          düğme         "Makine Live'ı indir" → makineceviri.org/live/

Yalnızca okuma açıkken (F9) gösterilir; durdurunca, çıkışta, oturum kapanınca temizlenir. Discord kapalıysa 20 sn'de
bir yeniden denenir. Discord saniyede çok güncellemeye izin vermez (20 sn'de 5): değişiklikler en az 15 sn arayla gider.

Discord Developer Portal'da "Makine Live" adlı bir uygulama gerekir (profilde "Makine Live oynuyor" yazan ad odur);
kimliği (Application ID, gizli değildir; Public Key ve Client Secret gerekmez) DISCORD_APP_ID'dedir. Görseller adresle
verilir (sitenin logosu, Steam kapağı), Portal'a görsel yüklemek gerekmez. Kimlik boşken bileşen hiçbir şey yapmaz.
"""

from __future__ import annotations

import json
import os
import struct
import threading
import time
import uuid
from typing import Optional

DISCORD_APP_ID = "1573370697676488765"   # Discord Developer Portal → Makine Live → Application ID
PIPE = r"\\.\pipe\discord-ipc-{}"
DOWNLOAD_URL = "https://makineceviri.org/live/"
LOGO_URL = "https://makineceviri.org/images/icon-512.png"

OP_HANDSHAKE, OP_FRAME, OP_CLOSE = 0, 1, 2


def app_id() -> str:
    """Geliştirmede ortam değişkeniyle denenebilir (MAKINE_DISCORD_APP_ID)."""
    v = (os.environ.get("MAKINE_DISCORD_APP_ID") or DISCORD_APP_ID).strip()
    return v if v.isdigit() else ""


def build_activity(game: str, started_at: float, lines: int, console: bool = False,
                   steam_appid: str = "") -> dict:
    """Discord'a gidecek etkinlik (saf fonksiyon: test edilir)."""
    game = (game or "Bir oyun").strip()
    details = f"{game} · {'konsolda ' if console else ''}Türkçe oynuyor"
    state = f"Bu oturumda {lines} diyalog çevrildi" if lines else "Türkçe altyazı açık"
    large = (f"https://cdn.cloudflare.steamstatic.com/steam/apps/{steam_appid}/header.jpg"
             if str(steam_appid).isdigit() else LOGO_URL)
    return {
        "type": 0,
        "details": details[:128],
        "state": state[:128],
        "timestamps": {"start": int(started_at)},
        "assets": {"large_image": large, "large_text": game[:128], "small_image": LOGO_URL, "small_text": "Makine Live"},
        "buttons": [{"label": "Makine Live'ı indir", "url": DOWNLOAD_URL}],
        "instance": False,
    }


class _Pipe:
    """Discord IPC çerçevesi: [işlem kodu u32][uzunluk u32][JSON] (küçük sonlu)."""

    def __init__(self, path: str):
        self.f = open(path, "r+b", buffering=0)

    def send(self, op: int, payload: dict):
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.f.write(struct.pack("<II", op, len(data)) + data)

    def recv(self) -> tuple:
        head = self._read(8)
        op, n = struct.unpack("<II", head)
        return op, json.loads(self._read(n).decode("utf-8") or "{}")

    def _read(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.f.read(n - len(buf))
            if not chunk:
                raise OSError("Discord bağlantıyı kapattı")
            buf += chunk
        return buf

    def close(self):
        try:
            self.f.close()
        except OSError:
            pass


class DiscordPresence:
    """
    Arka planda tek iş parçacığı. set(activity) / clear() ana iş parçacığından çağrılır, hemen döner; en son istenen
    durum gönderilir (araya giren güncellemeler birleşir).
    """

    MIN_INTERVAL = 15.0
    RETRY_S = 20.0

    def __init__(self, client_id: Optional[str] = None, pipe: str = PIPE, on_state=None):
        self.client_id = app_id() if client_id is None else client_id
        self.pipe_fmt = pipe
        self.on_state = on_state                  # on_state(bağlı mı) — panelde göstermek için
        self.connected = False
        self._want = None                         # gönderilecek etkinlik (None = temizle)
        self._dirty = False
        self._sent_at = 0.0
        self._cv = threading.Condition()
        self._thread = None
        self._stop = False
        self._conn: Optional[_Pipe] = None

    @property
    def available(self) -> bool:
        return bool(self.client_id)

    def start(self):
        if not self.available or (self._thread and self._thread.is_alive()):
            return
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True, name="discord")
        self._thread.start()

    def stop(self):
        """Etkinliği temizleyip bağlantıyı kapatır (ayar kapatıldı / çıkış)."""
        with self._cv:
            self._want, self._dirty, self._stop = None, True, True
            self._cv.notify_all()
        if self._thread:
            self._thread.join(timeout=2.0)
        self._thread = None

    def set(self, activity: Optional[dict]):
        with self._cv:
            if activity == self._want and not self._dirty:
                return
            self._want, self._dirty = activity, True
            self._cv.notify_all()

    def clear(self):
        self.set(None)

    # ── iş parçacığı ──

    def _run(self):
        next_try = 0.0                            # Discord kapalıyken bir sonraki bağlanma denemesi
        while True:
            with self._cv:
                while not self._dirty and not self._stop:
                    self._cv.wait(timeout=5.0)
                if not self._dirty:               # durduruldu, gönderilecek bir şey yok
                    break
                activity, stopping = self._want, self._stop
                now = time.monotonic()
                if activity is not None and not stopping:
                    wait = max(self._sent_at + self.MIN_INTERVAL - now,
                               next_try - now if self._conn is None else 0.0)
                    if wait > 0:                  # Discord'un hız sınırı / yeniden deneme zamanı
                        self._cv.wait(timeout=wait)
                        continue
                self._dirty = False
            conn = self._conn
            if conn is None:
                if activity is None:              # bağlı değilken temizlenecek bir şey yok
                    if stopping:
                        break
                    continue
                if not self._connect():
                    next_try = time.monotonic() + self.RETRY_S
                    with self._cv:
                        self._dirty = self._want is not None
                    continue
                conn = self._conn
            try:
                conn.send(OP_FRAME, {"cmd": "SET_ACTIVITY", "nonce": str(uuid.uuid4()),
                                     "args": {"pid": os.getpid(), "activity": activity}})
                conn.recv()
                self._sent_at = time.monotonic()
            except (OSError, ValueError, struct.error):
                self._disconnect()
                next_try = time.monotonic() + self.RETRY_S
                with self._cv:
                    self._dirty = self._want is not None
            if stopping:
                break
        self._disconnect()

    def _connect(self) -> bool:
        for i in range(10):
            try:
                conn = _Pipe(self.pipe_fmt.format(i))
            except OSError:
                continue
            try:
                conn.send(OP_HANDSHAKE, {"v": 1, "client_id": self.client_id})
                op, data = conn.recv()
                if op == OP_FRAME and data.get("evt") == "READY":
                    self._conn = conn
                    self._set_connected(True)
                    return True
            except (OSError, ValueError, struct.error):
                pass
            conn.close()
        return False

    def _disconnect(self):
        if self._conn is not None:
            self._conn.close()
            self._conn = None
        self._set_connected(False)

    def _set_connected(self, on: bool):
        if on != self.connected:
            self.connected = on
            if self.on_state:
                self.on_state(on)
