# Makine Live

> Gerçek zamanlı ekran OCR ve çeviri overlay eklentisi — Makine Launcher için.

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL%203.0-blue.svg)](LICENSE)

Bu eklenti, açık kaynak kodlu **[Makine Launcher](https://github.com/MakineCeviri/Makine-Launcher)** uygulamasının resmi eklentisidir. Makine Launcher, Türkçe oyun çeviri paketlerini yöneten ücretsiz bir masaüstü uygulamasıdır.

## Ozellikler

- **Ekran Yakalama** -- DXGI Desktop Duplication (~5ms, fullscreen oyun destegi) + GDI fallback
- **OCR Metin Tanima** -- RapidOCR (PaddleOCR + ONNX Runtime) ile yuksek dogruluk
- **Ceviri Motorlari** -- ChatGPT, DeepL, Google Translate (WinHTTP native)
- **Seffaf Overlay** -- Ceviriyi oyun ustunde frameless pencere ile gosterir
- **Akilli Pipeline** -- 6 katmanli optimizasyon:
  1. DXGI capture (~5ms)
  2. Frame hash ile degisiklik algilama
  3. Fuzzy text matching (Levenshtein <%10 -> cache)
  4. Translation LRU cache (200 girdi)
  5. Context-aware GPT (onceki 5 ceviri baglam olarak gider)
  6. Adaptive timer (aktif: 200ms, sabit: 2000ms)

## Nasil Calisir

```
Ekran -> [DXGI/GDI] -> [Frame Hash] -> [RapidOCR] -> [Cache?] -> [GPT/DeepL/Google] -> Overlay
            5ms          ~0ms          ~100ms        ~0ms        ~200ms
```

## Kurulum

### Makine Launcher uzerinden (onerilen)
1. Ayarlar -> Eklentiler -> **Makine Live** -> **Kur**
2. Eklentiyi etkinlestir
3. API anahtarini gir (GPT, DeepL veya Google)
4. Bolge sec -> OCR Baslat

### Elle kurulum
1. [Releases](https://github.com/MakineCeviri/Makine-LauncherPlugin-Live/releases) sayfasindan `.makine` dosyasini indirin
2. Launcher -> Eklentiler -> **Dosya Sec** ile `.makine` dosyasini yukleyin

## Ayarlar

| Ayar | Tur | Varsayilan | Aciklama |
|------|-----|------------|----------|
| OCR Aktif | Toggle | Kapali | Her acilista kapali, elle acilir |
| Kaynak Dil | Secim | Ingilizce | en, ja, ko, zh, de, fr, es, ru |
| Hedef Dil | Secim | Turkce | tr, en, de, fr, es, ru |
| Ceviri Motoru | Secim | GPT | gpt, deepl, google |
| API Anahtari | Metin | -- | Secilen motorun API anahtari |
| GPT Modeli | Secim | gpt-4o-mini | gpt-4o-mini, gpt-4o, gpt-4-turbo |
| Yakalama Yontemi | Secim | Otomatik | auto, dxgi, gdi |
| OCR Araligi | Secim | 2000ms | 500, 1000, 2000, 3000, 5000 ms |

## Gelistirme

### Gereksinimler
- CMake 3.25+
- C++23 derleyici (MSVC 2022 veya MinGW GCC 13.1+)
- Windows 10/11 SDK

### Derleme
```bash
mkdir build && cd build
cmake .. -G Ninja
cmake --build .
```

### Paketleme
```bash
python makine-pack.py build/release -o makine-live.makine
```

`makine-pack.py` aracini [Makine-LauncherPlugin-Template](https://github.com/MakineCeviri/Makine-LauncherPlugin-Template) deposundan edinebilirsiniz.

## Proje Yapisi

```
|- manifest.json          -- Eklenti meta verileri ve ayar tanimlari
|- CMakeLists.txt         -- Derleme yapilandirmasi
|- src/
|   |- plugin.cpp         -- C ABI giris noktasi (disa aktarilan fonksiyonlar)
|   |- live_core.cpp      -- Pipeline koordinatoru (cache, hash, ceviri)
|   |- capture.h/cpp      -- Ekran yakalama (DXGI + GDI)
|   |- ocr.h              -- OCR motor arayuzu (soyut sinif)
|   |- ocr_rapid.h/cpp    -- RapidOcrOnnx.dll sarmalayici
|   |- translator.h/cpp   -- Ceviri motorlari (GPT, DeepL, Google)
|   +- settings.h         -- Anahtar=deger ayar kaliciligi
+- include/
    +- makine/plugin/     -- Plugin SDK basliklari
```

## Bagimliliklar (runtime)

Bu dosyalar plugin dizinine dahil edilmelidir:
- `RapidOcrOnnx.dll` -- OCR motoru (prebuilt)
- `onnxruntime.dll` -- ONNX Runtime
- `models/` -- PaddleOCR model dosyalari

## Katkida Bulunma

1. Fork edin
2. Feature branch olusturun (`feat/yeni-ozellik`)
3. Degisikliklerinizi commit edin
4. Pull Request acin

## Lisans

[GPL-3.0](LICENSE)
