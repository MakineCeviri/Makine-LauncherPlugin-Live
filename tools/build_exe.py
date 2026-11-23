"""
Makine Live — kapalı beta uygulamasını tek bir .exe olarak paketler.

    py tools/build_exe.py    → dist/MakineLive.exe

İçerik: makine_live paketi (giriş noktası makine_live/__main__.py) + makine-live.dll ve OCR çalışma dosyaları
(build/release/*) + spec/ (tasarım değerleri, örnek replikler) + assets/ (logo) + resources/en_words.txt + Makine TV paketleri (runtime/makine-tv.apk Android TV, runtime/makine-tv.ipk LG webOS;
"TV'ye kur" bunları TV'ye yükler).
Kullanıcı Python ya da başka bir şey kurmaz; exe'yi açar, test hesabıyla girer.

Önce DLL derlenmiş olmalı:  cmake --build build
Makine TV APK'sı yoksa ya da tv/ kaynaklarından eskiyse Gradle ile derlenir (tv\\gradlew.bat assembleRelease).
PyInstaller / adb-shell yoksa kullanıcı site-packages'a kurulur.
"""

import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.join(ROOT, "build", "release")
WORK = os.path.join(ROOT, "build", "pyinstaller")
DIST = os.path.join(ROOT, "dist")
ICON = os.path.join(ROOT, "assets", "app.ico")
NAME = "MakineLive"

# Uygulamanın kullanmadığı standart kütüphane / paketler (exe küçülsün)
EXCLUDES = [
    "unittest", "pydoc", "pydoc_data", "doctest", "lib2to3", "xmlrpc", "idlelib", "turtle", "turtledemo",
    "tkinter.test", "test", "distutils", "setuptools", "pip", "asyncio", "sqlite3", "curses", "pygame",
    "PIL.ImageQt", "PIL.ImageShow", "PIL.SpiderImagePlugin",
]


# PyInstaller'ı kendi derlediğimiz başlatıcıyla çalıştırır (tools/build_bootloader.py): kurulu paketin dosyalarına dokunmadan,
# EXE adımının seçtiği başlatıcı dosyası build/bootloader/<sürüm>/ altındakiyle değiştirilir.
PYINSTALLER_WITH_OWN_BOOTLOADER = r"""
import os, sys
from PyInstaller.building import api
own_dir = os.environ["MAKINE_BOOTLOADER_DIR"]
_orig = api.EXE._bootloader_file
def _own(self, exe, extension=None):
    path = _orig(self, exe, extension)
    own = os.path.join(own_dir, os.path.basename(path))
    if not os.path.isfile(own):
        raise SystemExit("Kendi başlatıcımız eksik: " + own)
    return own
api.EXE._bootloader_file = _own
import PyInstaller.__main__
PyInstaller.__main__.run(sys.argv[1:])
"""


def fail(msg: str) -> int:
    print(f"\nHata: {msg}")
    return 1


def check_release() -> str:
    """build/release eksiksiz ve DLL kaynaklardan yeni mi? Sürümü döndürür."""
    from pack import REQUIRED  # .makine paketiyle aynı dosya listesi
    missing = [f for f in REQUIRED if not os.path.isfile(os.path.join(RELEASE, f))]
    if missing:
        raise SystemExit(fail("build/release eksik: " + ", ".join(missing) +
                              "\nÖnce derleyin:  cmake --build build"))
    dll_mtime = os.path.getmtime(os.path.join(RELEASE, "makine-live.dll"))
    newest = max(os.path.getmtime(os.path.join(d, f))
                 for base in ("src", os.path.join("include", "makine", "plugin"))
                 for d, _, files in os.walk(os.path.join(ROOT, base)) for f in files)
    if newest > dll_mtime + 1:
        raise SystemExit(fail("makine-live.dll kaynak dosyalardan eski.\nÖnce derleyin:  cmake --build build"))
    with open(os.path.join(ROOT, "manifest.json"), encoding="utf-8") as fh:
        version = json.load(fh)["version"]
    with open(os.path.join(RELEASE, "manifest.json"), encoding="utf-8") as fh:
        built = json.load(fh)["version"]
    if built != version:
        raise SystemExit(fail(f"build/release/manifest.json eski ({built}, kaynak {version}).\n"
                              "Önce derleyin:  cmake --build build"))
    return version


def ensure_pyinstaller():
    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("PyInstaller kuruluyor (kullanıcı site-packages)…")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--user", "pyinstaller"])
    try:
        import adb_shell  # noqa: F401  ("TV'ye kur": Makine TV'yi Android TV'ye ağ üzerinden kurar)
    except ImportError:
        print("adb-shell kuruluyor (kullanıcı site-packages)…")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--user", "adb-shell==0.4.4"])
    try:
        import paramiko  # noqa: F401  ("TV'ye kur": LG geliştirici moduna SSH ile kurar)
    except ImportError:
        print("paramiko kuruluyor (kullanıcı site-packages)…")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--user", "paramiko==3.5.1"])


def webos_ipk() -> str:
    """Makine TV'nin LG paketi (tv-webos/ → .ipk; her derlemede yeniden üretilir, birkaç ms)."""
    from build_webos import build
    return build(os.path.join(WORK, "makine-tv.ipk"))


TV_DIR = os.path.join(ROOT, "tv")
TV_APK = os.path.join(TV_DIR, "app", "build", "outputs", "apk", "release", "app-release.apk")


def tv_apk() -> str:
    """Makine TV APK'sı: yoksa ya da tv/ kaynakları / manifest sürümü daha yeniyse Gradle ile derlenir."""
    sources = [os.path.join(ROOT, "manifest.json"), os.path.join(TV_DIR, "app", "build.gradle.kts")]
    for d, _, files in os.walk(os.path.join(TV_DIR, "app", "src")):
        sources += [os.path.join(d, f) for f in files]
    newest = max(os.path.getmtime(p) for p in sources if os.path.isfile(p))
    if not os.path.isfile(TV_APK) or os.path.getmtime(TV_APK) + 1 < newest:
        print("Makine TV derleniyor (Gradle)…")
        rc = subprocess.call([os.path.join(TV_DIR, "gradlew.bat"), "assembleRelease", "--console=plain", "-q"],
                             cwd=TV_DIR)
        if rc != 0 or not os.path.isfile(TV_APK):
            raise SystemExit(fail("Makine TV derlenemedi (tv\\gradlew.bat assembleRelease çıktısına bakın)."))
    staged = os.path.join(WORK, "makine-tv.apk")
    os.makedirs(WORK, exist_ok=True)
    shutil.copyfile(TV_APK, staged)
    return staged


def make_icon():
    """Uygulama ikonu: koyu yuvarlak karede Makine Çeviri logosu (assets/logo_white-96.webp → ui.brand.app_mark_image)."""
    sys.path.insert(0, ROOT)
    from makine_live.ui.brand import app_mark_image
    os.makedirs(os.path.dirname(ICON), exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [app_mark_image(s) for s in sizes]
    images[-1].save(ICON, format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])


def version_file(version: str) -> str:
    parts = [int(p) if p.isdigit() else 0 for p in version.split(".")][:4]
    parts += [0] * (4 - len(parts))
    t = tuple(parts)
    path = os.path.join(WORK, "version_info.txt")
    os.makedirs(WORK, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={t}, prodvers={t}, mask=0x3f, flags=0x0, OS=0x40004, fileType=0x1, subtype=0x0,
                    date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('041F04B0', [
      StringStruct('CompanyName', 'Makine Çeviri'),
      StringStruct('FileDescription', 'Makine Live v{version} beta — oyun içi canlı çeviri'),
      StringStruct('FileVersion', '{version}'),
      StringStruct('InternalName', '{NAME}'),
      StringStruct('OriginalFilename', '{NAME}.exe'),
      StringStruct('ProductName', 'Makine Live'),
      StringStruct('ProductVersion', '{version} beta'),
      StringStruct('LegalCopyright', 'Makine Çeviri · AGPL-3.0 + Commons Clause')])]),
    VarFileInfo([VarStruct('Translation', [0x041F, 1200])])
  ]
)
""")
    return path


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    version = check_release()
    ensure_pyinstaller()
    make_icon()
    vfile = version_file(version)
    apk = tv_apk()
    ipk = webos_ipk()

    # Antivirüslerin yanlış alarmı: hazır başlatıcı yerine kaynaktan derlenmiş olanı, UPX sıkıştırması asla
    from build_bootloader import build as build_bootloader
    env = dict(os.environ, MAKINE_BOOTLOADER_DIR=build_bootloader())

    sep = os.pathsep
    args = [
        sys.executable, "-c", PYINSTALLER_WITH_OWN_BOOTLOADER, "--noconfirm", "--clean", "--onefile", "--windowed", "--noupx",
        "--name", NAME, "--icon", ICON, "--version-file", vfile,
        "--distpath", DIST, "--workpath", os.path.join(WORK, "work"), "--specpath", WORK,
        "--add-data", f"{RELEASE}{sep}runtime",
        "--add-data", f"{os.path.join(ROOT, 'spec', 'design_tokens.json')}{sep}spec",
        "--add-data", f"{os.path.join(ROOT, 'spec', 'sample_quotes.json')}{sep}spec",
        "--add-data", f"{os.path.join(ROOT, 'assets', 'logo_white-96.webp')}{sep}assets",
        "--add-data", f"{os.path.join(ROOT, 'resources', 'en_words.txt')}{sep}resources",
        "--paths", ROOT,
        "--add-data", f"{apk}{sep}runtime",
        "--add-data", f"{ipk}{sep}runtime",
        # tv_install yalnızca "TV'ye kur"da içe aktarır; PyInstaller'ın kaçırmaması için açıkça eklenir
        "--hidden-import", "adb_shell.adb_device", "--hidden-import", "adb_shell.auth.keygen",
        "--hidden-import", "adb_shell.auth.sign_pythonrsa", "--hidden-import", "paramiko",
        "--log-level", "WARN",
    ]
    for mod in EXCLUDES:
        args += ["--exclude-module", mod]
    args.append(os.path.join(ROOT, "makine_live", "__main__.py"))

    print(f"Paketleniyor: Makine Live {version} → dist/{NAME}.exe")
    t0 = time.time()
    rc = subprocess.call(args, cwd=ROOT, env=env)
    if rc != 0:
        return fail("PyInstaller başarısız oldu (çıktıya bakın).")
    exe = os.path.join(DIST, f"{NAME}.exe")
    shutil.rmtree(os.path.join(WORK, "work"), ignore_errors=True)
    print(f"\nHazır: {exe}  ({os.path.getsize(exe) / 1024 / 1024:.1f} MB, {time.time() - t0:.0f} sn)")
    print(f"Doğrulama:  dist\\{NAME}.exe --selftest   → %APPDATA%\\MakineLive\\selftest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
