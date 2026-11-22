# -*- coding: utf-8 -*-
"""
Kendi kendini güncelleme (net/app_update.py): komut dosyası yok; çalışan .exe'nin adı .old olur, yeni sürüm yerine
kopyalanıp `--after-update <pid>` ile açılır, yeni süreç .old'u siler. Gerçek .exe yerine geçici dosyalarla.

    py tests/test_app_update.py
"""

import os
import tempfile
import unittest
from unittest import mock

import _root  # noqa: F401
from makine_live.net import app_update


class SwapTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.exe = os.path.join(self.dir, "MakineLive.exe")
        self.new = os.path.join(self.dir, "MakineLive-0.3.2.exe")
        with open(self.exe, "wb") as fh:
            fh.write(b"eski")
        with open(self.new, "wb") as fh:
            fh.write(b"yeni")
        self.popen = mock.patch.object(app_update.subprocess, "Popen").start()
        mock.patch.object(app_update.sys, "executable", self.exe).start()
        mock.patch.object(app_update.sys, "frozen", True, create=True).start()

    def tearDown(self):
        mock.patch.stopall()

    def read(self, path):
        with open(path, "rb") as fh:
            return fh.read()

    def test_swap(self):
        app_update.swap_and_restart(self.new)
        self.assertEqual(self.read(self.exe), b"yeni")
        self.assertEqual(self.read(self.exe + ".old"), b"eski")
        self.assertFalse(os.path.exists(self.new), "indirilen kopya silinir")
        cmd = self.popen.call_args[0][0]
        self.assertEqual(cmd[:2], [self.exe, app_update.AFTER_UPDATE_ARG])
        self.assertEqual(cmd[2], str(os.getpid()))
        self.assertNotIn("cmd", [os.path.basename(c).lower() for c in cmd], "komut dosyası yok")

    def test_fallback_when_target_not_writable(self):
        with mock.patch.object(app_update.shutil, "copyfile", side_effect=PermissionError("salt okunur")):
            app_update.swap_and_restart(self.new)
        self.assertEqual(self.read(self.exe), b"eski", "eski sürüm yerinde kalır")
        self.assertFalse(os.path.exists(self.exe + ".old"))
        self.assertEqual(self.popen.call_args[0][0][0], self.new, "yeni sürüm indirildiği yerden açılır")

    def test_after_update_cleans_old(self):
        app_update.swap_and_restart(self.new)
        self.assertTrue(app_update.after_update(["MakineLive.exe", app_update.AFTER_UPDATE_ARG, "0"]))
        self.assertFalse(os.path.exists(self.exe + ".old"))
        self.assertFalse(app_update.after_update(["MakineLive.exe"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
