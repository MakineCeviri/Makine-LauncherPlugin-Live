# -*- coding: utf-8 -*-
"""
Kullanım bildirimi ve ekibin istediği günlük (net/telemetry.py): okuma süresi / satır farkı, günlüğün sonu (gzip),
sistem bilgisi. Ağ yok; sunucu tarafı Makine-WEB tests/live-api.test.ts ("activity").

    py tests/test_telemetry.py
"""

import gzip
import os
import tempfile
import unittest

import _root  # noqa: F401
from makine_live.net import telemetry


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class BeatCounterTest(unittest.TestCase):
    def test_run_seconds_and_lines(self):
        clk = Clock()
        b = telemetry.BeatCounter(clk)
        self.assertEqual(b.take(0), {"run_s": 0, "lines": 0})
        b.running(True)
        clk.t += 125.6
        self.assertEqual(b.take(7), {"run_s": 125, "lines": 7})
        clk.t += 30.6                         # küsurat (0,6) bir sonrakine kaldı: 31,2 sn
        b.running(False)
        clk.t += 500                          # durmuşken geçen süre sayılmaz
        self.assertEqual(b.take(9), {"run_s": 31, "lines": 2})
        self.assertEqual(b.take(9), {"run_s": 0, "lines": 0})


class LogTest(unittest.TestCase):
    def test_collect_newest_within_budget(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "makine-live.log.1"), "w", encoding="utf-8") as fh:
            fh.write("eski satır\n" * 5000)
        with open(os.path.join(d, "makine-live.log"), "w", encoding="utf-8") as fh:
            fh.write("".join(f"{i:05d} havuz onaylı \"Hello\"\n" for i in range(3000)))
        with open(os.path.join(d, "crash.log"), "w", encoding="utf-8") as fh:
            fh.write("Fatal Python error: test\n")
        text = gzip.decompress(telemetry.collect_log(d, {"sürüm": "0.3.2 beta"}, limit=60_000)).decode("utf-8")
        self.assertIn("sürüm: 0.3.2 beta", text)
        self.assertIn("Fatal Python error", text)
        self.assertIn("02999 havuz", text, "en yeni satır gelir")
        self.assertLess(len(text.encode("utf-8")), 62_000, "bütçe aşılmaz")
        self.assertLess(text.index("crash.log"), text.index("02999"), "başlık ve çökme kaydı önce")

    def test_crash_log_without_crash_is_left_out(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "crash.log"), "w", encoding="utf-8") as fh:
            fh.write("\n──── 2026-11-22 16:31:59 · Makine Live 0.3.2 beta\n" * 40)
        text = gzip.decompress(telemetry.collect_log(d, {})).decode("utf-8")
        self.assertNotIn("crash.log", text, "yalnızca açılış başlıkları: çökme yok, bölüm eklenmez")

    def test_empty_dir(self):
        text = gzip.decompress(telemetry.collect_log(tempfile.mkdtemp(), {})).decode("utf-8")
        self.assertIn("Makine Live günlüğü", text)


class SystemTest(unittest.TestCase):
    def test_system_info(self):
        info = telemetry.system_info()
        self.assertIn("os", info)
        self.assertRegex(info.get("screen", "1x1"), r"^\d+x\d+$")
        self.assertGreater(info.get("ram_gb", 1), 0)

    def test_median(self):
        self.assertEqual(telemetry.median([30, 10, 20]), 20)
        self.assertIsNone(telemetry.median([]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
