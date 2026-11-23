#!/usr/bin/env bash
# Fetches the extra OCR models and runtimes the benchmark compares against the shipped ch_PP-OCRv3 set.
# Official sources only (RapidAI's ModelScope repo, nuget.org Microsoft packages), every file SHA-256 pinned.
# Model hashes come from RapidAI/RapidOCR python/rapidocr/default_models.yaml (v3.10.0); dictionary and
# nupkg hashes were pinned on first download over HTTPS from the same official locations (2026-11-24).
# Nothing downloaded is executed: models are data, the nupkgs are zip archives whose DLLs the bench loads.
#
# Usage: bash tools/ocr_bench/fetch_models.sh [dest]   (default: third_party/ocr_bench, gitignored)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-$ROOT/third_party/ocr_bench}"
MS="https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0"
NUGET="https://api.nuget.org/v3-flatcontainer"

# name|url|sha256
FILES=(
  "models/en_PP-OCRv4_rec_mobile.onnx|$MS/onnx/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile.onnx|e8770c967605983d1570cdf5352041dfb68fa0c21664f49f47b155abd3e0e318"
  "models/en_PP-OCRv5_rec_mobile.onnx|$MS/onnx/PP-OCRv5/rec/en_PP-OCRv5_rec_mobile.onnx|c3461add59bb4323ecba96a492ab75e06dda42467c9e3d0c18db5d1d21924be8"
  "models/latin_PP-OCRv5_rec_mobile.onnx|$MS/onnx/PP-OCRv5/rec/latin_PP-OCRv5_rec_mobile.onnx|b20bd37c168a570f583afbc8cd7925603890efbcdc000a59e22c269d160b5f5a"
  "models/ch_PP-OCRv5_det_mobile.onnx|$MS/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx|4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae"
  "models/en_dict.txt|$MS/paddle/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile/en_dict.txt|5662df9d2d03f0e8ca0d3b0649d6acbab904b6a14b3d3521463c71c37c668ce3"
  "models/ppocrv5_en_dict.txt|$MS/paddle/PP-OCRv5/rec/en_PP-OCRv5_rec_mobile/ppocrv5_en_dict.txt|e025a66d31f327ba0c232e03f407ae8d105e1e709e7ccb3f408aa778c24e70d6"
  "models/ppocrv5_latin_dict.txt|$MS/paddle/PP-OCRv5/rec/latin_PP-OCRv5_rec_mobile/ppocrv5_latin_dict.txt|3c0a8a79b612653c25f765271714f71281e4e955962c153e272b7b8c1d2b13ff"
  "pkg/microsoft.ml.onnxruntime.directml.1.24.3.nupkg|$NUGET/microsoft.ml.onnxruntime.directml/1.24.3/microsoft.ml.onnxruntime.directml.1.24.3.nupkg|0d42ecd9a672f8621d238c0edfc243b1087a10fa90d5071399e451311350996b"
  "pkg/microsoft.ai.directml.1.15.4.nupkg|$NUGET/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg|4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9"
)

mkdir -p "$DEST/models" "$DEST/pkg"
for entry in "${FILES[@]}"; do
  IFS='|' read -r name url sha <<<"$entry"
  out="$DEST/$name"
  if [[ -f "$out" ]] && [[ "$(sha256sum "$out" | cut -d' ' -f1)" == "$sha" ]]; then continue; fi
  tmp="$out.part"
  curl -sfL --retry 3 -o "$tmp" "$url"
  got="$(sha256sum "$tmp" | cut -d' ' -f1)"
  if [[ "$got" != "$sha" ]]; then
    rm -f "$tmp"
    echo "SHA-256 mismatch for $name: got $got, want $sha" >&2
    exit 1
  fi
  mv "$tmp" "$out"
done

# DirectML runtime: onnxruntime.dll (DML build) + DirectML.dll side by side, so LOAD_WITH_ALTERED_SEARCH_PATH
# never picks up System32's older DirectML.dll. Extracted only from the verified archives above.
DML="$DEST/ort-dml"
if [[ ! -f "$DML/onnxruntime.dll" || ! -f "$DML/DirectML.dll" ]]; then
  rm -rf "$DML" "$DEST/pkg/x"
  mkdir -p "$DML" "$DEST/pkg/x"
  unzip -q -o "$DEST/pkg/microsoft.ml.onnxruntime.directml.1.24.3.nupkg" "runtimes/win-x64/native/*" "build/native/include/*" -d "$DEST/pkg/x/ort"
  unzip -q -o "$DEST/pkg/microsoft.ai.directml.1.15.4.nupkg" "bin/x64-win/*" -d "$DEST/pkg/x/dml"
  cp "$DEST/pkg/x/ort/runtimes/win-x64/native/"*.dll "$DML/"
  cp "$DEST/pkg/x/dml/bin/x64-win/DirectML.dll" "$DML/"
  mkdir -p "$DEST/include"
  cp "$DEST/pkg/x/ort/build/native/include/"*.h "$DEST/include/"
fi
echo "ocr_bench assets ready in $DEST"
