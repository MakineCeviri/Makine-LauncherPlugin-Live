#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
console_mode.py testleri: 16:9 video alanı, oransal bölge ↔ ekran, kaynak eşleme, oyun slug'ları,
gerçek bir pencereyle kaynak bulma (Tk penceresi "seçilen pencere" olarak).

    py tests/test_console_mode.py
"""

import os
import sys
import time
import unittest

from _root import ROOT  # noqa: E402,F401
import console_mode as cm


class GeometryTest(unittest.TestCase):
    def test_video_rect_exact(self):
        self.assertEqual(cm.video_rect((0, 0, 1920, 1080)), (0, 0, 1920, 1080))

    def test_video_rect_pillarbox(self):
        # 4:3'e yakın pencere: yanlar değil üst/alt bant
        self.assertEqual(cm.video_rect((100, 50, 1100, 850)), (100, 169, 1100, 731))

    def test_video_rect_wide(self):
        # 21:9 pencere: yanlarda bant
        l, t, r, b = cm.video_rect((0, 0, 2560, 1080))
        self.assertEqual((t, b, r - l), (0, 1080, 1920))
        self.assertEqual(l, 320)

    def test_region_roundtrip(self):
        video = (200, 100, 1480, 820)            # 1280×720
        rect = cm.region_to_screen(video, cm.DEFAULT_REGION)
        self.assertEqual(rect, (354, 604, 973, 187))
        back = cm.screen_to_region(video, rect)
        for a, b in zip(back, cm.DEFAULT_REGION):
            self.assertAlmostEqual(a, b, places=2)

    def test_region_clipped_to_video(self):
        video = (0, 0, 1000, 500)
        frac = cm.screen_to_region(video, (-50, 400, 300, 200))
        self.assertEqual(frac, (0.0, 0.8, 0.25, 0.2))

    def test_region_follows_window(self):
        frac = (0.1, 0.8, 0.8, 0.15)
        a = cm.region_to_screen((0, 0, 1280, 720), frac)
        b = cm.region_to_screen((640, 360, 2560, 1440), frac)   # taşındı ve büyüdü
        self.assertEqual(a, (128, 576, 1024, 108))
        self.assertEqual(b, (832, 1224, 1536, 162))

    def test_valid_region(self):
        self.assertTrue(cm.valid_region(cm.DEFAULT_REGION))
        self.assertFalse(cm.valid_region((0.5, 0.5, 0.6, 0.1)))   # sağdan taşar
        self.assertFalse(cm.valid_region((0, 0, 0, 0)))
        self.assertFalse(cm.valid_region("bozuk"))


class MatchTest(unittest.TestCase):
    def test_known_sources(self):
        self.assertEqual(cm.match_source("RemotePlay.exe", "PS Remote Play").key, "ps-remote-play")
        self.assertEqual(cm.match_source("XboxPcApp.exe", "Xbox").console, "xbox")
        self.assertEqual(cm.match_source("4KCaptureUtility.exe", "").console, "capture")

    def test_title_patterns(self):
        self.assertIsNotNone(cm.match_source("obs64.exe", "Windowed Projector (Preview)"))
        self.assertIsNotNone(cm.match_source("obs64.exe", "Pencereli Projektör (Önizleme)"))
        self.assertIsNone(cm.match_source("obs64.exe", "OBS 31.0.0 - Profile: Untitled"))
        self.assertIsNotNone(cm.match_source("ApplicationFrameHost.exe", "Kamera"))
        self.assertIsNone(cm.match_source("ApplicationFrameHost.exe", "Ayarlar"))

    def test_not_sources(self):
        for exe in ("chrome.exe", "zen.exe", "discord.exe", "spotify.exe", ""):
            self.assertIsNone(cm.match_source(exe, "PS Remote Play"), exe)

    def test_custom(self):
        s = cm.match_source("PotPlayerMini64.exe", "x", custom_exes=["potplayermini64.exe"])
        self.assertEqual((s.key, s.console), ("custom", "custom"))


class GameTest(unittest.TestCase):
    def test_always_gta6(self):
        """Konsol modu yalnızca GTA VI içindir: bütün satırlar aynı havuza gider."""
        g = cm.console_game()
        self.assertEqual((g.name, g.slug, g.store), ("Grand Theft Auto VI", "grand-theft-auto-vi", "console"))
        from game_library import slugify
        self.assertEqual(slugify(cm.DEFAULT_GAME), cm.CONSOLE_SLUG)   # sunucunun ürettiği slug ile aynı


class FindTest(unittest.TestCase):
    """Gerçek pencere: bu süreç (python/py) "seçilen pencere" exe'si olarak tanıtılır."""

    def test_find_window(self):
        import tkinter as tk
        root = tk.Tk()
        root.title("Konsol kaynağı testi")
        root.geometry("1100x640+40+40")
        root.update()
        time.sleep(0.2)
        root.update()
        try:
            exe = os.path.basename(sys.executable).lower()
            t = cm.find_console_source(custom_exes=[exe])
            self.assertIsNotNone(t)
            self.assertEqual(t.slug, "grand-theft-auto-vi")
            self.assertEqual(t.store, "console")
            self.assertEqual(t.source.console, "custom")
            self.assertIsNone(cm.console_block_reason(t), "1100×640 pencere okunabilir olmalı")
            # bizim süreç yok sayılınca bulunmaz
            self.assertIsNone(cm.find_console_source(ignore_pids={os.getpid()}, custom_exes=[exe]))
            root.geometry("700x400")
            root.update()
            time.sleep(0.1)
            self.assertIn("küçük", cm.console_block_reason(t) or "")
        finally:
            root.destroy()


if __name__ == "__main__":
    unittest.main(verbosity=2)
