"""
Makine Live — PyInstaller'ın başlatıcısını (bootloader) kaynaktan derler.

    py tools/build_bootloader.py      → build/bootloader/<PyInstaller sürümü>/runw.exe, run.exe

Neden: PyInstaller'ın hazır başlatıcısı (pip ile gelen runw.exe) dünyada binlerce programda, aralarında zararlı yazılımlarda
da birebir aynıdır. Bu yüzden Microsoft (Wacatac !ml), Google, ESET, Fortinet, Ikarus, Rising, Bkav gibi motorlar tek
dosyalık PyInstaller paketlerini sık sık yanlışlıkla "Trojan" sayar. Kaynaktan, bizim derleyicimizle (GCC, MinGW-w64)
derlenen başlatıcı başka bir dosyadır: bu imzalara takılmaz. Kod aynıdır, yalnızca derleme bizimdir.

Kaynak: kurulu PyInstaller sürümünün PyPI'daki kaynak paketi (sdist); SHA-256'sı PyPI'nın bildirdiğiyle doğrulanır.
Derleyici: PATH'teki gcc (scoop gcc 15 ya da başka bir MinGW-w64). tools/build_exe.py başlatıcı yoksa bunu kendisi çağırır.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.join(ROOT, "build", "bootloader")
FILES = ("runw.exe", "run.exe")


def pyinstaller_version() -> str:
    import PyInstaller
    return PyInstaller.__version__


def bootloader_dir(version: str = "") -> str:
    return os.path.join(BASE, version or pyinstaller_version())


def ready(version: str = "") -> bool:
    d = bootloader_dir(version)
    return all(os.path.isfile(os.path.join(d, f)) for f in FILES)


def _fetch_sdist(version: str, dest_dir: str) -> str:
    with urllib.request.urlopen(f"https://pypi.org/pypi/pyinstaller/{version}/json", timeout=30) as r:
        meta = json.load(r)
    sdist = next(u for u in meta["urls"] if u["packagetype"] == "sdist")
    path = os.path.join(dest_dir, sdist["filename"])
    print(f"PyInstaller {version} kaynağı indiriliyor…")
    with urllib.request.urlopen(sdist["url"], timeout=120) as r, open(path, "wb") as fh:
        shutil.copyfileobj(r, fh)
    with open(path, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()
    if digest != sdist["digests"]["sha256"]:
        raise SystemExit(f"Hata: indirilen kaynak paketin SHA-256'sı PyPI'dakiyle uyuşmuyor ({digest}).")
    return path


def build(version: str = "") -> str:
    version = version or pyinstaller_version()
    out = bootloader_dir(version)
    if ready(version):
        return out
    gcc = shutil.which("gcc")
    if not gcc:
        raise SystemExit("Hata: gcc bulunamadı (MinGW-w64; ör. scoop install gcc). Başlatıcı derlenemedi.")
    work = os.path.join(BASE, "src")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    archive = _fetch_sdist(version, work)
    with tarfile.open(archive) as tf:
        tf.extractall(work, filter="data")
    src = os.path.join(work, f"pyinstaller-{version}", "bootloader")
    print(f"Başlatıcı derleniyor ({gcc})…")
    rc = subprocess.call([sys.executable, "./waf", "distclean", "all", "--target-arch=64bit", "--gcc"], cwd=src)
    built = os.path.join(work, f"pyinstaller-{version}", "PyInstaller", "bootloader", "Windows-64bit-intel")
    if rc != 0 or not all(os.path.isfile(os.path.join(built, f)) for f in FILES):
        raise SystemExit("Hata: başlatıcı derlenemedi (waf çıktısına bakın).")
    os.makedirs(out, exist_ok=True)
    for f in FILES:
        shutil.copyfile(os.path.join(built, f), os.path.join(out, f))
    shutil.rmtree(work, ignore_errors=True)
    print(f"Hazır: {out}")
    return out


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    build()
