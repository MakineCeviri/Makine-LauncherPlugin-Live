# -*- coding: utf-8 -*-
"""OCR metninin süzgeçleri: sistem / arayüz yazıları ve grafik gürültüsü."""

import re


def clean_ocr_text(raw: str) -> str:
    """OCR çıktısı: kutular "\n" ile gelir; çok satırlı altyazı tek cümle olarak çevrilir."""
    return " ".join(raw.split())


# Satırın TAMAMI bunlardan biriyse arayüz/sistem yazısıdır (alt dize aranmaz: "STARTED", "SPILL" geçerli)
_SYSTEM_LINES = {
    "start", "search", "taskbar", "widgets", "weather", "battery", "microsoft", "google chrome", "discord", "steam",
    "başlat", "ara", "görev çubuğu", "hava durumu", "pil", "duraklat", "kilit", "alan seç", "taşıma",
    "canlı çeviri", "havuz", "f8", "f9", "f10",
}


def is_valid_dialogue_text(text: str) -> bool:
    """Çöp, grafik gürültüsü ve sistem yazılarını eler. Uzun / çok satırlı / boşluğu kaybolmuş cümleleri ELEMEZ."""
    cleaned = clean_ocr_text(text)
    if len(cleaned) < 3:
        return False

    # 1. Ardışık 3 veya daha fazla aynı harf (örn: 'aaa', 'epieanoaisanadeaaa,a')
    if re.search(r'([a-zA-Z])\1{2,}', cleaned):
        return False

    # 2. Tek parça, boşluksuz, çok uzun dizi: doku/yağmur gürültüsü. (Eski kural her >13 harfli kelimeyi
    #    eliyordu; OCR boşluk yuttuğunda uzun cümleler tamamen kayboluyordu.)
    if " " not in cleaned and len(cleaned) > 28:
        return False

    # 3. Harf oranı: karakterlerin en az %60'ı harf olmalı
    non_space = [c for c in cleaned if not c.isspace()]
    letters = [c for c in non_space if c.isalpha()]
    if not non_space or len(letters) < 2 or len(letters) / len(non_space) < 0.60:
        return False

    # 4. Arayüz / görev çubuğu yazıları (yalnızca satırın tamamı eşleşirse)
    if cleaned.lower().strip(" .:") in _SYSTEM_LINES:
        return False
    return True
