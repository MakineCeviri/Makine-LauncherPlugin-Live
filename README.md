# MakineAI Live

> Gerçek zamanlı ekran OCR ve çeviri overlay eklentisi

MakineAI Launcher için resmi canlı çeviri eklentisi. Oyun ekranındaki metinleri otomatik olarak tanır, çevirir ve şeffaf bir pencere üzerinde gösterir.

## Özellikler

- **Ekran Yakalama** — DXGI Desktop Duplication (hızlı) + GDI BitBlt (evrensel yedek)
- **OCR Metin Tanıma** — RapidOCR (PaddleOCR modelleri, ONNX Runtime ile)
- **Çeviri Motorları** — DeepL, Google Cloud Translation, ChatGPT/Claude
- **Şeffaf Overlay** — Oyun penceresinin üstünde çeviri gösterimi
- **Akıllı Pipeline** — Değişiklik algılama, önbellek, otomatik frekans ayarı

## Nasıl Çalışır

```
Oyun Penceresi
    ↓ Ekran Yakalama (DXGI/GDI)
Yakalanan Kare
    ↓ Bölge Kırpma (kullanıcının seçtiği alan)
Metin Bölgesi
    ↓ Değişiklik Algılama (piksel karşılaştırma)
Değişti mi? ── Hayır → Önbellekteki çeviriyi kullan
    ↓ Evet
    ↓ OCR (RapidOCR)
Tanınan Metin
    ↓ Çeviri (seçili motor)
Çevrilmiş Metin
    ↓ Overlay Gösterimi
Şeffaf Pencere
```

## Kurulum

### MakineAI Launcher Üzerinden (Önerilen)
1. MakineAI Launcher'ı açın
2. **Ayarlar → Eklentiler** sayfasına gidin
3. **MakineAI Live** yanındaki **"Kur"** butonuna tıklayın

### Manuel Kurulum
1. [Releases](https://github.com/MakineCeviri/makineai-plugin-live/releases) sayfasından `.makine` dosyasını indirin
2. Dosyayı `AppData/Local/MakineAI/plugins/live/` dizinine çıkartın
3. Launcher'ı yeniden başlatın

## Geliştirme

### Gereksinimler
- CMake 3.25+
- MinGW GCC 13.1+ (veya herhangi bir C++23 derleyici)
- Ninja (opsiyonel, önerilen)

### Derleme
```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

Çıktı: `build/release/makineai-live.dll`

### Paketleme
```bash
pip install zstandard
python makine-pack.py ./build/release/
```

Çıktı: `com-makineceviri-live-0.1.0.makine`

## Proje Yapısı

```
├── manifest.json              — Eklenti meta verileri
├── CMakeLists.txt             — Derleme yapılandırması
├── include/makineai/plugin/   — Plugin SDK başlık dosyaları
│   ├── plugin_api.h           — C ABI fonksiyon tipleri
│   └── plugin_types.h         — Hata kodları, yapılar
└── src/
    ├── plugin.cpp             — Giriş noktası (5 zorunlu export)
    ├── live_core.cpp          — Başlatma, kapatma, koordinasyon
    ├── capture.h/.cpp         — Ekran yakalama (DXGI + GDI)
    └── ocr.h                  — OCR motor arayüzü
```

## Plugin API

Her eklenti DLL'i şu 5 fonksiyonu dışa aktarmalıdır:

| Fonksiyon | İmza | Açıklama |
|-----------|------|----------|
| `makineai_get_info` | `MakineAiPluginInfo (void)` | Eklenti bilgilerini döndürür |
| `makineai_initialize` | `MakineAiError (const char* dataPath)` | Eklentiyi başlatır |
| `makineai_shutdown` | `void (void)` | Kaynakları temizler |
| `makineai_is_ready` | `bool (void)` | Hazır olup olmadığını kontrol eder |
| `makineai_get_last_error` | `const char* (void)` | Son hata mesajını döndürür |

## Yol Haritası

- [x] Plugin iskeleti ve C ABI uyumluluğu
- [x] GDI ekran yakalama
- [ ] DXGI Desktop Duplication
- [ ] RapidOCR entegrasyonu (ONNX Runtime)
- [ ] Çeviri motoru adaptörleri
- [ ] Şeffaf overlay penceresi
- [ ] Bölge seçici (rubber-band selector)
- [ ] Çeviri önbelleği

## Katkıda Bulunma

Katkılarınızı bekliyoruz! Lütfen bir issue açın veya pull request gönderin.

1. Bu repoyu fork edin
2. Bir dal oluşturun (`git checkout -b feat/yeni-ozellik`)
3. Değişikliklerinizi commit edin
4. Push edin ve PR açın

## Lisans

GPL-3.0 — Detaylar için [LICENSE](LICENSE) dosyasına bakın.
