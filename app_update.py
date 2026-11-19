"""
Makine Live — kendi kendini güncelleme (masaüstü uygulaması, tek dosyalık .exe).

[TEST] Launcher entegrasyonunda güncellemeyi Launcher yapar; bu modül yalnızca kapalı beta .exe'si içindir.

Akış: yeni sürüm %LOCALAPPDATA%\\MakineLive\\update altına indirilir ve SHA-256'sı doğrulanır (pool_client.download_release).
Sonra küçük bir komut dosyası başlatılır: uygulamanın kapanmasını bekler, yeni .exe'yi eskisinin üzerine kopyalar ve
yeniden açar. Kopyalanamazsa (klasör yazılamıyorsa) yeni sürümü indirildiği yerden çalıştırır.
"""

import os
import subprocess
import sys

CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008


def update_dir() -> str:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("TEMP") or os.path.expanduser("~")
    return os.path.join(base, "MakineLive", "update")


def can_self_update() -> bool:
    return bool(getattr(sys, "frozen", False)) and sys.executable.lower().endswith(".exe")


def swap_and_restart(new_exe: str) -> None:
    """Komut dosyasını başlatır; çağıran hemen ardından uygulamayı kapatmalıdır."""
    target = sys.executable
    pid = os.getpid()
    script = os.path.join(update_dir(), "guncelle.cmd")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    with open(script, "w", encoding="mbcs", errors="replace") as fh:
        fh.write(f"""@echo off
setlocal
set PID={pid}
:bekle
tasklist /FI "PID eq %PID%" 2>nul | find "%PID%" >nul && (ping -n 2 127.0.0.1 >nul & goto bekle)
ping -n 2 127.0.0.1 >nul
copy /y "{new_exe}" "{target}" >nul 2>&1
if errorlevel 1 goto yedek
start "" "{target}"
del "{new_exe}" >nul 2>&1
goto son
:yedek
start "" "{new_exe}"
:son
del "%~f0" >nul 2>&1
""")
    subprocess.Popen(["cmd", "/c", script], creationflags=CREATE_NO_WINDOW | DETACHED_PROCESS, close_fds=True)
