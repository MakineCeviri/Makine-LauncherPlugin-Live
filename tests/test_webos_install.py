#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tv_install.install_webos testleri: sahte bir "geliştirici modundaki LG TV" ile (gerçek TV gerekmez).
  - 9991: parola ile şifrelenmiş SSH anahtarı (Key Server)
  - 9922: SSH, kullanıcı prisoner; SFTP yok (cat > dosya yedeğine düşülür), luna-send-pub kurulum yanıtları
Denenenler: başarılı kurulum (paket TV'ye birebir ulaşır, kur → sil → aç), yanlış parola, TV'nin kurulumu reddetmesi,
Key Server kapalı.

    py tests/test_webos_install.py
"""

import http.server
import io
import json
import os
import socket
import threading
import time
import unittest

import paramiko

from _root import ROOT  # noqa: E402
from makine_live.tv import tv_install

PASS = "A1B2C3"


class FakeLgTv:
    """old_ssh: gerçek LG'deki OpenSSH 6.1 gibi yalnızca ssh-rsa (SHA-1) imzası kabul eder, server-sig-algs bildirmez."""

    def __init__(self, install_result="installed", old_ssh=True, silent=False, key_port=None, ssh_port=None,
                 sftp=False):
        # Gerçek TV'de ölçülen (LG webOS 3.4.3): pty isteği kanalı kapatır; luna-send-pub çıktısı pty'siz anında gelir;
        # applicationManager/getAppInfo yok; kurulan uygulama /media/developer/apps/… altında.
        # silent: kurulum akışı hiç yanıt vermez ama kurulum yapılır (yalnızca appinfo.json ile anlaşılır).
        # key_port / ssh_port: None = gerçek TV'deki kapılar (9991 / 9922); 0 = boş bir kapı (C++ testi, key_port / ssh_port'ta).
        self.old_ssh = old_ssh
        self.silent = silent
        self.sftp = sftp                                 # SFTP alt sistemi (yoksa istemci cat > dosya yoluna düşer)
        self.installed_version = ""
        self.pty_channels = set()
        self.key = paramiko.RSAKey.generate(2048)
        buf = io.StringIO()
        self.key.write_private_key(buf, password=PASS)
        self.pem = buf.getvalue().encode()
        self.install_result = install_result
        self.files, self.commands = {}, []
        self.host_key = paramiko.RSAKey.generate(2048)
        self._stop = False

        tv = self

        class KeyHandler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == "/webos_rsa":
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(tv.pem)
                else:
                    self.send_error(404)

            def log_message(self, *_):
                pass

        self.http = http.server.ThreadingHTTPServer(
            ("127.0.0.1", tv_install.WEBOS_KEY_PORT if key_port is None else key_port), KeyHandler)
        self.key_port = self.http.server_address[1]
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        self.ssh = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.ssh.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ssh.bind(("127.0.0.1", tv_install.WEBOS_SSH_PORT if ssh_port is None else ssh_port))
        self.ssh_port = self.ssh.getsockname()[1]
        self.ssh.listen(4)
        threading.Thread(target=self._accept, daemon=True).start()

    def close(self):
        self._stop = True
        self.http.shutdown()
        self.http.server_close()
        self.ssh.close()

    def _accept(self):
        while not self._stop:
            try:
                conn, _ = self.ssh.accept()
            except OSError:
                return
            disabled = {"pubkeys": ["rsa-sha2-512", "rsa-sha2-256"]} if self.old_ssh else None
            t = paramiko.Transport(conn, disabled_algorithms=disabled, server_sig_algs=not self.old_ssh)
            t.add_server_key(self.host_key)
            if self.sftp:
                t.set_subsystem_handler("sftp", paramiko.SFTPServer, _Sftp)
            t.start_server(server=_Server(self))


class _Server(paramiko.ServerInterface):
    def __init__(self, tv):
        self.tv = tv

    def check_auth_publickey(self, username, key):
        ok = username == "prisoner" and key.get_base64() == self.tv.key.get_base64()
        return paramiko.AUTH_SUCCESSFUL if ok else paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "publickey"

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_pty_request(self, channel, term, width, height, pixelwidth, pixelheight, modes):
        self.tv.pty_channels.add(channel.get_id())
        return False                                      # gerçek TV pty vermiyor

    def check_channel_exec_request(self, channel, command):
        cmd = command.decode()
        self.tv.commands.append(cmd)
        threading.Thread(target=self._run, args=(channel, cmd), daemon=True).start()
        return True

    def _run(self, ch, cmd):
        tv = self.tv
        if cmd.startswith("cat > "):
            data = b""
            while True:
                chunk = ch.recv(65536)
                if not chunk:
                    break
                data += chunk
            tv.files[cmd[6:].strip()] = data
        elif "appInstallService/dev/install" in cmd:
            payload = json.loads(cmd.rsplit(" '", 1)[1].rstrip("'"))
            assert payload["id"] == tv_install.WEBOS_APP and payload["ipkUrl"] in tv.files
            if tv.install_result == "installed":
                tv.installed_version = "9.9.9"
            if not tv.silent:                             # gerçek TV'nin akışından kısaltılmış satırlar
                ch.sendall(b'{"subscribed":true,"returnValue":true}\n')
                ch.sendall(b'{"id":"org.makineceviri.tv","statusValue":35,"details":{"state":"ipk parsing"}}\n')
                ch.sendall(b'{"id":"org.makineceviri.tv","statusValue":13,"details":{"state":"installing : start"}}\n')
                if tv.install_result == "installed":
                    ch.sendall(b'{"id":"org.makineceviri.tv","statusValue":30,"details":{"state":"installed"}}\n')
                else:
                    ch.sendall(b'{"details":{"state":"FAILED","reason":"Bozuk paket"},"returnValue":true}\n')
            while not ch.closed:                          # -i: istemci kapatana kadar açık kalır
                if ch.recv_ready():
                    ch.recv(1024)
                threading.Event().wait(0.05)
            return
        threading.Event().wait(0.1)                       # gerçek sunucu gibi: önce exec onayı, sonra çıkış
        if "applicationManager/getAppInfo" in cmd:
            ch.sendall(b'{"returnValue":false,"errorCode":-1,"errorText":"Unknown method \\"getAppInfo\\" for category \\"/\\""}\n')
        elif cmd.startswith(f"cat {tv_install.WEBOS_APPS_DIR}/") and tv.installed_version:
            ch.sendall(json.dumps({"id": tv_install.WEBOS_APP, "version": tv.installed_version}, indent=2).encode())
        ch.send_exit_status(0)
        ch.close()


class _Sftp(paramiko.SFTPServerInterface):
    """Yalnızca dosya yazma: yüklenen paket tv.files'a düşer."""

    def __init__(self, server, *args, **kwargs):
        super().__init__(server, *args, **kwargs)
        self.tv = server.tv

    def open(self, path, flags, attr):
        tv, buf = self.tv, bytearray()

        class Handle(paramiko.SFTPHandle):
            def write(self, offset, data):
                buf[offset:offset + len(data)] = data
                return paramiko.SFTP_OK

            def close(self):
                tv.files[path] = bytes(buf)

        return Handle(flags)


def ports_free():
    for p in (tv_install.WEBOS_KEY_PORT, tv_install.WEBOS_SSH_PORT):
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", p)) == 0:
                return False
    return True


@unittest.skipUnless(ports_free(), "9991 / 9922 kapıları dolu")
class InstallWebosTest(unittest.TestCase):
    def setUp(self):
        self.ipk = os.path.join(ROOT, "build", "webos", "test.ipk")
        os.makedirs(os.path.dirname(self.ipk), exist_ok=True)
        with open(self.ipk, "wb") as fh:
            fh.write(b"!<arch>\n" + os.urandom(50000))

    def tearDown(self):
        os.remove(self.ipk)

    def fake(self, **kw):
        tv = FakeLgTv(**kw)
        self.addCleanup(tv.close)
        return tv

    def test_install(self):
        tv = self.fake()
        steps = []
        tv_install.install_webos("127.0.0.1", PASS.lower(), self.ipk, progress=steps.append)  # küçük harf de kabul
        with open(self.ipk, "rb") as fh:
            self.assertEqual(tv.files[tv_install.WEBOS_REMOTE], fh.read())
        kinds = [c.split()[0] for c in tv.commands]
        self.assertEqual(kinds, ["cat", "/usr/bin/luna-send-pub", "rm", "/usr/bin/luna-send-pub"])
        self.assertIn("applicationManager/launch", tv.commands[-1])
        self.assertIn("Makine TV açılıyor…", steps)

    def test_install_silent_stream(self):
        # Kurulum akışı hiç yanıt vermese de (bazı TV'ler) getAppInfo ile kurulduğu anlaşılır
        tv = self.fake(silent=True)
        t0 = time.monotonic()
        tv_install.install_webos("127.0.0.1", PASS, self.ipk)
        self.assertLess(time.monotonic() - t0, 30, "akış sessizken 15 sn'de bir TV'ye sorulmalı")
        self.assertTrue(any("appinfo.json" in c for c in tv.commands))

    def test_not_installed_shows_tv_output(self):
        self.fake(silent=True, install_result="failed")
        with self.assertRaisesRegex(tv_install.InstallError, "bitirmedi"):
            tv_install.install_webos("127.0.0.1", PASS, self.ipk, stream_timeout=20)

    def test_install_modern_ssh(self):
        # Yeni webOS sürümlerinde daha güncel SSH: rsa-sha2 ile ilk denemede girilir
        tv = self.fake(old_ssh=False)
        tv_install.install_webos("127.0.0.1", PASS, self.ipk)
        self.assertIn(tv_install.WEBOS_REMOTE, tv.files)

    def test_wrong_passphrase(self):
        self.fake()
        with self.assertRaisesRegex(tv_install.InstallError, "Parola"):
            tv_install.install_webos("127.0.0.1", "ZZZZZZ", self.ipk)

    def test_install_rejected(self):
        self.fake(install_result="failed")
        with self.assertRaisesRegex(tv_install.InstallError, "reddetti: Bozuk paket"):
            tv_install.install_webos("127.0.0.1", PASS, self.ipk)

    def test_key_server_off(self):
        with self.assertRaisesRegex(tv_install.InstallError, "Key Server"):
            tv_install.install_webos("127.0.0.1", PASS, self.ipk)


if __name__ == "__main__":
    unittest.main(verbosity=2)
