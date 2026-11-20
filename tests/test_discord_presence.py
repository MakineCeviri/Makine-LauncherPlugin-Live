"""
Discord'da gösterme (makine_live/net/discord_presence.py): sahte bir Discord IPC sunucusuyla (Windows adlandırılmış
kanalı) el sıkışma, etkinlik içeriği, hız sınırı, temizleme ve Discord kapalıyken yeniden deneme.
Çalıştırma: py tests/test_discord_presence.py
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import threading
import time
import unittest

from _root import ROOT  # noqa: F401
from makine_live.net import discord_presence as dp

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateNamedPipeW.restype = wt.HANDLE
k32.CreateNamedPipeW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
k32.ConnectNamedPipe.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ReadFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
k32.WriteFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
k32.DisconnectNamedPipe.argtypes = [wt.HANDLE]
k32.CloseHandle.argtypes = [wt.HANDLE]
PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_WAIT = 0x3, 0x0, 0x0
INVALID = wt.HANDLE(-1).value


class FakeDiscord:
    """Discord masaüstü uygulamasının IPC kanalı: READY ile el sıkışır, gelen her çerçeveyi kaydeder ve yanıtlar."""

    def __init__(self, name):
        self.path = r"\\.\pipe\\" + name + "-0"
        self.frames = []
        self.handshake = None
        self._stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()
        time.sleep(0.1)

    def _read(self, h, n):
        buf = ctypes.create_string_buffer(n)
        got = wt.DWORD()
        out = b""
        while len(out) < n:
            if not k32.ReadFile(h, buf, n - len(out), ctypes.byref(got), None) or got.value == 0:
                raise OSError("okunamadı")
            out += buf.raw[:got.value]
        return out

    def _send(self, h, op, payload):
        data = json.dumps(payload).encode()
        msg = struct.pack("<II", op, len(data)) + data
        k32.WriteFile(h, msg, len(msg), ctypes.byref(wt.DWORD()), None)

    def _serve(self):
        while not self._stop:
            h = k32.CreateNamedPipeW(self.path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, None)
            if h == INVALID:
                return
            self.handle = h
            k32.ConnectNamedPipe(h, None)
            try:
                while not self._stop:
                    op, n = struct.unpack("<II", self._read(h, 8))
                    payload = json.loads(self._read(h, n))
                    if op == 0:
                        self.handshake = payload
                        self._send(h, 1, {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1}})
                    else:
                        self.frames.append((time.monotonic(), payload))
                        self._send(h, 1, {"cmd": payload.get("cmd"), "nonce": payload.get("nonce"), "data": {}})
            except OSError:
                pass
            k32.DisconnectNamedPipe(h)
            k32.CloseHandle(h)

    def close(self):
        self._stop = True
        try:
            with open(self.path, "r+b", buffering=0):
                pass                                   # ConnectNamedPipe'ı uyandır
        except OSError:
            pass


def wait_for(cond, timeout=5.0):
    t = time.time()
    while time.time() - t < timeout:
        if cond():
            return True
        time.sleep(0.02)
    return False


class ActivityTests(unittest.TestCase):
    def test_content(self):
        a = dp.build_activity("Grand Theft Auto VI", 1_700_000_000.4, 124)
        self.assertEqual(a["details"], "Grand Theft Auto VI · Türkçe oynuyor")
        self.assertEqual(a["state"], "Bu oturumda 124 diyalog çevrildi")
        self.assertEqual(a["timestamps"], {"start": 1_700_000_000})
        self.assertEqual(a["assets"]["large_image"], dp.LOGO_URL)
        self.assertEqual(a["buttons"][0]["url"], "https://makineceviri.org/live/")
        self.assertEqual(dp.build_activity("Heavy Rain", 0, 0)["state"], "Türkçe altyazı açık")
        self.assertIn("konsolda", dp.build_activity("GTA VI", 0, 3, console=True)["details"])
        self.assertTrue(dp.build_activity("Fallout", 0, 1, steam_appid="377160")["assets"]["large_image"]
                        .endswith("/377160/header.jpg"))

    def test_no_app_id_does_nothing(self):
        p = dp.DiscordPresence(client_id="")
        self.assertFalse(p.available)
        p.start()
        p.set(dp.build_activity("X", 0, 0))
        p.stop()


class PipeTests(unittest.TestCase):
    def setUp(self):
        self.name = f"makine-test-{os.getpid()}-{time.monotonic_ns()}"
        self.fake = FakeDiscord(self.name)
        self.p = dp.DiscordPresence(client_id="123456789012345678", pipe=r"\\.\pipe\\" + self.name + "-{}")
        self.p.MIN_INTERVAL = 0.6

    def tearDown(self):
        self.p.stop()
        self.fake.close()

    def test_handshake_and_activity(self):
        self.p.start()
        self.p.set(dp.build_activity("Grand Theft Auto VI", time.time(), 3))
        self.assertTrue(wait_for(lambda: self.fake.frames))
        self.assertEqual(self.fake.handshake, {"v": 1, "client_id": "123456789012345678"})
        frame = self.fake.frames[0][1]
        self.assertEqual(frame["cmd"], "SET_ACTIVITY")
        self.assertEqual(frame["args"]["pid"], os.getpid())
        self.assertEqual(frame["args"]["activity"]["state"], "Bu oturumda 3 diyalog çevrildi")
        self.assertTrue(self.p.connected)

    def test_rate_limit_and_latest_wins(self):
        self.p.start()
        self.p.set(dp.build_activity("A", 0, 1))
        self.assertTrue(wait_for(lambda: len(self.fake.frames) == 1))
        for n in range(2, 8):                          # hızlı güncellemeler birleşir, en sonuncusu gider
            self.p.set(dp.build_activity("A", 0, n))
        self.assertTrue(wait_for(lambda: len(self.fake.frames) == 2))
        time.sleep(0.8)
        self.assertEqual(len(self.fake.frames), 2)
        (t1, _), (t2, last) = self.fake.frames
        self.assertGreaterEqual(t2 - t1, 0.55)
        self.assertEqual(last["args"]["activity"]["state"], "Bu oturumda 7 diyalog çevrildi")

    def test_stop_clears(self):
        self.p.start()
        self.p.set(dp.build_activity("A", 0, 1))
        self.assertTrue(wait_for(lambda: self.fake.frames))
        self.p.stop()
        self.assertTrue(wait_for(lambda: len(self.fake.frames) == 2))
        self.assertIsNone(self.fake.frames[-1][1]["args"]["activity"])


class OfflineTests(unittest.TestCase):
    def test_discord_closed_then_opened(self):
        name = f"makine-test-off-{os.getpid()}-{time.monotonic_ns()}"
        p = dp.DiscordPresence(client_id="1", pipe=r"\\.\pipe\\" + name + "-{}")
        p.RETRY_S = 0.5
        p.start()
        p.set(dp.build_activity("A", 0, 1))
        time.sleep(0.3)
        self.assertFalse(p.connected)                  # Discord kapalı: hata yok, bekliyor
        fake = FakeDiscord(name)
        try:
            self.assertTrue(wait_for(lambda: fake.frames, timeout=4.0))
        finally:
            p.stop()
            fake.close()


if __name__ == "__main__":
    unittest.main(verbosity=1)
