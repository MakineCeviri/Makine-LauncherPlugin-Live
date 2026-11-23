<p align="center">
  <img src="resources/icon.png" width="96" alt="Makine Live">
</p>

<h1 align="center">Makine Live</h1>

<p align="center">
  Oyun ekranındaki İngilizce diyalogları okuyup <b>anında Türkçe altyazı</b> olarak gösteren canlı çeviri motoru.<br>
  <a href="https://makineceviri.org">Makine Çeviri</a> ekosisteminin parçası; <a href="https://github.com/MakineCeviri/Makine-Launcher">Makine Launcher</a> eklentisi.
</p>

<p align="center">
  <img alt="Sürüm" src="https://img.shields.io/badge/s%C3%BCr%C3%BCm-0.3.2%20beta-FCCD66">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D4">
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C">
  <a href="LICENSE"><img alt="Lisans" src="https://img.shields.io/badge/lisans-AGPL--3.0%20%2B%20Commons%20Clause-555"></a>
</p>

<p align="center">
  <a href="https://makineceviri.org/live/"><b>İndir</b></a> ·
  <a href="docs/GTA6_MAKINE_LIVE_REHBERI.md">Kullanım rehberi</a> ·
  <a href="docs/KONSOL_TV.md">Konsol / TV modu</a> ·
  <a href="docs/MIMARI.md">Teknik mimari</a> ·
  <a href="SECURITY.md">Güvenlik</a>
</p>

---

## Ne yapar?

Makine Live, oyun oynarken ekranın altyazı bölgesini sürekli okur (OCR), yeni bir satır belirdiğinde onu Türkçeye çevirir
ve oyunun üstünde, okunması kolay bir altyazı kutusu olarak gösterir. Oyunun dosyalarına dokunmaz; yama gerektirmez.

Canlı çeviride iki sorun vardır: makine çevirisi her satırda 400–900 ms sürer ve aynı cümleler binlerce oyuncu tarafından
tekrar tekrar çevrilir. Makine Live bunu bir **ortak çeviri havuzu** ile çözer:

1. Okunan satır önce bilgisayardaki önbelleğe ve oyunun **onaylı çevirilerine** bakılarak aranır; bulunursa ağa hiç çıkılmaz.
2. Yoksa satır [makineceviri.org](https://makineceviri.org) havuzuna sorulur. Havuzda da yoksa sunucu satırı **bir kez**
   çevirir; aynı satırı gören sonraki oyuncular aynı çeviriyi alır.
3. Her oyuncu bir oy sayılır. Farklı hesaplardan uyuşan çeviriler gelince satır kendiliğinden onaylanır; uyuşmayanları
   Makine Çeviri çevirmenleri inceler, düzeltir ve onaylar. Oyun oynandıkça havuz büyür ve kalitesi artar.

## Özellikler

- **Hızlı ekran okuma:** DXGI Desktop Duplication (değişmeyen sahnede ~1 ms), gerektiğinde GDI; tam ekran oyunlarda çalışır.
- **Yerel OCR:** RapidOCR (PaddleOCR modelleri + ONNX Runtime). Görüntü bilgisayardan çıkmaz; sunucuya yalnızca metin gider.
- **Oyun yazılarına özel temizlik:** yapışık kelimeleri ayırma ("Iwas goingto" → "I was going to"), yabancı alfabe gürültüsü ve
  çöp satır filtresi, daktilo efektinde satır takibi, titreyen OCR için kararlılık beklemesi.
- **Ortak çeviri havuzu:** onaylı çeviriler bilgisayara iner ve çevrimdışı da çalışır; aynı satır bir daha çevrilmez.
- **Okunabilir altyazı:** piksel bazında saydam kutu, satırın ekranda kaldığı süre ve okuma hızına göre zamanlama,
  kutu yayında ve ekran paylaşımında gizli (isteğe bağlı görünür).
- **Yalnızca oyunlar:** Steam, Epic, GOG, Ubisoft Connect, EA, Xbox / Game Pass ve Windows oyun listesinden kurulu oyunları
  tanır; tarayıcı, Discord ya da başka bir pencere asla okunmaz.
- **Konsol / TV modu:** PS5 ve Xbox oyunları için. Konsolun görüntüsü PC'de (PS Remote Play, Xbox uygulaması, yakalama
  kartı) okunur; altyazı TV'deki **Makine TV** uygulamasında (Android TV / Google TV, LG webOS), PC ekranında,
  telefonda ya da OBS tarayıcı kaynağında gösterilir.
- **Makine Çeviri hesabıyla giriş:** uygulama bir kod gösterir, tarayıcıda onaylarsınız. Kullanıcının API anahtarı gerekmez.

## Nasıl çalışır?

```mermaid
flowchart LR
    G[Oyun ekranı] -->|DXGI / GDI| C[Yakalama<br/>kare özeti]
    C -->|kare değiştiyse| O[RapidOCR<br/>ONNX]
    O --> F[Temizlik<br/>boşluk · çöp · satır takibi]
    F --> L{Yerel havuz<br/>önbellek}
    L -->|bulundu| S[Altyazı kutusu]
    L -->|yok| P[(makineceviri.org<br/>çeviri havuzu)]
    P -->|onaylı / ortak / yeni çeviri| S
    S --> TV[Makine TV · telefon · OBS]
```

Ayrıntılar (C ABI, yakalama, pipeline, sunucuyla sözleşme, konsensüs kuralları, normalizasyon, uygulama ve TV protokolü):
**[docs/MIMARI.md](docs/MIMARI.md)**.

## Bileşenler

| Bileşen | Klasör | Durum |
|---|---|---|
| **`makine-live.dll`** — C++23 çekirdeği: yakalama, OCR, temizlik, önbellek, çeviri. Makine Launcher'ın yüklediği nihai ürün (`.makine` paketi) | `src/`, `include/` | beta |
| **Makine Live uygulaması** — tek dosyalık `MakineLive.exe`: kontrol paneli, oyun algılama, yerel havuz, altyazı kutusu, konsol modu. Özellikler önce burada denenir, sonra DLL'e taşınır | `makine_live/` | 0.3.2 beta |
| **Makine TV (Android TV / Google TV)** — HDMI görüntüsünün üstüne altyazı çizen katman uygulaması | `tv/` | beta |
| **Makine TV (LG webOS)** — konsol görüntüsünü kendi içinde açıp altyazıyı üstüne yazan uygulama + servis | `tv-webos/` | beta |
| **Sunucu** — çeviri havuzu, hesaplar, çevirmen portalı (`makineceviri.org/api/live/`, Cloudflare Workers) | ayrı depo | canlı |

## Kullanım

1. [makineceviri.org/live](https://makineceviri.org/live/) adresinden `MakineLive.exe`'yi indirip açın (kurulum gerekmez).
2. **Makine Çeviri hesabıyla giriş yap**'a basın, tarayıcıda açılan sayfada kodu onaylayın.
3. Oyunu açın; Makine Live oyunu kendisi bulur. Altyazı bölgesini **F10** ile seçin, **F9** ile başlatın.
   **F8** altyazı kutusunu kilitler / taşınabilir yapar.

Konsolda (PS5 / Xbox) oynuyorsanız: [docs/KONSOL_TV.md](docs/KONSOL_TV.md). Kurulumdan TV modellerine, telefon ve OBS'ye
kadar ayrıntılı rehber: [docs/GTA6_MAKINE_LIVE_REHBERI.md](docs/GTA6_MAKINE_LIVE_REHBERI.md).

**Makine Launcher eklentisi olarak:** `makine-live.makine` paketini Launcher → Ayarlar → Eklentiler'den yükleyin.

## Geliştirme

### Gereksinimler

- Windows 10/11 (x64)
- GCC 15 (MinGW-w64) ya da C++23 destekli başka bir derleyici, CMake ≥ 3.25, Ninja
- Python 3.13+ (masaüstü uygulaması, testler ve paketleme için): `py -m pip install -r requirements.txt`
- İsteğe bağlı: Android SDK + JDK 17 (Makine TV), Node.js (LG servis testi)

### OCR çalışma dosyaları

Boyutları nedeniyle depoda değiller. `python tools/fetch_ocr.py` hepsini adresi ve SHA-256'sı sabitlenmiş olarak
`.cache/ocr/` altına indirir (ikinci çalıştırmada yalnızca doğrular): RapidOcrOnnx 1.2.3 C kütüphanesi (statik
CRT; ONNX Runtime ve OpenCV içinde, tek DLL) ve PP-OCRv3 modelleri. CMake bunları `build/package/` ve
`build/release/` altına kopyalar.

```
.cache/ocr/
├── bin/RapidOcrOnnx.dll
├── models/
│   ├── ch_PP-OCRv3_det_infer.onnx
│   ├── ch_ppocr_mobile_v2.0_cls_infer.onnx
│   ├── ch_PP-OCRv3_rec_infer.onnx
│   └── ppocr_keys_v1.txt
└── licenses/
```

### Derleme ve paketleme

Tek adımda sürüm: `build.cmd`'ye çift tıklayın ya da komut satırından çalıştırın. DLL'i derler, testleri çalıştırır,
`dist/MakineLive.exe`'yi paketler, exe'nin içinden selftest yapar ve sonunda boyutu ve SHA-256'yı yazar.

```powershell
build.cmd                        # yeni sürüm numarasını sorar (boş = aynı sürüm)
build.cmd 0.3.3                  # sürümü her yerde 0.3.3 yapar, sonra derler
build.cmd --hizli                # testleri atlar
```

Sunucuya bağlanmadan denemek için `test.cmd`: bilgisayarda küçük bir sahte sunucu açar ve uygulamayı ona bağlar
(giriş kendiliğinden onaylanır, çeviriler örnek repliklerden ya da indirilmiş onaylı havuzdan gelir). Gerçek oturuma ve
ayarlara dokunmaz; `test.cmd --exe` derlenmiş exe'yi, `test.cmd --temiz` ilk açılışı (bağış penceresi) dener.

Adım adım:

```powershell
python tools/fetch_ocr.py        # OCR çalışma dosyaları (bir kez)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build              # → build/package/ (Makine'nin paketlediği eklenti) + build/release/ (Python koşucusu)
cmake --build build --target pack   # → build/org.makineceviri.live-<sürüm>.mkplugin
py -m makine_live                # test koşucusunu derlemeden çalıştırma (geliştirme)
```

`build/package/`: `manifest.json`, `bin/makine-live.dll` (girdi), `bin/RapidOcrOnnx.dll`, `models/`,
`resources/en_words.txt`, `licenses/`. Makine bu klasörü `plugins/org.makineceviri.live/` olarak paketine koyar.

### Testler

```powershell
py tests/test_plugin.py          # DLL ABI, ayarlar ve DPAPI, gerçek OCR
py tests/test_normalization.py   # ortak normalizasyon vektörleri
ctest --test-dir build           # C++: spec vektörleri (normalizasyon, çöp), OCR süzgeci, altyazı zamanlaması,
                                 # konsol geometrisi, yerel havuz, boşluk düzeltici (tests/cpp/test_logic.cpp)
```

Bütün testlerin listesi ve gereksinimleri: [tests/README.md](tests/README.md).

### Geliştirme sunucusu

Uygulama varsayılan olarak `https://makineceviri.org` ile konuşur. Yerel bir sunucuya bağlamak için
`MAKINE_LIVE_SERVER` ortam değişkenini ayarlayın.

### Makine olmadan çalıştırma

Eklentiyi Makine'nin gerçek eklenti süreciyle (`makine-plugin-host.exe`) ama Makine'yi açmadan çalıştırmak için:

```powershell
python tools/plugin_dev.py build/package --host <Makine klasörü>/makine-plugin-host.exe --invoke state --invoke readOnce
```

`--region X,Y,W,H` bölge seçimini yanıtlar; `--invoke login.start` giriş kodunu ister (kod `ui.state` olayında görünür).

## Makine Launcher eklentisi (ABI 2)

Makine Live, Makine'nin içinde gelen (bundled) bir eklentidir: `org.makineceviri.live`, eklenti ABI 2
(`include/makine/plugin/abi.h`, `sdk.hpp`; Makine-Launcher `core/include/makine/plugin/` ile birebir aynı).
Makine onu kendi `makine-plugin-host.exe` sürecinde çalıştırır; ekranını Makine çizer, eklenti yalnızca durum
yayınlar. Python ya da `MakineLive.exe` gerekmez, eklenti hiçbir şey indirip çalıştırmaz.

- `initialize` yalnız ayarları okur ve iş parçacıklarını başlatır (< 1 sn). OCR çalışma dosyaları ve modeller ilk
  `start` / `readOnce`'ta yüklenir. Her şey `dataDir`'e yazılır (`settings.cfg`, `device_id`, `pool/`, `ocr/`);
  `pluginDir` salt okunurdur (MSIX).
- RapidOcrOnnx `char*` yolları bayt bayt genişlettiği için modellere ve görüntüye göreli ASCII adlarla, geniş API ile
  ayarlanan çalışma klasöründen erişilir (`ocr_rapid.cpp` `setPaths`).
- Oturum jetonu `settings.cfg`'de Windows DPAPI ile şifreli durur. Cihaz kimliği eklentiye özgü, `dataDir\device_id`.
- İstekler `pool_client.py` ile aynı başlıkları taşır (`X-Makine-Device`, `X-Makine-Version`, `User-Agent:
  MakineLive/<sürüm>`) ve `X-Makine-Client: launcher/<Makine sürümü>`. Sunucunun güncelleme yanıtındaki indirme
  kullanılmaz (eklentiyi Makine günceller); bakım ve zorunlu güncelleme `ui.state` → `server` ile gösterilir.
- Çöp / HUD süzgeci, normalizasyon ve havuz anahtarı, satır takibi ve altyazı zamanlaması Python koşucusunun birebir
  C++ karşılığıdır (`src/garbage.cpp`, `src/subtitle_timing.cpp`, `src/unitext.cpp`); `ctest` aynı vektörlerle sınar.
- ABI 1 dışa aktarımları (`src/abi1.cpp`) yalnız Python test koşucusu içindir.

### Launcher screen API

Makine'nin Makine Live ekranı `PluginManager.invoke(id, method, params)` ile çağırır. Her yanıt JSON'dur; hata
`{"message"}` ile döner. Uzun işler (giriş, bölge seçimi) hemen döner, sonucu `ui.state` getirir.

| Yöntem | Parametre → yanıt |
|---|---|
| `state` | → tam durum (aşağıda) |
| `login.start` | → `{message}`; kod ve bağlantı `ui.state.login`'de, tarayıcı `ui.openUrl` ile açılır |
| `login.cancel` | giriş beklemesini bırakır |
| `logout` | oturumu kapatır (sunucuda da) |
| `mode.set` | `{mode: "pc" \| "console"}` |
| `console.sources` | `{all?: bool}` → `{sources: [{id, key, label, console, exe, title, pid}]}`; `all` başka büyük pencereleri de listeler |
| `console.select` | `{sourceId}` (`console.sources`'tan); listede olmayan bir pencere seçilirse exe'si kaynak olarak hatırlanır |
| `region.pick` | PC'de ekrandan, konsolda kaynağın 16:9 video alanı içinden (`overlay.pickRegion {within}`) bölge seçtirir |
| `start` / `stop` | okumayı başlatır / durdurur (giriş ve PC'de bölge gerekir) |
| `readOnce` | → `{text, filtered, region, window}`: bölgeyi bir kez okur, çevirmez |
| `autoStart.set` / `usageReport.set` | `{enabled}` → Makine'nin ayar deposuna `settings.set` ile yazılır (manifest ayarları; Makine `autoStart`'ı eklenti kapalıyken de okur) |
| `style.set` | `{preset, font, sizePx, …}` (Makine'nin `overlay.show.style` anahtarları; `null` siler) → `{style}` |
| `output.set` | `{output: "pc" \| "tv" \| "phone"}`: konsol modunda altyazı nerede (Remote Play penceresinin üstü / Makine TV / telefon-tablet sayfası) |
| `obs.set` | `{enabled}` → `{obsUrl}`: OBS tarayıcı kaynağı (`http://127.0.0.1:47712/obs`, yalnız bu bilgisayar) |
| `tv.discover` | ev ağında Makine TV arar (~1,6 sn); sonuç `ui.state.tv.found` |
| `tv.pair` | `{id, code}` (`tv.found`'dan) ya da `{host, port?, code}` (adresle bağlan); konsol modunda çıktıyı `tv` yapar |
| `tv.unpair` | eşleşmeyi siler |
| `tv.install.scan` | `{platform: "android" \| "lg"}`: ev ağında ADB (5555) / LG geliştirici modu (9922) açık cihazlar → `tvInstall.hosts` |
| `tv.install` | `{platform: "android", host, port?}`: Android TV / Google TV / Fire TV'ye ADB ile kurar; ilerleme `tvInstall.step`, sonuç `tvInstall.state` (`running` · `done` · `error`) ve `message`. `lg` → `E_UNSUPPORTED` (elle kurulum: `tvInstall.guide.lg`) |
| `tv.install.saveFile` | `{platform: "android" \| "lg"}` → `{path}`: `makine-tv.apk` / `.ipk` İndirilenler klasörüne (USB bellek ya da elle kurulum) |
| `announcement.read` | `{id}`: duyuruyu kapatır (sunucuya `ann_read`) |
| `support.dismiss` | "kayıt gönderildi" kartını kapatır |
| `donate` / `donate.dismiss` | bağış sayfasını (`makineceviri.org/destek`) `ui.openUrl` ile açar / ilk açılış kartını kapatır |

`ui.state` (her değişiklikte `emitEvent`):

```json
{
  "login": {"state": "signedOut|checking|starting|waiting|signedIn|offline", "code", "url", "user", "message"},
  "mode": "pc|console",
  "sources": [ ... ], "selectedSource": {"key", "pid"} | null,
  "region": {"pc": {"x", "y", "width", "height"} | null, "console": {"x", "y", "width", "height"} (video alanına oran)},
  "picking": false, "running": false, "status": "…",
  "lastLine": {"source", "text", "kind", "origin"} | null,
  "counters": {"lines", "poolHits"},
  "server": {"maintenance", "message"}, "notice": "…",
  "style": { ... }, "game": {"name", "slug"}
  "autoStart": false, "usageReport": true,
  "output": "pc|tv|phone", "obs": false,
  "web": {"running", "lan", "port", "clients", "obsUrl", "phoneUrl", "error", "firewallNote"},
  "tv": {"state": "idle|connecting|connected|bad_code|offline", "detail", "paired": {...} | null, "found": [{"id", "name", "host", "port"}], "searching"},
  "tvInstall": {"platform", "state", "step", "message", "hosts": [...], "scanning", "apkAvailable", "ipkAvailable",
                "guide": {"android": [...], "firetv": [...], "lg": [...]}},
  "discord": {"connected"},
  "announcements": [{"id", "kind", "kindLabel", "title", "body", "author", "createdAt"}],
  "support": {"time", "sizeKb"} | null,
  "donate": {"url", "seen"}
}
```

Çeviri `overlay.show` ile gider: `text` (eski Makine'ler için), `lines: [{text, sourceText?}]`, bölge (`x, y, width,
height`; konsolda video alanındaki bölgenin ekran karşılığı) ve kayıtlı `style`. PC oyununda oyun Makine'nin
`game.started` / `game.stopped` olaylarından ve `games.running`'den gelir; konsol modu her zaman GTA VI havuzudur.
PC modunda okuma yalnızca oyun öndeyken yapılır (test koşucusunun hedef kilidi): Makine `game.started` /
`games.running` içinde isteğe bağlı `pid` gönderirse öndeki pencerenin süreci o olmalı; göndermezse öndeki pencere
Makine olmamalı ve bölgenin ortasını kapsamalı. Ekrandan okunan ve gösterdiğimiz çeviriye benzeyen OCR kutuları
cümle birleştirilmeden önce atılır; yine de Makine'nin kaplama penceresinin `SetWindowDisplayAffinity(
WDA_EXCLUDEFROMCAPTURE)` ile yakalamadan gizlenmesi gerekir. Yerel havuz, oyun Makine'nin kütüphanesinden
geldiği için sormadan indirilir.

**Çıktılar.** Çeviri, PC modunda ve konsol modunda `output: "pc"` iken Makine'nin kaplamasıyla gösterilir; `tv` iken
Makine TV'ye (UDP 47710 keşif, TCP satır-JSON, TV'deki 4 haneli kod; `src/tv_link.cpp`), `phone` iken telefon / tablet
sayfasına (`http://<PC>:47712`, SSE) gider. OBS kaynağı (`/obs`) her modda açılabilir. Sayfalar Python koşucusunun
sayfalarıyla aynıdır (`tools/gen_web_pages.py` → `src/web_pages.inc`), dışarıdan bir şey yüklemez (CSP). Sunucu
telefon açık değilken yalnız 127.0.0.1'i dinler; telefon açılınca ev ağını dinler ve Windows bir kez güvenlik duvarı
izni sorar (`web.firewallNote`). Eklenti güvenlik duvarına kural eklemez.

**Discord** (`src/discord.cpp`): okuma açıkken profilde "… · Türkçe oynuyor", oturum süresi ve çevrilen diyalog;
GTA VI'da Makine × GTA VI görseli. Discord'un yerel IPC kanalı kullanılır, ağa çıkılmaz. Makine'de her zaman açık.

**TV'ye kur.** Android TV, Google TV ve Fire TV: eklenti ADB protokolünü kendisi konuşur (`src/adb.cpp`; adb.exe yok):
RSA-2048 anahtarı Windows CNG ile üretilir, DPAPI ile `dataDir\adbkey.bin`'de durur; TV bir kez "bu bilgisayara izin
verilsin mi" diye sorar. APK `sync:` ile yüklenir, `pm install` ile kurulur, altyazı katmanı izni verilir, uygulama
açılır. LG webOS: SSH istemcisi gerektirdiği için şimdilik elle kurulum (`tvInstall.guide.lg`, `tv.install.saveFile`).
Paketler: `resources/makine-tv.apk` (`tv\gradlew.bat assembleRelease`; imzalı sürüm için `MAKINE_TV_KEYSTORE`) ve
`resources/makine-tv.ipk` (`tools/build_webos.py`, derlemede kendiliğinden).

**Duyurular, destek, bağış.** Sunucunun duyuruları `announcements`'ta; ekip bir oturumun kaydını isterse
(`beat` → `log_request`) eklentinin kendi kayıt satırları (bellekte son 512 KB) gzip ile gider ve `support` kartı çıkar.
Bağış bağlantısı `https://makineceviri.org/destek` (site bağış sayfasına yönlendirir), `ui.openUrl` ile açılır.

**Konsol / yakalama kaynakları** (öncelik: GTA VI, PlayStation ve Xbox): PS Remote Play, chiaki / chiaki-ng, Asobi,
PXPlay (PS5); Xbox uygulaması (bulut ve uzaktan oynatma dahil), Greenlight (Xbox); Elgato 4K Capture Utility, Elgato
Studio, AVerMedia RECentral, AVerMedia Streaming Center, OBS projektörü, Windows Kamera (yakalama kartı). Hepsi aynı
16:9 bölge mantığıyla okunur.

**Ayarlar** (manifest `contributes.settings`, Makine saklar): `autoStart` (oyun başlayınca kendiliğinden aç; giriş
doğrulanırken gelen `game.started` bekletilir) ve `usageReport` (varsayılan açık). Kullanım bildirimi 0.3.2'deki
`net/telemetry.py` ile aynıdır: girişten 2 sn sonra ve sunucunun söylediği `beat_s` aralığında `POST /api/live/app?action=beat`
(oyun, mod, okuma süresi, çevrilen satır, sistem ve hız bilgisi; ekran görüntüsü ya da okunan metin gitmez), çıkışta ve
kapanışta `bye`. `usageReport` kapalıysa hiç gönderilmez. Yanıttaki `log_request` yanıtlanmaz: eklentinin kendi günlük
dosyası yoktur, satırları Makine'nin eklenti günlüğündedir. Konsol görüntüsünü getiren uygulamalar (PS Remote Play, Asobi,
chiaki, Xbox uygulaması, Greenlight) PC modunda da GTA VI sayılır.

## Proje yapısı

```
├── src/, include/        C++ çekirdeği (makine-live.dll) ve Launcher ile ortak C ABI başlıkları
├── makine_live/          Makine Live masaüstü uygulaması (py -m makine_live; test koşucusu)
│   ├── ui/               kontrol paneli, oyun içi çeviri kutusu, giriş ekranı
│   ├── core/             DLL sarmalayıcısı, satır takibi ve zamanlayıcı, OCR süzgeçleri
│   ├── games/            oyun kütüphanesi ve algılama, konsol (Remote Play) penceresi
│   ├── net/              sunucu istemcisi, yerel havuz, güncelleme, Discord
│   └── tv/               Makine TV bağlantısı ve kurulumu, telefon / OBS sayfası
├── tv/, tv-webos/        Makine TV: Android TV / Google TV ve LG webOS
├── assets/               logo ve uygulama ikonu
├── spec/                 istemci ve sunucunun paylaştığı test vektörleri, tasarım dili
├── resources/            kelime sıklık listesi, eklenti ikonu
├── tools/                exe ve eklenti paketleme, ikon üretimi, LG paketleyici, TV emülatör testi
├── tests/                bütün testler
├── docs/                 teknik mimari, kullanıcı rehberleri, sürüm notları
├── build.cmd             tek adımda sürüm: DLL → testler → exe → selftest (tools/build.py)
└── test.cmd              sunucusuz deneme: yerel sahte sunucuyla uygulamayı açar (tools/offline.py)
```

## Gizlilik ve güvenlik

- Ekran görüntüsü ve OCR bilgisayarda kalır; sunucuya yalnızca okunan metin satırı ve oyunun adı gider.
- Oturum jetonu ve (geliştirici motorları için) API anahtarı Windows DPAPI ile şifrelenerek saklanır.
- Depoda hiçbir anahtar, şifre ya da jeton yoktur. Açık bildirmek için: [SECURITY.md](SECURITY.md).

## Katkıda bulunma

Hata bildirimi ve öneriler için [Issues](https://github.com/MakineCeviri/Makine-LauncherPlugin-Live/issues) kullanın.
Kod katkısında:

1. Değişikliği küçük ve konuya göre ayrı tutun (`fix(capture): …`, `feat(overlay): …`).
2. Python'da denenen her yetenek sonunda C++ çekirdeğine taşınır; Python'a kalıcı mimari kurmayın.
3. İlgili testleri çalıştırın; mimari değiştiyse [docs/MIMARI.md](docs/MIMARI.md)'yi aynı değişiklikte güncelleyin.
4. Normalizasyon ya da çöp kurallarını değiştirirseniz `spec/` vektörlerini de güncelleyin.

## Lisans

[AGPL-3.0](LICENSE) + Commons Clause: kaynak kodu açıktır, değiştirilip paylaşılabilir; yazılımın kendisi ücretli olarak
satılamaz. Üçüncü taraf bileşenler: `resources/en_words.txt` ([wordninja](https://github.com/keredson/wordninja), MIT),
çalışma zamanında [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx) ve [ONNX Runtime](https://onnxruntime.ai) (depoda değil).

<sub>Makine Live, [Makine Çeviri](https://makineceviri.org) topluluğu tarafından geliştirilir.</sub>
