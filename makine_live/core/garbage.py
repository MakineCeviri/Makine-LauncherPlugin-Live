# -*- coding: utf-8 -*-
"""
OCR metninin süzgeçleri. Oyuncuya ve havuza giden her satır buradan geçer:

  filter_ocr(raw)       DLL'in satırlarını ("\\n" ile ayrılmış kutular) birleştirmeden önce tek tek süzer: gürültü,
                        oyunun arayüz yazıları (HUD, araç adı, konuşan adı), tuş simgeleri. Kalan cümleyi döndürür.
  garbage_reason(text)  sunucunun çöp kuralının birebir kopyası (Makine-WEB src/live/text.ts → garbageReason;
                        ortak vektörler spec/garbage_vectors.json). Sunucunun 422 ile reddedeceği satır hiç gönderilmez.

Kurallar ilk kez gerçek oyun kayıtlarıyla (GTA VI, Kasım 2026) ayarlandı: "RES", "SECURITY TRACKER", "VAPID JASON'S '70",
"Ya Ya You must be busy", "ee id n na do rs error a ra" gibi satırlar çeviriye gidiyor, çevirilerde rastgele harf ve
rakam olarak görünüyordu.
"""

from __future__ import annotations

import re
import unicodedata
from typing import Optional

from makine_live.net.pool_client import compact_pool_key, normalize_dialogue_text
from makine_live.paths import bundled


def clean_ocr_text(raw: str) -> str:
    """OCR çıktısı: kutular "\\n" ile gelir; çok satırlı altyazı tek cümle olarak çevrilir."""
    return " ".join(raw.split())


# Satırın TAMAMI bunlardan biriyse arayüz/sistem yazısıdır (alt dize aranmaz: "STARTED", "SPILL" geçerli)
_SYSTEM_LINES = {
    "start", "search", "taskbar", "widgets", "weather", "battery", "microsoft", "google chrome", "discord", "steam",
    "başlat", "ara", "görev çubuğu", "hava durumu", "pil", "duraklat", "kilit", "alan seç", "taşıma",
    "canlı çeviri", "havuz", "f8", "f9", "f10",
}


def is_valid_dialogue_text(text: str) -> bool:
    """Grafik gürültüsü ve sistem yazıları. Uzun / çok satırlı / boşluğu kaybolmuş cümleleri ELEMEZ."""
    cleaned = clean_ocr_text(text)
    if len(cleaned) < 3:
        return False
    # Tek parça, boşluksuz, çok uzun dizi: doku/yağmur gürültüsü (OCR boşluk yuttuğunda uzun cümleler kaybolmasın diye
    # yalnızca 28 harften uzun tek token)
    if " " not in cleaned and len(cleaned) > 28:
        return False
    # Harf oranı: karakterlerin en az %60'ı harf olmalı
    non_space = [c for c in cleaned if not c.isspace()]
    letters = [c for c in non_space if c.isalpha()]
    if not non_space or len(letters) < 2 or len(letters) / len(non_space) < 0.60:
        return False
    # Arayüz / görev çubuğu yazıları (yalnızca satırın tamamı eşleşirse)
    if cleaned.lower().strip(" .:") in _SYSTEM_LINES:
        return False
    return True


# ─────────────────────────────────────────────────────────────
# Sunucunun çöp kuralı (text.ts garbageReason ile aynı; değiştirirseniz ikisini ve vektörleri birlikte)
# ─────────────────────────────────────────────────────────────

LATIN_SOURCE_LANGS = {"en", "de", "fr", "es", "it", "pt", "tr", "pl", "nl", "sv", "da", "no", "fi", "cs", "hu", "ro"}

_CJK = re.compile("[ᄀ-ᇿ　-〿぀-ヿ㄰-㆏㐀-䶿一-鿿가-힯"
                  "豈-﫿＀-￯\U00020000-\U0002fa1f]")
_INTERJECTION = re.compile(
    r"^(h*m+h*m*|h+m+p+h*|u+h+|u+m+|uh-?huh|a+h+|o+h+|w+h?o+a+h*|s+h+|p+s+t+|a?r+g+h+|e+r+m*|u+g+h+|h+u+h+|o+w+|o+p+s+"
    r"|y+e+a+h+|y+e+p+|n+o+p+e+|ok(ay)?|a+h+a+|wo+w+|he+y+|ha+(ha+)*|he+(he+)*|pf+t*|tsk|brr+|gr+)$", re.I)
_SPLIT_SHORT = {"i", "a", "to", "of", "in", "is", "it", "on", "at", "be", "do", "go", "he", "me", "my", "no",
                "so", "up", "us", "we", "an", "as", "by", "if", "or", "oh", "ok", "am"}
# İki harfli kelimelerden yalnızca bunlar gerçek sayılır: sıklık listesi neredeyse her harf çiftini içeriyor
_REAL_SHORT = _SPLIT_SHORT | {"hi", "ya", "yo", "ye", "ah", "eh", "uh", "hm", "mm", "ha", "ow", "aw", "em", "ma", "pa",
                              "mr", "ms", "dr", "st", "tv", "pc", "ex", "th", "ta"}
_ADDRESS = re.compile(r"https?://|www\.|[\w.+-]+@[\w-]+\.\w|\b[a-z]:[\\/]|\.(exe|dll|png|jpe?g|txt|zip|php|html?)\b", re.I)
_EN_TOKEN = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)?")

_words: Optional[dict] = None


def _english_words() -> dict:
    """Kelime → sıklık sırası. Sunucuyla aynı küme: en_words.txt'nin ilk 30 000 kelimesi + kesme işaretli kelimeler."""
    global _words
    if _words is None:
        path = bundled("resources/en_words.txt", "runtime/resources/en_words.txt")
        words: dict = {}
        try:
            with open(path, encoding="utf-8") as fh:
                all_words = fh.read().split()
        except OSError:
            all_words = []
        for i, w in enumerate(all_words):
            if (i < 30000 or "'" in w) and w not in words:
                words[w] = i
        _words = words
    return _words


def is_interjection(t: str) -> bool:
    return bool(_INTERJECTION.match(t))


def _splits_into_words(lc: str, words: dict) -> bool:
    """"goingto" → going + to: her parça yaygın bir kelime (ilk 10 000) ya da listedeki kısa kelime."""
    n = len(lc)
    if n < 3 or n > 40 or not re.fullmatch(r"[a-z]+", lc):
        return False
    ok = [False] * (n + 1)
    ok[0] = True
    for i in range(1, n + 1):
        for k in range(max(0, i - 20), i):
            if ok[i]:
                break
            if not ok[k]:
                continue
            piece = lc[k:i]
            if (words.get(piece, 10 ** 9) < 10000) if len(piece) >= 3 else (piece in _SPLIT_SHORT):
                ok[i] = True
    return ok[n]


def english_word_score(text: str) -> tuple:
    """(gerçek İngilizce kelime oranı, kelime sayısı). Sözlükte olmayan büyük harfli kelime 0.3 (isim olabilir)."""
    words = _english_words()
    score, count = 0.0, 0
    for tok in _EN_TOKEN.findall(text):
        if len(tok) < 2:
            continue
        count += 1
        lc = tok.lower()
        if len(lc) == 2:
            if lc in _REAL_SHORT or is_interjection(lc):
                score += 1
            continue
        if lc in words or re.sub(r"'.*$", "", lc) in words or is_interjection(lc) or _splits_into_words(lc, words):
            score += 1
        elif re.fullmatch(r"[A-Z][a-z]{2,}", tok):
            score += 0.3
    return (score / count if count else 1.0), count


def _is_letter(ch: str) -> bool:
    return unicodedata.category(ch).startswith("L")


def garbage_reason(source: str, translation: str = "", source_lang: str = "en") -> Optional[str]:
    """Satır diyalog değilse nedeni (OCR gürültüsü, adres, çevrilmemiş…), değilse None. Sunucuyla aynı kural."""
    src = source.strip()
    norm = normalize_dialogue_text(src)
    lang = source_lang.lower()
    if lang in LATIN_SOURCE_LANGS and _CJK.search(src):
        return "yabancı alfabe (OCR gürültüsü)"
    if len(norm) < 3:
        return "çok kısa"
    letters = sum(1 for ch in norm if _is_letter(ch))
    if letters < 2:
        return "harf yok"
    if re.fullmatch(r"[il1|!\s.,]+", norm) and len(re.findall(r"[il1|]", norm)) >= 4:
        return "çizgi gürültüsü"
    ws = [w for w in re.split(r"[^\w]+|[\d_]+", norm) if w and all(_is_letter(c) for c in w)]
    all_interjections = bool(ws) and all(is_interjection(w) for w in ws)
    if not all_interjections and re.search(r"([^\W\d_])\1{2,}", norm) and len({c for c in norm if _is_letter(c)}) < 4:
        return "tekrarlayan harf"
    if _ADDRESS.search(src):
        return "adres veya dosya adı"
    visible = sum(1 for ch in norm if not ch.isspace())
    if visible > 0 and letters / visible < 0.6:
        return "sembol veya rakam ağırlıklı"
    tokens = [t for t in norm.split(" ") if t]
    single = sum(1 for t in tokens if len(t) == 1)
    if len(tokens) >= 3 and single / len(tokens) >= 0.6:
        return "parçalı OCR gürültüsü"
    if re.fullmatch(r"[\x20-\x7e]+", norm):
        long_ = [t for t in tokens if len(re.sub(r"[^a-z]", "", t)) >= 4]
        no_vowel = [t for t in long_ if not re.search(r"[aeiouy]", t) and not is_interjection(re.sub(r"[^a-z]", "", t))]
        if long_ and len(no_vowel) / len(long_) > 0.5:
            return "anlamsız harf dizisi"
    if lang == "en":
        score, n = english_word_score(src)
        if n >= 3 and score < 0.5:
            return "anlamsız kelimeler"
        if 1 <= n <= 2 and score == 0:
            return "anlamsız kelime"
    if translation and compact_pool_key(translation) == compact_pool_key(src):
        return "çevrilmemiş"
    return None


# ─────────────────────────────────────────────────────────────
# OCR satırları: gürültü ve oyun arayüzü
# ─────────────────────────────────────────────────────────────

# Satır tek başına bunlardan biri yüzünden düşerse atılır. "çok kısa" ve "anlamsız kelime" (1–2 kelime) parçaları
# atılmaz: altyazı iki kutuya bölünebilir ("I was going to" / "go.").
_LINE_DROP = {"yabancı alfabe (OCR gürültüsü)", "harf yok", "çizgi gürültüsü", "tekrarlayan harf", "adres veya dosya adı",
              "sembol veya rakam ağırlıklı", "parçalı OCR gürültüsü", "anlamsız harf dizisi", "anlamsız kelimeler"}
_WORD = re.compile(r"[^\W\d_]+(?:'[^\W\d_]+)?")
# Kumanda / klavye tuş simgelerinin OCR okumaları (Y, A, X, B, O düğmeleri)
_GLYPHS = {"a", "ya", "y", "x", "b", "o", "e", "ii"}
_SENTENCE_END = re.compile(r"[.!?…]")


def _short_noise(line: str) -> bool:
    """
    Yalnızca istemcide (sunucudan sıkı): gerçek kelime olmayan 1–2 harfli parçalar satırın %40'ından fazlası
    ("an no re ea a s na r areas"). Sunucunun %50 kelime kuralı bunları geçirebiliyor.
    """
    toks = [t.lower() for t in re.findall(r"[^\W\d_]+", line)]
    if len(toks) < 4:
        return False
    junk = sum(1 for t in toks if len(t) <= 2 and t not in _REAL_SHORT)
    return junk / len(toks) > 0.4


def _has_lower(s: str) -> bool:
    return sum(1 for ch in s if ch.islower()) >= 2


def _strip_glyphs(line: str) -> str:
    """
    Baştaki tuş / simge okumaları: en az iki kısa parça ("Ya Ya You must be busy", "a a a VAPID") ve satırın başındaki
    büyük harfli konuşan adı ("TOMIKO Listen…", "RUFINO Is it done?").
    """
    toks = line.split(" ")
    i = 0
    while i < len(toks) and len(toks[i]) <= 2 and toks[i].isalpha():
        i += 1
    run = [t.lower() for t in toks[:i]]
    # Gerçek kısa kelimeler ("It is unfortunate", "Oh oh") atılmaz: yalnızca aynı simgenin tekrarı ("Ya Ya", "a a a")
    # ya da çoğu kelime olmayan parçalar
    glyph = len(set(run)) == 1 and (run[0] in _GLYPHS or run[0] not in _REAL_SHORT) if run else False
    if i >= 2 and (glyph or sum(t not in _REAL_SHORT for t in run) > len(run) / 2):
        toks = toks[i:]
    if len(toks) >= 2 and toks[0].isalpha() and toks[0].isupper() and len(toks[0]) >= 3 \
            and re.match(r"^[A-Z][a-z]", toks[1]):
        toks = toks[1:]
    return " ".join(toks)


def _is_hud(text: str) -> bool:
    """Bütün okuma büyük harfli, en fazla 6 kelime ve cümle sonu yok: oyunun arayüzü ("RES", "SECURITY TRACKER", araç adı)."""
    if _has_lower(text):
        return False
    words = _WORD.findall(text)
    return 0 < len(words) <= 6 and not _SENTENCE_END.search(text)


def filter_ocr(raw: str, source_lang: str = "en") -> str:
    """
    DLL'in okuduğu kutuları ("\\n" ile ayrılmış) tek tek süzüp tek cümleye birleştirir. Geriye kalan bir oyun arayüzü,
    gürültü ya da sunucunun reddedeceği bir satırsa '' (gönderilmez, ekranda bir şey gösterilmez).
    """
    lines = [" ".join(part.split()) for part in (raw or "").split("\n")]
    # Önce gürültü denetimi, sonra simge temizliği: tersi gürültüyü makul bir metne çevirebilir ("… error a ra")
    lines = [_strip_glyphs(ln) for ln in lines
             if ln and not _short_noise(ln) and garbage_reason(ln, "", source_lang) not in _LINE_DROP]
    lines = [ln for ln in lines if ln]
    if len(lines) > 1 and any(_has_lower(ln) for ln in lines):
        # Altyazının yanındaki arayüz yazıları: küçük harfi olmayan kısa satırlar ve rakam ağırlıklı satırlar
        def keep(ln: str) -> bool:
            if not _has_lower(ln) and len(_WORD.findall(ln)) <= 4:
                return False
            digits = sum(ch.isdigit() for ch in ln)
            letters = sum(ch.isalpha() for ch in ln)
            return digits < 0.3 * max(1, digits + letters)
        lines = [ln for ln in lines if keep(ln)]
    text = " ".join(lines)
    if not text or _is_hud(text) or not is_valid_dialogue_text(text) or garbage_reason(text, "", source_lang):
        return ""
    return text
