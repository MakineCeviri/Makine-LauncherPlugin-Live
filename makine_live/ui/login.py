# -*- coding: utf-8 -*-
"""Giriş, bakım ve güncelleme ekranı."""

from PIL import ImageTk

import tkinter as tk
import tkinter.font as tkfont

from makine_live.ui import hud_ui as ui
from makine_live.ui.hud_ui import (BG, SURFACE2, TEXT, TEXT2, TEXT3, ACCENT, mix)
from makine_live.ui.brand import logo_image
from makine_live.ui.layout import W, H, PAD, INNER
from makine_live.version import APP_CHANNEL, APP_VERSION_LABEL


class LoginView:
    """
    Giriş ekranı (panelle aynı tuval dili). Durumlar:
      choice    Makine Çeviri hesabıyla giriş (ana yol) ya da ekip hesabı
      device    kod gösterilir; kullanıcı tarayıcıda açılan makineceviri.org/live/baglan/ sayfasında onaylar
      form      ekip hesabı: kullanıcı adı + şifre
      checking  kayıtlı oturum sunucuda doğrulanıyor
      offline   kayıtlı oturum var ama sunucuya ulaşılamadı (jeton silinmez; tekrar dene)
    """

    def __init__(self, root, fonts, on_submit, on_retry, on_switch, on_device=None, on_device_cancel=None,
                 on_device_reopen=None):
        self.root, self.f = root, fonts
        self.on_submit, self.on_retry, self.on_switch = on_submit, on_retry, on_switch
        self.on_device = on_device or (lambda: None)
        self.on_device_cancel = on_device_cancel or (lambda: None)
        self.on_device_reopen = on_device_reopen or (lambda: None)
        self.c = tk.Canvas(root, width=W, height=H, bg=BG, highlightthickness=0, bd=0)
        self.u = ui.UI(root, self.c, fonts)
        c, u, f = self.c, self.u, fonts
        cx = W / 2

        self._mark = ImageTk.PhotoImage(logo_image(64))
        c.create_image(cx, 170, image=self._mark)
        c.create_text(cx, 236, text="Makine Live", font=f.disp(26), fill=TEXT)
        if APP_CHANNEL:
            chip_f = f.ui(11, semi=True)
            cw = tkfont.Font(font=chip_f).measure(APP_CHANNEL.upper()) + 18
            c.create_image(cx - cw / 2, 260, anchor="nw",
                           image=ui.rrect_image(round(cw), 20, 10, fill=mix(BG, ACCENT, 0.12), outline=mix(BG, ACCENT, 0.5)))
            c.create_text(cx, 270, text=APP_CHANNEL.upper(), font=chip_f, fill=ACCENT)

        # ── Makine Çeviri hesabı (ana yol) ──
        y = 318
        c.create_text(PAD, y, anchor="nw", width=INNER, font=f.ui(13), fill=TEXT2, tags="choice",
                      text="Makine Çeviri hesabınızla giriş yapın. Discord ya da e-posta adresiniz yeter; şifre gerekmez.")
        self.btn_device = ui.Button(u, PAD, y + 62, INNER, 46, text="Makine Çeviri hesabıyla giriş yap", icon="arrow",
                                    kind="primary", command=self._device, font=f.ui(14, semi=True), tags=("choice",))
        self.err_choice = c.create_text(PAD, y + 120, anchor="nw", text="", font=f.ui(12), fill=ui.DISPUTED, width=INNER,
                                        tags="choice")
        c.create_text(cx, y + 176, text="Hesabınız yoksa açılan sayfada birkaç saniyede oluşturabilirsiniz.",
                      font=f.ui(12), fill=TEXT3, width=INNER, justify="center", tags="choice")
        ui.Button(u, PAD, y + 206, INNER, 38, text="Ekip hesabıyla giriş", kind="ghost",
                  command=lambda: self.show_form(), font=f.ui(13), tags=("choice",))

        # ── kod onayı ──
        c.create_text(cx, 330, text="Tarayıcıda açılan sayfada bu kodu onaylayın", font=f.ui(13), fill=TEXT2,
                      width=INNER, justify="center", tags="device")
        self.dev_code = c.create_text(cx, 384, text="", font=f.code(30), fill=TEXT, tags="device")
        self.dev_icon = ui.AnimatedIcon(u, cx, 440, "spinner", 22, ACCENT, tags=("device",))
        self.dev_sub = c.create_text(cx, 474, anchor="n", text="", font=f.ui(12), fill=TEXT3, width=INNER, justify="center",
                                     tags="device")
        ui.Button(u, PAD, 540, INNER, 42, text="Sayfayı yeniden aç", kind="secondary",
                  command=lambda: self.on_device_reopen(), font=f.ui(13, semi=True), tags=("device",))
        ui.Button(u, PAD, 592, INNER, 38, text="Vazgeç", kind="ghost",
                  command=lambda: self.on_device_cancel(), font=f.ui(13), tags=("device",))

        # ── ekip hesabı ──
        y = 318
        c.create_text(PAD, y, anchor="w", text="Ekip hesabınızla giriş yapın.", font=f.ui(13), fill=TEXT2,
                      tags="form")
        y += 30
        c.create_text(PAD, y + 8, anchor="w", text="Kullanıcı adı", font=f.ui(12, semi=True), fill=TEXT2, tags="form")
        self.var_user = tk.StringVar()
        self.fld_user = ui.Field(u, PAD, y + 22, INNER, 40, self.var_user, tags=("form",))
        y += 76
        c.create_text(PAD, y + 8, anchor="w", text="Şifre", font=f.ui(12, semi=True), fill=TEXT2, tags="form")
        self.var_pass = tk.StringVar()
        self._pass_visible = False
        self.fld_pass = ui.Field(u, PAD, y + 22, INNER, 40, self.var_pass, show="•", tags=("form",), lead_icon="key",
                                 trail=("eye_off", self._toggle_pass))
        y += 78
        self.err = c.create_text(PAD, y, anchor="nw", text="", font=f.ui(12), fill=ui.DISPUTED, width=INNER,
                                 tags="form")
        y += 34
        self.btn = ui.Button(u, PAD, y, INNER, 46, text="Giriş yap", icon="arrow", kind="primary",
                             command=self._submit, font=f.ui(14, semi=True), tags=("form",))
        for e in (self.fld_user.entry, self.fld_pass.entry):
            e.bind("<Return>", lambda ev: self._submit())
        ui.Button(u, PAD, y + 62, INNER, 38, text="Makine Çeviri hesabıyla giriş", kind="ghost",
                  command=lambda: self.show_choice(), font=f.ui(13), tags=("form",))

        # ── doğrulanıyor / çevrimdışı ──
        self.chk_icon = ui.AnimatedIcon(u, cx, 360, "spinner", 26, ACCENT, tags=("check",))
        self.chk_title = c.create_text(cx, 400, text="", font=f.ui(15, semi=True), fill=TEXT, tags="check")
        self.chk_sub = c.create_text(cx, 426, text="", font=f.ui(12), fill=TEXT3, width=INNER, justify="center",
                                     tags="check")
        self.btn_retry = ui.Button(u, PAD, 470, INNER, 44, text="Tekrar dene", icon="refresh", kind="primary",
                                   command=lambda: self.on_retry(), font=f.ui(14, semi=True), tags=("offline",))
        self.btn_switch = ui.Button(u, PAD, 524, INNER, 38, text="Başka hesapla giriş yap", kind="ghost",
                                    command=lambda: self.on_switch(), font=f.ui(13), tags=("offline",))

        c.create_text(cx, H - 34, text=f"sürüm {APP_VERSION_LABEL}", font=f.code(11), fill=TEXT3)
        self.show_choice()

    def _device(self):
        self.set_error("")
        self.btn_device.set_enabled(False)
        self.btn_device.set_text("Bağlanılıyor…")
        self.on_device()

    def show_choice(self, error=""):
        self.chk_icon.stop()
        self.dev_icon.stop()
        self._state(False, False, False, choice=True)
        self.btn_device.set_enabled(True)
        self.btn_device.set_text("Makine Çeviri hesabıyla giriş yap")
        self.set_error(error)

    def show_device(self, code, sub=""):
        """Kod onayı: kod büyük, altında ne yapılacağı. sub: süre / bağlantı durumu."""
        self.chk_icon.stop()
        self._state(False, False, False, device=True)
        self.c.itemconfigure(self.dev_code, text=code)
        self.c.itemconfigure(self.dev_sub, text=sub or "Sayfa açılmadıysa “Sayfayı yeniden aç”a basın. "
                                                       "Onaylayınca uygulama kendiliğinden giriş yapar.")
        self.dev_icon.set(name="spinner", color=ACCENT)
        self.dev_icon.loop(800)

    def set_device_note(self, text):
        self.c.itemconfigure(self.dev_sub, text=text)

    def _toggle_pass(self):
        self._pass_visible = not self._pass_visible
        self.fld_pass.set_show("" if self._pass_visible else "•")
        icon = self.fld_pass.trail
        icon.animate(1.0, 140, frm=0.0, done=lambda: (icon.set(name="eye" if self._pass_visible else "eye_off"),
                                                      icon.animate(0.0, 160)))

    def _state(self, form, check, offline, choice=False, device=False):
        self.c.delete("gate")
        if not device:
            self.dev_icon.stop()
        self.c.itemconfigure("choice", state="normal" if choice else "hidden")
        self.c.itemconfigure("device", state="normal" if device else "hidden")
        self.c.itemconfigure("form", state="normal" if form else "hidden")
        self.c.itemconfigure("check", state="normal" if check else "hidden")
        self.c.itemconfigure("offline", state="normal" if offline else "hidden")

    def _submit(self):
        user, pw = self.var_user.get().strip(), self.var_pass.get()
        if not user or not pw:
            self.set_error("Kullanıcı adı ve şifre gerekli.")
            return
        self.set_busy(True)
        self.on_submit(user, pw)

    def set_error(self, text):
        self.c.itemconfigure(self.err, text=text or "")
        self.c.itemconfigure(self.err_choice, text=text or "")

    def set_busy(self, busy):
        self.btn.set_enabled(not busy)
        self.btn.set_text("Giriş yapılıyor…" if busy else "Giriş yap")
        if busy:
            self.set_error("")

    def show_form(self, error="", username=None):
        self.chk_icon.stop()
        self._state(True, False, False)
        self.set_busy(False)
        self.set_error(error)
        if username is not None:
            self.var_user.set(username)
        self.var_pass.set("")
        target = self.fld_pass.entry if self.var_user.get() else self.fld_user.entry
        self.root.after(60, target.focus_set)

    def show_checking(self, user):
        self._state(False, True, False)
        self.chk_icon.set(name="spinner", color=ACCENT)
        self.chk_icon.loop(800)
        self.c.itemconfigure(self.chk_title, text="Oturum doğrulanıyor")
        self.c.itemconfigure(self.chk_sub, text=f"{user} olarak giriş yapılıyor…")

    def show_gate(self, icon, color, title, message, primary=None, secondary=None, progress=None):
        """Bakım / güncelleme ekranı. primary, secondary: (etiket, komut). progress: 0–1 ya da None."""
        self._state(False, False, False)
        self.chk_icon.stop()
        c, u, f, cx = self.c, self.u, self.f, W / 2
        icon_item = ui.AnimatedIcon(u, cx, 330, icon, 28, color, tags=("gate",))
        if icon == "spinner":
            icon_item.loop(800)
        c.create_text(cx, 372, text=title, font=f.ui(16, semi=True), fill=TEXT, tags="gate")
        c.create_text(cx, 398, anchor="n", text=message, font=f.ui(12), fill=TEXT2, width=INNER, justify="center", tags="gate")
        y = 540
        if progress is not None:
            c.create_image(PAD, y - 40, anchor="nw", tags="gate", image=ui.rrect_image(INNER, 6, 3, fill=SURFACE2))
            w = max(6, round(INNER * max(0.0, min(1.0, progress))))
            c.create_image(PAD, y - 40, anchor="nw", tags="gate", image=ui.rrect_image(w, 6, 3, fill=ACCENT))
        if primary:
            ui.Button(u, PAD, y, INNER, 44, text=primary[0], kind="primary", command=primary[1],
                      font=f.ui(14, semi=True), tags=("gate",))
        if secondary:
            ui.Button(u, PAD, y + 54, INNER, 38, text=secondary[0], kind="ghost", command=secondary[1],
                      font=f.ui(13), tags=("gate",))

    def show_offline(self, user, message):
        self._state(False, True, True)
        self.chk_icon.stop()
        self.chk_icon.set(name="link", t=0.0, color=ui.DISPUTED)
        self.c.itemconfigure(self.chk_title, text="Sunucuya ulaşılamıyor")
        self.c.itemconfigure(self.chk_sub, text=f"{user} hesabı kayıtlı. {message}")
