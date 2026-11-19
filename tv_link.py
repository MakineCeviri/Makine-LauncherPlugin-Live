#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Makine Live → Makine TV bağlantısı [TEST→C++] (konsol / TV modu).

PC, konsolun Remote Play / yakalama penceresini okur ve çevirir; TV uygulaması (tv/, Android TV) çeviriyi HDMI
görüntüsünün üstüne çizer. TV sunucuya hiç bağlanmaz: yalnızca bu bağlantıdan gelen satırı gösterir.

Protokol (v1), ev ağı:
  Keşif  UDP 47710, yayın:  {"t":"discover","v":1}  →  TV yanıtı  {"t":"here","v":1,"id","name","port"}
  Akış   TCP <port>, satır başına bir UTF-8 JSON:
           PC → TV  hello{v,code,pc}   line{id,text,kind}   clear{}   style{size,pos,opacity}   ping{}
                    (style isteğe bağlı: size 0–2, pos alt|ust, opacity 40–100; TV'nin kendi ayarlarını değiştirir)
           TV → PC  ok{tv}  |  bad_code{}
  Eşleşme kodu TV ekranında görünen 4 hanedir; aynı ağdaki başka bir bilgisayar kod olmadan TV'ye yazamaz.
  TV, 15 sn hiçbir şey gelmezse altyazıyı temizler (PC kapandı / ağ koptu).

Gönderim ayrı bir iş parçacığında yapılır ve "en yeni kazanır": TV'ye yetişemeyen eski satır atlanır.
Arayüz (Tk) asla ağı beklemez; durum değişiklikleri on_state geri çağrısıyla bildirilir (çağıran ana thread'e taşır).
"""

from __future__ import annotations

import json
import os
import select
import socket
import threading
import time
from dataclasses import dataclass, asdict
from typing import Callable, List, Optional, Tuple

PROTOCOL_V = 1
DISCOVERY_PORT = 47710
TCP_PORT = 47711                 # TV'lerin varsayılan akış kapısı ("Adresle bağlan" bunu kullanır)
PING_S = 5.0
CONNECT_TIMEOUT_S = 3.0
RETRY_S = 2.0


@dataclass
class TvInfo:
    id: str
    name: str
    host: str
    port: int

    def to_json(self) -> dict:
        return asdict(self)

    @staticmethod
    def from_json(d) -> Optional["TvInfo"]:
        try:
            return TvInfo(str(d["id"]), str(d.get("name") or "TV"), str(d["host"]), int(d["port"]))
        except (KeyError, TypeError, ValueError):
            return None


def _encode(msg: dict) -> bytes:
    return (json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def _broadcast_addresses() -> List[str]:
    """Genel yayın adresi + bu bilgisayarın IPv4 ağlarının /24 yayını (bazı yönlendiriciler 255.255.255.255'i iletmez)."""
    out = ["255.255.255.255"]
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = str(info[4][0])
            if ip.startswith("127."):
                continue
            b = ip.rsplit(".", 1)[0] + ".255"
            if b not in out:
                out.append(b)
    except OSError:
        pass
    return out


def discover(timeout: float = 1.5, port: int = DISCOVERY_PORT, targets: Optional[List[str]] = None) -> List[TvInfo]:
    """Ağdaki Makine TV'leri bulur (yanıt veren her TV bir kez)."""
    found = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("", 0))
        probe = _encode({"t": "discover", "v": PROTOCOL_V})
        for addr in targets or _broadcast_addresses():
            try:
                sock.sendto(probe, (addr, port))
            except OSError:
                continue
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            sock.settimeout(left)
            try:
                data, (host, _) = sock.recvfrom(4096)
            except socket.timeout:
                break
            except OSError:
                continue
            try:
                msg = json.loads(data.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                continue
            if not isinstance(msg, dict) or msg.get("t") != "here" or not msg.get("id"):
                continue
            tv = TvInfo.from_json({**msg, "host": host})
            if tv:
                found[tv.id] = tv
    finally:
        sock.close()
    return sorted(found.values(), key=lambda t: t.name.lower())


class TvLink:
    """
    Eşleşmiş tek bir TV'ye kalıcı bağlantı.

    Durumlar (on_state(durum, ayrıntı)):
      idle        TV seçilmedi
      connecting  bağlanıyor / yeniden bağlanıyor
      connected   bağlı (ayrıntı: TV adı)
      bad_code    TV kodu kabul etmedi (yeniden kod girilmeli; bağlantı denemesi durur)
      offline     TV'ye ulaşılamıyor (RETRY_S'de bir yeniden denenir; adresi değiştiyse ağda kimliğiyle aranır)
    """

    def __init__(self, pc_name: str = "", on_state: Optional[Callable[[str, str], None]] = None,
                 discovery_port: int = DISCOVERY_PORT):
        self.pc_name = pc_name or os.environ.get("COMPUTERNAME", "PC")
        self.on_state = on_state
        self.discovery_port = discovery_port
        self.state, self.detail = "idle", ""
        self.tv: Optional[TvInfo] = None
        self.code = ""
        self._cv = threading.Condition()
        self._line: Optional[dict] = None      # en yeni satır ya da clear (tek yuva: en yeni kazanır)
        self._style: Optional[dict] = None
        self._gen = 0                          # TV / kod değişince eski bağlantı bırakılır
        self._closed = False
        self._thread = threading.Thread(target=self._run, daemon=True, name="tv-baglanti")
        self._thread.start()

    # ── Dışarıya açık API (her thread'den çağrılabilir) ──

    def pair(self, tv: Optional[TvInfo], code: str = ""):
        """Yeni TV / kod. tv=None bağlantıyı bırakır."""
        with self._cv:
            self.tv, self.code = tv, (code or "").strip()
            self._gen += 1
            self._cv.notify_all()
        self._set_state("idle" if tv is None else "connecting", tv.name if tv else "")

    @property
    def connected(self) -> bool:
        return self.state == "connected"

    def send_line(self, line_id: int, text: str, kind: str = "api"):
        self._put({"t": "line", "id": int(line_id), "text": text, "kind": kind})

    def clear(self):
        self._put({"t": "clear"})

    def set_style(self, size: int, pos: str, opacity: int):
        with self._cv:
            self._style = {"t": "style", "size": int(size), "pos": pos, "opacity": int(opacity)}
            self._cv.notify_all()

    def close(self):
        with self._cv:
            self._closed = True
            self._gen += 1
            self._cv.notify_all()

    # ── İç ──

    def _put(self, msg):
        with self._cv:
            self._line = msg
            self._cv.notify_all()

    def _set_state(self, state, detail=""):
        if (state, detail) == (self.state, self.detail):
            return
        self.state, self.detail = state, detail
        if self.on_state:
            try:
                self.on_state(state, detail)
            except Exception:  # noqa: BLE001 — geri çağrının hatası bağlantıyı düşürmesin
                pass

    def _wait_target(self) -> Optional[Tuple[int, TvInfo, str]]:
        with self._cv:
            while not self._closed and (self.tv is None or not self.code):
                self._cv.wait()
            if self._closed or self.tv is None:
                return None
            return self._gen, self.tv, self.code

    def _run(self):
        while True:
            target = self._wait_target()
            if target is None:
                return
            gen, tv, code = target
            if self.state != "offline":           # yeniden denemelerde durum "ulaşılamıyor"da kalır (titremesin)
                self._set_state("connecting", tv.name)
            result = self._session(gen, tv, code)
            if result == "bad_code":
                self._set_state("bad_code", tv.name)
                with self._cv:
                    if self._gen == gen:          # yeni kod girilene kadar bekle
                        self.code = ""
                continue
            if result == "stale":
                continue
            self._set_state("offline", tv.name)
            moved = self._relocate(tv)            # DHCP adresi değiştirmiş olabilir
            with self._cv:
                if self._gen == gen and moved and self.tv is tv:
                    self.tv = moved
                    continue
                self._cv.wait(RETRY_S)

    def _relocate(self, tv: TvInfo) -> Optional[TvInfo]:
        try:
            for t in discover(1.0, self.discovery_port):
                if t.id == tv.id and (t.host, t.port) != (tv.host, tv.port):
                    return t
        except OSError:
            pass
        return None

    def _session(self, gen, tv: TvInfo, code: str) -> str:
        """Tek bağlantı ömrü. Dönüş: bad_code | stale (TV/kod değişti) | lost."""
        try:
            sock = socket.create_connection((tv.host, tv.port), timeout=CONNECT_TIMEOUT_S)
        except OSError:
            return "lost"
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.sendall(_encode({"t": "hello", "v": PROTOCOL_V, "code": code, "pc": self.pc_name}))
            reply = self._read_line(sock, CONNECT_TIMEOUT_S)
            if not reply or reply.get("t") == "bad_code":
                return "bad_code" if reply else "lost"
            if reply.get("t") != "ok":
                return "lost"
            self._set_state("connected", str(reply.get("tv") or tv.name))
            with self._cv:
                pending_style = self._style
            if pending_style:
                sock.sendall(_encode(pending_style))
            last_send = time.monotonic()
            sock.settimeout(CONNECT_TIMEOUT_S)
            while True:
                if self._peer_closed(sock):          # TV kapandı / uygulamadan çıkıldı: hemen yeniden bağlan
                    return "lost"
                with self._cv:
                    if self._gen != gen:
                        return "stale"
                    if self._line is None and self._style is pending_style:
                        self._cv.wait(min(1.0, max(0.0, PING_S - (time.monotonic() - last_send))))
                    if self._gen != gen:
                        return "stale"
                    out = []
                    if self._style is not pending_style:
                        pending_style = self._style
                        out.append(pending_style)
                    if self._line is not None:
                        out.append(self._line)
                        self._line = None
                if not out and time.monotonic() - last_send >= PING_S:
                    out.append({"t": "ping"})
                if out:
                    sock.sendall(b"".join(_encode(m) for m in out))
                    last_send = time.monotonic()
        except OSError:
            return "lost"
        finally:
            try:
                sock.close()
            except OSError:
                pass

    @staticmethod
    def _peer_closed(sock) -> bool:
        """TV bağlantıyı kapattı mı? (TV, ok'tan sonra bir şey yollamaz; okunabilir olması kapanış demektir.)"""
        try:
            readable, _, _ = select.select([sock], [], [], 0)
            if not readable:
                return False
            return not sock.recv(1024)
        except OSError:
            return True

    @staticmethod
    def _read_line(sock, timeout) -> Optional[dict]:
        sock.settimeout(timeout)
        buf = b""
        try:
            while b"\n" not in buf and len(buf) < 4096:
                chunk = sock.recv(1024)
                if not chunk:
                    return None
                buf += chunk
        except OSError:
            return None
        try:
            msg = json.loads(buf.split(b"\n", 1)[0].decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return None
        return msg if isinstance(msg, dict) else None


if __name__ == "__main__":
    tvs = discover()
    print("bulunan TV'ler:", tvs or "yok")
