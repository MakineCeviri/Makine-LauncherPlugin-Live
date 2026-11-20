# -*- coding: utf-8 -*-
"""Makine Çeviri logosu ve uygulama ikonu (pencere, exe, giriş ekranı)."""

from PIL import Image

from makine_live.paths import LOGO_PATH
from makine_live.ui import hud_ui as ui
from makine_live.ui.hud_ui import SURFACE2, LINE_STRONG


def logo_image(size: int, color=None) -> Image.Image:
    """
    Logo, size × size RGBA (saydam zemin, varsayılan beyaz). Kaynak 96 px olduğu için büyük boyutlarda alfa kanalı
    yumuşak büyütülüp kenarları yeniden keskinleştirilir: tek renkli düz bir işaret olduğundan bulanıklaşmaz.
    """
    src = Image.open(LOGO_PATH).convert("RGBA")
    alpha = src.getchannel("A")
    if size > src.width:
        big = alpha.resize((size * 2, size * 2), Image.BICUBIC)
        big = big.point([max(0, min(255, (a - 128) * 3 + 128)) for a in range(256)])
        alpha = big.resize((size, size), Image.LANCZOS)
    else:
        alpha = alpha.resize((size, size), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (ui.rgb(color) if color else (255, 255, 255)) + (0,))
    out.putalpha(alpha)
    return out


def app_mark_image(size: int) -> Image.Image:
    """Uygulama ikonu: mürekkep koyusu yuvarlak karede beyaz logo (pencere ve exe ikonu; açık zeminde de görünür)."""
    from PIL import ImageDraw
    k = 4
    s = size * k
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=round(s * 0.22), fill=ui.rgb(SURFACE2) + (255,),
                        outline=ui.rgb(LINE_STRONG) + (255,), width=max(1, round(s * 0.02)))
    im = im.resize((size, size), Image.LANCZOS)
    inner = max(8, round(size * (0.86 if size <= 32 else 0.78)))   # logonun kendi iç boşluğu var (96 px'te 5 px)
    im.alpha_composite(logo_image(inner), ((size - inner) // 2, (size - inner) // 2))
    return im
