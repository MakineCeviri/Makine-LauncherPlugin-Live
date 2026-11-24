#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

"""
make_scene_frames.py — whole game screens for the subtitle-finder gate (tests/cpp/finder_gate.cpp).

    python tools/make_scene_frames.py --bench ../Makine-LauncherPlugin-Live-ocrbench [--out build/scene_frames] [--seed 11]

The OCR benchmark's frames are capture strips (what a player selects), not screens. Here each strip is
put back on a full screen of its resolution (1080p, 1440p, 4K, a 720p Remote Play stream) at a known
place, its edges feathered into the scene, among HUD text that is not a subtitle: a minimap street
name, money, ammo, a top-left objective, sometimes a big mission title at the top and a "Press E"
prompt. An episode is one game scene sampled every 2 s (the finder's cadence) for 10 samples: the
subtitle comes and goes and changes like dialogue. Most bands are in the lower third, some at the top;
some episodes have no subtitle at all (only HUD) and must never produce a band.

Output: <out>/<episode>/<nn>.jpg and <out>/truth.json:
  [{"episode", "width", "height", "band": [x0, y0, x1, y1] | null (px, the strip's rect), "frames": [{"file", "subtitle"}]}]
"""

import argparse
import json
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCREENS = {"1080p": (1920, 1080), "1440p": (2560, 1440), "4k": (3840, 2160), "rp720": (1280, 720)}
FONTS = Path("C:/Windows/Fonts")
STREETS = ["Vinewood", "Vice Beach", "Little Havana", "Port Gellhorn", "Leonida Keys", "Grassrivers", "Downtown"]
OBJECTIVES = ["Go to the marina.", "Follow Lucia.", "Lose the cops.", "Get in the car.", "Find the boat.", "Collect the cash."]


def font(name, size):
    try:
        return ImageFont.truetype(str(FONTS / name), size)
    except OSError:
        return ImageFont.load_default()


def scene(rng, size):
    w, h = size
    top = tuple(rng.randint(60, 200) for _ in range(3))
    bottom = tuple(rng.randint(10, 120) for _ in range(3))
    img = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        img.putpixel((0, y), tuple(int(top[k] * (1 - t) + bottom[k] * t) for k in range(3)))
    img = img.resize((w, h))
    d = ImageDraw.Draw(img)
    for _ in range(rng.randint(8, 20)):   # buildings, cars, palms: blocks of colour
        x0, y0 = rng.randint(0, w), rng.randint(h // 4, h)
        bw, bh = rng.randint(w // 30, w // 6), rng.randint(h // 20, h // 2)
        d.rectangle([x0, y0 - bh, x0 + bw, y0], fill=tuple(rng.randint(0, 255) for _ in range(3)))
    noise = Image.effect_noise((w // 4, h // 4), 40).convert("RGB").resize((w, h))
    return Image.blend(img, noise, 0.15).filter(ImageFilter.GaussianBlur(radius=w / 1200))


def hud(rng, img, title):
    w, h = img.size
    d = ImageDraw.Draw(img)
    s = h / 1080
    small, mid = font("arialbd.ttf", int(26 * s)), font("arialbd.ttf", int(34 * s))

    def outlined(xy, text, f, fill=(255, 255, 255), anchor="la"):
        d.text(xy, text, font=f, fill=fill, anchor=anchor, stroke_width=max(1, int(2 * s)), stroke_fill=(0, 0, 0))

    # minimap (bottom left) with the street name under it
    mx, my, mw = int(40 * s), int(h - 300 * s), int(260 * s)
    d.rectangle([mx, my, mx + mw, my + int(200 * s)], fill=(40, 60, 50), outline=(0, 0, 0), width=max(1, int(3 * s)))
    outlined((mx, my + int(210 * s)), rng.choice(STREETS), small)
    outlined((w - int(40 * s), int(40 * s)), f"${rng.randint(100, 99999):,}", mid, fill=(120, 230, 120), anchor="ra")
    outlined((w - int(40 * s), int(90 * s)), f"{rng.randint(1, 30)}/{rng.randint(30, 240)}", small, anchor="ra")
    outlined((int(40 * s), int(40 * s)), rng.choice(OBJECTIVES), small, fill=(255, 230, 120))
    if title:
        outlined((w // 2, int(120 * s)), title, font("impact.ttf", int(72 * s)), anchor="ma")
    if rng.random() < 0.3:
        outlined((int(w * 0.72), int(h * 0.55)), "Press E to talk", small)


def paste_strip(img, strip, x, y):
    sw, sh = strip.size
    mask = Image.new("L", (sw, sh), 255)
    feather = max(4, sh // 6)
    md = ImageDraw.Draw(mask)
    for i in range(feather):   # fade the strip's own background into the scene at its edges
        v = int(255 * (i + 1) / feather)
        md.rectangle([i, i, sw - 1 - i, sh - 1 - i], outline=v)
    img.paste(strip, (x, y), mask)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True, help="the ocrbench repository")
    ap.add_argument("--out", default=str(ROOT / "build" / "scene_frames"))
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--episodes", type=int, default=40)
    ap.add_argument("--negatives", type=int, default=10)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    frames_dir = Path(args.bench) / "build" / "ocr_bench" / "frames"
    truth = json.loads((frames_dir / "truth.json").read_text(encoding="utf-8"))
    by_res = {}
    for t in truth:
        if t.get("variant") == "full":   # whole lines; typewriter frames are half-written subtitles
            by_res.setdefault(t["res"], []).append(t)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    episodes = []
    total = args.episodes + args.negatives
    for e in range(total):
        negative = e >= args.episodes
        res = rng.choice(sorted(by_res))
        size = SCREENS[res]
        w, h = size
        pool = by_res[res]
        strip_w, strip_h = Image.open(frames_dir / pool[0]["file"]).size
        top = rng.random() < 0.2
        x = (w - strip_w) // 2 + rng.randint(-w // 100, w // 100)
        y = rng.randint(int(h * 0.03), int(h * 0.08)) if top else rng.randint(int(h * 0.70), h - strip_h - int(h * 0.02))
        title = "" if top else rng.choice(["", "", "MISSION PASSED", "VICE CITY"])   # a title never covers a top band
        base = scene(rng, size)
        present = [rng.random() < 0.65 for _ in range(10)]
        if not negative:
            for i in rng.sample(range(10), 3):
                present[i] = True
        ep = f"e{e:03d}"
        (out / ep).mkdir(exist_ok=True)
        frames = []
        for i in range(10):
            img = base.copy()
            # the camera moves a little between samples
            img = img.transform(size, Image.AFFINE, (1, 0, rng.randint(-8, 8), 0, 1, rng.randint(-4, 4)), Image.BILINEAR)
            sub = ""
            if present[i] and not negative:
                t = rng.choice(pool)
                paste_strip(img, Image.open(frames_dir / t["file"]).convert("RGB"), x, y)
                sub = "\n".join(t["lines"])
            hud(random.Random(args.seed * 1000 + e), img, title)   # the same HUD all episode long
            name = f"{i:02d}.jpg"
            img.save(out / ep / name, quality=88)
            frames.append({"file": f"{ep}/{name}", "subtitle": sub})
        episodes.append({"episode": ep, "width": w, "height": h, "res": res,
                         "band": None if negative else [x, y, x + strip_w, y + strip_h], "frames": frames})
    (out / "truth.json").write_text(json.dumps(episodes, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"{total} episodes ({args.negatives} without subtitles) -> {out}")


if __name__ == "__main__":
    main()
