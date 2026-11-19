"""
Makine TV'nin LG webOS paketini (.ipk) üretir — LG'nin ares-package aracı gerekmez.

    py tools/build_webos.py          → build/webos/makine-tv.ipk

İçerik: tv-webos/app (web uygulaması) + tv-webos/service (arka plan servisi) + spec/sample_quotes.json (örnek altyazılar).
Sürüm manifest.json'dan gelir (PC uygulaması, Android ve LG aynı sürüm numarasını taşır).
Paket biçimi: ar arşivi → debian-binary, control.tar.gz (control), data.tar.gz
(usr/palm/applications/<app>/, usr/palm/services/<servis>/, usr/palm/packages/<app>/packageinfo.json).
"""

import gzip
import io
import json
import os
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tv-webos")
OUT = os.path.join(ROOT, "build", "webos", "makine-tv.ipk")
APP_ID = "org.makineceviri.tv"
SERVICE_ID = "org.makineceviri.tv.service"
UID, GID = 0, 5000


def version() -> str:
    with open(os.path.join(ROOT, "manifest.json"), encoding="utf-8") as fh:
        return json.load(fh)["version"]


def _tar(entries) -> bytes:
    """entries: [(arcname, bytes | None)] — None dizin demektir. gzip'li tar, sabit sahiplik ve zaman."""
    raw = io.BytesIO()
    mtime = int(time.time())
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for name, data in entries:
            info = tarfile.TarInfo(name)
            info.uid, info.gid, info.mtime = UID, GID, mtime
            info.uname, info.gname = "root", "root"
            if data is None:
                info.type, info.mode = tarfile.DIRTYPE, 0o777
                tar.addfile(info)
            else:
                info.size, info.mode = len(data), 0o644
                tar.addfile(info, io.BytesIO(data))
    return gzip.compress(raw.getvalue(), mtime=0)


def _ar(members) -> bytes:
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, data in members:
        header = (name.ljust(16) + str(int(time.time())).ljust(12) + "0".ljust(6) + "0".ljust(6) +
                  "100644".ljust(8) + str(len(data)).ljust(10) + "`\n")
        out.write(header.encode("ascii"))
        out.write(data)
        if len(data) % 2:
            out.write(b"\n")
    return out.getvalue()


def _files(src_dir, dest_dir, skip=()):
    for d, dirs, files in os.walk(src_dir):
        dirs.sort()
        rel = os.path.relpath(d, src_dir).replace(os.sep, "/")
        base = dest_dir if rel == "." else f"{dest_dir}/{rel}"
        yield base, None
        for f in sorted(files):
            if f in skip:
                continue
            with open(os.path.join(d, f), "rb") as fh:
                yield f"{base}/{f}", fh.read()


def build(out: str = OUT) -> str:
    ver = version()
    app_dir = f"usr/palm/applications/{APP_ID}"
    svc_dir = f"usr/palm/services/{SERVICE_ID}"
    pkg_dir = f"usr/palm/packages/{APP_ID}"

    entries: list = [("usr", None), ("usr/palm", None), ("usr/palm/applications", None),
               ("usr/palm/services", None), ("usr/palm/packages", None)]
    for name, data in _files(os.path.join(SRC, "app"), app_dir):
        if name == f"{app_dir}/appinfo.json":
            info = json.loads(data.decode("utf-8"))
            info["version"] = ver
            data = json.dumps(info, ensure_ascii=False, indent=2).encode("utf-8")
        entries.append((name, data))
    with open(os.path.join(ROOT, "spec", "sample_quotes.json"), "rb") as fh:
        entries.append((f"{app_dir}/samples.json", fh.read()))
    # state.json: TV'de servisin kendi yazdığı dosya; geliştirme sırasında oluştuysa pakete girmez
    for name, data in _files(os.path.join(SRC, "service"), svc_dir, skip=("state.json",)):
        if name == f"{svc_dir}/package.json":
            pj = json.loads(data.decode("utf-8"))
            pj["version"] = ver
            data = json.dumps(pj, ensure_ascii=False, indent=2).encode("utf-8")
        entries.append((name, data))
    pkginfo = {"app": APP_ID, "id": APP_ID, "loc_name": "Makine TV", "package_format_version": 2,
               "services": [SERVICE_ID], "vendor": "Makine Çeviri", "version": ver}
    entries += [(pkg_dir, None), (f"{pkg_dir}/packageinfo.json",
                                  json.dumps(pkginfo, ensure_ascii=False, indent=2).encode("utf-8"))]

    size_kb = max(1, sum(len(d) for _, d in entries if d) // 1024)
    control = (f"Package: {APP_ID}\nVersion: {ver}\nSection: misc\nPriority: optional\nArchitecture: all\n"
               f"Installed-Size: {size_kb}\nMaintainer: Makine Çeviri <nobody@example.com>\n"
               f"Description: This is a webOS application.\nwebOS-Package-Format-Version: 2\n"
               f"webOS-Packager-Version: x.y.x\n").encode("utf-8")
    ipk = _ar([("debian-binary", b"2.0\n"),
               ("control.tar.gz", _tar([("control", control)])),
               ("data.tar.gz", _tar(entries))])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(ipk)
    return out


if __name__ == "__main__":
    path = build(sys.argv[1] if len(sys.argv) > 1 else OUT)
    print(f"{os.path.relpath(path, ROOT)}  {os.path.getsize(path) / 1024:.0f} KB  (sürüm {version()})")
