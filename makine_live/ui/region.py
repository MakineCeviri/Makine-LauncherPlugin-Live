# -*- coding: utf-8 -*-
"""F10: ekrandan diyalog bölgesi seçme aracı."""

import tkinter as tk
import tkinter.font as tkfont

from makine_live.ui import hud_ui as ui
from makine_live.ui.hud_ui import (RAISED, LINE_STRONG, TEXT, TEXT2, TEXT3, ACCENT, ACCENT_INK)


class RegionSelector(tk.Toplevel):
    """
    Ekranı karartır, seçilen alanı "delik" olarak açık bırakır (renk anahtarı), imleci vurgu
    renginde artı işaretiyle izler ve canlı boyut etiketi gösterir.
    """

    KEY = "#010203"

    def __init__(self, master, fonts, callback, current=None):
        super().__init__(master)
        self.callback = callback
        self.f = fonts
        self.overrideredirect(True)
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        self.geometry(f"{sw}x{sh}+0+0")
        self.attributes("-topmost", True)
        self.attributes("-alpha", 0.72)
        self.attributes("-transparentcolor", self.KEY)
        self.configure(bg="#000000", cursor="none")

        self.c = tk.Canvas(self, bg="#08090a", highlightthickness=0, bd=0, cursor="none")
        self.c.pack(fill="both", expand=True)
        self.sw, self.sh = sw, sh
        self.hole = self.c.create_rectangle(0, 0, 0, 0, fill=self.KEY, outline="", state="hidden")
        self.frame = self.c.create_rectangle(0, 0, 0, 0, outline=ACCENT, width=2, state="hidden")
        self.handles = [self.c.create_rectangle(0, 0, 0, 0, fill=ACCENT, outline="", state="hidden") for _ in range(4)]
        self.vline = self.c.create_line(0, 0, 0, sh, fill=ACCENT, dash=(4, 4))
        self.hline = self.c.create_line(0, 0, sw, 0, fill=ACCENT, dash=(4, 4))
        self.size_bg = self.c.create_image(0, 0, anchor="nw", state="hidden")
        self.size_txt = self.c.create_text(0, 0, anchor="w", font=fonts.code(13), fill=ACCENT_INK, state="hidden")

        tip = "Diyalog alanını sürükleyerek seçin     Esc  vazgeç"
        fnt = tkfont.Font(font=fonts.ui(14, semi=True))
        tw = fnt.measure(tip) + 36
        self.c.create_image(sw // 2 - tw // 2, 36, anchor="nw",
                            image=ui.rrect_image(tw, 40, 8, fill=RAISED, outline=LINE_STRONG))
        self.c.create_text(sw // 2, 56, text=tip, font=fonts.ui(14, semi=True), fill=TEXT)

        if current:
            x, y, w, h = current
            self.c.create_rectangle(x, y, x + w, y + h, outline=TEXT3, dash=(6, 4))
            self.c.create_text(x, y - 8, anchor="sw", text="mevcut bölge", font=fonts.ui(12), fill=TEXT2)

        self.start = None
        self.c.bind("<Motion>", self._motion)
        self.c.bind("<ButtonPress-1>", self._press)
        self.c.bind("<B1-Motion>", self._drag)
        self.c.bind("<ButtonRelease-1>", self._release)
        self.bind("<Escape>", lambda e: self.destroy())
        self.focus_force()

    def _motion(self, e):
        self.c.coords(self.vline, e.x, 0, e.x, self.sh)
        self.c.coords(self.hline, 0, e.y, self.sw, e.y)

    def _press(self, e):
        self.start = (e.x, e.y)
        for it in (self.hole, self.frame, self.size_bg, self.size_txt, *self.handles):
            self.c.itemconfigure(it, state="normal")
        self._drag(e)

    def _drag(self, e):
        self._motion(e)
        if not self.start:
            return
        x0, y0 = self.start
        x1, y1 = e.x, e.y
        l, t, r, b = min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)
        self.c.coords(self.hole, l, t, r, b)
        self.c.coords(self.frame, l, t, r, b)
        for hnd, (hx, hy) in zip(self.handles, ((l, t), (r, t), (l, b), (r, b))):
            self.c.coords(hnd, hx - 3, hy - 3, hx + 3, hy + 3)
        label = f"{r - l} × {b - t}"
        lw = tkfont.Font(font=self.f.code(13)).measure(label) + 16
        ly = t - 30 if t > 40 else b + 8
        self.c.coords(self.size_bg, l, ly)
        self.c.itemconfigure(self.size_bg, image=ui.rrect_image(lw, 22, 4, fill=ACCENT))
        self.c.coords(self.size_txt, l + 8, ly + 11)
        self.c.itemconfigure(self.size_txt, text=label)

    def _release(self, e):
        if not self.start:
            return
        x0, y0 = self.start
        x, y, w, h = min(x0, e.x), min(y0, e.y), abs(e.x - x0), abs(e.y - y0)
        self.destroy()
        if w > 15 and h > 15:
            self.callback(x, y, w, h)
