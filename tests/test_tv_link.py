#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tv_link.py testleri: Python'da sahte bir Makine TV (aynı protokol) ile keşif, eşleşme kodu, satır akışı,
"en yeni kazanır", kopma ve yeniden bağlanma.

    py tests/test_tv_link.py
"""

import json
import socket
import threading
import time
import unittest

from _root import ROOT  # noqa: E402,F401
import tv_link
from tv_link import TvLink, TvInfo, discover


class FakeTv:
    """tv/ uygulamasının LinkServer'ıyla aynı davranış (kod doğrulama, tek bağlantı, satır başına JSON)."""

    def __init__(self, code="4821", name="Salon TV", tv_id="tv-1", slow_s=0.0):
        self.code, self.name, self.id, self.slow_s = code, name, tv_id, slow_s
        self.received = []
        self.hellos = []
        self._stop = False
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(("127.0.0.1", 0))
        self.tcp.listen(2)
        self.port = self.tcp.getsockname()[1]
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", 0))
        self.udp_port = self.udp.getsockname()[1]
        self.conns = []
        threading.Thread(target=self._accept, daemon=True).start()
        threading.Thread(target=self._discovery, daemon=True).start()

    def info(self):
        return TvInfo(self.id, self.name, "127.0.0.1", self.port)

    def _discovery(self):
        self.udp.settimeout(0.2)
        while not self._stop:
            try:
                data, addr = self.udp.recvfrom(1024)
            except (socket.timeout, OSError):
                continue
            msg = json.loads(data)
            if msg.get("t") == "discover":
                reply = {"t": "here", "v": 1, "id": self.id, "name": self.name, "port": self.port}
                self.udp.sendto(json.dumps(reply).encode(), addr)

    def _accept(self):
        while not self._stop:
            try:
                conn, _ = self.tcp.accept()
            except OSError:
                return
            for old in self.conns:          # yeni gelen eskisini düşürür
                old.close()
            self.conns = [conn]
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        f = conn.makefile("rb")
        try:
            hello = json.loads(f.readline())
            self.hellos.append(hello)
            if hello.get("code") != self.code:
                conn.sendall(b'{"t":"bad_code"}\n')
                conn.close()
                return
            conn.sendall(json.dumps({"t": "ok", "tv": self.name}).encode() + b"\n")
            for raw in f:
                if self.slow_s:
                    time.sleep(self.slow_s)
                self.received.append(json.loads(raw))
        except (OSError, ValueError):
            pass

    def drop_clients(self):
        for c in self.conns:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            c.close()
        self.conns = []

    def close(self):
        self._stop = True
        self.drop_clients()
        self.tcp.close()
        self.udp.close()

    def lines(self):
        return [m for m in self.received if m.get("t") in ("line", "clear")]


def wait_for(pred, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.02)
    return False


class TvLinkTest(unittest.TestCase):
    def setUp(self):
        self.tv = FakeTv()
        self.states = []

    def tearDown(self):
        self.tv.close()

    def make_link(self):
        link = TvLink("TEST-PC", on_state=lambda s, _d: self.states.append(s), discovery_port=self.tv.udp_port)
        self.addCleanup(link.close)
        return link

    def test_discover(self):
        found = discover(0.5, port=self.tv.udp_port, targets=["127.0.0.1"])
        self.assertEqual(len(found), 1)
        self.assertEqual((found[0].id, found[0].name, found[0].port), ("tv-1", "Salon TV", self.tv.port))

    def test_pair_and_lines(self):
        link = self.make_link()
        link.set_style(2, "alt", 80)
        link.pair(self.tv.info(), "4821")
        self.assertTrue(wait_for(lambda: link.connected))
        self.assertEqual(self.tv.hellos[0]["pc"], "TEST-PC")
        link.send_line(1, "Savaş asla değişmez.", "verified")
        self.assertTrue(wait_for(lambda: len(self.tv.lines()) == 1))
        link.clear()
        self.assertTrue(wait_for(lambda: len(self.tv.lines()) == 2))
        self.assertEqual(self.tv.lines()[0], {"t": "line", "id": 1, "text": "Savaş asla değişmez.", "kind": "verified"})
        self.assertEqual(self.tv.lines()[1]["t"], "clear")
        styles = [m for m in self.tv.received if m["t"] == "style"]
        self.assertEqual(styles[0], {"t": "style", "size": 2, "pos": "alt", "opacity": 80})

    def test_bad_code(self):
        link = self.make_link()
        link.pair(self.tv.info(), "0000")
        self.assertTrue(wait_for(lambda: link.state == "bad_code"))
        n = len(self.tv.hellos)
        time.sleep(tv_link.RETRY_S + 0.5)
        self.assertEqual(len(self.tv.hellos), n, "yanlış kodla yeniden denememeli")
        link.pair(self.tv.info(), "4821")
        self.assertTrue(wait_for(lambda: link.connected))

    def test_newest_wins(self):
        # TV'ye ulaşılamazken gelen satırlar birikmez: bağlanınca yalnızca en yenisi gider
        link = self.make_link()
        for i in range(1, 30):
            link.send_line(i, f"satır {i}")
        link.pair(self.tv.info(), "4821")
        self.assertTrue(wait_for(lambda: any(m.get("id") == 29 for m in self.tv.lines())))
        time.sleep(0.2)
        self.assertEqual([m["id"] for m in self.tv.lines()], [29])

    def test_reconnect_after_drop(self):
        link = self.make_link()
        link.pair(self.tv.info(), "4821")
        self.assertTrue(wait_for(lambda: link.connected))
        self.tv.drop_clients()
        # Kapanış en geç 1 sn içinde fark edilir; keşifle adres aranır, sonra yeniden bağlanılır
        self.assertTrue(wait_for(lambda: len(self.tv.hellos) >= 2, timeout=tv_link.RETRY_S + 4))
        self.assertTrue(wait_for(lambda: link.connected))
        link.send_line(3, "yeniden bağlandı")
        self.assertTrue(wait_for(lambda: any(m.get("id") == 3 for m in self.tv.lines())))

    def test_unpair(self):
        link = self.make_link()
        link.pair(self.tv.info(), "4821")
        self.assertTrue(wait_for(lambda: link.connected))
        link.pair(None)
        self.assertEqual(link.state, "idle")

    def test_tvinfo_json(self):
        info = self.tv.info()
        self.assertEqual(TvInfo.from_json(info.to_json()), info)
        self.assertIsNone(TvInfo.from_json({"name": "x"}))


if __name__ == "__main__":
    unittest.main(verbosity=2)
