#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""
make_dc_frames.py — D-C, the compressed-stream set (OCR_KARAR §6) for the K3 text-mask gate.

    python tools/make_dc_frames.py --bench ../Makine-LauncherPlugin-Live-ocrbench [--out build/ocr_dc] [--seed 7]

Uses the benchmark's renderer (tools/ocr_bench/make_frames.py) for the subtitles. A Remote Play stream
never repeats a frame: here each subtitle stays on screen for several frames while the background behind
it changes (another random background of the same kind), every frame is JPEG-compressed with 4:2:0
chroma at a quality that drops to 15 (bit-rate collapse in a fast scene), some sequences are 720p
(downscaled and soft), some frames are frozen repeats, and empty frames (no subtitle) sit between lines.

Output, in playback order: <out>/<n>.jpg and <out>/truth.tsv ("file<TAB>text", text with \\n for line breaks).
"""

import argparse
import importlib.util
import io
import json
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True, help="the ocrbench repository")
    ap.add_argument("--out", default=str(ROOT / "build" / "ocr_dc"))
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    spec = importlib.util.spec_from_file_location("make_frames", Path(args.bench) / "tools" / "ocr_bench" / "make_frames.py")
    mf = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mf)

    rng = random.Random(args.seed)
    quotes = json.loads((ROOT / "spec" / "sample_quotes.json").read_text(encoding="utf-8"))["quotes"]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.jpg"):
        old.unlink()

    rows = []
    n = 0

    def save(img, text):
        nonlocal n
        buf = io.BytesIO()
        quality = rng.choice([15, 25, 35, 50, 65]) if rng.random() < 0.7 else rng.randint(66, 80)
        img.convert("RGB").save(buf, "JPEG", quality=quality, subsampling=2)   # 4:2:0
        name = f"{n:04d}.jpg"
        (out / name).write_bytes(buf.getvalue())
        rows.append(f"{name}\t" + "\\n".join(text))
        n += 1
        return buf.getvalue()

    original_background = mf.background
    for qi, q in enumerate(quotes):
        res = "rp720" if qi % 3 == 0 else "1080p"
        kind = mf.BACKGROUNDS[qi % len(mf.BACKGROUNDS)]
        speaker = qi % 4 == 1
        layout_seed = rng.randrange(1 << 30)
        size = None
        # The same subtitle over a moving background: the renderer's own rng lays the text out the same
        # way each time, the background comes from a separate rng
        for frame in range(rng.randint(3, 6)):
            bg_rng = random.Random(rng.randrange(1 << 30))
            mf.background = lambda _rng, s, k, bg_rng=bg_rng: original_background(bg_rng, s, k)
            img, truth, _ = mf.render(random.Random(layout_seed), q["source"], res, "full", speaker, kind)
            size = img.size
            data = save(img, truth)
            if frame == 1 and rng.random() < 0.3:   # a frozen frame: the stream repeats the last picture
                (out / f"{n:04d}.jpg").write_bytes(data)
                rows.append(f"{n:04d}.jpg\t" + "\\n".join(truth))
                n += 1
        mf.background = original_background
        for _ in range(rng.randint(0, 2)):          # the subtitle is gone, the scene goes on
            save(original_background(random.Random(rng.randrange(1 << 30)), size, kind), [])
    (out / "truth.tsv").write_text("#file\ttext\n" + "\n".join(rows) + "\n", encoding="utf-8")
    print(f"{n} frames in {out}")


if __name__ == "__main__":
    sys.exit(main())
