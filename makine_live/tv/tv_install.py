#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
"TV'ye kur" [TEST]: Makine TV'yi PC'den ağ üzerinden kurar — Android TV (install) ve LG webOS (install_webos).

LG webOS: TV'de LG'nin "Developer Mode" uygulaması açık (Dev Mode Status ve Key Server açık) olmalı. PC, TV'nin
geliştirici anahtarını http://<TV>:9991/webos_rsa adresinden alır, Developer Mode ekranındaki 6 karakterlik parola
(Passphrase) ile çözer, SSH ile (9922, kullanıcı prisoner) bağlanır, .ipk'yı /media/developer/temp'e yükler ve
appInstallService/dev/install ile kurar (LG'nin ares-install aracının yaptığı). paramiko (LGPL-2.1) kullanılır.

Android TV:

TV'de bir kez "Ağ üzerinden hata ayıklama" (ADB, 5555) açılır; PC uygulaması:
  1. TV'ye bağlanır (ilk seferde TV ekranında "Bu bilgisayara izin verilsin mi?" sorusu çıkar → İzin ver),
  2. APK'yı yükler ve kurar,
  3. altyazı katmanı iznini verir (appops SYSTEM_ALERT_WINDOW; TV menülerinde bu ayar çoğu zaman gizlidir),
  4. uygulamayı açar.
ADB, adb-shell kütüphanesiyle (Apache-2.0, saf Python) konuşulur; ayrı bir adb.exe gerekmez. PC'nin ADB anahtarı
%APPDATA%\\MakineLive\\adbkey içinde durur (TV bu anahtarı bir kez onaylar).

Yedek yol: APK'yı USB belleğe kaydedip TV'nin dosya yöneticisiyle kurmak (apk_path() ile dosya bulunur).
"""

from __future__ import annotations

import concurrent.futures
import ipaddress
import os
import socket
import sys
from typing import Callable, List, Optional

from makine_live.paths import app_data_dir, bundled

PACKAGE = "org.makineceviri.tv"
ADB_PORT = 5555
REMOTE_APK = "/data/local/tmp/makine-tv.apk"


def apk_path() -> str:
    """Pakete gömülü APK (exe) ya da geliştirmede Gradle çıktısı. Yoksa ''."""
    return bundled(os.path.join("runtime", "makine-tv.apk"),
                   os.path.join("tv", "app", "build", "outputs", "apk", "release", "app-release.apk"))


def ipk_path() -> str:
    """Pakete gömülü LG paketi (exe) ya da geliştirmede tools/build_webos.py çıktısı. Yoksa ''."""
    return bundled(os.path.join("runtime", "makine-tv.ipk"), os.path.join("build", "webos", "makine-tv.ipk"))


def _local_networks() -> List[ipaddress.IPv4Network]:
    nets = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = str(info[4][0])
            if ip.startswith(("127.", "169.254.")):
                continue
            net = ipaddress.ip_network(f"{ip}/24", strict=False)
            if net not in nets:
                nets.append(net)
    except OSError:
        pass
    return nets


def _scan(port: int, timeout: float) -> List[str]:
    hosts = [str(h) for net in _local_networks() for h in net.hosts()]

    def probe(h):
        try:
            with socket.create_connection((h, port), timeout=timeout):
                return h
        except OSError:
            return None

    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
        return [h for h in ex.map(probe, hosts) if h]


def find_adb_hosts(timeout: float = 0.35) -> List[str]:
    """Ev ağında (/24) ADB kapısı açık cihazlar: kurulum ekranında TV'nin adresi kendiliğinden dolsun."""
    return _scan(ADB_PORT, timeout)


WEBOS_SSH_PORT = 9922
WEBOS_KEY_PORT = 9991
WEBOS_APP = "org.makineceviri.tv"
WEBOS_REMOTE = "/media/developer/temp/makine-tv.ipk"
WEBOS_APPS_DIR = "/media/developer/apps/usr/palm/applications"   # geliştirici modu kurulumları (installBasePath)


def find_webos_hosts(timeout: float = 0.35) -> List[str]:
    """Ev ağında geliştirici modu açık LG TV'ler (SSH 9922)."""
    return _scan(WEBOS_SSH_PORT, timeout)


def _ipk_version(ipk: str) -> str:
    """.ipk'nın control dosyasındaki Version (ar → control.tar.gz → control). Okunamazsa ''."""
    import gzip
    import io
    import tarfile
    try:
        with open(ipk, "rb") as fh:
            data = fh.read()
        pos = 8
        while pos + 60 <= len(data):
            name, size = data[pos:pos + 16].decode("ascii").strip().rstrip("/"), int(data[pos + 48:pos + 58])
            body = data[pos + 60:pos + 60 + size]
            if name == "control.tar.gz":
                with tarfile.open(fileobj=io.BytesIO(gzip.decompress(body))) as tar:
                    for m in tar.getmembers():
                        if m.name.lstrip("./") == "control":
                            text = tar.extractfile(m).read().decode("utf-8", "replace")
                            for line in text.splitlines():
                                if line.startswith("Version:"):
                                    return line.split(":", 1)[1].strip()
            pos += 60 + size + (size % 2)
    except (OSError, ValueError, tarfile.TarError, AttributeError):
        pass
    return ""


def _webos_ssh(host: str, key):
    """
    Geliştirici modundaki TV'ye SSH (prisoner@9922). Eski LG'lerde sunucu OpenSSH 6.x'tir: hangi imzaları kabul ettiğini
    bildirmez (server-sig-algs yok) ve yalnızca ssh-rsa (SHA-1) imzasını tanır; paramiko ise varsayılan olarak rsa-sha2
    dener ve "Authentication failed" alır. Önce varsayılanla (yeni sunucular ssh-rsa'yı kapatmış olabilir), olmazsa
    yalnızca ssh-rsa ile yeniden bağlanılır.
    """
    import paramiko
    last = None
    for disabled in (None, {"pubkeys": ["rsa-sha2-512", "rsa-sha2-256"]}):
        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())   # TV'nin anahtarı her geliştirici modu açılışında değişir
        try:
            ssh.connect(host, WEBOS_SSH_PORT, username="prisoner", pkey=key, look_for_keys=False, allow_agent=False,
                        timeout=10, banner_timeout=10, auth_timeout=10, disabled_algorithms=disabled)
            return ssh
        except paramiko.AuthenticationException as e:
            last = e
            ssh.close()
        except (OSError, paramiko.SSHException) as e:
            ssh.close()
            raise InstallError(f"TV'ye bağlanılamadı: {e}")
    raise InstallError(f"TV bağlantıyı kabul etmedi ({last}). Developer Mode'da Key Server'ı kapatıp açın ve "
                       "ekrandaki yeni Passphrase ile tekrar deneyin.")


def install_webos(host: str, passphrase: str, ipk: str = "", progress: Optional[Callable[[str], None]] = None,
                  stream_timeout: float = 150.0) -> str:
    """Makine TV'yi geliştirici modundaki LG TV'ye kurar ve açar. Hata: InstallError (Türkçe, kullanıcıya gösterilir)."""
    import io
    import json
    import time
    import urllib.request
    import paramiko

    def step(msg):
        if progress:
            progress(msg)

    host = host.strip()
    ipk = ipk or ipk_path()
    if not ipk:
        raise InstallError("Makine TV'nin LG paketi bulunamadı (makine-tv.ipk).")
    step("TV'nin geliştirici anahtarı alınıyor…")
    try:
        with urllib.request.urlopen(f"http://{host}:{WEBOS_KEY_PORT}/webos_rsa", timeout=6) as r:
            pem = r.read().decode("ascii", "replace")
    except OSError as e:
        raise InstallError(f"TV'ye ulaşılamadı ({host}). Developer Mode uygulamasında Dev Mode Status ve Key Server "
                           f"açık mı, adres doğru mu? ({e})")
    key = None
    for pw in dict.fromkeys((passphrase.strip(), passphrase.strip().upper())):
        try:
            key = paramiko.RSAKey.from_private_key(io.StringIO(pem), password=pw or None)
            break
        except (paramiko.SSHException, ValueError):
            continue
    if key is None:
        raise InstallError("Parola (Passphrase) yanlış. Developer Mode uygulamasında yazan 6 karakteri girin.")

    step("TV'ye bağlanılıyor…")
    ssh = _webos_ssh(host, key)

    def session(cmd):
        """Komut kanalı: stderr stdout'a katılır. pty İSTENMEZ (LG'deki SSH sunucusu pty isteğinde kanalı kapatıyor)."""
        ch = ssh.get_transport().open_session()
        ch.set_combine_stderr(True)
        ch.exec_command(cmd)
        return ch

    def run(cmd, timeout=15.0):
        """Komutu çalıştırıp bitmesini bekler (bağlantı erken kapanırsa TV'de komut yarıda kalır). (çıkış kodu, çıktı)"""
        ch = session(cmd)
        ch.settimeout(timeout)
        out = b""
        try:
            while True:
                data = ch.recv(4096)
                if not data:
                    break
                out += data
            return ch.recv_exit_status(), out.decode("utf-8", "replace")
        except OSError:
            return -1, out.decode("utf-8", "replace")
        finally:
            ch.close()

    want_version = _ipk_version(ipk)

    def installed() -> bool:
        """Makine TV (bu sürüm) TV'de kurulu mu? Uygulamanın kendi appinfo.json'una bakılır
        (applicationManager/getAppInfo webOS 3.x'te yok)."""
        _, out = run(f"cat {WEBOS_APPS_DIR}/{WEBOS_APP}/appinfo.json 2>/dev/null", timeout=10.0)
        compact = out.replace(" ", "")
        return f'"id":"{WEBOS_APP}"' in compact and (not want_version or f'"version":"{want_version}"' in compact)

    try:
        return _install_webos_steps(ssh, ipk, step, session, run, installed, stream_timeout)
    except InstallError:
        raise
    except Exception as e:  # noqa: BLE001 — beklenmeyen her hata kullanıcıya mesaj olarak gider (pencere donmasın)
        raise InstallError(f"Kurulum yarıda kaldı: {type(e).__name__}: {e}") from e
    finally:
        ssh.close()


def _install_webos_steps(ssh, ipk, step, session, run, installed, stream_timeout) -> str:
    """Yükle → kur (akış izlenir; sessizse 15 sn'de bir dosyaya bakılır) → paketi sil → aç."""
    import json
    import time
    import paramiko

    step("Uygulama yükleniyor…")
    try:
        with ssh.open_sftp() as sftp:
            sftp.put(ipk, WEBOS_REMOTE)
    except (OSError, paramiko.SSHException):
        with open(ipk, "rb") as fh:                          # SFTP kapalıysa: cat ile yaz
            ch = session(f"cat > {WEBOS_REMOTE}")
            ch.sendall(fh.read())
            ch.shutdown_write()
            ch.recv_exit_status()
            ch.close()

    step("Uygulama kuruluyor…")
    payload = json.dumps({"id": WEBOS_APP, "ipkUrl": WEBOS_REMOTE, "subscribe": True})
    ch = session(f"/usr/bin/luna-send-pub -i 'luna://com.webos.appInstallService/dev/install' '{payload}'")
    ch.settimeout(2.0)
    raw, buf, result = "", "", None
    start = time.monotonic()
    next_check = start + 15
    while result is None and time.monotonic() < start + stream_timeout:
        try:
            chunk = ch.recv(4096).decode("utf-8", "replace")
        except OSError:
            chunk = ""
        if not chunk and ch.exit_status_ready():
            break
        raw += chunk
        buf += chunk
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            try:
                msg = json.loads(line.strip())
            except ValueError:
                continue
            st = str((msg.get("details") or {}).get("state") or msg.get("state") or "")
            if msg.get("returnValue") is False or "FAIL" in st.upper():
                result = "fail:" + (msg.get("errorText") or (msg.get("details") or {}).get("reason") or st or line)
            elif st.lower() == "installed" or "SUCCESS" in st.upper():
                result = "ok"
            elif st:
                step(f"Uygulama kuruluyor… ({st})")
        if result is None and time.monotonic() >= next_check:   # akış sessizse uygulamanın dosyasına bak
            next_check = time.monotonic() + 10
            if installed():
                result = "ok"
    ch.close()
    if result is None and installed():
        result = "ok"
    run(f"rm -f {WEBOS_REMOTE}")
    if result is None:
        said = " ".join(raw.split())[-300:]
        raise InstallError("TV kurulumu bitirmedi. " + (f"TV'nin yanıtı: {said}" if said else
                           "TV hiç yanıt vermedi. TV'yi kapatıp açın (fişten çekmeden) ve tekrar deneyin."))
    if result != "ok":
        raise InstallError(f"TV kurulumu reddetti: {result[5:][:200]}")

    step("Makine TV açılıyor…")
    run(f"/usr/bin/luna-send-pub -n 1 'luna://com.webos.applicationManager/launch' '{json.dumps({'id': WEBOS_APP})}'")
    return ""


class InstallError(Exception):
    pass


def _signer(key: str = ""):
    from adb_shell.auth.keygen import keygen
    from adb_shell.auth.sign_pythonrsa import PythonRSASigner
    key = key or os.path.join(app_data_dir(), "adbkey")
    if not os.path.isfile(key):
        os.makedirs(os.path.dirname(key), exist_ok=True)
        keygen(key)
    with open(key, encoding="utf-8") as fh:
        priv = fh.read()
    with open(key + ".pub", encoding="utf-8") as fh:
        pub = fh.read()
    return PythonRSASigner(pub, priv)


def install(host: str, apk: str = "", progress: Optional[Callable[[str], None]] = None,
            port: int = ADB_PORT, key_path: str = "") -> str:
    """
    APK'yı TV'ye kurar ve açar. Dönüş: kurulan sürüm adı. Hata: InstallError (kullanıcıya gösterilecek Türkçe metin).
    progress(metin) her adımda çağrılır (iş parçacığından; çağıran arayüze taşır).
    key_path: ADB özel anahtarı (testlerde emülatörün tanıdığı anahtar); boşsa uygulamanın kendi anahtarı.
    """
    from adb_shell.adb_device import AdbDeviceTcp
    from adb_shell.exceptions import AdbConnectionError, AdbTimeoutError, DeviceAuthError

    def step(msg):
        if progress:
            progress(msg)

    apk = apk or apk_path()
    if not apk:
        raise InstallError("Makine TV paketi bulunamadı (makine-tv.apk).")
    step("TV'ye bağlanılıyor…")
    dev = AdbDeviceTcp(host.strip(), port, default_transport_timeout_s=10.0)
    try:
        try:
            step("TV'ye bağlanılıyor… TV'de \"hata ayıklamaya izin ver\" sorusu çıkarsa İzin ver'i seçin.")
            dev.connect(rsa_keys=[_signer(key_path)], auth_timeout_s=60.0)
        except DeviceAuthError:
            raise InstallError("TV bağlantıyı onaylamadı. TV ekranındaki soruya \"İzin ver\" deyip tekrar deneyin.")
        except (OSError, AdbConnectionError, AdbTimeoutError) as e:
            raise InstallError(f"TV'ye bağlanılamadı ({host}:{port}). TV'de \"Ağ üzerinden hata ayıklama\" açık mı, "
                               f"adres doğru mu? ({e})")
        step("Uygulama yükleniyor…")
        dev.push(apk, REMOTE_APK, read_timeout_s=60.0)
        step("Uygulama kuruluyor…")
        out = str(dev.shell(f"pm install -r {REMOTE_APK}", read_timeout_s=120.0) or "")
        if "UPDATE_INCOMPATIBLE" in out or "INSTALL_FAILED_VERSION_DOWNGRADE" in out:
            # TV'de başka bir imzayla (ör. geliştirme sürümü) ya da daha yeni bir sürüm kurulu: kaldırıp yeniden kur.
            # Makine TV'nin tek kalıcı verisi eşleşme kodudur; yenisi ekranda görünür.
            step("Eski Makine TV kaldırılıyor…")
            dev.shell(f"pm uninstall {PACKAGE}", read_timeout_s=60.0)
            out = str(dev.shell(f"pm install {REMOTE_APK}", read_timeout_s=120.0) or "")
        dev.shell(f"rm -f {REMOTE_APK}")
        if "Success" not in out:
            raise InstallError(f"TV kurulumu reddetti: {out.strip()[:200] or 'yanıt yok'}")
        step("Altyazı katmanı izni veriliyor…")
        dev.shell(f"appops set {PACKAGE} SYSTEM_ALERT_WINDOW allow")
        mode = str(dev.shell(f"appops get {PACKAGE} SYSTEM_ALERT_WINDOW") or "")
        overlay_ok = "allow" in mode.lower()
        step("Makine TV açılıyor…")
        dev.shell(f"am start -n {PACKAGE}/.MainActivity")
        if not overlay_ok:
            low_ram = str(dev.shell("getprop ro.config.low_ram") or "").strip() == "true"
            raise InstallError("Makine TV kuruldu, ama bu TV altyazı katmanına izin vermedi"
                               + (" (düşük bellekli cihaz)" if low_ram else "") + ". Altyazıyı telefonda okumak için "
                               "Konsol › Altyazı nerede görünsün › Telefonda'yı seçin.")
        ver = str(dev.shell(f"dumpsys package {PACKAGE} | grep versionName") or "").strip()
        return ver.split("=", 1)[-1] if "=" in ver else ""
    except InstallError:
        raise
    except Exception as e:  # noqa: BLE001 — adb-shell'in iç hataları kullanıcıya tek tip mesajla gider
        raise InstallError(f"Kurulum yarıda kaldı: {type(e).__name__}: {e}") from e
    finally:
        try:
            dev.close()
        except Exception:  # noqa: BLE001
            pass


if __name__ == "__main__":
    if len(sys.argv) > 1:
        print(install(sys.argv[1], progress=print, port=int(sys.argv[2]) if len(sys.argv) > 2 else ADB_PORT,
                      key_path=sys.argv[3] if len(sys.argv) > 3 else ""))
    else:
        print("APK:", apk_path() or "yok")
        print("ADB açık cihazlar:", find_adb_hosts() or "yok")
