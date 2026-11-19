#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""subtitle_web: telefon / OBS sayfaları ve /events akışı (gerçek HTTP, yerel kapı)."""

import http.client
import json
import re
import socket
import time
import unittest

from _root import ROOT  # noqa: E402,F401
import subtitle_web as sw


def _events(port):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", "/events")
    resp = conn.getresponse()
    assert resp.status == 200, resp.status
    assert resp.getheader("Content-Type", "").startswith("text/event-stream")

    def next_event():
        while True:
            line = resp.fp.readline().decode("utf-8")
            if line.startswith("data: "):
                resp.fp.readline()
                return json.loads(line[6:])

    next_event.resp = resp
    return conn, next_event


class SubtitleWebTest(unittest.TestCase):
    def setUp(self):
        self.web = sw.SubtitleWeb()
        self.web.configure(True, lan=False)
        self.assertTrue(self.web.running, self.web.error)

    def tearDown(self):
        self.web.stop()

    def get(self, path):
        conn = http.client.HTTPConnection("127.0.0.1", self.web.port, timeout=5)
        conn.request("GET", path)
        r = conn.getresponse()
        body = r.read()
        conn.close()
        return r, body

    def test_pages(self):
        for path in ("/", "/obs"):
            r, body = self.get(path)
            self.assertEqual(r.status, 200)
            self.assertIn("default-src 'self'", r.getheader("Content-Security-Policy") or "")
            html = body.decode("utf-8")
            # dışarıdan hiçbir şey yüklenmez (ev ağında internet olmayabilir; yayın sahnesi kendi başına çalışmalı)
            self.assertIsNone(re.search(r"(src|href)=\"https?:", html), path)
        r, _ = self.get("/nope")
        self.assertEqual(r.status, 404)
        r, _ = self.get("/../settings.cfg")
        self.assertEqual(r.status, 404)

    def test_stream(self):
        conn, nxt = _events(self.web.port)
        try:
            first = nxt()
            self.assertEqual(first["t"], "state")
            self.assertIsNone(first["line"])
            self.web.send_line(1, "Birinci satır", "verified")
            s = nxt()
            self.assertEqual((s["line"]["text"], s["line"]["kind"]), ("Birinci satır", "verified"))
            self.web.send_line(2, "İkinci satır", "pending")
            s = nxt()
            self.assertEqual(s["line"]["text"], "İkinci satır")
            self.assertEqual([h["text"] for h in s["history"]], ["Birinci satır"])
            self.web.clear()
            s = nxt()
            self.assertIsNone(s["line"])
            self.assertEqual([h["text"] for h in s["history"]], ["Birinci satır", "İkinci satır"])
            self.web.set_style(32, 90)
            self.assertEqual(nxt()["style"], {"size": 32, "opacity": 90})
            # sayaç: bir istemci bağlı
            self.assertEqual(self.web.clients, 1)
        finally:
            nxt.resp.close()
            conn.close()
        for _ in range(40):            # kopan bağlantı bir sonraki yazımda / kalp atışında düşer
            self.web.send_line(99, "x")
            if self.web.clients == 0:
                break
            time.sleep(0.05)
        self.assertEqual(self.web.clients, 0)

    def test_history_limit(self):
        for i in range(10):
            self.web.send_line(i, f"satır {i}")
        snap = self.web.snapshot()
        self.assertEqual(len(snap["history"]), sw.HISTORY)
        self.assertEqual(snap["history"][-1]["text"], "satır 8")

    def test_busy_port(self):
        """Kapı başka bir süreçte doluysa sonraki kapı kullanılır, aynı kapı ikinci kez alınmaz."""
        other = sw.SubtitleWeb()
        other.configure(True, lan=False)
        try:
            self.assertTrue(other.running)
            self.assertNotEqual(other.port, self.web.port)
        finally:
            other.stop()

    def test_lan_switch(self):
        self.web.configure(True, lan=True)
        self.assertTrue(self.web.running)
        ip = sw.lan_ip()
        self.assertRegex(self.web.phone_url, r"^http://[\d.]+:\d+$")
        if not ip.startswith("127."):
            with socket.create_connection((ip, self.web.port), timeout=2):
                pass
        self.web.configure(False, lan=True)
        self.assertFalse(self.web.running)


if __name__ == "__main__":
    unittest.main(verbosity=1)
