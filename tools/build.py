"""
Makine Live — tek komutla sürüm: (isteğe bağlı) sürüm numarası → Makine TV APK → DLL → testler → .mkplugin.

    build.cmd                        çift tıklayın: yeni sürüm numarasını sorar (boş = aynı sürüm)
    py tools/build.py                aynı sürümle derle
    py tools/build.py 0.4.1          sürümü her yerde 0.4.1 yap, sonra derle
    py tools/build.py --hizli        testleri atla (yalnızca APK + DLL + paket)

Sürüm şu dosyalarda birlikte değişir: manifest.json (CMake ve DLL oradan okur), tv-webos/app/appinfo.json,
tv-webos/service/package.json (Makine TV Android sürümü manifest'ten okunur). Yeni sürüm son sürümden büyük olmalı;
sunucu küçük ya da aynı numarayı kabul etmez.

Makine TV APK'sı tv/gradlew.bat assembleRelease ile derlenir (tv/local.properties ya da ANDROID_HOME Android SDK'yı
gösterir). İmza: MAKINE_TV_KEYSTORE tanımlıysa o anahtar, değilse bu bilgisayarın ~/.android/debug.keystore'u; TV'lerde
kurulu sürümün üzerine güncelleme ancak aynı anahtarla olur.

Sonunda eklenti paketinin yeri (build/org.makineceviri.live-X.Y.Z.mkplugin), boyutu, SHA-256'sı ve sürüm notu dosyası
(docs/surum-notlari/vX.Y.Z.txt) yazılır.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TV = os.path.join(ROOT, "tv")
TV_APK = os.path.join(TV, "app", "build", "outputs", "apk", "release", "app-release.apk")
PACKAGE = os.path.join(BUILD, "package")

# Derlemeden sonra çalışan testler (ayrıntı: tests/README.md). LG servis testi Node.js ister; yoksa atlanır.
TESTS = ["test_normalization", "test_garbage", "test_local_pool", "test_subtitle_scheduler", "test_game_library",
         "test_console_mode", "test_tv_link", "test_subtitle_web", "test_discord_presence", "test_telemetry", "test_app_update",
         "test_webos_install"]
# test_plugin / test_dll_server ABI 1 dışa aktarımlarını (makine_initialize…) yükler; DLL artık yalnızca
# makine_plugin_entry (ABI 2) verir. Yerlerine: eklenti gerçek makine-plugin-host.exe'de açılır (plugin_smoke).
NODE_TESTS = ["test_webos_service"]

# Sürümün yazılı olduğu yerler: (dosya, desen; 1. grup sürüm)
VERSION_SPOTS = [
    ("manifest.json", r'"version":\s*"([0-9.]+)"'),
    ("tv-webos/app/appinfo.json", r'"version":\s*"([0-9.]+)"'),
    ("tv-webos/service/package.json", r'"version":\s*"([0-9.]+)"'),
]


def say(msg=""):
    print(msg, flush=True)


def step(title):
    say(f"\n── {title} " + "─" * max(0, 60 - len(title)))


def fail(msg) -> int:
    say(f"\nHata: {msg}")
    return 1


def current_version() -> str:
    with open(os.path.join(ROOT, "manifest.json"), encoding="utf-8") as fh:
        return json.load(fh)["version"]


def vtuple(v: str):
    return tuple(int(p) for p in v.split("."))


def set_version(new: str):
    for rel, pat in VERSION_SPOTS:
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8", newline="") as fh:
            text = fh.read()
        m = re.search(pat, text)
        if not m:
            raise SystemExit(fail(f"{rel}: sürüm satırı bulunamadı"))
        text = text[:m.start(1)] + new + text[m.end(1):]
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        say(f"  {rel}")


def find_tool(name, *fallbacks):
    found = shutil.which(name)
    if found:
        return found
    for p in fallbacks:
        if os.path.isfile(p):
            return p
    return None


def run(args, **kw) -> int:
    return subprocess.call(args, cwd=kw.pop("cwd", ROOT), **kw)


def build_dll() -> int:
    cmake = find_tool("cmake", r"C:\Qt\Tools\CMake_64\bin\cmake.exe", r"C:\Program Files\CMake\bin\cmake.exe")
    if not cmake:
        return fail("CMake bulunamadı (CMake ≥ 3.25 kurun ya da PATH'e ekleyin).")
    # OCR çalışma zamanı, modeller ve onnxruntime başlığı (git'te yok); ikinci çalıştırma hiçbir şey indirmez
    if not os.path.isfile(os.path.join(ROOT, ".cache", "ocr", "include", "onnxruntime_c_api.h")):
        step("OCR çalışma zamanı indiriliyor (tools/fetch_ocr.py)")
        if run([sys.executable, os.path.join(ROOT, "tools", "fetch_ocr.py")]) != 0:
            return fail("OCR dosyaları indirilemedi (çıktıya bakın).")
    # build.ninja yoksa yapılandırma yarım kalmış demektir (başarısız bir yapılandırma da CMakeCache.txt bırakır)
    if not os.path.isfile(os.path.join(BUILD, "build.ninja")):
        # CMake'in testleri bu Python'la çalıştırması (paramiko vb. buraya kurulu; webos_install testi ister)
        args = [cmake, "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DPython3_EXECUTABLE={sys.executable}"]
        # GCC 15 (scoop); PATH'teki daha eski bir MinGW C++23'ü tam desteklemeyebilir
        gxx = os.path.join(os.path.expanduser("~"), "scoop", "apps", "gcc", "current", "bin", "g++.exe")
        if os.path.isfile(gxx):
            args.append(f"-DCMAKE_CXX_COMPILER={gxx}")
        ninja = find_tool("ninja", r"C:\Qt\Tools\Ninja\ninja.exe")
        if ninja:
            args.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
        if run(args) != 0:
            return fail("CMake yapılandırması başarısız (çıktıya bakın).")
    if run([cmake, "--build", "build"]) != 0:
        return fail("DLL derlenemedi (çıktıya bakın).")
    return 0


def run_tests() -> int:
    ctest = find_tool("ctest", r"C:\Qt\Tools\CMake_64\bin\ctest.exe", r"C:\Program Files\CMake\bin\ctest.exe")
    failed = []
    if ctest:
        say("  ctest (C++ testleri)")
        if run([ctest, "--test-dir", "build", "--output-on-failure"], stdout=subprocess.DEVNULL) != 0:
            failed.append("ctest")
    tests = TESTS + (NODE_TESTS if shutil.which("node") else [])
    for name in tests:
        t0 = time.time()
        proc = subprocess.run([sys.executable, os.path.join("tests", name + ".py")], cwd=ROOT,
                              capture_output=True, text=True, encoding="utf-8", errors="replace")
        ok = proc.returncode == 0
        say(f"  {'tamam' if ok else 'HATA '}  {name}  ({time.time() - t0:.1f} sn)")
        if not ok:
            failed.append(name)
            say("\n".join("        " + ln for ln in (proc.stdout + proc.stderr).strip().splitlines()[-15:]))
    if failed:
        return fail("başarısız testler: " + ", ".join(failed) + "\nTestleri atlamak için:  py tools/build.py --hizli")
    return 0


def build_tv_apk() -> int:
    if not os.path.isfile(os.path.join(TV, "local.properties")) and not (
            os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")):
        return fail("Android SDK bulunamadı: tv/local.properties'e sdk.dir=<SDK klasörü> yazın ya da ANDROID_HOME tanımlayın.")
    if os.environ.get("MAKINE_TV_KEYSTORE"):
        say(f"  imza: {os.environ['MAKINE_TV_KEYSTORE']}")
    else:
        # Anahtar yoksa Gradle yenisini üretir: APK TV'lerde kurulu sürümün üzerine kurulamaz
        debug_key = os.path.join(os.path.expanduser("~"), ".android", "debug.keystore")
        if not os.path.isfile(debug_key):
            return fail(f"{debug_key} yok. Yedeğini geri koyun ya da MAKINE_TV_KEYSTORE tanımlayın; yeni bir anahtarla "
                        "imzalanan APK TV'lerdeki sürümün üzerine kurulamaz.")
        say(f"  imza: {debug_key}")
    if run([os.path.join(TV, "gradlew.bat"), "assembleRelease", "--console=plain", "-q"], cwd=TV) != 0:
        return fail("Makine TV APK derlenemedi (çıktıya bakın).")
    if not os.path.isfile(TV_APK):
        return fail(f"Gradle bitti ama {TV_APK} yok.")
    return 0


def find_host():
    candidates = [os.environ.get("MAKINE_PLUGIN_HOST", ""),
                  os.path.join(os.path.dirname(ROOT), "Makine-Launcher", "build", "dev", "makine-plugin-host.exe"),
                  r"C:\Makine-Launcher\build\dev\makine-plugin-host.exe"]
    return next((c for c in candidates if c and os.path.isfile(c)), None)


def plugin_smoke() -> int:
    """build/package'ı gerçek makine-plugin-host.exe'de açar: ABI 2 el sıkışması, state, TV paketleri."""
    host = find_host()
    if not host:
        say("  atlandı  eklenti host'ta  (makine-plugin-host.exe yok; MAKINE_PLUGIN_HOST ile gösterin)")
        return 0
    t0 = time.time()
    try:
        proc = subprocess.run([sys.executable, os.path.join("tools", "plugin_dev.py"), PACKAGE, "--host", host,
                               "--invoke", "state"], cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=120)
        out = proc.stdout + proc.stderr
        problems = [label for label, ok in [
            ("host hata koduyla çıktı", proc.returncode == 0),
            ("ABI 2 el sıkışması yok", '"abi": 2' in out),
            ("Android TV APK pakette yok", '"apkAvailable": true' in out),
            ("LG paketi pakette yok", '"ipkAvailable": true' in out)] if not ok]
    except subprocess.TimeoutExpired:
        out, problems = "", ["host 2 dakikada kapanmadı"]
    say(f"  {'tamam' if not problems else 'HATA '}  eklenti host'ta  ({time.time() - t0:.1f} sn)")
    if problems:
        say("\n".join("        " + ln for ln in problems + out.strip().splitlines()[-10:]))
        return 1
    return 0


def pack(version):
    cmake = find_tool("cmake", r"C:\Qt\Tools\CMake_64\bin\cmake.exe", r"C:\Program Files\CMake\bin\cmake.exe")
    if run([cmake, "--build", "build", "--target", "pack"]) != 0:
        return None, fail("eklenti paketlenemedi (çıktıya bakın).")
    missing = [f for f in ("makine-tv.apk", "makine-tv.ipk") if not os.path.isfile(os.path.join(PACKAGE, "resources", f))]
    if missing:
        return None, fail("pakette eksik: " + ", ".join(missing))
    out = os.path.join(BUILD, f"org.makineceviri.live-{version}.mkplugin")
    if not os.path.isfile(out):
        return None, fail(f"{out} oluşmadı.")
    return out, 0


def sha256(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = sys.argv[1:]
    quick = "--hizli" in args
    ask = "--sor" in args
    wanted = next((a for a in args if not a.startswith("-")), "").lstrip("v")
    t_start = time.time()

    cur = current_version()
    if ask and not wanted:
        say(f"Şu anki sürüm: {cur}")
        try:
            wanted = input("Yeni sürüm numarası (örnek 0.4.1; boş bırakırsanız aynı kalır): ").strip().lstrip("v")
        except EOFError:
            wanted = ""
    if wanted and wanted != cur:
        if not re.fullmatch(r"\d+\.\d+\.\d+", wanted):
            return fail(f"sürüm X.Y.Z biçiminde olmalı (verilen: {wanted})")
        if vtuple(wanted) < vtuple(cur):
            return fail(f"yeni sürüm ({wanted}) şu ankinden ({cur}) küçük olamaz")
        step(f"Sürüm {cur} → {wanted}")
        set_version(wanted)
        cur = wanted

    step(f"Makine TV APK derleniyor ({cur})")
    if build_tv_apk():
        return 1

    step(f"DLL derleniyor (Makine Live {cur})")
    if build_dll():
        return 1

    if not quick:
        step("Testler")
        failed = run_tests()
        if plugin_smoke():
            failed = failed or fail("eklenti host'ta açılamadı\nTestleri atlamak için:  py tools/build.py --hizli")
        if failed:
            return 1

    step("Eklenti paketleniyor")
    out, err = pack(cur)
    if err:
        return 1

    notes = os.path.join("docs", "surum-notlari", f"v{cur}.txt")
    step("Hazır")
    say(f"  Dosya    {out}")
    say(f"  Sürüm    {cur}")
    say(f"  Boyut    {os.path.getsize(out) / 1048576:.1f} MB")
    say(f"  SHA-256  {sha256(out)}")
    say(f"  TV APK   {sha256(TV_APK)[:16]}…  (pakette resources/makine-tv.apk)")
    say(f"  Notlar   {notes}" + ("" if os.path.isfile(os.path.join(ROOT, notes)) else "  (YOK: yayımlamadan önce yazın)"))
    say(f"  Süre     {time.time() - t_start:.0f} sn")
    return 0


if __name__ == "__main__":
    sys.exit(main())
