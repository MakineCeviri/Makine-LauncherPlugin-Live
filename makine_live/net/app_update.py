"""
Makine Live — kendi kendini güncelleme (masaüstü uygulaması, tek dosyalık .exe).

[TEST] Launcher entegrasyonunda güncellemeyi Launcher yapar; bu modül yalnızca kapalı beta .exe'si içindir.

Akış: yeni sürüm %LOCALAPPDATA%\\MakineLive\\update altına indirilir ve SHA-256'sı doğrulanır (pool_client.download_release).
Sonra çalışan .exe'nin adı "MakineLive.exe.old" yapılır (Windows çalışan bir programın adının değişmesine izin verir),
yeni sürüm eski adıyla yerine kopyalanır ve `--after-update <pid>` ile açılır; yeni süreç eskisinin kapanmasını bekler,
sonra açılır ve .old dosyasını siler. Yerine kopyalanamazsa (klasör yazılamıyorsa) yeni sürüm indirildiği yerden açılır.

0.3.0 ve öncesinde bu iş gizli bir komut dosyasıyla yapılıyordu (tasklist / ping döngüsü, kendi .exe'sinin üzerine kopyalama,
kendini silen betik). Bu kalıp zararlı yazılım "dropper"larınınkine benzediği için bazı antivirüslerin sezgisel
taramasına takılıyordu; şimdi yalnızca dosya adı değişikliği ve normal bir süreç başlatma var.
"""

import ctypes
import os
import shutil
import subprocess
import sys

DETACHED_PROCESS = 0x00000008
AFTER_UPDATE_ARG = "--after-update"


def update_dir() -> str:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("TEMP") or os.path.expanduser("~")
    return os.path.join(base, "MakineLive", "update")


def can_self_update() -> bool:
    return bool(getattr(sys, "frozen", False)) and sys.executable.lower().endswith(".exe")


def swap_and_restart(new_exe: str) -> None:
    """Yeni sürümü yerine koyup açar; çağıran hemen ardından uygulamayı kapatmalıdır."""
    target = sys.executable
    old = target + ".old"
    args = [AFTER_UPDATE_ARG, str(os.getpid())]
    try:
        if os.path.exists(old):
            os.remove(old)
        os.replace(target, old)                      # çalışan .exe'nin adı değişebilir, silinemez
        try:
            shutil.copyfile(new_exe, target)
        except OSError:
            os.replace(old, target)                  # geri al: eski sürüm yerinde kalsın
            raise
    except OSError:
        subprocess.Popen([new_exe] + args, creationflags=DETACHED_PROCESS, close_fds=True)
        return
    try:
        os.remove(new_exe)
    except OSError:
        pass
    subprocess.Popen([target] + args, creationflags=DETACHED_PROCESS, close_fds=True)


def after_update(argv=None, timeout_ms: int = 20000) -> bool:
    """
    Yeni sürümün ilk açılışı: eski süreç kapanana kadar bekle (tek örnek kilidi ondadır), sonra eski .exe'yi sil.
    `--after-update` ile açılmadıysa yalnızca önceki güncellemeden kalan .old dosyasını temizler. Dönüş: güncellemeden mi açıldı.
    """
    argv = sys.argv if argv is None else argv
    updated = False
    if AFTER_UPDATE_ARG in argv:
        updated = True
        try:
            pid = int(argv[argv.index(AFTER_UPDATE_ARG) + 1])
        except (IndexError, ValueError):
            pid = 0
        if pid > 0:
            k32 = ctypes.windll.kernel32
            k32.OpenProcess.restype = ctypes.c_void_p
            h = k32.OpenProcess(0x00100000, False, pid)       # SYNCHRONIZE
            if h:
                k32.WaitForSingleObject(ctypes.c_void_p(h), timeout_ms)
                k32.CloseHandle(ctypes.c_void_p(h))
    if can_self_update():
        old = sys.executable + ".old"
        for _ in range(10):
            if not os.path.exists(old):
                break
            try:
                os.remove(old)
                break
            except OSError:
                ctypes.windll.kernel32.Sleep(300)          # eski süreç henüz tamamen çıkmadı
    return updated
