# Makine Live — Teknik Mimari

> Bu belge projenin teknik başvuru kaynağıdır: C++ çekirdeği ve C ABI, sunucuyla sözleşme, masaüstü uygulaması,
> konsol / TV modu, Makine TV ve komutlar. Mimari değiştiğinde önce bu belgeyi güncelleyin.
> Sunucu makineceviri.org'un Cloudflare Worker'ıdır; kodu ayrı ve özel **Makine-WEB** deposundadır
> (`src/live/`, sayfalar `src/pages/live/`). Bu depoda sunucu kodu yoktur.

> [!IMPORTANT]
> ## Python yalnızca test ve prototipleme içindir
> - Nihai ürün C++23 ile derlenen **`makine-live.dll`** ve onu içeren **`makine-live.makine`** paketidir.
> - Makine Launcher eklentiyi native C ABI üzerinden yükler (`manifest.json` → `"entry": "makine-live.dll"`).
> - Python paketi (`makine_live/`: `app.py`, `games/game_detector.py`, `net/pool_client.py`…) özellikleri gerçek oyunda denemek,
>   hız ölçmek ve görselleştirmek için kullanılan bir **test koşucusudur** (test harness).
> - Python'da doğrulanan her yetenek (havuz istemcisi, Alt+Tab koruması, çöp filtresi, tekrar engelleme,
>   çeviri kutusu tasarımı) sonunda **C++ çekirdeğine** taşınır. Python'a kalıcı mimari kurmayın.

> [!WARNING]
> ## Gizli bilgiler bu depoya girmez
> Şifre, API anahtarı, jeton, imza anahtarı gibi bilgileri hiçbir dosyaya (bu belge dahil) yazmayın.
> Sunucunun DeepL anahtarı ve hesapları sunucudadır; istemcide kullanıcıya ait hiçbir anahtar yoktur.
> İstemci, uygulama jetonunu ve (geliştirici motorları için) API anahtarını Windows DPAPI ile şifreleyerek saklar.
> Testlerde anahtarları ortam değişkeninden okuyun (örnek: `DEEPL_API_KEY`). Ayrıntılar: [`SECURITY.md`](../SECURITY.md).

---

## 1. Proje özeti

**Makine Live**, [Makine Çeviri](https://makineceviri.org) ekosistemi için oyun içi canlı ekran okuma (OCR),
çeviri ve kitle kaynaklı **çeviri havuzu** motorudur.

- **Sorun:** Canlı oyunda DeepL/GPT çağrıları 400–900 ms sürer ve aynı cümleler binlerce oyuncu tarafından
  tekrar tekrar çevrilir.
- **Çözüm:** Algılanan her diyalog önce yerel önbelleğe, sonra merkezi havuza (`https://makineceviri.org/api/live/`) sorulur.
  Havuzda yoksa sunucu kendi DeepL anahtarıyla bir kez çevirir; sonraki oyuncular aynı çeviriyi alır ve her hesap bir oy
  sayılır. Yeterince farklı oyuncudan uyuşan çeviriler gelince diyalog otomatik onaylanır. Çevirmenler
  `makineceviri.org/live/portal/` portalından onaylar, düzenler veya siler.
- **Açık sürüm (0.3.0):** indirme herkese açık (`makineceviri.org/live/`); oyuncular **Makine Çeviri hesabıyla** girer
  (sitenin topluluk hesabı: Discord ya da e-posta kodu; uygulama kod gösterir, oyuncu `/live/baglan/`'da onaylar).
  Ekip hesapları (admin / çevirmen / tester) Live'ın kendi hesaplarıdır, şifreyle girer.

```mermaid
graph TD
    subgraph Üretim
        ML[Makine Launcher] -->|C ABI| DLL(makine-live.dll C++23)
        DLL -->|DXGI / GDI| Game[Oyun ekranı]
        DLL -->|RapidOcrOnnx| OCR[ONNX OCR]
        DLL -->|WinHTTP, makine motoru| Pool[(makineceviri.org /api/live)]
    end
    subgraph Test koşucusu / MakineLive.exe
        Py[makine_live/app.py] -->|ctypes| DLL
        Py --> PC[net/pool_client.py] -->|HTTPS| Pool
        Py --> Det[games/game_detector.py] --> Lib[games/game_library.py]
    end
    subgraph makineceviri.org Cloudflare Worker, Makine-WEB
        Pool --> LiveApi[src/live/app.ts]
        Portal[/live/portal/ çevirmen portalı] --> LiveApi
        Baglan[/live/baglan/ hesap onayı] --> LiveApi
        LiveApi --> D1[(D1 makine-live)]
        LiveApi --> R2[(R2 makine-live-releases)]
        LiveApi -->|yalnız okur| Site[(CATALOG_DB topluluk hesapları)]
        LiveApi --> DeepL[DeepL]
    end
```

---


## 2. Dosya yapısı

```
├── README.md, SECURITY.md, LICENSE
├── build.cmd                   # tek adımda sürüm (tools/build.py; çift tıklanınca sürüm numarasını sorar)
├── test.cmd                    # sunucusuz deneme: yerel sahte sunucu + uygulama (tools/offline.py)
├── CMakeLists.txt              # DLL derlemesi; OCR çalışma dosyalarını build/release'e kopyalar
├── manifest.json               # Launcher eklenti manifesti (sürüm burada; plugin.cpp ile aynı tutulur)
├── include/makine/plugin/      # Launcher ile ortak ABI başlıkları
│   ├── plugin_api.h            #   fonksiyon işaretçi tipleri
│   └── plugin_types.h          #   MakineError, MakinePluginInfo, API sürümü
├── src/                        # [NİHAİ ÜRÜN] C++ çekirdeği
│   ├── plugin.cpp              #   extern "C" export'lar, global kilit, hata metni
│   ├── live_core.cpp           #   pipeline: yakalama → kare hash → OCR → benzerlik → LRU → çeviri
│   ├── capture.h/.cpp          #   DXGI Desktop Duplication (+ son kare önbelleği) / GDI
│   ├── ocr.h, ocr_rapid.h/.cpp #   RapidOcrOnnx.dll sarmalayıcı (LoadLibrary + GetProcAddress)
│   ├── translator.h/.cpp       #   WinHTTP: makine (sunucu), GPT, DeepL, Google
│   ├── settings.h/.cpp         #   settings.cfg (key=value, kaçışlı, apiKey ve authToken DPAPI ile şifreli)
│   ├── text_fix.h/.cpp         #   OCR'ın İngilizcede yuttuğu kelime boşluklarını geri koyar (SpacingFixer)
│   └── winutil.h               #   UTF-8 ↔ UTF-16 ↔ ANSI yol dönüşümleri
├── third_party/rapidocr/       # (git'te yok) RapidOcrOnnx.dll, onnxruntime.dll, models/
├── resources/en_words.txt      # İngilizce kelime sıklık listesi (wordninja, MIT) → build/release/resources/
├── resources/icon.png          # Launcher eklenti ikonu (manifest "icon"; logodan üretilir) → build/release/icon.png
├── assets/
│   ├── logo_white-96.webp      # Makine Çeviri logosu: her logo/ikonun TEK kaynağı (beyaz, saydam zemin, 96 px)
│   └── app.ico                 # .exe ikonu (logodan üretilir)
├── spec/                       # Python / sunucu / (C++) ortak test vektörleri ve tasarım dili
│   ├── normalization_vectors.json  # metin normalizasyonu (sunucunun kopyası Makine-WEB tests/fixtures/live/)
│   ├── garbage_vectors.json    #   sunucunun çöp kuralları
│   ├── sample_quotes.json      #   oyunlarda GERÇEKTEN geçen örnek replikler + Türkçesi
│   └── design_tokens.json      #   renkler, yazı tipleri, ikon/hareket kuralları, overlay ölçüleri
├── docs/
│   ├── MIMARI.md               # bu belge
│   ├── KONSOL_TV.md            # kullanıcı kılavuzu: PS5 / Xbox oyunlarını TV'de Türkçe oynamak
│   ├── GTA6_MAKINE_LIVE_REHBERI.md  # ayrıntılı kullanıcı rehberi: kurulum, giriş, konsol + TV, monitör, telefon, OBS
│   └── surum-notlari/          # sürüm notları (vX.Y.Z.txt)
│
│   # ── Makine Live masaüstü uygulaması (Python test koşucusu, dist/MakineLive.exe; py -m makine_live) ──
├── makine_live/
│   ├── __main__.py             # giriş noktası (exe de buradan açılır)
│   ├── app.py                  # [TEST] kontrol paneli + pipeline (MakineGameLiveApp), selftest
│   ├── paths.py, version.py    #   bütün dosya yolları (geliştirme / exe) ve manifest'ten sürüm
│   ├── ui/
│   │   ├── hud_ui.py           # [TEST] panel bileşenleri ve animasyonlu Canvas ikonları
│   │   ├── overlay_render.py   # [TEST→C++] oyun içi kutu: Pillow ile çizim + UpdateLayeredWindow
│   │   ├── login.py, region.py, widgets.py, brand.py, layout.py  # giriş ekranı, F10 bölge, geçmiş, logo, yerleşim
│   ├── core/
│   │   ├── dll.py              #   makine-live.dll C ABI sarmalayıcısı (ctypes)
│   │   ├── subtitle_timing.py  # [TEST→C++] satır takibi + altyazı zamanlayıcısı (saf, zaman dışarıdan verilir)
│   │   └── garbage.py          # [TEST→C++] OCR süzgeçleri: satır başına gürültü / HUD, sunucunun çöp kuralının kopyası
│   ├── games/
│   │   ├── game_library.py     # [TEST→C++] kurulu oyunlar: Steam (appinfo.vdf türü), Epic, GOG, Ubisoft, EA, Xbox, Windows listesi
│   │   ├── game_detector.py    # [TEST→C++] çalışan oyunu bulma (yalnızca oyunlar) + hedef pencere kilidi
│   │   └── console_mode.py     # [TEST→C++] konsol modu: Remote Play / Xbox / yakalama penceresi, 16:9 oransal bölge
│   ├── net/
│   │   ├── pool_client.py      # [TEST] sunucu istemcisi (makineceviri.org/api/live): hesapla giriş, oturum (DPAPI), translate, keep-alive
│   │   ├── local_pool.py       # [TEST→C++] yerel havuz: oyunun ONAYLI çevirileri bilgisayarda (arama, eşitleme, sayaçlar)
│   │   ├── app_update.py       # [TEST] .exe'nin kendini güncellemesi (indir → SHA-256 → kapanınca üzerine yaz → yeniden aç)
│   │   └── discord_presence.py #   Discord'da "Makine Live ile Türkçe oynuyor" (yerel IPC, bağımlılıksız)
│   └── tv/
│       ├── tv_link.py          # [TEST→C++] PC → Makine TV bağlantısı (UDP keşif + TCP satır-JSON, eşleşme kodu)
│       ├── tv_install.py       # [TEST] "TV'ye kur": Android TV'ye ADB, LG'ye geliştirici modu SSH ile kurulum
│       └── subtitle_web.py     # [TEST→C++] yerel altyazı yayını: telefon / tablet (/) ve OBS tarayıcı kaynağı (/obs), SSE
├── requirements.txt            # Python bağımlılıkları (uygulama, testler, paketleme)
│
│   # ── Makine TV ──
├── tv/                         # Android TV / Google TV altyazı katmanı (Kotlin, bağımlılıksız; Gradle, tv\gradlew.bat)
├── tv-webos/                   # LG webOS (app/ web uygulaması: HDMI'ı içinde açar + altyazı; service/ JS servisi, ES5)
│
├── tools/
│   ├── build.py                # sürüm numarası → DLL → testler → exe → selftest → SHA-256 (build.cmd bunu çağırır)
│   ├── offline.py              # sahte /api/live sunucusu (127.0.0.1) + uygulama; veriler build/offline (test.cmd)
│   ├── build_exe.py            # tek dosyalık dist/MakineLive.exe (PyInstaller)
│   ├── pack.py                 # build/release → makine-live.makine (eksik dosya varsa reddeder)
│   ├── brand_assets.py         # logodan türetilenleri üretir: eklenti ikonu, app.ico, Makine TV ikon ve afişi
│   ├── build_webos.py          # tv-webos → build/webos/makine-tv.ipk (LG'nin ares-package aracı gerekmez)
│   └── tv_emulator_check.py    # Makine TV'yi resmî Android TV 8/9/12 + Google TV 14/16 emülatörlerinde uçtan uca dener
└── tests/                      # bütün testler (çalıştırma: tests/README.md)
    ├── _root.py                #   testler makine_live paketini bununla bulur
    ├── test_plugin.py          #   DLL ABI duman testi (gerçek OCR dahil)
    ├── test_dll_server.py      #   DLL'in "makine" motoru: jetonla /api/live/translate (küçük sahte sunucuyla)
    ├── test_normalization.py   #   Python normalizasyonu ↔ spec vektörleri
    ├── test_game_library.py    #   oyun kütüphanesi (sahte Steam/Epic klasörleriyle)
    ├── test_local_pool.py      #   yerel havuz
    ├── test_subtitle_scheduler.py  # zamanlayıcı
    ├── test_console_mode.py, test_tv_link.py  # konsol modu ve TV bağlantısı (sahte TV ile)
    ├── test_subtitle_web.py    #   telefon / OBS sayfaları ve SSE akışı (gerçek HTTP)
    ├── test_discord_presence.py  # Discord'da gösterme (sahte Discord IPC kanalı)
    ├── test_webos_service.py, test_webos_install.py  # LG servisi (Node + sahte webos-service), geliştirici modu kurulumu (sahte TV)
    ├── webos_mock/, webos_preview.html  # sahte 'webos-service' modülü; LG arayüzünü tarayıcıda görme sayfası
    ├── text_fix_test.cpp       #   boşluk düzeltici (ctest --test-dir build)
    └── test_ui.py, test_winhttp.py  # eski deneme araçları
```

---

## 3. C++ çekirdeği (`makine-live.dll`)

- **Derleyici:** GCC 15 (MinGW-w64, scoop), CMake ≥ 3.25, Ninja. DLL statik bağlanır, yalnızca sistem
  DLL'lerini içe aktarır (KERNEL32, USER32, GDI32, d3d11, WINHTTP, CRYPT32, msvcrt).
- **Çalışma bağımlılıkları** (DLL'in yanında olmalı): `RapidOcrOnnx.dll`, `onnxruntime.dll`, `models/`
  (`ch_PP-OCRv3_det_infer.onnx`, `ch_ppocr_mobile_v2.0_cls_infer.onnx`, `ch_PP-OCRv3_rec_infer.onnx`, `ppocr_keys_v1.txt`).
  `RapidOcrOnnx.dll` `LOAD_WITH_ALTERED_SEARCH_PATH` ile yüklenir; böylece `onnxruntime.dll` Launcher'ın klasöründe değil
  eklentinin klasöründe aranır.

### 3.1. C ABI (`src/plugin.cpp`)

```c
MakinePluginInfo makine_get_info(void);                 // id, name, version, apiVersion=1
MakineError makine_initialize(const char* dataPath);    // UTF-8 yol. OCR yüklenemezse MAKINE_ERR_INIT_FAILED
void        makine_shutdown(void);
bool        makine_is_ready(void);                      // yakalama + OCR hazır
const char* makine_get_last_error(void);                // son hatanın açıklaması (UTF-8)
const char* makine_get_setting(const char* key);
void        makine_set_setting(const char* key, const char* value);
const char* makine_capture_and_ocr(void* hwnd, int x, int y, int w, int h);       // ham OCR metni
const char* makine_capture_ocr_translate(void* hwnd, int x, int y, int w, int h); // ocrEnabled=true ise çeviri
const char* makine_get_last_ocr_text(void);             // son yakalamanın ham OCR metni
```

Sözleşme:
- `MakineError` değerleri `plugin_types.h` içinde sıralıdır: `MAKINE_OK=0`, `INIT_FAILED=1`, `NOT_READY=2`, `INVALID_PARAM=3`, …
- Hatalar metin kanalına **karışmaz**: başarısızlıkta fonksiyon `""` döndürür, sebep `makine_get_last_error()` ile okunur.
- Dönen `const char*` UTF-8'dir, eklentiye aittir ve **aynı fonksiyonun bir sonraki çağrısına kadar** geçerlidir.
  Çağıran kopyalamalıdır. C# tarafında dönüş tipi `IntPtr` olmalı ve `Marshal.PtrToStringUTF8` kullanılmalı;
  `string` yazılırsa marshaller eklentinin belleğini serbest bırakmaya çalışır ve uygulama çöker.
- Bütün export'lar tek bir global kilit altında çalışır. `capture_ocr_translate` API yanıtı gelene kadar
  (en fazla ~10 sn zaman aşımı) çağıranı **bloke eder**. Bloke etmeyen ABI planı: §3.5.
- `hwnd == NULL` ise bölge ekran (sanal masaüstü) koordinatıdır ve DXGI kullanılır. `hwnd` verilirse pencerenin
  istemci koordinatıdır ve GDI kullanılır.

### 3.2. Yakalama (`capture.cpp`)
- DXGI Desktop Duplication, birincil monitör. `AcquireNextFrame` yalnızca **değişiklik** bildirir; bu yüzden son masaüstü
  görüntüsü GPU'da (`m_lastDesktop`) saklanır. Sahne sabitken zaman aşımı beklenmez, son kare kullanılır.
- `DXGI_ERROR_ACCESS_LOST` (tam ekran geçişi, çözünürlük değişikliği, UAC) durumunda duplication en fazla 500 ms'de
  bir yeniden kurulur, arada GDI kullanılır.
- Bölge birincil monitörün dışındaysa GDI (`GetDC(NULL)`, bütün monitörler) kullanılır. Staging texture yalnızca bölge boyutu değişince yeniden oluşturulur.
- `hwnd` verilirse (konsol modu) pencerenin **kendi içeriği** okunur: `PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT)`
  (`captureWindow`). Üstünde başka pencere olsa da ve DirectX / video pencerelerinde (PS Remote Play, yakalama önizlemesi)
  çalışır; küçültülmüş pencere okunamaz. Görüntü tamamen siyahsa (korumalı içerik) pencere DC'sinden `BitBlt`'e düşülür.
- HUD penceresi kendi metnini okumasın diye `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` uygulanır. Bu, kutuyu OBS'nin
  ekran yakalamasından ve Discord / Teams ekran paylaşımından da gizler. Okuma pencerenin kendi içeriğinden yapılıyorsa
  (`hwnd` yolu) kutu yakalamaya açılır (`WDA_NONE`): konsol modunda her zaman, PC oyununda "yayında göster" açıkken ve oyun
  penceresi `PrintWindow`'la okunabiliyorsa (§5). Şu an test koşucusu uyguluyor; native overlay yazıldığında oraya taşınacak.

### 3.3. Pipeline (`live_core.cpp`)
1. Kare hash'i (tüm baytlar + bölge geometrisi, 64-bit). Değişmemiş kare için OCR yapılmaz, önceki sonuç döner
   (`capture_and_ocr` ~1 ms; bu yüzden istemci ~100 ms aralıkla yoklayabilir). İki export'un önbelleği ayrıdır.
2. OCR (RapidOcrOnnx dosyadan okur → `%TEMP%\MakineLive\capture.bmp`, `FILE_ATTRIBUTE_TEMPORARY`).
   `%TEMP%` ANSI kod sayfasında gösterilemiyorsa `%ProgramData%\MakineLive\tmp` kullanılır.
   Açı sınıflandırması kapalıdır (altyazılar yataydır). Tanıma modeli varsayılan olarak Çince PP-OCRv3'tür:
   oyun yazı tiplerinde ölçülen en güvenilir model. İngilizce model (`ocrModel=en`, `models/en_PP-OCRv3_rec_infer.onnx`
   + `en_dict.txt` gerekir) bazı stilize satırlarda anlamsız çıktı verdiği için isteğe bağlıdır.
2a. **Yabancı alfabe gürültüsü:** Latin alfabeli kaynak dillerde (en, de, fr, es, tr…) Çince/Japonca/Korece ya da tam genişlik
   karakter içeren parçalar atılır. Çince model arka plan gürültüsünü bazen "园图B" gibi okur (`dropForeignScriptTokens`).
2b. **Boşluk düzeltme** (`sourceLang=en`, `fixSpacing=true`): Çince model İngilizce kelimeleri sık sık yapıştırır
   ("Iwas goingto findyou"). `SpacingFixer` sözlükte olan kelimeye dokunmaz (isimler, oyun terimleri), bilinmeyen bir
   kelimeyi yalnızca **yaygın** kelimelere bölünebiliyorsa ayırır; ayrıca "Shaun.I" ve "PressXto" sınırlarını açar.
   Zor koşullu ölçümde (küçük, konturlu, sıkışık yazı) boşluğu doğru satırlar 11/32 → 29/32.
3. Bulanık benzerlik: uzunluk farkı %30'dan az ve Levenshtein mesafesi %10'un altındaysa önceki çeviri kullanılır.
   Önek kısayolu bilerek yoktur; ortak başlangıçlı farklı cümleler karışıyordu.
4. Tam eşleşme LRU önbelleği (200 kayıt).
5. Çeviri. Varsayılan motor **`makine`**: `POST {serverUrl}/api/live/translate`, kullanıcının uygulama jetonuyla
   (`Authorization: Bearer` + `X-Makine-Token`; sunucu ikisini de okur). Sunucu havuzdan döner ya da kendi DeepL anahtarıyla bir kez çevirir;
   kullanıcıda API anahtarı yoktur. Launcher entegrasyonunda host jetonu `makine_set_setting("authToken", …)` ile verir;
   401 gelirse `makine_get_last_error()` "HTTP 401" içerir ve host yeniden giriş istemelidir. Doğrudan `gpt`/`deepl`/`google`
   motorları (`apiKey` ile) geliştirici kullanımı için durur. GPT'ye son 5 çeviri bağlam olarak verilir. WinHTTP zaman aşımları: çözümleme 3 sn, bağlantı 3 sn,
   gönderme 5 sn, alma 10 sn. 2xx dışındaki yanıtlar hata sayılır.

### 3.4. Ayarlar (`settings.cpp`)
`<dataPath>\settings.cfg` dosyası UTF-8 `key=value` satırlarından oluşur (`\n`, `\r`, `\\` kaçışlı). `apiKey`, Windows DPAPI ile
(geçerli kullanıcıya bağlı) `apiKey=dpapi:<hex>` olarak yazılır. Eski düz metin değer okunur ve ilk kayıtta şifrelenir.
Başka bir kullanıcı veya makinede çözülemez; o durumda anahtar yeniden girilir.
Bilinen anahtarlar: `ocrEnabled`, `sourceLang`, `targetLang`, `translatorEngine` (makine [varsayılan]|gpt|deepl|google),
`serverUrl` (varsayılan https://makineceviri.org), `authToken` (DPAPI ile şifreli), `gameSlug`, `gameName`, `apiKey`, `gptModel`,
`captureMethod` (auto|dxgi|gdi), `ocrInterval`, `ocrModel` (ch|en), `fixSpacing` (true|false).
Salt okunur tanılama anahtarı: `_ocrModel` (etkin tanıma modeli).

### 3.5. Planlanan: bloke etmeyen ABI

`makine_capture_ocr_translate` çeviri yanıtı gelene kadar çağıranı bekletir. Önerilen ek ABI (eskisi korunur,
`MAKINE_PLUGIN_API_VERSION` artırılır, Makine Launcher deposuyla birlikte değiştirilir):

```c
// DLL içinde bir iş parçacığı başlatır: yakalama → OCR → havuz → API → sonuç
MakineError makine_live_start(void* hwnd, int x, int y, int w, int h, int interval_ms);
void        makine_live_stop(void);
// Son sonucu ÇAĞIRANIN tamponuna kopyalar (bellek sahipliği sorunu ve C# marshalling tuzağı ortadan kalkar).
// Dönüş: yeni sonuç varsa sıra numarası (>0), yeni bir şey yoksa 0, tampon küçükse -(gerekli boyut).
// status: 0 ham OCR, 1 havuz-bekleyen, 2 havuz-onaylı, 3 API çevirisi
int64_t     makine_live_poll(char* source, int source_size, char* translated, int translated_size, int* status);
```

Python'da doğrulanıp C++'a taşınacaklar: havuz istemcisi ve yerel havuz, çöp filtresi ve tekrar engelleme, oyun algılama
(`appinfo.vdf` ayrıştırıcısı dahil), piksel bazında alfalı native çeviri kutusu, konsol / TV modu ve telefon / OBS yayını.

---

## 4. Sunucuyla sözleşme (makineceviri.org `/api/live/`)

Sunucu makineceviri.org'un Cloudflare Worker'ıdır (D1 + R2); kodu bu depoda değil, özel **Makine-WEB** deposundadır.
Bu bölüm istemcinin dayandığı davranışı anlatır. Bütün yanıtlar JSON'dur; her istekte `X-Makine-Version` ve
`X-Makine-Device` (cihaz kimliği, aşağıda) gider.

### 4.1. İstemcinin kullandığı uçlar

| Uç | Erişim | Açıklama |
|---|---|---|
| `POST /api/live/auth?action=device_start` `{device}` → `{device, code, url, expires_in, interval}` | herkese açık, hız sınırlı | Makine Çeviri hesabıyla giriş: uygulama kodu gösterir, oyuncu `makineceviri.org/live/baglan/?kod=…` sayfasında onaylar |
| `GET /api/live/auth?action=device_poll&device=` | herkese açık | Onaylanınca uygulama jetonu **bir kez** döner (`status: approved`); süre dolunca 410 |
| `POST /api/live/auth?action=app_login\|app_logout`, `GET ?action=app_me` | — / jeton | Ekip hesabıyla şifreli giriş; jeton doğrulama |
| `GET /api/live/app?action=status`, `POST ?action=ann_read` | jeton | Bakım modu, sürüm kontrolü (`available` / `mandatory`), okunmamış duyurular |
| `POST /api/live/translate` `{game, text, source_lang, target_lang}` | jeton | Havuz + (gerekirse) sunucu tarafı DeepL + kayıt. 422 çöp, 429 günlük sınır, 503 anahtar yok / bakım |
| `GET /api/live/pool?action=sync&game=<slug>[&rev=][&head=1]` | jeton | Yerel havuz: oyunun **yalnızca onaylı** satırları `[id, anahtar, kaynak, çeviri]`. `head=1` yalnızca `{count, rev}`; `rev` aynıysa `{unchanged}` |
| `POST /api/live/pool?action=hits` `{game, hits: {id: adet}}` | jeton | Yerel havuzdan gösterilen satırların sayacı |
| `POST /api/live/pool?action=submit` | yalnız ekip jetonu (oyuncuya 403 `team_only`) | DLL'in geliştirici motorlarının (gpt / deepl / google) ürettiği çeviriyi oy olarak gönderir |
| `GET /api/live/releases?action=latest\|download[&id=]` | herkese açık | Uygulama sürümleri (indirilen `.exe`'nin SHA-256'sı istemcide doğrulanır) |

Jeton `Authorization: Bearer <jeton>` (ve uyumluluk için `X-Makine-Token`) ile gönderilir. 401 gelirse istemci oturumu
silip yeniden giriş ister; 403 `code: banned` hesabın askıya alındığını, 503 `code: maintenance` bakım modunu bildirir.

### 4.2. Çeviri havuzu ve konsensüs

- Her yeni satır için tek bir `translate` isteği atılır. Sunucu sırayla çöp kontrolü, havuz (tam + benzer eşleşme) ve
  gerekirse kendi DeepL anahtarıyla çeviri yapar. Kullanıcıların kendi API anahtarı yoktur.
- Yanıttaki `origin`: `pool` (onaylı havuz çevirisi), `own` (bu hesabın aynı satır için önceki çevirisi; DeepL çağrısı yok),
  `shared` (başka bir oyuncunun **aynı satır** için aldığı makine çevirisi; DeepL çağrısı yok, günlük sınırdan düşmez),
  `deepl` (sunucu yeni çevirdi).
- Her hesap bir satıra **bir oy** verir. Bir satır 3 farklı hesaptan oy toplayınca karara bağlanır: oyların ortalama
  benzerliği ≥ %60 ise otomatik onaylanır (`verified`), değilse çevirmen incelemesine düşer (`disputed`).
  Benzerlik, normalize edilmiş iki çeviri arasında `1 − Levenshtein / uzun metnin uzunluğu`'dur.
- Havuz oyunculara yalnızca **onaylı** satırları dağıtır. Çevirmenler `makineceviri.org/live/portal/` portalından onaylar,
  düzenler ya da siler. İstemcinin kendi ürettiği çeviriyi oy olarak göndermek (`submit`) yalnızca ekip hesaplarına açıktır;
  böylece birkaç hesapla havuza çeviri sokulamaz.
- Yönetici dışındaki hesaplar için günlük DeepL karakter sınırı vardır. Sınır dolunca yalnızca yeni DeepL çevirileri durur;
  havuz çevirileri gelmeye devam eder. Hedef dil şimdilik yalnızca `tr`'dir (havuz anahtarı hedef dili içermez).
- **Çöp filtresi** (vektörler `spec/garbage_vectors.json`): Latin alfabeli kaynakta CJK karakter, "lIlI" çizgi gürültüsü,
  İngilizce kaynakta düşük anlam skoru (`resources/en_words.txt` ile; yapışık yaygın kelimeler ve ünlemler gerçek sayılır),
  çok kısa / harf oranı düşük / tekrarlayan harfli satırlar, adres / e-posta / dosya yolu, kaynakla aynı çeviri. Sunucu
  bunları 422 ile reddeder; istemci aynı satırı bir daha göndermez.

### 4.3. Metin normalizasyonu (spec v2) ve havuz anahtarı

Havuz anahtarı `md5(dialogue_key(source_text))`, yani normalize edilmiş metnin **yalnızca harf ve rakamlarıdır**.
OCR'ın boşluk ve noktalama hataları ("Shaun?I" / "Shaun. I") ya da yuttuğu boşluklar ("Iwas going" / "I was going")
anahtarı değiştirmez. Kayıtta her zaman en çok kelime aralığı olan okunaklı yazım tutulur. Anahtar **yalnızca sunucuda**
hesaplanır; istemciler aynı kuralı yerel önbellek ve yerel havuz için kullanır.

**Benzer satır eşleşmesi:** Tam anahtar bulunamazsa aynı oyunda anahtarı ≥ %90 benzer (Levenshtein) bir kayıt aranır;
tek harflik OCR hatası ("cal"/"call", "Jasn"/"Jason") yeni kayıt açmaz. Anahtarı 12 karakterden kısa satırlar ("Yes"/"Yet")
ve rakamları farklı satırlar ("floor 3"/"floor 4") asla eşleşmez. Normalizasyon kuralları:

1. Baştaki ve sondaki boşlukları kırp.
2. Unicode küçük harfe çevir.
3. Boşluk dizilerini tek boşluğa indir.
4. Baştan `"'`‘’“”«»([-–—…` karakterlerini sil.
5. Sondan `.,!?:;"'`~_-–—…“”‘’«»)]` karakterlerini sil.
6. Kırp.

"Boşluk" ASCII boşlukları ve NBSP'dir; PCRE `\s` kullanılmaz. Ortak vektörler `spec/normalization_vectors.json`
içindedir; sunucu aynı vektörlerin kopyasıyla test edilir. C++'a taşırken aynı vektörlerle test edin. Kural değişirse
vektörler iki depoda birlikte güncellenir ve sunucudaki anahtarlar yeniden hesaplanır.

### 4.4. Gizlilik

- Sunucu IP adreslerini oy sayımında yalnızca HMAC özeti olarak kullanır; giriş kayıtları (IP, cihaz kimliği, sürüm) kötüye
  kullanımı ve hesap paylaşımını önlemek için sınırlı bir süre tutulur. Ayrıntı: makineceviri.org gizlilik politikası.
- Cihaz kimliği Windows `MachineGuid` değerinin uygulamaya özgü SHA-256 özetidir (`pool_client.device_id`); ham değer
  gönderilmez.
- OCR ve ekran görüntüsü bilgisayarda kalır; sunucuya yalnızca okunan **metin satırı** ve oyunun adı gider.

---

## 5. Test koşucusu (Python) ve Makine Live uygulaması

Bu test koşucusu oyunculara **tek bir `MakineLive.exe`** olarak verilir (`py tools/build_exe.py`); herkese açık indirme
`makineceviri.org/live/`. Python ya da başka bir şey kurmak gerekmez. Nihai ürün yine DLL'dir; Launcher entegrasyonunda
bu uygulamanın yerini Launcher alır.

- **Sürüm:** `manifest.json` → `0.3.0`, kanal `beta` (ekranda "0.3.0 beta"; `.exe` FileVersion 0.3.0, ProductVersion
  "0.3.0 beta"; DLL `makine_get_info` 0.3.0; Makine TV `versionName` aynı sayıyı manifest'ten alır; `tv-webos` paket
  dosyaları da 0.3.0). 0.3.0 ilk açık sürümdür: sunucu makineceviri.org. Her istekte `X-Makine-Version` gider.
- **Açılış sırası:** önce `/api/live/app?action=status`: bakım (yönetici değilse) → bakım ekranı; zorunlu güncelleme → "Güncelleme
  gerekli" ekranı (**Şimdi güncelle**: `app_update` ile indir, SHA-256 doğrula, uygulama kapanınca kendi `.exe`'sinin üzerine
  yaz ve yeniden aç; yazılamazsa yeni dosyayı bulunduğu yerden açar). Sonra oturum doğrulanır. Girişten sonra ve her
  `STATUS_POLL_S` (5 dk): isteğe bağlı yeni sürüm penceresi ("Şimdi güncelle / Sonra") ve okunmamış duyurular panelin üstünde
  birer birer (aciliyet, gönderen, tarih, "1 / 3"); kapatılan duyuru okundu sayılır.
- **Giriş:** Açılışta hesap ister. Ana düğme **Makine Çeviri hesabıyla giriş yap**: `device_start` kodu ve bağlantıyı
  alır, kod ekranda büyük görünür, tarayıcıda `makineceviri.org/live/baglan/?kod=…` açılır (**Sayfayı yeniden aç**,
  **Vazgeç**); uygulama `interval` (3 sn) arayla `device_poll` sorar, onaylanınca jetonu alır (10 dk'da onaylanmazsa
  süre doldu der). İkinci yol **Ekip hesabıyla giriş**: kullanıcı adı + şifre (`app_login`). Jeton `%APPDATA%\MakineLive\session.dat` içinde DPAPI ile şifreli
  saklanır, sonraki açılışlarda `app_me` ile doğrulanır. 401 gelirse oturum silinip giriş ekranına dönülür; ağ hatasında
  jeton korunur ve "Tekrar dene" çıkar. Oturum yokken F9 çalışmaz. **Hesap** sekmesinde yalnızca kullanıcı adı, rol, sürüm, bağış, çıkış ve
  **Discord'da göster** anahtarı bulunur; sunucu adresi ve karakter kullanımı uygulamanın hiçbir yerinde gösterilmez (ürün kararı).
- **Sabit ayarlar (ürün kararı):** Dil yalnızca İngilizce → Türkçe (`SOURCE_LANG`, `TARGET_LANG`; seçim yok).
  Okuma her zaman yalnızca algılanan oyun öndeyken yapılır (`only_focus` daima açık; anahtar yok).
- **Yerel havuz** (`local_pool.py`): Oyun algılanınca sunucuya o oyunun kaç onaylı satırı olduğu sorulur (`sync&head=1`).
  Varsa panelde **"Hazır çeviriler bulundu · … için N onaylı çeviri var. Oynayacağınız oyun bu mu?"** sorusu çıkar.
  **Evet, indir** ile yalnızca onaylı satırlar `%APPDATA%\MakineLive\pool\<slug>.json` dosyasına iner; oyun hatırlanır
  (`pool\games.json`) ve sonraki açılışlarda sormadan eşitlenir. **Hayır** o oturumda yeniden sormaz. **Oyun değil** yerel
  havuzu da siler. Oyun açıkken her `POOL_SYNC_S` (60 sn), F9 ile başlarken ve girişte `rev` sorulur; bir satır onaylanır,
  düzenlenir, onayı geri alınır ya da silinirse sürüm değişir ve liste yeniden iner (kayıtta `+eklenen · ~değişen · −kalkan`).
  Satır önce yerelde aranır (tam anahtar, sonra sunucuyla aynı ≥%90 benzerlik kuralı, `keys_similar`); bulunursa ağa çıkılmaz
  (kayıtta `yerel onaylı`). Yerelden gösterilen satırlar sayılır ve eşitlemede `hits` ile gönderilir. Ağ yoksa diskteki kopya
  kullanılır. Oyun kartında "Steam · N hazır çeviri" görünür.
- **Çeviri:** Yerelde olmayan her kararlı satır için tek bir `/api/live/translate` isteği atılır (keep-alive). Oturum içi önbellek
  yalnızca **onaylı** sonuçlar için kısa yoldur: onaylanmamış satırlar her seferinde sunucuya gider (oy kayıtlı kalır, satır bu
  arada onaylandıysa onaylı çeviri gelir; sunucu aynı hesabın önceki çevirisini ya da başka bir oyuncunun aynı satır için aldığı
  makine çevirisini DeepL çağırmadan döndürür: kayıtta `önceki` / `ortak`). Kullanıcıda API anahtarı ya da
  motor seçimi yoktur. 422 alan satır bir daha gönderilmez. 429 ve 503 durumlarında sakin bir uyarı gösterilir; havuz
  çevirileri gelmeye devam eder. Ağ hatası ya da 502 alınırsa bir kez yeniden denenir.
- **Paketli çalışma:** `.exe` içinde DLL, OCR ve modeller `sys._MEIPASS\runtime`'dan yüklenir; veriler `%APPDATA%\MakineLive`
  altındadır. Tek örnek çalışır (ikinci açılış mevcut pencereyi öne getirir).
- **Günlük:** `%APPDATA%\MakineLive\logs\makine-live.log` paneldeki kayıtları ve yakalanmayan hataların tam izini içerir
  (1 MB × 3). Testerlar sorun bildirirken bu dosyayı gönderir.
- **Doğrulama:** `MakineLive.exe --selftest` arayüz açmadan DLL'i yükler, bilinen bir satırı OCR ile okur, sonucu
  `%APPDATA%\MakineLive\selftest.json` dosyasına yazar.
- **Sürüm çıkarma:** `build.cmd` (ya da `py tools/build.py [X.Y.Z] [--hizli]`): sürüm numarasını `manifest.json`,
  `CMakeLists.txt`, `src/plugin.cpp` ve `tv-webos` paket dosyalarında birlikte değiştirir (yalnızca büyütür), DLL'i
  derler, testleri çalıştırır (Node yoksa LG servis testi atlanır), exe'yi paketler, exe'nin içinden `--selftest` yapar ve
  boyut, SHA-256 ve sürüm notu dosyasını (`docs/surum-notlari/vX.Y.Z.txt`) yazar. Exe açıkken başlamaz.
- **Derleme:** `py tools/build_exe.py` (PyInstaller, tek dosya, pencereli, ikon ve sürüm bilgisiyle). `build/release` eksikse, DLL
  `src/`'den eskiyse ya da manifest sürümü uyuşmuyorsa reddeder. **Her DLL derlemesinden sonra `.exe` yeniden derlenmelidir.**
  Boyut yaklaşık 53 MB, ilk açılış yaklaşık 3 sn (tek dosyalık paketin açılması).
- Geliştirmede sunucu adresi `MAKINE_LIVE_SERVER` ortam değişkeniyle değiştirilebilir (yerel test sunucusu için).
  `test.cmd` (`tools/offline.py`) bunu kullanır: 127.0.0.1'de sahte bir `/api/live` açar (giriş kendiliğinden onaylanır,
  ekip girişi her adı kabul eder; çeviri `spec/sample_quotes.json`'dan ya da `[Test] …`; yerel havuz kullanıcının indirdiği
  dosyalardan kopyalanır; bir test duyurusu) ve uygulamayı `APPDATA=build/offline/appdata` ile açar, gerçek oturuma
  dokunmaz. Adres modül yüklenirken okunur: ortam değişkeni süreç başlamadan verilmelidir.
- **Discord'da göster** (`net/discord_presence.py`, Hesap sekmesi, `prefs.discord_presence`, varsayılan açık): okuma açıkken
  Discord profilinde "Makine Live oynuyor": oyunun adı ("… · Türkçe oynuyor"; konsolda "konsolda"), oturumun başından beri
  geçen süre ve bu oturumda (F9'dan beri) çevrilen diyalog sayısı, Steam kapağı (yoksa logo), "Makine Live'ı indir" düğmesi.
  Durdurunca, çıkışta ve ayar kapanınca temizlenir. Discord masaüstü uygulamasının yerel IPC kanalı (`discord-ipc-0..9`)
  kullanılır; bağımlılık yok, ağa çıkılmaz. Discord kapalıysa 20 sn'de bir yeniden denenir; güncellemeler Discord'un hız
  sınırına göre en az 15 sn arayla birleştirilerek gider. Discord Developer Portal'daki "Makine Live" uygulamasının kimliği
  `DISCORD_APP_ID`'dedir (gizli değildir; geliştirmede `MAKINE_DISCORD_APP_ID`); kimlik boşsa ayar görünmez.
  Görseller adresle verilir (sitenin `icon-512.png`'si, Steam kapağı); Portal'a görsel yüklenmez.
- **Bağış:** ilk açılışta (girişten sonra, güncelleme / duyuru pencerelerinin ardından) bir kez "Makine Live gönüllü ve
  ücretsiz bir proje" penceresi: **Bağış yap** Kreosus sayfasını (`DONATE_URL`) açar, **Daha sonra** kapatır; görüldüğü
  `prefs.donate_seen`'e yazılır. Hesap sekmesinde **Bağış yap** düğmesi her zaman durur.

- **Logo:** `ui/brand.py` (`logo_image()` / `app_mark_image()`) `assets/logo_white-96.webp`'den çizer (giriş ekranı, pencere ikonu,
  `.exe` ikonu). Logo `.exe`'ye gömülür (`tools/build_exe.py --add-data`). Büyük boyutlarda alfa yumuşak büyütülüp keskinleştirilir.
- `app.py` + `ui/hud_ui.py`: koyu kontrol paneli (Canvas ile çizilen bileşenler ve animasyonlu ikonlar).
  Sekmeler: Canlı (istatistikler, gecikme grafiği, geçmiş: tıkla = kopyala, çift tıkla = yeniden göster), Çeviri, Görünüm,
  Konsol (konsol / TV modu, aşağıda) ve Hesap.
  Kısayollar: **F8** kilitle/taşı (tıklama geçirgenliği), **F9** başlat/durdur, **F10** bölge seç.
- **Pipeline:** Her ~100 ms yoklama yapılır (DLL değişmeyen kareyi ~1 ms'de döndürür). Bir satır 2 yoklama boyunca
  sabit kalınca (≈200 ms) ya da OCR titremeye devam ediyorsa 0,6 sn sonra çeviriye gönderilir. Çeviri 3 iş parçacıklı
  havuzda yapılır, yani döngü ağı beklemez. `HttpPool` (pool_client.py) havuz ve çeviri API'leri için HTTPS
  bağlantılarını açık tutar (havuz isteği medyanı 108 → 32 ms). Aşama süreleri kayda yazılır.
- **Satır takibi** (`subtitle_timing.LineTracker`): OCR satır sonları tek cümleye birleştirilir. ≥%80 benzer satırlar
  ya da büyüyen daktilo metni aynı satır sayılır. Gönderimden önce en çok boşluk içeren okuma seçilir.
- **Zamanlama** (`SubtitleScheduler`): Çeviri, kaynak satırın oyunda göründüğü süre kadar (gösterildiği andan itibaren)
  ve en az okuma süresi (1,2 sn + 45 ms/karakter, en fazla 7 sn) boyunca ekranda kalır. Kaynak hâlâ ekrandaysa çeviri de kalır.
  Satırlar sırayla gösterilir. Gecikme 2 sn'yi aşarsa satır süresi max(1 sn, okuma süresinin %70'i)'ne iner.
  Kaynağı 3 sn'den uzun süre önce kaybolmuş bir satır, yenisi hazırsa atlanır (en yeni satır asla atlanmaz).
- **Oyun algılama: yalnızca oyunlar** (`game_library.GameLibrary` + `game_detector.detect_game`). Tahmin ve yedek yoktur:
  bir pencere ancak süreci kurulu bir oyun olarak tanınıyorsa hedef olur. Tarayıcı, müzik çalar, IDE ve Discord asla
  algılanmaz; oyun yoksa hiçbir şey okunmaz ("oyun algılanmadı"). Kaynaklar:
  1. Kullanıcı işaretleri: `%APPDATA%\MakineLive\games_custom.json` (panelde **Oyun ekle** / **Oyun değil**).
  2. Steam: `libraryfolders.vdf` + `appmanifest_*.acf`. Tür, SteamDB'nin de kullandığı yerel `appcache\appinfo.vdf`
     önbelleğinden okunur (`common.type` Game/Application/Tool… ve `primary_genre`; ikili KeyValues v27–29, ~20 ms, çevrimdışı,
     mağazadan kaldırılmış oyunlar dahil). Soundpad, Lossless Scaling, Crosshair X, SteamVR ve Steamworks paketleri oyun değildir.
     Yerelde türü olmayan uygulama için mağaza API'si (`appdetails`) türlere bakılarak sorulur, sonuç `steam_app_types.json`
     içinde 30 gün tutulur. Türü bilinmeyen uygulama klasör eşleşmesiyle oyun sayılmaz.
  3. Windows oyun listesi: `HKCU\System\GameConfigStore\Children\*\MatchedExeFullPath` (exe düzeyinde).
  4. Epic (`AppCategories` içinde `games`), GOG, Ubisoft Connect, EA, Xbox/Game Pass (`XboxGames\*\Content\MicrosoftGame.config`).
  5. `KNOWN_GAMES` exe adları. Bu liste ayrıca havuzdaki slug'ı sabitler (mağaza adı farklı yazılsa bile).
  Klasör eşleşmelerinde oyunun yardımcı programları (`HELPER_EXE`: crash, launcher, setup, anticheat, redist…) oyun sayılmaz.
  Birden çok oyun açıksa öndeki oyun, sonra Steam'in `RunningAppID`'si, sonra en büyük pencere seçilir. Java ile çalışan
  oyunlarda (Minecraft istemcileri) ad pencere başlığından gelir. Durdurulmuşken kart 3 sn'de bir, çalışırken hedef yoksa ya da
  kapandıysa saniyede bir yeniden algılanır. Yenile düğmesi kütüphaneyi yeniden tarar. Kontrol: `py -m makine_live.games.game_detector --steam`.
- **Hedef kilidi** (`capture_block_reason`): Okuma yalnızca algılanan oyunun penceresinden yapılır. Hedef ön planda değilse
  ya da z-sırasında üstündeki görünür bir pencere bölgeyle örtüşüyorsa o yoklama atlanır. Panelde oyun adı, kaynağı
  (Steam, Windows oyun listesi…) ve exe; bekleme sebebi durum satırında görünür.
- **Kutu** (`overlay_render.py`): Yalnızca **çeviri** gösterilir. İngilizce kaynak satırı ve "bekliyor" durumu yoktur;
  kutu, çeviri hazır olunca belirir. Pillow ile çizilip `UpdateLayeredWindow` ile piksel bazında saydam gösterilir.
  Uzun metinde yazı en fazla 4 satıra sığacak şekilde %74'e kadar küçülür, kutu ekrana sığdırılır.
  Native overlay'e taşınacak parçalar: `render_overlay()`, `paint_strip()`, `LayeredWindow.present()`, `SubtitleOverlay`.
  **Kural:** katmanlı pencerenin yordamından (`LayeredWindow._handle`, ctypes geri çağrısı) Tk / Tkinter çağrılmaz. Yordam Tcl olay
  döngüsünün içinden çalışır; oradan Tkinter çağrısı "Fatal Python error: PyEval_RestoreThread" ile süreci iz bırakmadan kapatır
  (v0.1.0 çökmesi). Sürükleme kendi fare mesajlarıyla yapılır (SetCapture + SetWindowPos; Windows'un taşıma döngüsü yok),
  bırakılan konum `win.moved`'a yazılır ve `SubtitleOverlay._poll_moved` onu Tk zamanlayıcısıyla işler. Yerel çökmeler
  `faulthandler` ile `logs/crash.log`'a yazılır.
- **Konsol / TV modu** (PS5, Xbox; kullanıcı kılavuzu `docs/KONSOL_TV.md`). Konsol HDMI ile TV'ye bağlı kalır; görüntünün
  kopyası PC'de bir pencerede açılır ve çeviri TV'deki **Makine TV** uygulamasında HDMI görüntüsünün üstünde gösterilir.
  Kullanıcı kararları: ilk sürümde PC şart, Android / Google TV (sonradan LG webOS da istendi), PS5 + Xbox.
  - **Konsol modu yalnızca GTA VI içindir** (ürün kararı: DeepL kotası tek havuzda toplansın): oyun seçimi yoktur, her
    satır `grand-theft-auto-vi` havuzuna gider (`console_mode.console_game()`).
  - Panelde **Konsol** sekmesi: "Oyun nerede? Bu bilgisayarda | Konsolda" (`prefs.mode` = `pc` | `konsol`, `hud.json`),
    konsol görüntüsü kartı (**Pencere seç**: listede olmayan program) ve "Altyazı nerede görünsün? **TV'de | Bu ekranda |
    Telefonda**" (`prefs.console_out` = `tv` | `pc` | `web`; eski `pc_box` → `pc`) ile duruma göre değişen kart:
    TV (**TV bul**, **Kur**), bu bilgisayarın ekranı (monitörde oynayanlar: kutu Remote Play / yakalama penceresinin 16:9
    görüntüsüne göre yerleşir, `SubtitleOverlay.set_frame`), telefon (adres + **Kopyala**, bağlı cihaz sayısı).
    Konsol modunda oyun kartındaki düğme "Konsol"dur.
  - **Telefon / tablet ve OBS** (`subtitle_web.SubtitleWeb`, kapı 47712, doluysa sonraki 7 kapı): `/` telefon sayfası (son
    satır büyük, önceki 4 satır küçük, A−/A+, tam ekran), `/obs` saydam OBS tarayıcı kaynağı (kutuyla aynı görünüm,
    `?size=&opacity=&pos=ust`), `/events` SSE (her değişiklikte tam durum). Yalnızca açıkken çalışır; telefon seçiliyken ev
    ağına (0.0.0.0), yalnızca OBS açıkken 127.0.0.1'e bağlanır; `SO_REUSEADDR` kapalı. Dış kaynak yok (CSP `'self'`).
    Hesap / jeton taşımaz. Zamanlayıcının gösterdiği her satır sunucu açıksa ona da gider.
  - **Yayın** (Görünüm sekmesi): "Yayında ve ekran paylaşımında göster" (`stream_visible`) PC oyununda bölgeyi oyun
    penceresinin istemci koordinatına çevirip DLL'e `hwnd` ile verir; kutu yakalamaya açılır. `console_mode.window_has_image`
    (PrintWindow, ~9 ms) pencere siyah gelirse 5 sn arayla 6 kez dener, sonra dakikada bir; o sürede kutu gizli kalır ve ekran
    kopyası okunur. Kutu yakalamaya açıkken ekran kopyası **asla** okunmaz (yoklama atlanır). "OBS tarayıcı kaynağı"
    (`obs_source`) sunucuyu 127.0.0.1'de açar ve adresi kopyalar.
  - **Kaynak** (`console_mode.find_console_source`): PS Remote Play (`RemotePlay.exe`), chiaki, Xbox uygulaması (`XboxPcApp.exe`),
    Greenlight, Elgato, OBS pencereli projektör, Windows Kamera + kullanıcının seçtiği exe'ler (`console_exes`). Bu pencereler
    yalnızca konsol modu açıkken hedef olur; PC modundaki "yalnızca oyunlar" kuralı aynen geçerlidir. Oyun her zaman GTA VI;
    Steam araması yapılmaz.
  - **Okuma:** DLL'e `hwnd` + istemci koordinatı verilir; pencerenin kendi içeriği okunur (§3.2), yani üstünde başka pencere
    olabilir ve önde olması gerekmez. Bekleme sebepleri: pencere yok / kapandı / küçültülmüş / 960×540'tan küçük.
    Bölge pencerenin 16:9 video alanına göre oransaldır (`console_region`, varsayılan alt orta `0.12, 0.70, 0.76, 0.26`);
    F10 seçimi orana çevrilir, pencere taşınsa da kaymaz.
  - **TV bağlantısı** (`tv_link.TvLink`, protokol v1): keşif UDP 47710 yayını (`discover` → `here{id,name,port}`), akış TCP 47711,
    satır başına JSON: `hello{v,code,pc}` → `ok{tv}` | `bad_code`; sonra `line{id,text,kind}`, `clear`, `style{size,pos,opacity}`
    (isteğe bağlı), 5 sn'de bir `ping`. TV'deki 4 haneli **eşleşme kodu** olmadan bağlanılamaz. Deneme yanılmaya karşı TV
    her yanlış kodda 1 sn bekletir; 5 yanlıştan sonra kodu yeniler ve 60 sn hiçbir kodu kabul etmez (karşılaştırma sabit süreli). Gönderim ayrı iş parçacığında,
    tek yuvalı (en yeni satır kazanır; TV'ye ulaşılamazken satır birikmez). Kopunca 2 sn'de bir yeniden denenir; TV'nin adresi
    değiştiyse kimliğiyle ağda aranır. Eşleşen TV ve kod `hud.json`'da (`tv`, `tv_code`) saklanır. Zamanlayıcının gösterdiği her
    satır (`_tick_scheduler`) TV'ye de gider; satır bitince `clear`.
  - **Makine TV** (`tv/`, paket `org.makineceviri.tv`, minSdk 26): `OverlayService` ön plan servisi `TYPE_APPLICATION_OVERLAY`
    penceresinde altyazı şeridi çizer (`SubtitleView`: design_tokens overlay ölçüleri, onaylı = yeşil / havuzda bekleyen = sarı
    şerit, 4 satıra sığdırma). Android 12+ dokunmayı geçiren katmanı %80 opaklığa indirdiği için pencere yalnızca altyazı
    ekrandayken dokunmayı tutar (`setPassThrough`; odak hiç almaz, kumanda HDMI girişinde kalır). `LinkServer` keşif + TCP; 15 sn sessizlikte bağlantıyı kapatıp şeridi temizler. `MainActivity`
    kumandayla kullanılır: büyük eşleşme kodu, durum, yazı boyutu / konum / arka plan / örnek altyazı / açılışta başlat / yeni kod.
    Geri tuşu uygulamayı arka plana alır. **Sunucuya bağlanmaz, jeton taşımaz.** İkon ve afiş `tools/brand_assets.py` üretir.
    İmza anahtarı depoda değildir (`MAKINE_TV_KEYSTORE`, `MAKINE_TV_KEYSTORE_PASS`; yoksa hata ayıklama anahtarı).
  - **TV'ye kur** (`tv_install.install`): TV'de USB / ağ hata ayıklaması açıldıktan sonra ADB 5555 üzerinden (adb-shell,
    anahtar `%APPDATA%\MakineLive\adbkey`) APK'yı kurar, `appops set … SYSTEM_ALERT_WINDOW allow` ile katman iznini verir
    ve uygulamayı açar. Ağdaki ADB açık cihazlar `/24` taramasıyla bulunur. Yedek: **APK'yı kaydet** (USB bellek).
    Kur penceresinde **Android / Google TV | LG (webOS)** seçimi var (`prefs.tv_kind`).
  - **LG webOS** (`tv-webos/`, uygulama `org.makineceviri.tv`, servis `org.makineceviri.tv.service`). LG'de uygulamalar TV'nin
    HDMI ekranının üstüne çizemez (`defaultWindowType: overlay` yok sayılır), bu yüzden uygulama konsolun görüntüsünü
    **kendi içinde** açar: `<video>` + `<source type="service/webos-external" src="ext://hdmi:N">`, sayfa zemini saydam,
    altyazı HTML olarak üstte. Kullanıcı oynarken TV'nin giriş menüsü yerine Makine TV'yi açar. Kumanda: OK menü, 1–4 giriş,
    Geri → `com.webos.app.hdmiN`'i açıp kapanır (kendiliğinden dönüş bazı modellerde siyah kalıyor). Ekran koruyucu
    `tvpower/power/registerScreenSaverRequest` aboneliğine `ack:false` ile engellenir (PS5 kolu kullanıcı hareketi sayılmaz).
    - **Servis** (`service/service.js`, ES5: webOS 4.x'te Node 0.12): Android'deki `LinkServer` ile aynı protokol, aynı
      kod kilidi; durum (kimlik, kod) `state.json`'da. Web uygulaması `start`'a abone olur (olaylar: `state | line | clear |
      style`), abone varken `activityManager` ile canlı tutulur; uygulama 10 sn'de bir `state` ile yoklar. UDP keşfi
      açılamazsa panel TV'nin adresini gösterir ve PC'de **TV bul › Adresle bağlan** (`_tv_address_sheet`) kullanılır.
    - **Paket:** `py tools/build_webos.py` → `.ipk` (ar: `debian-binary`, `control.tar.gz`, `data.tar.gz`;
      `usr/palm/applications|services|packages/…`, dizinler 0777, sahiplik 0:5000). Sürüm manifest'ten.
    - **Kurulum** (`tv_install.install_webos`): TV'de LG'nin Developer Mode uygulaması (Dev Mode Status + Key Server açık).
      Anahtar `http://<TV>:9991/webos_rsa`, Developer Mode ekranındaki Passphrase ile çözülür (paramiko), SSH 9922
      `prisoner` (LG'deki sunucu OpenSSH 6.1: `server-sig-algs` bildirmez, yalnızca `ssh-rsa` imzası kabul eder; paramiko
      önce rsa-sha2 dener, "Authentication failed" alınca yalnızca ssh-rsa ile yeniden bağlanılır — `_webos_ssh`;
      **pty istenmez**: sunucu pty isteğinde kanalı kapatır; `applicationManager/getAppInfo` webOS 3.x'te yoktur, kurulu
      olup olmadığı `/media/developer/apps/usr/palm/applications/<id>/appinfo.json` ile anlaşılır), SFTP (olmazsa `cat >`) ile `/media/developer/temp/makine-tv.ipk`, `luna-send-pub -i
      luna://com.webos.appInstallService/dev/install`, sonra `applicationManager/launch`. Geliştirici modu oturumu
      sürelidir; dolunca uygulama kalkar, yeniden Kur yeter.
    - **Gerçek LG TV'de doğrulandı** (LG 49UH770V-ZA, webOS 3.4.3, Node 0.12, tarayıcı Chromium 38): PC'den kurulum
      (2,4 sn), uygulamanın açılması, servisin UDP keşfe yanıt vermesi ve TCP 47711'i dinlemesi. Bu yüzden CSS'te değişken,
      `inset`, `gap`, `max()` yok. Denenmeyenler: HDMI'ın uygulama içinde açılması, oyun modu ve gecikme.
      Ayrıca: servis gerçek Node ile (`tests/test_webos_service.py`), kurulum bu TV'nin davranışını taklit eden sahte TV ile
      (`tests/test_webos_install.py`), arayüz tarayıcıda (`tests/webos_preview.html`).
  - Doğrulananlar: resmî Android TV 8, 9, 12 ve Google TV 14, 16 emülatörlerinde (`tools/tv_emulator_check.py`, 1920×1080)
    kurulum, izin, kod ve adres, yanlış kod reddi, eşleşme, ana ekran öndeyken tam opak katman ve temizleme. Android 12+ Ayarlar
    ekranı uygulama katmanlarını kendisi gizler; test bu yüzden ana ekranın üstünde ölçer. Ayrıca uçtan uca panel → sahte Remote Play penceresi (üstünde başka pencere varken) → DLL OCR → çeviri → TV.
    Gerçek PS5 / Xbox / Android TV ile denenmedi.
- Çöp filtresi `is_valid_dialogue_text`: sistem kelimeleri yalnızca bütün satır olarak eşleşir. Boşluksuz yalnızca
  28+ karakterlik tek token reddedilir. Çeviri kaynakla aynıysa havuza gönderilmez.
- `pool_client.py`: `ServerClient` (hesapla giriş `device_start`/`device_poll`, ekip girişi `login`, `translate`, keep-alive `HttpPool`), `SessionStore` (DPAPI), oturum içi LRU önbellek.
  Varsayılan sunucu `https://makineceviri.org` (`MAKINE_LIVE_SERVER` ile değişir; yerelde `wrangler dev` adresi).
  Ekran yakalama `captureMethod=auto` (önce DXGI).

---

## 6. Komutlar

```powershell
# OCR çalışma dosyalarını bir kez yerleştirin (git'te yok)
#   third_party/rapidocr/RapidOcrOnnx.dll, onnxruntime.dll, models/*

# DLL derleme (çalışma dosyaları build/release'e kopyalanır)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Python bağımlılıkları (masaüstü uygulaması, testler, paketleme)
py -m pip install -r requirements.txt

# Testler (ayrıntı: tests/README.md)
py tests/test_plugin.py          # DLL: init, ayarlar/DPAPI, gerçek OCR, eksik paket hatası
py tests/test_dll_server.py      # DLL "makine" motoru: sahte sunucuyla jetonlu çeviri, jeton iptali
py tests/test_normalization.py   # Python normalizasyonu ↔ spec vektörleri
py tests/test_game_library.py    # oyun kütüphanesi: yalnızca oyunlar (sahte Steam/Epic)
py tests/test_local_pool.py      # yerel havuz: arama, benzer satır, eşitleme farkları, kalıcılık
py tests/test_subtitle_scheduler.py  # satır takibi ve altyazı zamanlayıcısı
py tests/test_console_mode.py    # konsol modu: 16:9 alan, oransal bölge, kaynak eşleme, gerçek pencereyle bulma
py tests/test_tv_link.py         # TV bağlantısı: sahte TV ile keşif, kod, satır, en yeni kazanır, yeniden bağlanma
py tests/test_subtitle_web.py    # telefon / OBS sayfaları ve SSE akışı
py tests/test_discord_presence.py  # Discord'da gösterme: sahte Discord IPC kanalıyla el sıkışma, içerik, hız sınırı
py tests/test_webos_service.py   # LG servisi gerçek Node ile (sahte webos-service): keşif, eşleşme, satır, kod kilidi
py tests/test_webos_install.py   # LG geliştirici modu kurulumu, sahte TV'ye (anahtar, parola, SSH, kurulum yanıtları)
ctest --test-dir build           # C++ boşluk düzeltici (tests/text_fix_test.cpp)
py tools/tv_emulator_check.py    # Makine TV: Android TV 8/9/12 + Google TV 14/16 emülatörlerinde kurulum → eşleşme → katman
py -m makine_live.games.game_detector --steam  # bu makinedeki kütüphane ve Steam uygulama kararları

# Paket
py tools/pack.py           # → makine-live.makine (Launcher → Ayarlar → Eklentiler)

# Oyun içi canlı test (geliştirme)
py -m makine_live

# Sürüm: DLL → testler → exe → selftest → SHA-256 tek adımda (çift tıklanınca sürüm numarasını sorar)
build.cmd                  # ya da: py tools/build.py 0.3.1   |   py tools/build.py --hizli (testsiz)

# Makine Live uygulaması (tek dosya; önce DLL'i derleyin). Makine TV APK'sı eskiyse kendisi derler ve gömer.
py tools/build_exe.py      # → dist/MakineLive.exe
dist\MakineLive.exe --selftest   # → %APPDATA%\MakineLive\selftest.json (tv_apk, tv_adb dahil)

# Makine TV (Android TV) — Android SDK + JDK 17 gerekir; tv/local.properties → sdk.dir
tv\gradlew.bat -p tv assembleRelease   # → tv/app/build/outputs/apk/release/app-release.apk
py -m makine_live.tv.tv_install <TV-IP>  # APK'yı TV'ye kur + katman izni (TV'de hata ayıklama açık olmalı)

# Makine TV (LG webOS)
py tools/build_webos.py                # → build/webos/makine-tv.ipk (tools/build_exe.py de üretip gömer)
# Arayüzü TV olmadan görmek: Edge/Chrome --allow-file-access-from-files ile tests/webos_preview.html?scene=menu|line|long|note
```

Ortam notları: Windows'ta `python` PATH'te olmayabilir; `py` başlatıcısını kullanın (Python 3.13+; geliştirme 3.14 ile
yapılıyor). Derleyici GCC 15 (MinGW-w64), CMake ≥ 3.25, Ninja. Makine TV için Android SDK + JDK 17, LG servis testi için Node.js.
