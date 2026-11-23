#!/usr/bin/env bash
# Runs the OCR benchmark candidates and writes build/ocr_bench/report.md + report.json.
#
#   bash tools/ocr_bench/run_all.sh [candidate-id ...]     (no ids = every candidate in CANDIDATES)
#   bash tools/ocr_bench/run_all.sh --report               (only rebuild the report from existing results)
#
# Wrap the whole run in sessiz.sh so it takes one build slot:  bash ~/.claude/bin/sessiz.sh ocrbench bash tools/ocr_bench/run_all.sh
# Prerequisites: fetch_models.sh, make_frames.py, the MinGW build in build/ocr_bench/bin, optionally
# winocr/build.cmd and the INT8 models from quantize.py. A candidate whose files are missing is skipped
# with a note in build/ocr_bench/skipped.txt.
# Real game frames: put images + truth.tsv ("file<TAB>line1\nline2") in build/ocr_bench/real/ — every candidate
# also runs on them and the report shows them as a separate set.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && (pwd -W 2>/dev/null || pwd))"  # C:/… form for the Windows executables
B="$ROOT/build/ocr_bench"
EXE="$B/bin/ocr_bench.exe"
RAPID="$ROOT/third_party/rapidocr"
M3="$RAPID/models"
MX="$ROOT/third_party/ocr_bench/models"
DML="$ROOT/third_party/ocr_bench/ort-dml"
PASSES="${PASSES:-2}"
WARMUP="${WARMUP:-10}"

DET3="$M3/ch_PP-OCRv3_det_infer.onnx"
DET5="$MX/ch_PP-OCRv5_det_mobile.onnx"
CLS="$M3/ch_ppocr_mobile_v2.0_cls_infer.onnx"
REC3="$M3/ch_PP-OCRv3_rec_infer.onnx"; DICT3="$M3/ppocr_keys_v1.txt"
REC4E="$MX/en_PP-OCRv4_rec_mobile.onnx"; DICT4E="$MX/en_dict.txt"
REC5E="$MX/en_PP-OCRv5_rec_mobile.onnx"; DICT5E="$MX/ppocrv5_en_dict.txt"
REC5L="$MX/latin_PP-OCRv5_rec_mobile.onnx"; DICT5L="$MX/ppocrv5_latin_dict.txt"
Q="$B/int8"

# id | label | engine | det | cls | rec | dict | threads | spin | extra
CANDIDATES=(
  "dll_v3ch|0.3.x DLL: det+rec ch v3 (taban, cls kapalı)|dll|$DET3|$CLS|$REC3|$DICT3|4|-|"
  "dll_v3ch_cls|0.3.x DLL: det+cls+rec ch v3|dll|$DET3|$CLS|$REC3|$DICT3|4|-|--cls-on"
  "ort_v3ch_det|ORT: det v3 + rec ch v3|ort|$DET3||$REC3|$DICT3|4|1|"
  "ort_v3ch_det_cls|ORT: det v3 + cls + rec ch v3|ort|$DET3|$CLS|$REC3|$DICT3|4|1|"
  "ort_v3ch_proj|ORT: izdüşüm + rec ch v3|ort|none||$REC3|$DICT3|4|1|"
  "ort_v4en_det|ORT: det v3 + rec en v4|ort|$DET3||$REC4E|$DICT4E|4|1|"
  "ort_v4en_proj|ORT: izdüşüm + rec en v4|ort|none||$REC4E|$DICT4E|4|1|"
  "ort_v5en_det|ORT: det v3 + rec en v5|ort|$DET3||$REC5E|$DICT5E|4|1|"
  "ort_v5en_proj|ORT: izdüşüm + rec en v5|ort|none||$REC5E|$DICT5E|4|1|"
  "ort_v5lat_det|ORT: det v3 + rec latin v5|ort|$DET3||$REC5L|$DICT5L|4|1|"
  "ort_v5lat_proj|ORT: izdüşüm + rec latin v5|ort|none||$REC5L|$DICT5L|4|1|"
  "ort_det5_v5en|ORT: det v5 + rec en v5|ort|$DET5||$REC5E|$DICT5E|4|1|"
  "ort_v3ch_det_q8|ORT: det v3 + rec ch v3, INT8 (kendi nicemleme)|ort|$Q/det_v3.onnx||$Q/rec_v3ch.onnx|$DICT3|4|1|"
  "ort_v4en_det_q8|ORT: det v3 + rec en v4, INT8 (kendi nicemleme)|ort|$Q/det_v3.onnx||$Q/rec_v4en.onnx|$DICT4E|4|1|"
  "ort_v5en_det_q8|ORT: det v3 + rec en v5, INT8 (kendi nicemleme)|ort|$Q/det_v3.onnx||$Q/rec_v5en.onnx|$DICT5E|4|1|"
  "ort_v4en_proj_q8|ORT: izdüşüm + rec en v4, INT8 (kendi nicemleme)|ort|none||$Q/rec_v4en.onnx|$DICT4E|4|1|"
  "ort_v5en_proj_q8|ORT: izdüşüm + rec en v5, INT8 (kendi nicemleme)|ort|none||$Q/rec_v5en.onnx|$DICT5E|4|1|"
  "winocr|Windows.Media.Ocr|winocr|||||-|-|"
)
# Thread caps / spinning / DirectML for the leaders: generated as <base>@t<N>s<0|1> and <base>@dml.
expand() {  # $1 = id → prints the candidate line
  local id="$1" base="${1%@*}" var="" line
  [[ "$id" == *@* ]] && var="${id#*@}"
  for line in "${CANDIDATES[@]}"; do
    [[ "${line%%|*}" == "$base" ]] || continue
    if [[ -z "$var" ]]; then echo "$line"; return 0; fi
    IFS='|' read -r _ label engine det cls rec dict threads spin extra <<<"$line"
    if [[ "$var" == dml ]]; then
      echo "$id|$label, DirectML|$engine|$det|$cls|$rec|$dict|$threads|$spin|--dml"
    elif [[ "$var" =~ ^t([0-9]+)s([01])$ ]]; then
      echo "$id|$label|$engine|$det|$cls|$rec|$dict|${BASH_REMATCH[1]}|${BASH_REMATCH[2]}|$extra"
    fi
    return 0
  done
  return 1
}

run_set() {  # $1 = candidate line, $2 = frames dir, $3 = result file
  local id label engine det cls rec dict threads spin extra
  IFS='|' read -r id label engine det cls rec dict threads spin extra <<<"$1"
  local args=(--frames "$2" --out "$3" --passes "$PASSES" --warmup "$WARMUP")
  for f in "$det" "$cls" "$rec"; do
    if [[ -n "$f" && "$f" != none && ! -f "$f" ]]; then echo "$id: missing $f" >>"$B/skipped.txt"; return 0; fi
  done
  case "$engine" in
    winocr)
      [[ -x "$B/winocr/winocr_bench.exe" ]] || { echo "$id: winocr_bench.exe not built (MSVC)" >>"$B/skipped.txt"; return 0; }
      "$B/winocr/winocr_bench.exe" "$2" "$3" "$PASSES" "$WARMUP" || echo "$id: failed (rc $?)" >>"$B/skipped.txt"
      return 0 ;;
    dll) args+=(--engine dll --rapid-dir "$RAPID") ;;
    ort)
      local ortdir="$RAPID"
      [[ "$extra" == *--dml* ]] && ortdir="$DML"
      args+=(--engine ort --ort-dir "$ortdir" --spin "$spin") ;;
  esac
  args+=(--id "$id" --label "$label" --det "$det" --rec "$rec" --dict "$dict" --threads "$threads")
  [[ -n "$cls" ]] && args+=(--cls "$cls")
  # shellcheck disable=SC2206
  [[ -n "$extra" ]] && args+=($extra)
  "$EXE" run "${args[@]}" >/dev/null || echo "$id: failed (rc $?)" >>"$B/skipped.txt"
}

mkdir -p "$B/results"
if [[ "${1:-}" != "--report" ]]; then
  ids=("$@")
  if [[ ${#ids[@]} -eq 0 ]]; then for l in "${CANDIDATES[@]}"; do ids+=("${l%%|*}"); done; fi
  for id in "${ids[@]}"; do
    line="$(expand "$id")" || { echo "unknown candidate $id" >&2; exit 2; }
    run_set "$line" "$B/frames" "$B/results/$id.tsv"
    if [[ -f "$B/real/truth.tsv" ]]; then run_set "$line" "$B/real" "$B/results/$id.real.tsv"; fi
  done
fi

commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)"
"$EXE" report --words "$ROOT/resources/en_words.txt" --baseline dll_v3ch --commit "$commit" --date "$(date +%F)" \
  --md "$B/report.md" --json "$B/report.json" "$B"/results/*.tsv
