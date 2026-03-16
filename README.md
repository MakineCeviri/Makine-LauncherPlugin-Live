# MakineAI OCR

> Gerçek zamanlı ekran OCR ve çeviri overlay eklentisi — MakineAI Launcher için.

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL%203.0-blue.svg)](LICENSE)

## Özellikler

- **Ekran Yakalama** — DXGI Desktop Duplication (~5ms, fullscreen oyun desteği) + GDI fallback
- **OCR Metin Tanıma** — RapidOCR (PaddleOCR + ONNX Runtime) ile yüksek doğruluk
- **Çeviri Motorları** — ChatGPT, DeepL, Google Translate (WinHTTP native)
- **Şeffaf Overlay** — Çeviriyi oyun üstünde frameless pencere ile gösterir
- **Akıllı Pipeline** — 6 katmanlı optimizasyon:
  1. DXGI capture (~5ms)
  2. Frame hash ile değişiklik algılama
  3. Fuzzy text matching (Levenshtein <%10 → cache)
  4. Translation LRU cache (200 girdi)
  5. Context-aware GPT (önceki 5 çeviri bağlam olarak gider)
  6. Adaptive timer (aktif: 200ms, sabit: 2000ms)

## Nasıl Çalışır

```
Ekran → [DXGI/GDI] → [Frame Hash] → [RapidOCR] → [Cache?] → [GPT/DeepL/Google] → Overlay
            5ms          ~0ms          ~100ms        ~0ms        ~200ms
```

## Kurulum

### MakineAI Launcher üzerinden (önerilen)
1. Ayarlar → Eklentiler → **MakineAI OCR** → **Kur**
2. Eklentiyi etkinleştir
3. API anahtarını gir (GPT, DeepL veya Google)
4. Bölge seç → OCR Başlat

### Elle kurulum
1. [Releases](https://github.com/MakineCeviri/MakineAI-Plugin-OCR/releases) sayfasından `.makine` dosyasını indirin
2. Launcher → Eklentiler → **Dosya Seç** ile `.makine` dosyasını yükleyin

## Ayarlar

| Ayar | Tür | Varsayılan | Açıklama |
|------|-----|------------|----------|
| OCR Aktif | Toggle | Kapalı | Her açılışta kapalı, elle açılır |
| Kaynak Dil | Seçim | İngilizce | en, ja, ko, zh, de, fr, es, ru |
| Hedef Dil | Seçim | Türkçe | tr, en, de, fr, es, ru |
| Çeviri Motoru | Seçim | GPT | gpt, deepl, google |
| API Anahtarı | Metin | — | Seçilen motorun API anahtarı |
| GPT Modeli | Seçim | gpt-4o-mini | gpt-4o-mini, gpt-4o, gpt-4-turbo |
| Yakalama Yöntemi | Seçim | Otomatik | auto, dxgi, gdi |
| OCR Aralığı | Seçim | 2000ms | 500, 1000, 2000, 3000, 5000 ms |

## Geliştirme

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
python makine-pack.py build/release -o makineai-ocr.makine
```

`makine-pack.py` aracını [MakineAI-Plugin-Template](https://github.com/MakineCeviri/MakineAI-Plugin-Template) deposundan edinebilirsiniz.

## Proje Yapısı

```
├── manifest.json          — Eklenti meta verileri ve ayar tanımları
├── CMakeLists.txt         — Derleme yapılandırması
├── src/
│   ├── plugin.cpp         — C ABI giriş noktası (dışa aktarılan fonksiyonlar)
│   ├── live_core.cpp      — Pipeline koordinatörü (cache, hash, çeviri)
│   ├── capture.h/cpp      — Ekran yakalama (DXGI + GDI)
│   ├── ocr.h              — OCR motor arayüzü (soyut sınıf)
│   ├── ocr_rapid.h/cpp    — RapidOcrOnnx.dll sarmalayıcı
│   ├── translator.h/cpp   — Çeviri motorları (GPT, DeepL, Google)
│   └── settings.h         — Anahtar=değer ayar kalıcılığı
└── include/
    └── makineai/plugin/   — Plugin SDK başlıkları
```

## Bağımlılıklar (runtime)

Bu dosyalar plugin dizinine dahil edilmelidir:
- `RapidOcrOnnx.dll` — OCR motoru (prebuilt)
- `onnxruntime.dll` — ONNX Runtime
- `models/` — PaddleOCR model dosyaları

## Katkıda Bulunma

1. Fork edin
2. Feature branch oluşturun (`feat/yeni-ozellik`)
3. Değişikliklerinizi commit edin
4. Pull Request açın

## Lisans

[GPL-3.0](LICENSE)
