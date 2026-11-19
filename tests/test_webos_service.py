#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LG webOS servisi (tv-webos/service/service.js) testleri: gerçek Node.js ile, sahte 'webos-service' modülüyle
(tests/webos_mock). PC tarafı gerçek tv_link.py'dir: keşif, eşleşme, satır akışı, deneme kilidi.

    py tests/test_webos_service.py          (Node.js gerekir)
"""

import json
import os
import queue
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

from _root import ROOT  # noqa: E402
from tv_link import TvLink, TvInfo, discover

SERVICE = os.path.join(ROOT, "tv-webos", "service", "service.js")
MOCK = os.path.join(ROOT, "tests", "webos_mock", "node_modules")


@unittest.skipUnless(shutil.which("node"), "Node.js yok")
class WebosServiceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        env = dict(os.environ, NODE_PATH=MOCK, MAKINE_TV_STATE=os.path.join(self.tmp, "state.json"))
        self.proc = subprocess.Popen(["node", SERVICE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env, text=True, encoding="utf-8")
        self.events = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.state = self.wait_event(lambda e: e.get("event") == "state" and e.get("discovery"))

    def tearDown(self):
        self.proc.kill()
        self.proc.wait()
        for f in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            if f:
                f.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _read(self):
        for line in self.proc.stdout:
            try:
                self.events.put(json.loads(line)["response"])
            except (ValueError, KeyError):
                pass

    def wait_event(self, pred, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                e = self.events.get(timeout=max(0.01, end - time.monotonic()))
            except queue.Empty:
                break
            if pred(e):
                return e
        self.fail("beklenen olay gelmedi")

    def hello(self, code):
        s = socket.create_connection(("127.0.0.1", self.state["port"]), timeout=5)
        s.sendall((json.dumps({"t": "hello", "v": 1, "code": code, "pc": "TEST"}) + "\n").encode())
        reply = json.loads(s.makefile("rb").readline())
        s.close()
        return reply["t"]

    def test_state_and_discovery(self):
        self.assertRegex(self.state["code"], r"^\d{4}$")
        self.assertEqual(self.state["name"], "Test LG TV")
        found = discover(1.0, targets=["127.0.0.1"])
        self.assertEqual([t.name for t in found], ["Test LG TV"])
        self.assertEqual(found[0].port, self.state["port"])

    def test_lines_through_tv_link(self):
        link = TvLink("TEST-PC")
        self.addCleanup(link.close)
        link.pair(TvInfo("lg", "LG", "127.0.0.1", self.state["port"]), self.state["code"])
        self.wait_event(lambda e: e.get("event") == "state" and e.get("pc") == "TEST-PC")
        self.assertTrue(link.connected or self._wait(lambda: link.connected))
        link.send_line(1, "Savaş asla değişmez.", "verified")
        e = self.wait_event(lambda e: e.get("event") == "line")
        self.assertEqual((e["text"], e["kind"]), ("Savaş asla değişmez.", "verified"))
        link.clear()
        self.wait_event(lambda e: e.get("event") == "clear")
        link.set_style(2, "ust", 90)
        e = self.wait_event(lambda e: e.get("event") == "style")
        self.assertEqual((e["size"], e["pos"], e["opacity"]), (2, "ust", 90))
        link.pair(None)                                  # PC ayrılınca TV altyazıyı temizler
        self.wait_event(lambda e: e.get("event") == "state" and e.get("pc") is None)

    def test_lockout(self):
        code = self.state["code"]
        self.assertEqual(self.hello(code), "ok")
        t0 = time.monotonic()
        for i in range(5):
            self.assertEqual(self.hello("%04d" % ((int(code) + 1 + i) % 10000)), "bad_code")
        self.assertGreaterEqual(time.monotonic() - t0, 4.5, "her yanlış deneme ~1 sn bekletmeli")
        new = self.wait_event(lambda e: e.get("event") == "state" and e.get("code") != code)["code"]
        self.assertEqual(self.hello(new), "bad_code", "kilitliyken yeni kod da reddedilmeli")
        self.assertEqual(self.hello(code), "bad_code")

    def test_new_code_kicks_pc(self):
        link = TvLink("TEST-PC")
        self.addCleanup(link.close)
        link.pair(TvInfo("lg", "LG", "127.0.0.1", self.state["port"]), self.state["code"])
        self.assertTrue(self._wait(lambda: link.connected))
        self.proc.stdin.write("newCode\n")
        self.proc.stdin.flush()
        self.assertTrue(self._wait(lambda: link.state == "bad_code", 6), "eski kodla yeniden bağlanamamalı")

    def test_code_persists(self):
        with open(os.path.join(self.tmp, "state.json"), encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["code"], self.state["code"])

    @staticmethod
    def _wait(pred, timeout=4.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if pred():
                return True
            time.sleep(0.05)
        return False


if __name__ == "__main__":
    unittest.main(verbosity=2)
