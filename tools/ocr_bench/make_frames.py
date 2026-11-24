"""Synthetic game-subtitle frames for the OCR benchmark (tools/ocr_bench).

Renders the English side of spec/sample_quotes.json as open-world-game subtitles: white or yellow text with a dark
outline and drop shadow, 1-2 lines, optional speaker prefix ("LUCIA: ..."), over dark / bright-sky / busy-noise /
gradient backgrounds. Each frame is the capture-region strip (what the player selects), not a full screen:
1080p, 1440p and 4K strips, a 720p Remote Play-like variant (downscaled, slightly soft, JPEG-compressed) and
typewriter partial-reveal frames whose truth is only the revealed prefix.

Only Windows system fonts are used (Arial, Segoe UI, Bahnschrift, Calibri, Tahoma, Trebuchet, Verdana) — never a
game's own typeface. Deterministic for a given --seed.

Output: <out>/<id>.png|.jpg, <out>/truth.tsv (read by ocr_bench) and <out>/truth.json (for people).
Usage: python tools/ocr_bench/make_frames.py [--out build/ocr_bench/frames] [--seed 1234]
"""

import argparse
import json
import random
import textwrap
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = Path("C:/Windows/Fonts")
FONTS = ["arial.ttf", "arialbd.ttf", "segoeui.ttf", "segoeuib.ttf", "bahnschrift.ttf",
         "calibrib.ttf", "tahoma.ttf", "trebucbd.ttf", "verdana.ttf"]
SPEAKERS = ["LUCIA", "JASON", "MARCUS", "DANA", "RAUL", "TONY"]
WHITE = (245, 245, 245)
YELLOW = (255, 214, 64)

# Capture strip per screen resolution: width x height in pixels, base font size (cap ~ 0.7 of it).
STRIPS = {
    "1080p": ((1190, 184), 36),
    "1440p": ((1590, 246), 48),
    "4k": ((2380, 368), 72),
}

# Nine renditions per quote: 37 quotes x 9 = 333 frames.
VARIANTS = [
    ("1080p", "full", False), ("1440p", "full", False), ("4k", "full", False), ("rp720", "full", False),
    ("1080p", "typewriter", False), ("1440p", "typewriter", True),
    ("1080p", "full", True), ("4k", "full", True), ("rp720", "full", True),
]
BACKGROUNDS = ["dark", "sky", "busy", "gradient"]


def background(rng, size, kind):
    w, h = size
    if kind == "dark":
        base = rng.randint(8, 40)
        img = Image.new("RGB", size, (base, base + rng.randint(0, 8), base + rng.randint(0, 12)))
        noise = Image.effect_noise(size, 18).convert("RGB")
        return Image.blend(img, noise, 0.08)
    if kind == "sky":
        top = (rng.randint(90, 140), rng.randint(160, 200), rng.randint(220, 255))
        bottom = (rng.randint(215, 245), rng.randint(225, 245), rng.randint(235, 255))
        img = vertical_gradient(size, top, bottom)
        draw = ImageDraw.Draw(img)
        for _ in range(rng.randint(3, 8)):  # soft clouds
            cx, cy = rng.randint(0, w), rng.randint(0, h)
            rx, ry = rng.randint(w // 10, w // 3), rng.randint(h // 6, h // 2)
            draw.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=(250, 250, 252))
        return img.filter(ImageFilter.GaussianBlur(radius=max(4, h // 12)))
    if kind == "busy":
        img = Image.new("RGB", size, tuple(rng.randint(40, 200) for _ in range(3)))
        draw = ImageDraw.Draw(img)
        for _ in range(140):  # street-scene clutter: signs, windows, foliage, lights
            x0, y0 = rng.randint(-50, w), rng.randint(-50, h)
            x1, y1 = x0 + rng.randint(5, w // 5), y0 + rng.randint(5, h // 2)
            color = tuple(rng.randint(0, 255) for _ in range(3))
            if rng.random() < 0.5:
                draw.rectangle((x0, y0, x1, y1), fill=color)
            else:
                draw.line((x0, y0, x1, y1), fill=color, width=rng.randint(1, 6))
        noise = Image.effect_noise(size, 60).convert("RGB")
        img = Image.blend(img, noise, 0.25)
        return img.filter(ImageFilter.GaussianBlur(radius=rng.choice([0, 0.8, 1.5])))
    # gradient: any two colours, any direction
    a = tuple(rng.randint(0, 255) for _ in range(3))
    b = tuple(rng.randint(0, 255) for _ in range(3))
    img = vertical_gradient((h, w), a, b).rotate(90, expand=True) if rng.random() < 0.5 \
        else vertical_gradient(size, a, b)
    return img.resize(size)


def vertical_gradient(size, top, bottom):
    w, h = size
    col = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        col.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return col.resize((w, h))


def wrap(text, width_chars):
    lines = textwrap.wrap(text, width=width_chars)
    if len(lines) > 2:  # subtitles show at most two lines: rebalance into two
        half = len(text) // 2
        cut = text.rfind(" ", 0, half + 10)
        lines = [text[:cut].strip(), text[cut:].strip()]
    return lines


def render(rng, quote, res, variant, speaker, bg_kind):
    strip_res = "1080p" if res == "rp720" else res
    (w, h), base_size = STRIPS[strip_res]
    font_file = rng.choice(FONTS)
    size = int(base_size * rng.uniform(0.9, 1.1))
    text = quote
    name = None
    if speaker:
        name = rng.choice(SPEAKERS)
        text = f"{name}: {quote}"
    lines = wrap(text, rng.randint(44, 52))

    font = ImageFont.truetype(str(FONT_DIR / font_file), size)
    while max(font.getlength(line) for line in lines) > w * 0.94 and size > 12:
        size -= 2
        font = ImageFont.truetype(str(FONT_DIR / font_file), size)

    truth_lines = list(lines)
    if variant == "typewriter":  # characters appear one by one; truth is what is visible now
        total = sum(len(line) for line in lines)
        shown = max(4, int(total * rng.uniform(0.3, 0.85)))
        truth_lines = []
        for line in lines:
            if shown <= 0:
                break
            truth_lines.append(line[:shown].rstrip())
            shown -= len(line)
        truth_lines = [line for line in truth_lines if line]

    img = background(rng, (w, h), bg_kind)
    main_color = YELLOW if rng.random() < 0.3 else WHITE
    name_color = YELLOW if main_color == WHITE and rng.random() < 0.6 else main_color
    stroke = max(1, round(size / 14))
    shadow_off = max(1, round(size / 18))

    line_h = int(size * 1.22)
    block_h = line_h * len(lines)
    top = int((h - block_h) * rng.uniform(0.35, 0.75))

    shadow = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(shadow)
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    for i, full_line in enumerate(lines):
        visible = truth_lines[i] if i < len(truth_lines) else ""
        if variant == "typewriter" and not visible:
            continue
        x = (w - font.getlength(full_line)) / 2  # layout of the full line: revealed text does not re-centre
        y = top + i * line_h
        to_draw = visible if variant == "typewriter" else full_line
        sd.text((x + shadow_off, y + shadow_off), to_draw, font=font, fill=200,
                stroke_width=stroke, stroke_fill=200)
        if name and i == 0 and to_draw.startswith(name + ":"):
            head = name + ":"
            ld.text((x, y), head, font=font, fill=name_color, stroke_width=stroke, stroke_fill=(0, 0, 0))
            ld.text((x + font.getlength(head), y), to_draw[len(head):], font=font, fill=main_color,
                    stroke_width=stroke, stroke_fill=(0, 0, 0))
        else:
            ld.text((x, y), to_draw, font=font, fill=main_color, stroke_width=stroke, stroke_fill=(0, 0, 0))
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=shadow_off))
    img.paste((0, 0, 0), mask=shadow.point(lambda v: int(v * 0.7)))
    img.paste(layer, mask=layer)

    if res == "rp720":  # PS Remote Play at 720p: 2/3 scale, soft, blocky JPEG
        img = img.resize((w * 2 // 3, h * 2 // 3), Image.BILINEAR).filter(ImageFilter.GaussianBlur(radius=0.6))
    return img, truth_lines, font_file


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build" / "ocr_bench" / "frames"))
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    quotes = json.loads((ROOT / "spec" / "sample_quotes.json").read_text(encoding="utf-8"))["quotes"]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in list(out.glob("*.png")) + list(out.glob("*.jpg")):
        old.unlink()

    records = []
    n = 0
    for qi, q in enumerate(quotes):
        for vi, (res, variant, speaker) in enumerate(VARIANTS):
            bg_kind = BACKGROUNDS[(qi + vi) % len(BACKGROUNDS)]
            img, truth, font_file = render(rng, q["source"], res, variant, speaker, bg_kind)
            ext = "jpg" if res == "rp720" else "png"
            name = f"f{n:04d}_{res}_{variant}_{bg_kind}.{ext}"
            if ext == "jpg":
                img.save(out / name, quality=rng.randint(50, 70), subsampling=2)
            else:
                img.save(out / name)
            records.append({"file": name, "res": res, "variant": variant + ("+speaker" if speaker else ""),
                            "bg": bg_kind, "font": font_file, "lines": truth})
            n += 1

    (out / "truth.json").write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding="utf-8")
    with open(out / "truth.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write("#file\tres\tvariant\tbg\tfont\ttext\n")
        for r in records:
            f.write("\t".join([r["file"], r["res"], r["variant"], r["bg"], r["font"], "\\n".join(r["lines"])]) + "\n")
    print(f"{n} frames -> {out}")


if __name__ == "__main__":
    main()
