#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Makine TV'yi resmî Android TV / Google TV emülatörlerinde uçtan uca dener (gerçek TV yokken).

Her imaj için: emülatörü sıfırdan açar → PC uygulamasının kendi kurulum yoluyla (tv_install.install, ADB) kurar ve
katman iznini verir → TV ekranındaki eşleşme kodunu okur (uiautomator) → yanlış kodun reddedildiğini dener → tv_link ile
eşleşir → ana ekrana döner (uygulama arka planda) → altyazı gönderir → ekran görüntüsü alır ve katmanın çizildiğini
denetler → altyazıyı temizler. Sonuçlar ve görüntüler build/tv-check/ altına yazılır.

Gerekenler: Android SDK (emulator, platform-tools) ve imajlar, ör.:
  sdkmanager "system-images;android-34;google-tv;x86"
Kullanım:
  py tools/tv_emulator_check.py                 # bütün imajlar
  py tools/tv_emulator_check.py 34-google-tv    # yalnızca biri
"""

from __future__ import annotations

import io
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import tv_install  # noqa: E402
from tv_link import TvInfo, TvLink  # noqa: E402

SDK = os.environ.get("ANDROID_SDK_ROOT") or os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")
ADB = os.path.join(SDK, "platform-tools", "adb.exe")
EMULATOR = os.path.join(SDK, "emulator", "emulator.exe")
AVD_HOME = os.path.join(os.path.expanduser("~"), ".android", "avd")
ADB_KEY = os.path.join(os.path.expanduser("~"), ".android", "adbkey")
OUT = os.path.join(ROOT, "build", "tv-check")

# (ad, API, etiket, mimari): Android TV 8 (en düşük desteklenen), 9 ve 12 (piyasadaki TV'lerin çoğu), Google TV 14 ve 16
IMAGES = [
    ("26-android-tv", 26, "android-tv", "x86"),
    ("28-android-tv", 28, "android-tv", "x86"),
    ("31-android-tv", 31, "android-tv", "x86"),
    ("34-google-tv", 34, "google-tv", "x86"),
    ("36-google-tv", 36, "google-tv", "x86_64"),
]
CONSOLE_PORT = 5570                 # emülatör konsolu; ADB = +1
SERIAL = f"emulator-{CONSOLE_PORT}"
HOST_TCP = 47811                    # PC tarafı yönlendirme (Makine Live açıksa 47711 ile çakışmasın)
LINE = "Makine TV denemesi: altyazı katmanı çalışıyor."


def adb(*args, timeout=60, check=False) -> str:
    r = subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, timeout=timeout)
    out = (r.stdout or b"").decode("utf-8", "replace")
    if check and r.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)}: {(r.stderr or b'').decode('utf-8', 'replace')}")
    return out


def make_avd(name, api, tag, abi) -> str:
    avd = f"MakineTvCheck_{name.replace('-', '_')}"
    sysdir = os.path.join("system-images", f"android-{api}", tag, abi) + os.sep
    if not os.path.isdir(os.path.join(SDK, sysdir)):
        raise RuntimeError(f"imaj yok: {sysdir} (sdkmanager \"system-images;android-{api};{tag};{abi}\")")
    d = os.path.join(AVD_HOME, avd + ".avd")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(AVD_HOME, avd + ".ini"), "w", encoding="utf-8") as fh:
        fh.write(f"avd.ini.encoding=UTF-8\npath={d}\npath.rel=avd\\{avd}.avd\ntarget=android-{api}\n")
    cfg = {
        "AvdId": avd, "avd.ini.displayname": avd, "PlayStore.enabled": "false", "abi.type": abi,
        "hw.cpu.arch": "x86_64" if abi == "x86_64" else "x86", "hw.cpu.ncore": "4",
        "image.sysdir.1": sysdir, "tag.id": tag, "hw.device.name": "tv_1080p",
        "hw.lcd.width": "1920", "hw.lcd.height": "1080", "hw.lcd.density": "320",
        "hw.initialOrientation": "landscape", "hw.ramSize": "2048", "disk.dataPartition.size": "4G",
        "hw.keyboard": "yes", "hw.dPad": "yes", "hw.gpu.enabled": "yes", "hw.gpu.mode": "swiftshader_indirect",
        "fastboot.forceColdBoot": "yes",
    }
    with open(os.path.join(d, "config.ini"), "w", encoding="utf-8") as fh:
        fh.writelines(f"{k} = {v}\n" for k, v in cfg.items())
    return avd


def boot(avd) -> subprocess.Popen:
    proc = subprocess.Popen([EMULATOR, "-avd", avd, "-port", str(CONSOLE_PORT), "-no-window", "-no-audio",
                             "-no-snapshot", "-no-boot-anim", "-wipe-data", "-gpu", "swiftshader_indirect"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < 600:
        if proc.poll() is not None:
            raise RuntimeError("emülatör kapandı")
        try:
            if adb("shell", "getprop", "sys.boot_completed", timeout=10).strip() == "1":
                time.sleep(8)          # başlatıcı ve paket yöneticisi otursun
                return proc
        except subprocess.TimeoutExpired:
            pass
        time.sleep(3)
    raise RuntimeError("açılış 10 dakikada bitmedi")


def ui_texts() -> list:
    adb("shell", "uiautomator", "dump", "/sdcard/ui.xml", timeout=30)
    xml = adb("shell", "cat", "/sdcard/ui.xml")
    return re.findall(r'text="([^"]*)"', xml)


def front_window() -> str:
    """Öndeki (odaklı) uygulama penceresi, ör. com.android.tv.settings/.MainSettings."""
    out = adb("shell", "dumpsys", "window", "windows") + adb("shell", "dumpsys", "window")
    m = re.search(r"mCurrentFocus=Window\{[^ ]+ [^ ]+ ([^ }]+)\}", out) or \
        re.search(r"mFocusedApp=.*? ([\w.]+/[\w.$]+)", out)
    return m.group(1) if m else ""


def bad_code_rejected(code) -> bool:
    wrong = "0000" if code != "0000" else "1111"
    with socket.create_connection(("127.0.0.1", HOST_TCP), timeout=8) as s:
        s.sendall((json.dumps({"t": "hello", "v": 1, "code": wrong, "pc": "deneme"}) + "\n").encode())
        s.settimeout(8)
        return b"bad_code" in s.recv(256)


def white_pixels(png_path) -> int:
    """Ekranın alt kısmındaki (altyazı şeridinin yeri) beyaz piksel sayısı. Eşik yüksek: Android'in soldurduğu
    (%80 opak) katmandaki gri yazı sayılmaz."""
    from PIL import Image
    im = Image.open(png_path).convert("RGB")
    w, h = im.size
    band = im.crop((int(w * 0.15), int(h * 0.6), int(w * 0.85), h))
    px = list(band.get_flattened_data()) if hasattr(band, "get_flattened_data") else list(band.getdata())
    return sum(1 for r, g, b in px if r > 235 and g > 235 and b > 235)


def screenshot(path):
    adb("shell", "screencap", "-p", "/sdcard/s.png")
    adb("pull", "/sdcard/s.png", path)
    return white_pixels(path)


def check(name, api, tag, abi) -> dict:
    res = {"image": name}
    proc = None
    link = None
    try:
        avd = make_avd(name, api, tag, abi)
        t0 = time.time()
        proc = boot(avd)
        res["boot_s"] = round(time.time() - t0)
        res["android"] = adb("shell", "getprop", "ro.build.version.release").strip()
        res["low_ram"] = adb("shell", "getprop", "ro.config.low_ram").strip() or "false"

        t0 = time.time()
        res["version"] = tv_install.install("127.0.0.1", port=CONSOLE_PORT + 1, key_path=ADB_KEY)
        res["install_s"] = round(time.time() - t0, 1)
        res["overlay_permission"] = adb("shell", "appops", "get", tv_install.PACKAGE, "SYSTEM_ALERT_WINDOW").strip()
        time.sleep(4)
        texts = ui_texts()
        code = next((t for t in texts if re.fullmatch(r"\d{4}", t)), "")
        res["code_on_screen"] = bool(code)
        res["address_on_screen"] = next((t for t in texts if t.startswith("Bu TV'nin adresi")), "")
        res["permission_card"] = any("Katman izni" in t or "izin vermiyor" in t for t in texts)
        screenshot(os.path.join(OUT, f"{name}-uygulama.png"))

        adb("forward", f"tcp:{HOST_TCP}", "tcp:47711", check=True)
        res["bad_code_rejected"] = bad_code_rejected(code)
        states = []
        link = TvLink("DENEME-PC", on_state=lambda s, d: states.append(s))
        link.pair(TvInfo("emu", name, "127.0.0.1", HOST_TCP), code)
        t1 = time.time()
        while link.state != "connected" and time.time() - t1 < 25:
            time.sleep(0.2)
        res["paired"] = link.state == "connected"
        res["pair_s"] = round(time.time() - t1, 1)

        # Makine TV arka planda, önde başka bir uygulama (gerçekte TV'nin HDMI girişi): katman yine de çizmeli.
        # Önce ana ekran; Android 8'de HOME Makine TV'yi önden almazsa Ayarlar. Android 12+ Ayarlar ekranı uygulama
        # katmanlarını kendisi gizler (güvenlik; HDMI girişinde böyle bir şey yok), bu yüzden orada ölçülmez.
        adb("shell", "input", "keyevent", "KEYCODE_HOME")
        time.sleep(5)
        res["front"] = front_window()
        if not res["front"] or tv_install.PACKAGE in res["front"]:
            adb("shell", "am", "start", "-a", "android.settings.SETTINGS")
            time.sleep(4)
            res["front"] = front_window()
        # Önde duran uygulamanın kendi beyaz yazıları sayılmasın: önce / sırasında / sonra karşılaştırılır
        base = screenshot(os.path.join(OUT, f"{name}-once.png"))
        link.send_line(1, LINE, "verified")
        time.sleep(2.5)
        line = screenshot(os.path.join(OUT, f"{name}-altyazi.png"))
        res["subtitle_over_home"] = line - base > 400
        link.clear()
        time.sleep(1.5)
        after = screenshot(os.path.join(OUT, f"{name}-temiz.png"))
        res["cleared"] = abs(after - base) < 200
        res["white_px"] = [base, line, after]
        res["service_alive"] = "OverlayService" in adb("shell", "dumpsys", "activity", "services", tv_install.PACKAGE)
        res["other_app_in_front"] = bool(res["front"]) and tv_install.PACKAGE not in res["front"]
        res["ok"] = all(res.get(k) for k in ("code_on_screen", "bad_code_rejected", "paired", "other_app_in_front",
                                             "subtitle_over_home", "cleared", "service_alive"))             and "allow" in res["overlay_permission"]
    except Exception as e:  # noqa: BLE001 — her imajın sonucu ayrı raporlanır
        res["error"] = f"{type(e).__name__}: {e}"
        res["ok"] = False
    finally:
        if link is not None:
            link.close()
        try:
            adb("forward", "--remove-all", timeout=10)
            adb("emu", "kill", timeout=20)
        except Exception:  # noqa: BLE001
            pass
        if proc is not None:
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
    return res


def main() -> int:
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", line_buffering=True)
    os.makedirs(OUT, exist_ok=True)
    want = set(sys.argv[1:])
    results = []
    for img in IMAGES:
        if want and img[0] not in want:
            continue
        print(f"── {img[0]} …", flush=True)
        r = check(*img)
        print(json.dumps(r, ensure_ascii=False), flush=True)
        results.append(r)
    with open(os.path.join(OUT, "sonuc.json"), "w", encoding="utf-8") as fh:
        json.dump(results, fh, ensure_ascii=False, indent=1)
    return 0 if results and all(r.get("ok") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
