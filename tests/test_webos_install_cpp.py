#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Eklentinin (C++) LG kurulumu, src/webos_install.cpp: test_webos_install.py'deki sahte "geliştirici modundaki LG TV"ye
karşı, live_webos_test.exe üzerinden. Sahte TV boş kapılarda açılır (9991 / 9922 dolu olsa da çalışır).
  - anahtar: paramiko'nun PEM'i (AES-256-CBC), elle şifrelenmiş AES-128-CBC ve DES-EDE3-CBC PEM'ler (OpenSSL'in eski
    biçimi), şifresiz PEM, yanlış parola, tanınmayan biçim
  - kurulum: eski SSH (yalnızca ssh-rsa, server-sig-algs yok) ve yeni SSH (rsa-sha2), SFTP'li ve SFTP'siz yükleme,
    sessiz akış, reddedilen kurulum,
    yanlış parola, Key Server kapalı

    py tests/test_webos_install_cpp.py          (exe: LIVE_WEBOS_TEST, yoksa build*/live_webos_test.exe)
ctest'te webos_install adıyla koşar; paramiko yoksa atlanır (çıkış 77).
"""

import glob
import io
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from _root import ROOT  # noqa: E402

try:
    import paramiko
    from test_webos_install import PASS, FakeLgTv
    from makine_live.tv import tv_install
except ImportError as e:  # paramiko yok: ctest atlar
    print(f"atlandı: {e}")
    sys.exit(77)


def _exe():
    env = os.environ.get("LIVE_WEBOS_TEST")
    if env:
        return os.path.abspath(env)
    found = sorted(glob.glob(os.path.join(ROOT, "build*", "live_webos_test.exe")), key=os.path.getmtime)
    return found[-1] if found else ""


EXE = _exe()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(*args, timeout=240):
    out = subprocess.run([EXE, *map(str, args)], capture_output=True, timeout=timeout).stdout.decode("utf-8", "replace")
    return [line.rstrip("\r") for line in out.splitlines()]


@unittest.skipUnless(EXE, "live_webos_test.exe yok (cmake --build)")
class KeyTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def pem(self, data: bytes) -> str:
        path = os.path.join(self.dir, f"k{len(os.listdir(self.dir))}.pem")
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_paramiko(self):
        key, buf = paramiko.RSAKey.generate(2048), io.StringIO()
        key.write_private_key(buf, password=PASS)
        self.assertIn("Proc-Type: 4,ENCRYPTED", buf.getvalue())
        path = self.pem(buf.getvalue().encode())
        self.assertEqual(run("key", path, PASS), ["key: ok"])
        self.assertEqual(run("key", path, PASS.lower()), ["key: ok"])      # küçük harfle yazılsa da
        self.assertEqual(run("key", path, " " + PASS + " "), ["key: ok"])  # kenar boşluğu
        self.assertIn("Parola (Passphrase) yanlış", run("key", path, "ZZZZZZ")[0])

    def test_ciphers_and_plain(self):
        import base64
        import hashlib
        from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
        from cryptography.hazmat.primitives import padding, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        k = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        der = k.private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.TraditionalOpenSSL,
                              serialization.NoEncryption())

        def traditional(name, algo, key_len, block):  # OpenSSL'in eski PEM şifrelemesi: EVP_BytesToKey(MD5) + CBC
            iv = os.urandom(block // 8)
            key, d = b"", b""
            while len(key) < key_len:
                d = hashlib.md5(d + PASS.encode() + iv[:8]).digest()
                key += d
            pad = padding.PKCS7(block).padder()
            enc = Cipher(algo(key[:key_len]), modes.CBC(iv)).encryptor()
            body = enc.update(pad.update(der) + pad.finalize()) + enc.finalize()
            b64 = base64.b64encode(body).decode()
            lines = "\n".join(b64[i:i + 64] for i in range(0, len(b64), 64))
            return (f"-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: {name},{iv.hex().upper()}\n\n"
                    f"{lines}\n-----END RSA PRIVATE KEY-----\n").encode()

        for name, algo, key_len, block in (("AES-128-CBC", algorithms.AES, 16, 128), ("AES-256-CBC", algorithms.AES, 32, 128),
                                           ("DES-EDE3-CBC", TripleDES, 24, 64)):
            path = self.pem(traditional(name, algo, key_len, block))
            self.assertEqual(run("key", path, PASS), ["key: ok"], name)
            self.assertIn("Parola (Passphrase) yanlış", run("key", path, "QQQQQQ")[0], name)
        plain = k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                serialization.NoEncryption())
        self.assertEqual(run("key", self.pem(plain), "herhangi"), ["key: ok"])
        self.assertIn("biçimde", run("key", self.pem(b"-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n"), PASS)[0])


@unittest.skipUnless(EXE, "live_webos_test.exe yok (cmake --build)")
class InstallTest(unittest.TestCase):
    def setUp(self):
        fd, self.ipk = tempfile.mkstemp(suffix=".ipk")
        with os.fdopen(fd, "wb") as fh:
            fh.write(b"!<arch>\n" + os.urandom(300000))

    def tearDown(self):
        os.remove(self.ipk)

    def fake(self, **kw):
        tv = FakeLgTv(key_port=0, ssh_port=0, **kw)
        self.addCleanup(tv.close)
        return tv

    def install(self, tv, passphrase=PASS, stream_timeout=None):
        args = ["install", "127.0.0.1", tv.key_port, tv.ssh_port, passphrase, self.ipk]
        if stream_timeout:
            args.append(stream_timeout)
        lines = run(*args)
        steps = [ln[6:] for ln in lines if ln.startswith("step: ")]
        result = next((ln[8:] for ln in lines if ln.startswith("result: ")), "(sonuç yok) " + " | ".join(lines))
        return result, steps

    def test_install_old_ssh(self):
        tv = self.fake()
        result, steps = self.install(tv, PASS.lower())                     # küçük harf de kabul
        self.assertEqual(result, "ok")
        with open(self.ipk, "rb") as fh:
            self.assertEqual(tv.files[tv_install.WEBOS_REMOTE], fh.read())  # SFTP yok: cat > dosya, birebir
        kinds = [c.split()[0] for c in tv.commands]
        self.assertEqual(kinds, ["cat", "/usr/bin/luna-send-pub", "rm", "/usr/bin/luna-send-pub"])
        self.assertIn("appInstallService/dev/install", tv.commands[1])
        self.assertIn('"ipkUrl":"/media/developer/temp/makine-tv.ipk"', tv.commands[1])
        self.assertIn("applicationManager/launch", tv.commands[-1])
        self.assertFalse(tv.pty_channels, "pty istenmemeli")
        self.assertEqual(steps[:3], ["TV'nin geliştirici anahtarı alınıyor…", "TV'ye bağlanılıyor…", "Uygulama yükleniyor…"])
        self.assertIn("Uygulama kuruluyor… (ipk parsing)", steps)
        self.assertEqual(steps[-1], "Makine TV açılıyor…")

    def test_install_sftp(self):
        tv = self.fake(sftp=True)
        result, _ = self.install(tv)
        self.assertEqual(result, "ok")
        with open(self.ipk, "rb") as fh:
            self.assertEqual(tv.files[tv_install.WEBOS_REMOTE], fh.read())   # SFTP ile, birebir
        self.assertEqual([c.split()[0] for c in tv.commands], ["/usr/bin/luna-send-pub", "rm", "/usr/bin/luna-send-pub"])

    def test_install_modern_ssh(self):
        tv = self.fake(old_ssh=False)
        result, _ = self.install(tv)
        self.assertEqual(result, "ok")
        self.assertIn(tv_install.WEBOS_REMOTE, tv.files)

    def test_install_silent_stream(self):
        tv = self.fake(silent=True)
        t0 = time.monotonic()
        result, _ = self.install(tv)
        self.assertEqual(result, "ok")
        self.assertLess(time.monotonic() - t0, 30, "akış sessizken 15 sn'de bir TV'ye sorulmalı")
        self.assertTrue(any("appinfo.json" in c for c in tv.commands))
        self.assertEqual(tv.commands[-2].split()[0], "rm")

    def test_not_installed_shows_tv_output(self):
        tv = self.fake(silent=True, install_result="failed")
        result, _ = self.install(tv, stream_timeout=20000)
        self.assertIn("TV kurulumu bitirmedi", result)
        self.assertTrue(any(c.startswith("rm -f ") for c in tv.commands))

    def test_install_rejected(self):
        tv = self.fake(install_result="failed")
        result, _ = self.install(tv)
        self.assertEqual(result, "error: TV kurulumu reddetti: Bozuk paket")
        self.assertFalse(any("applicationManager/launch" in c for c in tv.commands))

    def test_wrong_passphrase(self):
        tv = self.fake()
        result, _ = self.install(tv, "ZZZZZZ")
        self.assertIn("Parola (Passphrase) yanlış", result)
        self.assertEqual(tv.commands, [])

    def test_key_server_off(self):
        class Off:
            key_port, ssh_port = free_port(), free_port()
        result, _ = self.install(Off)
        self.assertIn("Key Server", result)
        self.assertIn("TV'ye ulaşılamadı", result)


if __name__ == "__main__":
    unittest.main(verbosity=2)
