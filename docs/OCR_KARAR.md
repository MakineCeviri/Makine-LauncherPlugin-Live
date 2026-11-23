# Makine Live — OCR Mimarisi Kararı

> **Karar veren:** Fable (ürün ve mimari yönü), 2026-11-24. Konu: [`OCR_MIMARI.md`](OCR_MIMARI.md) önerisi.
> Bu belge öneriyi **bağlar**: çelişen yerde bu belge geçerlidir. Para, imza sertifikası ve yayın kararları Sezer'dedir (§8).
> Rakamların hepsi hâlâ **(V)**: ölçüm bir kararı çürütürse karar bu dosyada, tarihiyle değiştirilir.

## 1. Özet

Ölçüt Sezer'in cümlesi: *"mevcut yapı çalışıyordu; geriye iyileştirmek ve kusursuzlaştırmak kalıyor."* Öneri mühendislik
olarak sağlam ama **yanlış ürünü önceliklendiriyor**: asıl kullanım PS5 / Xbox'ta GTA VI (PS Remote Play, chiaki, Xbox
uygulaması, yakalama kartı → `PrintWindow` yolu, altyazı TV'de), öneri ise PC'de DXGI ile oynanan oyunu merkeze alıyor
ve konsola özgü her şeyi (K18, D-C, HDR) F3'e, "aday" olarak bırakıyor. Karar bu sırayı çevirir.

Beş temel karar:

1. **Kusurun en küçük doğru düzeltmesi geometridir, model ya da dedektörsüz yol değil.** F1 = aynı modeller, aynı
   parametreler, yalnız kutular ve satır sırası bizde (K1 + K4a). Başka hiçbir şey F1'e girmez.
2. **Dedektörsüz bant yolu (K4) birincil yol değildir.** Hız kazancının büyüğü OCR'ı *çalıştırmamaktan* gelir (K3),
   dedektörü atlamaktan değil. K4 ölçümle kendini kanıtlayana kadar adaydır.
3. **Konsol yolu birinci sınıftır.** Sıkıştırılmış akış seti, gerçek Remote Play / yakalama kartı kareleri ve kaynak
   başına yakalama tablosu F0'dadır. Sentetik kare yapımı açar, **gerçek kare yayını açar**.
4. **Gecikme TV'de ölçülür.** Kapılar Launcher kutusunda bitmez; TV sıçraması ve akış gecikmesi ayrı kapıdır (§5).
5. **Havuz üründür.** Havuzu kirletebilecek her hızlandırma (tek okumayla kararlılık, model değişimi, önek) sıkıştırılmış
   sette kanıt ister; kanıt yoksa bugünkü temkinli davranış kalır.

## 2. Kusur (A10) için neden K1 — ve neden başka yol değil

| Seçenek | Karar | Neden |
|---|---|---|
| RapidOcrOnnx çıktısını sonradan sıralamak | Ret | C API kutu vermiyor; sıra metinden çıkarılamaz. |
| RapidOcrOnnx'i kutu döndürecek biçimde yamalayıp kendimiz derlemek | Ret | Sırayı düzeltir ama bölünmüş kelimeyi (`"Press\ns X"`) düzeltmez: onun için kutuları birleştirip **yalnız rec** çağırmak gerekir, yani API'sini yine yeniden yazarız. Üstüne kendi derlediğimiz, imzasız, içinde OpenCV olan bir DLL ve bir çatalın bakımı kalır; BMP dosya yolu da durur. |
| Kendi ORT hattı, **aynı modellerle** (K1) | **Onay** | Geometri, kırpma ve rec çağrısı bizde olur; BMP ve imzasız DLL kalkar. Bedeli: OpenCV'nin yaptığı işi (DB son işlemi, yeniden ölçekleme) kendimiz yazarız. |

K1'in bedelini küçülten karar: altyazı yataydır, bu yüzden DB son işleminde **eksen hizalı kutu** yeter (eşik →
bağlı bileşen → sınır kutusu → genişletme). Döndürülmüş dikdörtgen, çokgen, perspektif düzeltme yazılmaz.

## 3. Kararlar

Onay = öneride yazıldığı gibi. Değişiklik = aşağıdaki biçimiyle.

| # | Karar | Gerekçe / değişen |
|---|---|---|
| **G1** | **Onay** | Satır sırasına hiçbir kara kutu karar vermez. Ürünün ilk kuralı. |
| **K1** | **Onay, değişiklikle** | F1 **birebir** geçiştir: aynı `ch_PP-OCRv3` det + rec, aynı `maxSideLen` / dolgu; değişen yalnız geometri ve bellekten girdi. RapidOcrOnnx kıyas düzeninde referans olarak kalır, **paketten ancak kapılar geçtiği PR'da çıkar**. Ek kapılar: §5 G-ORT. |
| **K2** | **Değişiklik → F3, yalnız PC modu** | DXGI kirli dikdörtgeni konsol yoluna (`PrintWindow`) hiç dokunmuyor. Asıl üründe kazancı sıfır; PC modunda iyileştirme olarak sonra. |
| **K3** | **Onay, değişiklikle → F2'nin ilk işi** | Remote Play akışında her kare farklıdır (A1 konsolda *her zaman* doğru); OCR'ı durduran tek şey maskedir. Koşullar: eşikler **göreli** (bandın parlaklık yüzdeliğine göre; mutlak "parlak" eşiği soluk HDR önizlemesinde ve gece sahnesinde çöker); G-MISS = 0 **D-C'de** kanıtlanır; maske yalnız "atla" der, kuşkuda OCR çalışır. |
| **K4** | **Değişiklik → aday, F3; birincil yol değil** | Risk GTA tarzı altyazıda yüksek: parlak açık dünya zemini (gökyüzü, kum, neon) parlak dolgu maskesini doldurur; satır içi **renkli kelime** (GTA V görev metninde hedef adları renkliydi; GTA VI gerçek karede doğrulanacak) ve **düğme simgesi** izdüşümü böler. Kazancı da K3'ten sonra küçüktür: OCR saniyede bir kereden az çalışıyorsa dedektörün 40 ms'si E2E'ye girer ama CPU'ya girmez. B2'de gerçek karelerde G-ORDER/G-SPLIT = 0 ve CER farkı ≤ 0,3 görülürse girer. "Öğrenilmiş bant" K4 ile birlikte bekler. |
| **K4a** | **Onay, F1** | Asıl düzeltme budur. Eşikler satır yüksekliğine göreli kalır. Birleştirilen kırpıntı tek rec çağrısına gider. İki satırlı altyazı yukarıdan aşağı, tek boşlukla birleşir. |
| **K5** | **Onay** | cls yüklenmez, pakete girmez. |
| **K6** | **Onay, değişiklikle** | F1'de model değişmez. Geçiş ancak §4'teki kanıtla. `latin` v5 ve INT8 **girmiyor** (rec'in hedefi satır başına ~8 ms (V); B0'da S4 darboğaz çıkarsa yeniden açılır). |
| **K7** | **Onay, yalnız (a)** | RGB olduğu gibi. (b) ve (c) ancak D-C'de CER düşürürse. Keskinleştirme / ikilileştirme yok. |
| **K8** | **Onay, değişiklikle** | İş parçacığı politikası **moda göre**: PC modunda 1 iş parçacığı, spin kapalı, `BELOW_NORMAL` (oyun bu makinede). Konsol modunda oyun bu makinede değil; 2 iş parçacığı, normal öncelik, sınır "akış çözücüsü takılmasın". EcoQoS yalnız PC modunda aday. Isınma ve spin kapalı her modda. |
| **K9** | **Onay, F3** | GTA altyazısı satırı bütün gösterir, daktilo değil (V: GTA V böyleydi; GTA VI gerçek kare setinde doğrulanır, daktilo çıkarsa K9 F2'ye çıkar). |
| **K10** | **Bu turda girmiyor** | Gösterim tarafı dahil. GTA VI'nın konuşmacı adı gösterip göstermediği gerçek karede görülmeden tasarım yok; anahtar değişikliği ayrıca iki depolu karar. |
| **K11** | **Değişiklik** | Tek okumayla gönderme yalnız şu üçü birlikteyse: maske 2 uyanışta sabit **ve** rec güveni eşiğin üstünde **ve** D-C'de G-HALF ile G-KEY tabandan kötü değil. Kanıtlanana kadar konsol yolunda **iki okuma kuralı kalır**; sıkıştırma titremesinde havuzu koruyan odur. |
| **K12** | **Tam anahtar: onay (F2). Önek ön-yükleme: ret.** | Onaylı satırda tam anahtar isabeti risksiz ve en büyük E2E kazancı. Önek bilerek kaldırılmıştı; yalnız daktiloda işe yarar (K9'daki varsayımla aynı). |
| **K13** | **Onay** | Havuz eşitlemesi okuma başlamadan bitmiş olur. HTTP/2 aday, HTTP/3 girmiyor. Ek: TV ve telefon soketlerinde `TCP_NODELAY` (§5). |
| **K14** | **Ret (ürün yolu)** | Türkçe Windows'ta `en-US` OCR bileşeni büyük olasılıkla kurulu değil (V); kurdurmak oyuncuya iş, satır geometrisi de bizde olmaz (G1). Kıyas düzeni ucuzsa tavan rakamı olarak raporlar, eklentide WinRT işi yapılmaz. |
| **K15** | **Ret** | "Varsayılan kapalı" da değil: GPU oyunun, paket büyür, antivirüs yüzeyi artar. Kapandı. |
| **K16** | **Değişiklik: ikiye ayrılır** | (a) PC masaüstü HDR (DXGI): F3, önce ölçüm. (b) **Konsol HDR: F0'da soru.** PS5 HDR açıkken Remote Play / chiaki penceresi ve yakalama kartı önizlemesi `PrintWindow`'da ne veriyor (doğru SDR mi, soluk gri mi)? Varsayım yapılmaz, kaynak tablosunda ölçülür (§6). |
| **K17** | **Onay, F3, yalnız PC modu** | |
| **K18** | **Değişiklik** | `PrintWindow` çalışıyor: birincil kalır. WGC ancak kaynak tablosunda (§6) bir ana kaynak başarısızsa ya da maliyet ölçülürse yazılır. **Ölçümün kendisi F0'a çıkar.** |
| **K19** | **Onay, ekle** | Konsol durumları ve neden kodları §6. Not: Launcher dalında kaplama zaten yakalamadan gizli (`capturehide.h`, `PluginManager`); kalan iş "yayında göster" takasının sınanması. |
| **K20** | **Onay, ekle** | Ayrıca girmeyenler: çalışma anında model indirme, INT8, DirectML, Windows.Media.Ocr, önek eşleşmesi. |

## 4. İngilizce model: kıyas neyi kanıtlamak zorunda

Havuz anahtarı yalnız harf ve rakamdır (MIMARI §4.3): **boşluğu daha iyi okumak anahtarı değiştirmez.** Yani
G-SPACE ve temiz setteki CER geçiş gerekçesi olamaz. `en` modeli ancak şunların **hepsinde** `ch` v3'ü geçerse ya da
eşitlerse varsayılan olur:

1. **G-KEY, gerçek havuza karşı:** GTA VI havuzunun onaylı kaynak satırları karelere çizilir (D-A) ve sıkıştırılır
   (D-C); okunan metnin anahtarı havuzdaki anahtarla tam eşleşme oranı `ch` v3'ten düşük olamaz. Bu, bugünkü havuz
   isabetini kaybetmediğimizin kanıtıdır.
2. **CER, D-C ve gerçek karelerde** (720p ve 1080p akış, düşük bit hızı dahil) `ch` v3'ten düşük.
3. **Rakam ve kesme işareti:** rakamı farklı satırlar hiç eşleşmediği için rakam hatası ayrı sayılır; argo ve kısaltma
   (`ain't`, `y'all`, `$`, saat ve para) ayrı alt küme.
4. **Anlamsız çıktı = 0:** `en` v3'ün stilize satırlarda yaptığı hata (MIMARI §3.3) tekrarlanmaz.
5. Kazanırsa `SpacingFixer` ve `dropForeignScriptTokens` silinmez, güvenlik ağı olarak kalır.

Eşitlikte `ch` v3 kalır: çalışan yapı değişmez.

## 5. Kapılar

Öneri §6'daki kapılar şu değişikliklerle onaylıdır:

- **G-ORDER = 0, G-SPLIT = 0:** onay. İki aşamalı: sentetik D-R geçince F1 birleşebilir; **yayın için gerçek kare seti**
  (§6) de sıfır olmalı. D-R'ye eklenir: satır içi düğme simgesi (`Press [X] to …`), satır ortasında renkli kelime,
  iki satırda kısa alt satır, tırnak ve üç nokta ile başlayan satır, satır sonu tek kelime.
- **G-KEY:** §4.1'deki gibi gerçek havuza karşı ölçülür.
- **G-MISS = 0, G-HALF ≤ taban:** D-C'de de.
- **G-FT yalnız PC modu.** Konsol modunda yerine **G-STREAM:** Live çalışırken akış uygulamasının kare düşürmesi
  artmaz; host CPU'su ort. ≤ %10 tek çekirdek (V).
- **G-ORT (yeni):** (1) `onnxruntime.dll` Microsoft imzalı resmî sürüm, içe aktarmalarında paketimizde olmayan DLL yok
  (VC çalışma zamanı dahil; varsa birlikte gelir); (2) tam yolla yüklenir ve yüklenen sürüm doğrulanır: Windows'un
  System32'sinde eski bir `onnxruntime.dll` bulunabilir, yanlışı yüklenirse eklenti açık bir hatayla durur;
  (3) dosya `pluginDir`'den okunur, hiçbir şey çıkarılıp yazılmaz.
- **G-AV:** yalnız Defender taraması yetmez. Paketteki her ikili çok motorlu taramadan temiz geçer **ve** Defender
  gerçek zamanlı koruması açıkken tam bir oturum (okuma + TV arama + TV'ye kur) uyarısız biter.
- **G-E2E-L / -S / -D:** rakamlar (V) olarak kalır, B0'dan sonra kesinleşir.
- **TV kapıları (yeni).** Öneride `t1` Launcher kutusunda bitiyor; asıl üründe kutu yok.

| Kapı | Ölçülen | Kural (V) |
|---|---|---|
| G-TV-HOP | satırın TV'ye gönderildiği an → Makine TV'nin çizdiği kare (Android TV ve LG ayrı) | p50 ≤ 40 ms, p95 ≤ 100 ms; Wi-Fi ve kablolu ayrı |
| G-E2E-TV | İngilizce satır PC'deki akış penceresinde belirdi → Türkçe TV'de (yerel havuz isabeti) | p50 ≤ 300 ms, p95 ≤ 500 ms |
| G-E2E-GERÇEK | **Oyuncunun gördüğü:** İngilizce satır TV'de (HDMI) belirdi → Türkçe TV'de. TV'yi 240 kare/sn çeken telefon kamerasıyla, kaynak başına (Remote Play, chiaki, yakalama kartı) | Raporlanır; hedef yerel isabette p50 ≤ 500 ms |

G-E2E-GERÇEK'in içinde bizim olmayan bir kalem var: akışın PC'ye gecikmesi (TV görüntüyü HDMI'dan, PC akıştan alır).
Onu kısaltamayız; bu yüzden kendi payımız (K12, K13, TV sıçraması) önemlidir ve yakalama kartı yolu Remote Play'den
hızlı çıkarsa rehberde öyle söylenir. `tv_link` ve SSE soketlerinde `TCP_NODELAY` denetlenir (Nagle + gecikmeli ACK
tek başına ~40 ms ekleyebilir).

**G-TV-HOP nasıl ölçülür:** PC ve TV saatleri ayrı olduğu için tek yönlü zaman damgası kullanılmaz. Makine TV satırı
çizdiği karede `drawn{id}` yanıtı yollar; PC gidiş-dönüşü ölçer ve yarısını alır (yalnız ölçüm derlemesinde). Doğrulama:
PC kutusu ve TV aynı kadrajda, 240 kare/sn kamera.

## 6. Konsol / Remote Play için eksikler (F0'a girer)

**Kaynak tablosu (B-WIN, öne alındı).** Her kaynak için: `PrintWindow` çalışıyor mu, tur süresi, siyah kare, PS5 HDR
açıkken görüntü, pencere küçükken / üstü kapalıyken / başka monitördeyken, akış 540p-720p-1080p. Kaynaklar: PS Remote
Play, chiaki-ng, Xbox uygulaması, Greenlight, bir yakalama kartı programı (Elgato ya da OBS projektörü). Bu tablo
olmadan K16 ve K18 hakkında karar verilmez.

**Veri setine eklenecekler.**
- **Gerçek kareler (D-G):** GTA VI, PS Remote Play 1080p + 720p ve bir yakalama kartı; en az 300 altyazı satırı, elle
  etiketli. Yayın kapısı bu settir. MIMARI §5 açık yazıyor: gerçek PS5 / Xbox / Android TV ile hiç denenmedi.
- **D-C zorlayıcıları:** 4:2:0 renk alt örneklemesi (renkli yazı en çok bundan bozulur), hızlı sahnede bit hızı çöküşü
  (araba sürerken diyalog: GTA'nın en sık durumu), donmuş / yinelenen kare, yeniden ölçeklenmiş pencere.
- **Akış uygulamasının kendi arayüzü:** fare gelince çıkan Remote Play çubuğu, bağlantı uyarısı, konsol bildirimi.

**Durumlar ve neden kodları.** Motor bugün serbest metin (`status`, `notice`) yayıyor. Her bekleme / hata durumu sabit
bir `reason` kodu taşır; ekran tek cümle ve tek düzeltme düğmesi gösterir, aynı cümle TV'ye de gider:

| Kod | Algı | Oyuncuya |
|---|---|---|
| `source_missing` / `source_minimized` / `source_small` | pencere yok, küçültülmüş, 960×540 altı | "Konsol görüntüsü görünmüyor" + ne yapılacağı |
| `black_frame` | bant varyansı ≈ 0, süren | "Görüntü siyah geliyor" (yakalama kartında: PS5'te HDCP'yi kapat) |
| `stream_stalled` | aynı kare N sn, ardından altyazı yok | "Bağlantı dondu; görüntü gelince devam eder" |
| `stream_poor` | rec güveni düşük + çöp oranı yüksek, süren | "Görüntü kalitesi düşük, altyazı okunamıyor" (çöp gönderilmez) |
| `hdr_washed` | bant kontrastı düşük, parlaklık sıkışık | "Görüntü soluk geliyor (HDR)" + ayar yolu |
| `tv_offline` / `tv_bad_code` | bağlantı durumu | "TV'ye ulaşılamıyor" / "Kod eşleşmedi" |
| `quota` / `maintenance` / `offline` | sunucu yanıtı | "Yeni çeviri sınırı doldu; havuzdaki çeviriler sürüyor" vb. |

## 7. Yapım sırası

| Sıra | İş | Geçme |
|---|---|---|
| **F0** | B0 tabanı (bugünkü hat birebir) · D-R genişletilmiş · D-C · kaynak tablosu · gerçek kare çekimi başlar | Taban JSON'u; RapidOcrOnnx'in D-R'deki hata sayısı raporlu |
| **F1** | Kendi ORT hattı, **aynı modeller** + K4a + K5 + K8 (moda göre) | D-R'de G-ORDER = 0 ve G-SPLIT = 0 · aynı kırpıntıda rec metni RapidOcrOnnx ile birebir · **tam karede CER ve G-KEY (D-A + D-C) B0'dan kötü değil** (kutularımız ve yeniden ölçeklememiz OpenCV'ninkinden farklı kırpıntı üretir; kırpıntı eşliği bunu göremez) · S4 ≤ taban · G-IO = 0 · G-ORT · `ctest` vektörleri aynen |
| **F2** | K3 maskesi (konsol yolunda OCR kapısı) → K12 tam anahtar + K13 + TV kapıları → K11 (kanıtla) | G-RATE · G-MISS = 0 (D-C) · G-E2E-TV · G-WRONG = 0 |
| **F2b** | B1: model kararı (§4) | §4'ün beş maddesi |
| **F3** | K4 (aday), K2 / K17 (PC modu), K16a, K9 | ilgili kapılar |
| **Yayın** | Gerçek kare setinde G-ORDER / G-SPLIT = 0, G-E2E-GERÇEK raporu, G-AV | — |

## 8. Sezer'in masasında kalanlar

- **Kod imzası:** MSIX'i Store imzalar; ZIP dağıtımındaki ikililer imzasız. "Sıfır antivirüs alarmı" için kalıcı çözüm
  Authenticode sertifikasıdır (para).
- **Gerçek cihaz çekimi:** PS5 + Remote Play + TV ile D-G kareleri ve G-E2E-GERÇEK ölçümü; ekipten bir kişinin bir akşamı.
