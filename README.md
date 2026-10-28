# Makine Live

> Oyun ekranındaki İngilizce metni okur, çevirir ve oyunun üstünde gösterir — [Makine Launcher](https://github.com/MakineCeviri/Makine-Launcher) eklentisi.

[![License: AGPL-3.0 + Commons Clause](https://img.shields.io/badge/License-AGPL--3.0%20%2B%20Commons%20Clause-blue.svg)](LICENSE)

Türkçe yaması olmayan bir oyunda diyalogların ya da menülerin geçtiği ekran bölgesini bir kez seçersin; Makine Live o bölgeyi düzenli aralıklarla okur (OCR), metni seçtiğin çeviri servisine gönderir ve çeviriyi bölgenin hemen altında gösterir.

## Kurulum

Makine **0.2.0** ya da daha yenisi gerekir.

1. Makine → **Ayarlar → Eklentiler → Makine Live → Kur**
2. Kurulumda istenen izinleri onayla (aşağıda).
3. Eklenti ayarlarından çeviri servisini seç. Bulut servislerinde API anahtarı gerekir; yerel Ollama/LM Studio HTTP sunucularında anahtar boş bırakılabilir.

Kendi derlediğin bir `.mkplugin` dosyasını **Ayarlar → Eklentiler → Geliştirici modu → Dosyadan yükle** ile kurabilirsin; bu kurulum *Doğrulanmamış* olarak işaretlenir.

## Kullanım

| Düğme | Ne yapar |
|-------|----------|
| **Bölge seç** | Ekranda okunacak dikdörtgeni çizdirir. Bölge kaydedilir; bir sonraki açılışta yeniden seçmen gerekmez. |
| **Başlat** | Bölgeyi okuyup çevirmeye başlar. Durum satırında `Çalışıyor · 800×200` görünür. |
| **Durdur** | Okumayı durdurur ve çeviriyi gizler. Oyun kapanınca kendiliğinden durur. |
| **Bölgeyi oku (test)** | Bölgedeki metni çevirmeden bir kez okur ve gösterir. Bölgenin doğru seçildiğini ve OCR'ın metni okuyabildiğini denemek için. |

**Oyun açılınca başlat** açıksa ve kayıtlı bir bölge varsa, Makine'den bir oyun başladığında okuma kendiliğinden başlar.

Çeviri alınamazsa (anahtar yok ya da geçersiz, kota doldu, internet yok) bir kez hata bildirimi çıkar ve çeviri yerine okunan özgün metin gösterilir.

## Ayarlar

| Ayar | Tür | Açıklama |
|------|-----|----------|
| Çeviri servisi | Seçim | OpenAI, Groq, OpenRouter, Ollama (yerel), LM Studio (yerel), özel OpenAI-uyumlu servis, DeepL veya Google Cloud Translation |
| API anahtarı | Gizli | Bulut servislerinin anahtarı. Yerel Ollama/LM Studio HTTP sunucularında boş bırakılabilir. Windows'ta DPAPI ile şifreli saklanır. |
| Model / model ID | Metin | Seçili servisin model adı. Groq için boş bırakılırsa `openai/gpt-oss-20b`, Ollama için `llama3.2`, OpenAI için `gpt-4.1-mini` kullanılır. OpenRouter, LM Studio ve özel sunucularda model ID girilmelidir. |
| Custom endpoint base URL | Metin | Yalnız Özel OpenAI-uyumlu servis için. Ör. `https://api.example.com/v1` veya `http://127.0.0.1:1234/v1`. Uzak adresler HTTPS kullanmalıdır. |
| Kaynak dil | Seçim | İngilizce. OCR modeli şimdilik yalnız İngilizce okur. |
| Hedef dil | Seçim | Türkçe, İngilizce, Almanca, Fransızca, İspanyolca, Rusça |
| Okuma aralığı (ms) | Sayı | 1000 (250–10000). Değişmeyen ekran yeniden okunmaz. |
| Özgün metni de göster | Aç/kapa | Kapalı. Çevirinin yanında okunan İngilizce metni gösterir. |
| Oyun açılınca başlat | Aç/kapa | Kapalı. Kayıtlı bölge varsa oyun başlayınca başlar. |

### OpenAI-uyumlu servisler

OpenAI, Groq, OpenRouter, Ollama ve LM Studio aynı Chat Completions istek biçiminin uyumlu alt kümesini kullandığından ortak kod yolundan çalışır:

- **Groq:** `https://api.groq.com/openai/v1`; güncel model ID'leri sağlayıcının model listesinden alınır. Model alanı boşsa `openai/gpt-oss-20b` varsayılanı kullanılır.
- **OpenRouter:** `https://openrouter.ai/api/v1`; Model / model ID alanına OpenRouter'da kullanılabilen model kimliğini girin.
- **Ollama:** yerel sunucu için `http://127.0.0.1:11434/v1`; bilgisayarınızda bulunan modelin adını seçin. Model alanı boşsa `llama3.2` denenir.
- **LM Studio:** yerel sunucu için `http://127.0.0.1:1234/v1`; LM Studio'da Local Server etkin olmalı ve Model alanına sunucunun model kimliğini girin.
- **Özel OpenAI-uyumlu servis:** `Custom OpenAI-compatible` seçin; Base URL alanına adresi ve Model alanına sunucunun desteklediği model kimliğini girin.

Güvenlik için şifresiz HTTP yalnız `localhost`, `127.0.0.1` veya `::1` gibi loopback adreslerinde kabul edilir; uzak servis adresleri HTTPS kullanmalıdır. Yerel model kullanımında metin bilgisayarınızda kalabilir, ancak seçilen bulut sağlayıcıya gönderilen metin o sağlayıcının hizmet ve gizlilik koşullarına tabidir. DeepL'in ücretsiz anahtarları (`:fx` ile biten) `api-free.deepl.com`'a, diğerleri `api.deepl.com`'a gider.

## İzinler ve gizlilik

| İzin | Neden |
|------|-------|
| Ekranı okuma (`screen-capture`) | Seçtiğin bölgenin görüntüsü bilgisayarında okunur; görüntü hiçbir yere gönderilmez. |
| İnternet (`network`) | Yalnız bölgeden okunan **metin** seçtiğin çeviri servisine gider. |
| Ekran üstü gösterim (`overlay`) | Çeviri Makine'nin kaplamasıyla oyunun üstünde gösterilir; bölge seçimi de onunla yapılır. |

Eklenti oyunun dosyalarına ve sürecine dokunmaz; hile korumalı oyunlarda da yalnız ekranı okur.

## Sınırlamalar

- OCR yalnız İngilizce (Latin harfleri, aksansız) metni okur.
- Oyunun kendi tam ekran modu bazı ekran kartlarında yakalanamayabilir; öyleyse oyunu *kenarlıksız pencere* kipine al.
- Döndürülmüş monitörlerde ve iki monitöre taşan bölgelerde daha yavaş GDI yakalaması kullanılır.

---

## Geliştirme

Eklenti, Makine eklenti ABI 2'yi kullanır: tek bir `makine_plugin_entry` dışa aktarımı, JSON yükler, ayrı bir `makine-plugin-host.exe` sürecinde çalışma. Ayrıntılar: Makine-Launcher deposunda [`docs/plugins/README.md`](https://github.com/MakineCeviri/Makine-Launcher/blob/dev/docs/plugins/README.md).

### Gereksinimler

- CMake 3.25+, C++20 derleyici: MinGW-w64 GCC 13+ ya da MSVC 2022+
- Python 3.10+, `pip install zstandard` (paketleme), 7-Zip ya da `pip install py7zr` (OCR çalışma zamanını açmak için)

### Derleme

```bash
python tools/fetch_ocr.py          # OCR motoru + modeller → .cache/ocr (SHA-256 sabitli)
cmake -B build -G Ninja
cmake --build build                # → build/package/
cmake --build build --target pack  # → build/org.makineceviri.live-<sürüm>.mkplugin
```

`build/package/` eklentinin kurulmuş hâlidir: `manifest.json`, `bin/makine-live.dll`, `bin/RapidOcrOnnx.dll`, `models/`, `licenses/`. OCR dosyaları indirilmeden de derlenir, ama eklenti o hâliyle başlamayı reddeder; `pack` hedefi de durur. İkili dosyalar ve modeller git'e girmez.

### Makine olmadan çalıştırma

`tools/plugin_dev.py` eklentiyi gerçek `makine-plugin-host.exe` içinde çalıştırır ve launcher'ın yerine geçer (bölge seçimine `--region` ile cevap verir):

```bash
python tools/plugin_dev.py build/package --host <Makine klasörü>/makine-plugin-host.exe \
    --region 100,100,800,200 --invoke pickRegion --invoke readOnce
```

`--invoke` vermezsen komutları standart girdiden okur: `start`, `stop`, `!game.stopped {}`, `quit`. Ayarlar `--set provider=deepl --set apiKey=...` ile verilir.

### Proje yapısı

```
manifest.json              eklenti bildirimi (şema 2): izinler, ayarlar, düğmeler, olaylar
src/plugin.cpp             ABI girişi: initialize / invoke / onEvent / shutdown
src/worker.h/.cpp          yakalama → OCR → çeviri → kaplama döngüsü; tek iş parçacığı
src/capture.h/.cpp         DXGI Desktop Duplication, yedek olarak GDI BitBlt
src/ocr_rapid.h/.cpp       RapidOcrOnnx.dll sarmalayıcısı
src/translator.h/.cpp      OpenAI-uyumlu servisler / DeepL / Google, WinHTTP
include/makine/plugin/     ABI başlıkları (Makine-Launcher'dan birebir kopya)
tools/fetch_ocr.py         OCR motoru ve modellerini indirir, doğrular
tools/plugin_dev.py        eklentiyi Makine olmadan çalıştırır (launcher'dan kopya)
tools/plugin_registry.py   .mkplugin paketler (launcher'dan kopya)
licenses/NOTICE.txt        pakete giren üçüncü taraf bileşenler
```

### Üçüncü taraf bileşenler

| Bileşen | Lisans |
|---------|--------|
| [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx) 1.2.3 | Apache-2.0 |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) 1.15.1 (RapidOcrOnnx.dll içinde) | MIT |
| [OpenCV](https://github.com/opencv/opencv) 4.8.1 (RapidOcrOnnx.dll içinde) | Apache-2.0 |
| [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) modelleri, [RapidOCR](https://github.com/RapidAI/RapidOCR) ONNX dönüşümleri | Apache-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | MIT |

Lisans metinleri paketin `licenses/` klasöründedir.

## Lisans

[AGPL-3.0 + Commons Clause](LICENSE) · Lisans veren: Makine Çeviri
