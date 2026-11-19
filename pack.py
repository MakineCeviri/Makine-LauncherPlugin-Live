"""
Makine Live — eklenti paketleyici.
build/release içeriğini (alt klasörler dahil) makine-live.makine arşivine koyar.

Çalıştırma:  py pack.py
"""

import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
RELEASE = os.path.join(ROOT, "build", "release")
OUT = os.path.join(ROOT, "makine-live.makine")

# Launcher'ın eklentiyi çalıştırabilmesi için pakette olması gereken dosyalar
REQUIRED = [
    "makine-live.dll",
    "manifest.json",
    "icon.png",
    "RapidOcrOnnx.dll",
    "onnxruntime.dll",
    "models/ch_PP-OCRv3_det_infer.onnx",
    "models/ch_ppocr_mobile_v2.0_cls_infer.onnx",
    "models/ch_PP-OCRv3_rec_infer.onnx",
    "models/ppocr_keys_v1.txt",
    "resources/en_words.txt",
]

# Derleme artıkları pakete girmez
EXCLUDE_EXT = {".a", ".lib", ".exp", ".pdb", ".ilk", ".log", ".bmp"}


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    missing = [f for f in REQUIRED if not os.path.isfile(os.path.join(RELEASE, f))]
    if missing:
        print("Hata: build/release içinde eksik dosyalar var:")
        for f in missing:
            print(f"  - {f}")
        print("Önce derleyin (cmake --build build). OCR dosyaları third_party/rapidocr altından kopyalanır.")
        return 1

    with open(os.path.join(ROOT, "manifest.json"), encoding="utf-8") as f:
        source_version = json.load(f)["version"]
    with open(os.path.join(RELEASE, "manifest.json"), encoding="utf-8") as f:
        built_version = json.load(f)["version"]
    if source_version != built_version:
        print(f"Hata: build/release/manifest.json eski ({built_version}, kaynak {source_version}). Yeniden derleyin.")
        return 1

    files = []
    for dirpath, _, names in os.walk(RELEASE):
        for name in names:
            if os.path.splitext(name)[1].lower() in EXCLUDE_EXT:
                continue
            full = os.path.join(dirpath, name)
            files.append((full, os.path.relpath(full, RELEASE).replace("\\", "/")))
    files.sort(key=lambda f: f[1])

    tmp = OUT + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zf:
        for full, arc in files:
            zf.write(full, arcname=arc)
            print(f"  + {arc}")
    os.replace(tmp, OUT)

    size_mb = os.path.getsize(OUT) / (1024 * 1024)
    print(f"\n{os.path.basename(OUT)} hazır — sürüm {source_version}, {len(files)} dosya, {size_mb:.1f} MB")
    print("Kurulum: Makine Launcher → Ayarlar → Eklentiler → Dosya Seç")
    return 0


if __name__ == "__main__":
    sys.exit(main())
