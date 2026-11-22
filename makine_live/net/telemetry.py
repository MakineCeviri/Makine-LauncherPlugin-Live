"""
Makine Live — sunucuya kullanım bildirimi (heartbeat) ve ekibin istediği günlük.

[TEST] Saf yardımcılar; ağ çağrısı yapmaz (gönderim pool_client.ServerClient.beat / upload_log ile).

- Uygulama açıkken sunucunun söylediği aralıkta (`beat_s`, varsayılan 5 dk) bir bildirim gider: sürüm (başlıkta), oyun,
  mod (PC / konsol, altyazının nerede gösterildiği), okuma açık mı, bu oturumda ve son bildirimden beri çevrilen satır,
  okuma süresi ve sorun gidermeye yarayan sistem / hız bilgileri. Ekran görüntüsü ya da okunan metin gitmez.
  Portal: makineceviri.org/live/portal/#/canli (yönetici).
- Yönetici bir oturumun günlüğünü isteyebilir: bildirimin yanıtında `log_request` gelir, uygulama makine-live.log'un
  (döndürülmüş eski parçalar ve crash.log dahil) son ~2 MB'ını gzip ile yollar ve panelde bunu kullanıcıya söyler.
"""

from __future__ import annotations

import ctypes
import gzip
import os
import platform
import sys
import time
from typing import Dict, List, Optional

LOG_TAIL_BYTES = 2_000_000          # sunucu sıkıştırılmış 3 MB'a kadar kabul eder; metin ~8 kat sıkışır
_SYSTEM: Optional[Dict[str, object]] = None


def _windows_version() -> str:
    try:
        v = sys.getwindowsversion()
        name = "Windows 11" if v.major == 10 and v.build >= 22000 else f"Windows {v.major}"
        return f"{name} ({v.build})"
    except AttributeError:
        return platform.platform()


def _cpu_name() -> str:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
            return " ".join(str(winreg.QueryValueEx(k, "ProcessorNameString")[0]).split())
    except (ImportError, OSError):
        return platform.processor()


def _ram_gb() -> float:
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    st = MEMORYSTATUSEX()
    st.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
    try:
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return round(st.ullTotalPhys / 2 ** 30, 1)
    except (AttributeError, OSError):
        pass
    return 0.0


def system_info() -> Dict[str, object]:
    """Değişmeyen sistem bilgileri (bir kez okunur)."""
    global _SYSTEM
    if _SYSTEM is None:
        info: Dict[str, object] = {"os": _windows_version(), "cpu": _cpu_name()[:120], "ram_gb": _ram_gb()}
        try:
            u32 = ctypes.windll.user32
            info["screen"] = f"{u32.GetSystemMetrics(0)}x{u32.GetSystemMetrics(1)}"
            info["monitors"] = u32.GetSystemMetrics(80)                 # SM_CMONITORS
            dpi = u32.GetDpiForSystem() if hasattr(u32, "GetDpiForSystem") else 96
            info["scale"] = round(dpi / 96, 2)
        except (AttributeError, OSError):
            pass
        _SYSTEM = info
    return dict(_SYSTEM)


def median(xs) -> Optional[int]:
    xs = sorted(xs)
    return round(xs[len(xs) // 2]) if xs else None


class BeatCounter:
    """Son bildirimden beri geçen okuma süresi ve çevrilen satır (sunucu bunları günlük toplamlara ekler)."""

    def __init__(self, clock=time.monotonic):
        self._clock = clock
        self._run_acc = 0.0
        self._run_mark: Optional[float] = None
        self._lines_sent = 0

    def running(self, on: bool):
        now = self._clock()
        if on and self._run_mark is None:
            self._run_mark = now
        elif not on and self._run_mark is not None:
            self._run_acc += now - self._run_mark
            self._run_mark = None

    def take(self, total_lines: int) -> Dict[str, int]:
        """{run_s, lines}: son çağrıdan beri; okuma sürerken ölçüm kaldığı yerden devam eder."""
        now = self._clock()
        if self._run_mark is not None:
            self._run_acc += now - self._run_mark
            self._run_mark = now
        run_s = int(self._run_acc)
        self._run_acc -= run_s                      # küsurat bir sonrakine kalır
        lines = max(0, total_lines - self._lines_sent)
        self._lines_sent = max(total_lines, self._lines_sent)
        return {"run_s": run_s, "lines": lines}

    def reset_lines(self):
        """Oturum sayacı sıfırlandı (F9 ile yeniden başlatma): bir sonraki farkı sıfırdan say."""
        self._lines_sent = 0


def _tail(path: str, limit: int) -> bytes:
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            if size > limit:
                fh.seek(size - limit)
                fh.readline()                       # yarım satırla başlamasın
            return fh.read()
    except OSError:
        return b""


def collect_log(log_dir: str, header: Dict[str, object], limit: int = LOG_TAIL_BYTES) -> bytes:
    """
    Ekibe gidecek günlük (gzip): başlık (sürüm, sistem), crash.log'un sonu, sonra makine-live.log parçaları eskiden
    yeniye; toplam en fazla `limit` bayt, en yeni kayıtlar kalır.
    """
    parts: List[bytes] = []
    head = "".join(f"{k}: {v}\n" for k, v in header.items())
    parts.append(("──── Makine Live günlüğü\n" + head + "\n").encode("utf-8"))
    crash = _tail(os.path.join(log_dir, "crash.log"), 200_000)
    # faulthandler her açılışta yalnızca bir başlık satırı yazar: gerçek bir çökme izi yoksa bölüm eklenmez
    if any(ln.strip() and not ln.lstrip().startswith("────".encode("utf-8")) for ln in crash.splitlines()):
        parts.append("──── crash.log (son kısım)\n".encode("utf-8") + crash + b"\n")
    budget = max(0, limit - sum(len(p) for p in parts))
    logs = []
    for name in ("makine-live.log.2", "makine-live.log.1", "makine-live.log"):     # eskiden yeniye
        path = os.path.join(log_dir, name)
        if os.path.isfile(path):
            logs.append((name, path))
    chunks: List[bytes] = []
    for name, path in reversed(logs):                                               # bütçe en yeniden harcanır
        if budget <= 0:
            break
        data = _tail(path, budget)
        chunks.append(f"──── {name}\n".encode("utf-8") + data)
        budget -= len(data)
    parts.extend(reversed(chunks))
    return gzip.compress(b"".join(parts), compresslevel=6)
