# -*- coding: utf-8 -*-
"""Panelin büyük bileşenleri: geçmiş listesi."""

from collections import deque

import tkinter as tk
import tkinter.font as tkfont

from makine_live.ui import hud_ui as ui
from makine_live.ui.hud_ui import (BG, SURFACE2, LINE, TEXT, TEXT2, TEXT3, mix)


class HistoryList(tk.Canvas):
    """
    Son satırlar: tıkla = çeviriyi kopyala, çift tıkla = oyunda yeniden göster.

    Hız: satırın kısaltılmış metni eklenirken bir kez hesaplanır (ikili arama, ~8 ölçüm) ve liste en fazla bir kez,
    arayüz boşaldığında yeniden çizilir. Önceden her eklemede 50 satırın metni harf harf ölçülüyordu: satır başına ~1 sn,
    yüzlerce satır art arda gelince panel dakikalarca yanıt vermiyordu.
    """

    ROW_H = 62
    MAX = 50

    def __init__(self, master, fonts, width, height, on_copy, on_replay):
        super().__init__(master, width=width, height=height, bg=BG, highlightthickness=0, bd=0)
        self.f = fonts
        self.w = width
        self.on_copy, self.on_replay = on_copy, on_replay
        self.rows = deque(maxlen=self.MAX)
        self._hover = None
        self._fnt_main = tkfont.Font(font=fonts.ui(13))
        self._fnt_src = tkfont.Font(font=fonts.ui(12))
        self._fnt_badge = tkfont.Font(font=fonts.code(11))
        self._ctx = ui.UI(master.winfo_toplevel(), self, fonts)
        self.copy_icon = None
        self._redraw_job = None
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda e: self._set_hover(None))
        self.bind("<Button-1>", self._click)
        self.bind("<Double-Button-1>", self._dbl)
        self.bind("<MouseWheel>", lambda e: self.yview_scroll(int(-e.delta / 40), "units"))
        self.configure(yscrollincrement=8)
        self.redraw()

    def add(self, entry):
        entry = dict(entry)
        entry["_raw"] = self._fit(entry["raw"], self._fnt_src, self.w - 24)
        entry["_tr"] = self._fit(entry["translated"], self._fnt_main, self.w - 48)
        self.rows.appendleft(entry)
        self._schedule_redraw()

    def clear(self):
        self.rows.clear()
        self._schedule_redraw()

    def _schedule_redraw(self):
        if self._redraw_job is None:
            self._redraw_job = self.after_idle(self.redraw)

    @staticmethod
    def _fit(text, font, width):
        """Genişliğe sığan en uzun baş kısım + "…" (ölçüm sayısı metnin uzunluğuyla değil, logaritmasıyla artar)."""
        text = " ".join(text.split())
        if font.measure(text) <= width:
            return text
        lo, hi = 0, len(text)                  # text[:lo] + "…" sığar, text[:hi] + "…" sığmaz
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if font.measure(text[:mid] + "…") <= width:
                lo = mid
            else:
                hi = mid
        return text[:lo].rstrip() + "…"

    def redraw(self):
        if self._redraw_job is not None:
            try:
                self.after_cancel(self._redraw_job)
            except tk.TclError:
                pass
            self._redraw_job = None
        self.delete("all")
        self._hover = None
        if not self.rows:
            h = int(self.cget("height"))
            ic = ui.AnimatedIcon(self._ctx, self.w / 2, h / 2 - 22, "subtitle", 28, TEXT3)
            ic.set(t=1.0)
            self.create_text(self.w / 2, h / 2 + 12, text="Henüz satır yok", font=self.f.ui(13, semi=True), fill=TEXT2)
            self.create_text(self.w / 2, h / 2 + 32, text="F9 ile başlatın; çevrilen satırlar burada birikir.",
                             font=self.f.ui(12), fill=TEXT3)
            self.configure(scrollregion=(0, 0, self.w, h))
            self.copy_icon = None
            return
        for i, r in enumerate(self.rows):
            y = i * self.ROW_H
            tag = f"row{i}"
            self.create_image(0, y + 2, anchor="nw", tags=(tag, "rowbg"), state="hidden",
                              image=ui.rrect_image(self.w, self.ROW_H - 4, 6, fill=SURFACE2))
            self.create_text(12, y + 16, anchor="w", text=r["time"], font=self.f.code(11), fill=TEXT3, tags=tag)
            label, color = r["badge"]
            bw = self._fnt_badge.measure(label) + 12
            self.create_image(76, y + 7, anchor="nw", tags=tag,
                              image=ui.rrect_image(bw, 18, 4, fill=mix(BG, color, 0.14), outline=mix(BG, color, 0.45)))
            self.create_text(76 + bw / 2, y + 16, text=label, font=self.f.code(11), fill=color, tags=tag)
            if r.get("ms") is not None:
                self.create_text(self.w - 12, y + 16, anchor="e", text=f"{r['ms']} ms", font=self.f.code(11),
                                 fill=TEXT3, tags=tag)
            self.create_text(12, y + 34, anchor="w", text=r["_raw"], font=self.f.ui(12), fill=TEXT3, tags=tag)
            self.create_text(12, y + 51, anchor="w", text=r["_tr"], font=self.f.ui(13), fill=TEXT, tags=tag)
            if i < len(self.rows) - 1:
                self.create_line(12, y + self.ROW_H, self.w - 12, y + self.ROW_H, fill=LINE, tags=tag)
        self.copy_icon = ui.AnimatedIcon(self._ctx, -100, -100, "copy", 16, TEXT2)
        self.configure(scrollregion=(0, 0, self.w, len(self.rows) * self.ROW_H))

    def _index_at(self, e):
        i = int(self.canvasy(e.y) // self.ROW_H)
        return i if 0 <= i < len(self.rows) else None

    def _set_hover(self, i):
        if i == self._hover:
            return
        if self._hover is not None:
            for it in self.find_withtag(f"row{self._hover}"):
                if "rowbg" in self.gettags(it):
                    self.itemconfigure(it, state="hidden")
        self._hover = i
        if i is not None and self.copy_icon:
            for it in self.find_withtag(f"row{i}"):
                if "rowbg" in self.gettags(it):
                    self.itemconfigure(it, state="normal")
                    self.tag_lower(it)
            self.copy_icon.move(self.w - 22, i * self.ROW_H + 51)
            self.tag_raise(self.copy_icon.item)
            self.configure(cursor="hand2")
        else:
            if self.copy_icon:
                self.copy_icon.move(-100, -100)
            self.configure(cursor="")

    def _motion(self, e):
        self._set_hover(self._index_at(e))

    def _click(self, e):
        i = self._index_at(e)
        if i is not None:
            if self.copy_icon:
                self.copy_icon.play(320)
            self.on_copy(self.rows[i])

    def _dbl(self, e):
        i = self._index_at(e)
        if i is not None:
            self.on_replay(self.rows[i])
