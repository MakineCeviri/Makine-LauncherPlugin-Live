"""
Logodan türetilen dosyaları üretir. Tek kaynak: logo_white-96.webp (depo kökü). Logo değişince bunu çalıştırın:

    py tools/brand_assets.py

Üretilenler:
  resources/icon.png              Launcher eklenti ikonu (manifest.json → "icon"; CMake build/release'e kopyalar)
  packaging/app.ico               .exe ikonu (build_exe.py de her derlemede yeniden üretir)
  tv/app/src/main/res/...         Makine TV (Android TV): uygulama ikonu, başlatıcı afişi (320×180), ekran logosu
  tv-webos/app/img/...            Makine TV (LG webOS): 80 / 130 px ikonlar, ekran logosu
  server/assets/...               yalnızca eski portal klasörü yerelde varsa: logo, favicon, apple-touch-icon
"""

import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from game_overlay import LOGO_PATH, app_mark_image, logo_image  # noqa: E402
import hud_ui as ui  # noqa: E402

TV_RES = os.path.join(ROOT, "tv", "app", "src", "main", "res")


def tv_banner():
    """Android TV başlatıcı afişi (xhdpi 320×180): mürekkep koyusu zemin, beyaz logo, "Makine TV"."""
    from PIL import Image, ImageDraw, ImageFont
    k = 2
    w, h = 320 * k, 180 * k
    im = Image.new("RGBA", (w, h), ui.rgb(ui.BG) + (255,))
    mark = logo_image(78 * k)
    im.alpha_composite(mark, (34 * k, (h - mark.height) // 2))
    fonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")

    def font(name, size):
        try:
            return ImageFont.truetype(os.path.join(fonts, name), size * k)
        except OSError:
            return ImageFont.load_default()

    d = ImageDraw.Draw(im)
    d.text((128 * k, 62 * k), "Makine TV", font=font("seguisb.ttf", 30), fill=ui.rgb(ui.TEXT))
    d.text((129 * k, 104 * k), "Türkçe altyazı", font=font("segoeui.ttf", 15), fill=ui.rgb(ui.ACCENT))
    return im.resize((320, 180), Image.LANCZOS)


def tv_assets() -> list:
    out = []
    for rel, img in (("drawable-nodpi/logo.png", logo_image(160)),
                     ("mipmap-xxxhdpi/ic_launcher.png", app_mark_image(192)),
                     ("mipmap-mdpi/ic_launcher.png", app_mark_image(48)),
                     ("drawable-xhdpi/banner.png", tv_banner())):
        path = os.path.join(TV_RES, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        img.save(path, optimize=True)
        out.append(os.path.relpath(path, ROOT).replace(os.sep, "/"))
    return out


def webos_assets() -> list:
    """Makine TV (LG webOS): uygulama ikonları (80 ve 130 px, LG'nin istediği boyutlar) ve ekran logosu."""
    out = []
    img_dir = os.path.join(ROOT, "tv-webos", "app", "img")
    os.makedirs(img_dir, exist_ok=True)
    for name, img in (("icon-80.png", app_mark_image(80)), ("icon-130.png", app_mark_image(130)),
                      ("logo.png", logo_image(144))):
        path = os.path.join(img_dir, name)
        img.save(path, optimize=True)
        out.append(os.path.relpath(path, ROOT).replace(os.sep, "/"))
    return out


def main() -> int:
    made = []
    assets = os.path.join(ROOT, "server", "assets")   # eski portal: yalnızca yerelde (depoda yok)
    if os.path.isdir(assets):
        shutil.copyfile(LOGO_PATH, os.path.join(assets, "logo-white.webp"))
        app_mark_image(64).save(os.path.join(assets, "favicon.png"), optimize=True)
        app_mark_image(180).save(os.path.join(assets, "apple-touch-icon.png"), optimize=True)
        made += ["server/assets/logo-white.webp", "server/assets/favicon.png", "server/assets/apple-touch-icon.png"]
    os.makedirs(os.path.join(ROOT, "resources"), exist_ok=True)
    logo_image(256).save(os.path.join(ROOT, "resources", "icon.png"), optimize=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [app_mark_image(s) for s in sizes]
    ico = os.path.join(ROOT, "packaging", "app.ico")
    images[-1].save(ico, format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])
    for rel in (*made, "resources/icon.png", "packaging/app.ico", *tv_assets(), *webos_assets()):
        print(f"  {rel:<36} {os.path.getsize(os.path.join(ROOT, rel)):>7} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
