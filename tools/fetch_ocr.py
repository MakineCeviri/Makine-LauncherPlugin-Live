#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""
fetch_ocr.py — download the OCR runtime and models Makine Live ships.

    python tools/fetch_ocr.py [--cache DIR]

Every file is pinned by URL and SHA-256 and lands in DIR (default: .cache/ocr in
this repository):

    bin/onnxruntime.dll         ONNX Runtime 1.24.3 (CPU), Microsoft-signed, from the official
                                Microsoft.ML.OnnxRuntime nupkg on nuget.org
    bin/msvcp140*.dll,          the VC++ runtime onnxruntime.dll imports, copied app-local from
    bin/vcruntime140*.dll       this PC's Visual Studio redist folder (Microsoft-signed; players
                                may not have the VC++ redistributable installed)
    include/onnxruntime_c_api.h the C API header src/ocr_ort.cpp compiles against (not shipped)
    models/                     PP-OCRv3 (Chinese) detection and recognition + the character list;
                                names as src/engine.cpp. No angle classifier (subtitles are level)
    licenses/                   the licenses of all of the above

Every DLL must carry a valid Authenticode signature from Microsoft Corporation, or the
script stops. CMake copies bin/, models/ and licenses/ into build/package (cache variable
LIVE_OCR_DIR). None of it belongs in git. The models come as a .7z: 7-Zip on PATH or
`pip install py7zr`. A second run checks the hashes and downloads nothing.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
import glob

# Archives and the members taken out of them: (member, dest, dest sha256).
# RapidOcrOnnx release: the C-library build for VS2022 with the static CRT (-mt).
# Its "init" release holds the PP-OCRv3 Chinese models: the recognition model measured
# most reliable on game fonts (docs/MIMARI.md §3.3); the text filters were tuned on it.
ARCHIVES = [
    {
        # Microsoft.ML.OnnxRuntime 1.24.3 (CPU) from nuget.org: the Microsoft-signed onnxruntime.dll
        # (the same file 0.3.x shipped in third_party/rapidocr) and the C API header the plugin compiles against
        "url": "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.24.3/microsoft.ml.onnxruntime.1.24.3.nupkg",
        "sha256": "e4ab3b236c1b803e1e7167fe9a75caa1b82a2fd4ae82abbd130937c07517bcdb",
        "members": [
            ("runtimes/win-x64/native/onnxruntime.dll", "bin/onnxruntime.dll",
             "6cd66092dbc72d681ed2c240c08ac6adc9a5dc86d6fe96fd46c47d1f8ed57c70"),
            ("build/native/include/onnxruntime_c_api.h", "include/onnxruntime_c_api.h",
             "7f03541b0233c53b8c91855910c9382c4fb4a26b8fe099965c7c3cad1af2ced6"),
            ("build/native/include/onnxruntime_ep_c_api.h", "include/onnxruntime_ep_c_api.h",   # included by the above
             "7de0ebae527f18b39fbeb3129d5781a995e76f685842c2e16aa0468230e6368b"),
            ("LICENSE", "licenses/onnxruntime-LICENSE.txt",
             "c250d6278f0b47a6439fb7592b08b58a55eb9f535aa49a1db63211c3f982b674"),
            ("ThirdPartyNotices.txt", "licenses/onnxruntime-ThirdPartyNotices.txt",
             "fb0af774b4d7cffc5b9d046f2aaeade2f37df2f80abf8033c95dfffcc77a8866"),
        ],
    },
    {
        "url": "https://github.com/RapidAI/RapidOcrOnnx/releases/download/init/models.7z",
        "sha256": "1074f94e2b33080b9a6ce9d2708b5a56fffd62af3fd86d6c469ec0ed64fd11b1",
        "members": [
            ("ch_PP-OCRv3_det_infer.onnx", "models/ch_PP-OCRv3_det_infer.onnx",              # text detection
             "3439588c030faea393a54515f51e983d8e155b19a2e8aba7891934c1cf0de526"),
            ("ch_PP-OCRv3_rec_infer.onnx", "models/ch_PP-OCRv3_rec_infer.onnx",              # recognition
             "897a3ededb38fee0dae2c1ccee38241f37df202c9509e3abca02e9217c5ee615"),
        ],
    },
]

PADDLE = "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR"
FILES = [
    # The recognition model's character list
    ("models/ppocr_keys_v1.txt", f"{PADDLE}/ff853c78c97729a6497b0f93968fcfa039c26d8a/ppocr/utils/ppocr_keys_v1.txt",
     "28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7"),

    ("licenses/PaddleOCR-LICENSE.txt",      # Apache-2.0: models and ppocr_keys_v1.txt
     f"{PADDLE}/b9da97bea9db53ea5cbb1c32bf5c53b9d09be7b1/LICENSE",
     "3840c5c0c61c294264d2dd77b8777be6ddd90121ef4e0e64abcd22edea581d6e"),
    ("licenses/RapidOCR-LICENSE.txt",       # Apache-2.0: the ONNX conversions
     "https://raw.githubusercontent.com/RapidAI/RapidOCR/ece63a28fe00c36b0872325068774fbc1d189b09/LICENSE",
     "3e0af25fdd06aa9586ae97adb00ea927ebe5a3805ac77d2d3a81ce5f55693333"),
]
OUTPUT_DIRS = ("bin", "models", "licenses", "include")
STAMP = "fetched.json"


def die(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def has(path: str, sha256: str) -> bool:
    return os.path.isfile(path) and sha256_of(path) == sha256


def download(url: str, sha256: str, path: str) -> None:
    if has(path, sha256):
        print(f"ok        {os.path.basename(path)}")
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + ".part"
    print(f"download  {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "makine-live-fetch_ocr"})
    h = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=60) as response, open(part, "wb") as out:
        for chunk in iter(lambda: response.read(1 << 20), b""):
            h.update(chunk)
            out.write(chunk)
    if h.hexdigest() != sha256:
        os.remove(part)
        die(f"{url}: sha256 {h.hexdigest()} is not the pinned {sha256}")
    os.replace(part, path)


VC_RUNTIME = ("msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")


def vc_redist_dir() -> str:
    """Newest x64 VC++ CRT folder of a local Visual Studio (its files may be redistributed app-local)."""
    found = []
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", "")):
        if pf:
            found += glob.glob(os.path.join(pf, "Microsoft Visual Studio", "*", "*", "VC", "Redist", "MSVC", "*", "x64",
                                            "Microsoft.VC14*.CRT"))
    found = [d for d in found if all(os.path.isfile(os.path.join(d, n)) for n in VC_RUNTIME)]
    if not found:
        die("No Visual Studio VC++ redist folder found (VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT). "
            "Install Visual Studio (or its C++ build tools) and run again.")
    return sorted(found)[-1]


def check_signed(path: str) -> None:
    """Authenticode: valid and signed by Microsoft Corporation, else stop."""
    shell = shutil.which("pwsh") or shutil.which("powershell")
    if not shell:
        die("PowerShell is needed to check the Authenticode signature of " + path)
    cmd = ("$s = Get-AuthenticodeSignature -LiteralPath $env:SIGN_PATH; "
           "\"$($s.Status)|$($s.SignerCertificate.Subject)\"")
    out = subprocess.run([shell, "-NoProfile", "-Command", cmd], capture_output=True, text=True,
                         env=dict(os.environ, SIGN_PATH=path)).stdout.strip()
    status, _, subject = out.partition("|")
    if status != "Valid" or "O=Microsoft Corporation" not in subject:
        die(f"{os.path.basename(path)}: Authenticode {status or 'unknown'} ({subject or 'no signer'})")
    print(f"signed    {os.path.basename(path)}  {subject.split(',')[0]}")


def extract(archive: str, members, cache: str) -> None:
    if archive.endswith((".nupkg", ".zip")):
        with zipfile.ZipFile(archive) as z:
            for member, dest, sha in members:
                target = os.path.join(cache, dest)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(member) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
                if not has(target, sha):
                    die(f"{member} does not have the pinned sha256")
        return
    with tempfile.TemporaryDirectory() as tmp:
        seven = shutil.which("7z") or shutil.which("7za")
        if not seven and os.path.isfile(r"C:\Program Files\7-Zip\7z.exe"):
            seven = r"C:\Program Files\7-Zip\7z.exe"
        if seven:
            subprocess.run([seven, "x", "-y", f"-o{tmp}", archive], check=True, stdout=subprocess.DEVNULL)
        else:
            try:
                import py7zr
            except ImportError:
                die("7-Zip is not on PATH: install it or run `pip install py7zr`")
            with py7zr.SevenZipFile(archive) as z:
                z.extractall(tmp)
        for member, dest, sha in members:
            source = os.path.join(tmp, *member.split("/"))
            if not os.path.isfile(source):
                die(f"{member} is not in {os.path.basename(archive)}")
            target = os.path.join(cache, dest)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(source, target)
            if not has(target, sha):
                die(f"{member} does not have the pinned sha256")


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", default=os.path.join(here, "..", ".cache", "ocr"))
    args = parser.parse_args()
    cache = os.path.abspath(args.cache)

    # A pin that changed leaves no stale file behind, and no stamp until all is in place.
    if os.path.exists(os.path.join(cache, STAMP)):
        os.remove(os.path.join(cache, STAMP))
    wanted = {dest: sha for dest, _, sha in FILES}
    for archive in ARCHIVES:
        wanted.update({dest: sha for _, dest, sha in archive["members"]})
    crt = vc_redist_dir()
    for name in VC_RUNTIME:
        wanted["bin/" + name] = sha256_of(os.path.join(crt, name))
    for sub in OUTPUT_DIRS:
        for root, _, names in os.walk(os.path.join(cache, sub)):
            for name in names:
                rel = os.path.relpath(os.path.join(root, name), cache).replace(os.sep, "/")
                if rel not in wanted:
                    os.remove(os.path.join(root, name))

    for dest, url, sha in FILES:
        download(url, sha, os.path.join(cache, dest))

    for archive in ARCHIVES:
        if all(has(os.path.join(cache, dest), sha) for _, dest, sha in archive["members"]):
            for _, dest, _ in archive["members"]:
                print(f"ok        {os.path.basename(dest)}")
            continue
        path = os.path.join(cache, "downloads", os.path.basename(archive["url"]))
        download(archive["url"], archive["sha256"], path)
        extract(path, archive["members"], cache)

    for name in VC_RUNTIME:
        target = os.path.join(cache, "bin", name)
        if not has(target, wanted["bin/" + name]):
            shutil.copyfile(os.path.join(crt, name), target)
    print(f"VC++ runtime from {crt}")
    for dll in sorted(glob.glob(os.path.join(cache, "bin", "*.dll"))):
        check_signed(dll)

    with open(os.path.join(cache, STAMP), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"files": wanted}, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"OCR runtime ready in {cache}")


if __name__ == "__main__":
    main()
