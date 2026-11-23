"""
Makine Live — tek komutla sürüm: (isteğe bağlı) sürüm numarası → DLL → testler → dist/MakineLive.exe → selftest.

    build.cmd                        çift tıklayın: yeni sürüm numarasını sorar (boş = aynı sürüm)
    py tools/build.py                aynı sürümle derle
    py tools/build.py 0.3.1          sürümü her yerde 0.3.1 yap, sonra derle
    py tools/build.py --hizli        testleri atla (yalnızca DLL + exe + selftest)

Sürüm şu dosyalarda birlikte değişir: manifest.json (CMake ve DLL oradan okur), tv-webos/app/appinfo.json,
tv-webos/service/package.json (Makine TV Android sürümü manifest'ten okunur). Yeni sürüm son sürümden büyük olmalı;
sunucu küçük ya da aynı numarayı kabul etmez.

Sonunda exe'nin yeri, boyutu, SHA-256'sı ve sürüm notu dosyası (docs/surum-notlari/vX.Y.Z.txt) yazılır. Exe'yi
portalda Ayarlar → Genel'den yükleyin.
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
EXE = os.path.join(ROOT, "dist", "MakineLive.exe")

# Derlemeden sonra çalışan testler (ayrıntı: tests/README.md). LG servis testi Node.js ister; yoksa atlanır.
TESTS = ["test_normalization", "test_garbage", "test_local_pool", "test_subtitle_scheduler", "test_game_library",
         "test_console_mode", "test_tv_link", "test_subtitle_web", "test_discord_presence", "test_plugin",
         "test_dll_server", "test_webos_install"]
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
    if not os.path.isfile(os.path.join(BUILD, "CMakeCache.txt")):
        args = [cmake, "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
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
        say("  ctest (C++ boşluk düzeltici)")
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


def exe_in_use() -> bool:
    if not os.path.isfile(EXE):
        return False
    try:
        with open(EXE, "ab"):
            return False
    except PermissionError:
        return True


def selftest() -> int:
    out = os.path.join(os.environ.get("APPDATA", ""), "MakineLive", "selftest.json")
    try:
        os.remove(out)
    except OSError:
        pass
    try:
        subprocess.run([EXE, "--selftest"], timeout=120)
    except subprocess.TimeoutExpired:
        return fail("selftest 2 dakikada bitmedi.")
    try:
        with open(out, encoding="utf-8") as fh:
            res = json.load(fh)
    except (OSError, ValueError):
        return fail(f"selftest sonucu okunamadı ({out}).")
    say(f"  OCR: {res.get('ocr_text', '')!r} ({res.get('ocr_ms', '?')} ms) · sürüm {res.get('version')}"
        f" · TV paketleri: {'var' if res.get('tv_apk') and res.get('tv_ipk') else 'EKSİK'}")
    if not res.get("ok"):
        return fail(f"selftest başarısız: {res.get('error') or res.get('ocr_text')}  (ayrıntı: {out})")
    return 0


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
            wanted = input("Yeni sürüm numarası (örnek 0.3.1; boş bırakırsanız aynı kalır): ").strip().lstrip("v")
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

    if exe_in_use():
        return fail("dist\\MakineLive.exe şu an açık. Uygulamayı kapatıp yeniden deneyin.")

    step(f"DLL derleniyor (Makine Live {cur})")
    if build_dll():
        return 1

    if not quick:
        step("Testler")
        if run_tests():
            return 1

    step("Exe paketleniyor")
    if run([sys.executable, os.path.join("tools", "build_exe.py")]) != 0:
        return fail("exe paketlenemedi (çıktıya bakın).")

    step("Selftest (DLL + OCR exe'nin içinden)")
    if selftest():
        return 1

    notes = os.path.join("docs", "surum-notlari", f"v{cur}.txt")
    step("Hazır")
    say(f"  Dosya    {EXE}")
    say(f"  Sürüm    {cur}")
    say(f"  Boyut    {os.path.getsize(EXE) / 1048576:.1f} MB")
    say(f"  SHA-256  {sha256(EXE)}")
    say(f"  Notlar   {notes}" + ("" if os.path.isfile(os.path.join(ROOT, notes)) else "  (YOK: yüklemeden önce yazın)"))
    say(f"  Süre     {time.time() - t_start:.0f} sn")
    say("\nYüklemek için: makineceviri.org/live/portal → Ayarlar → Genel → sürüm yükle (exe'yi sürükleyin, notları "
        "yapıştırın).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
