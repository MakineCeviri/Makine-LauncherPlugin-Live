# GTA VI'yı konsolda Türkçe oynamak (PS5 · Xbox)

Makine Live, konsolda oynadığınız **Grand Theft Auto VI**'nın altyazısını Türkçeye çevirir ve **TV'nizde, oyunun
üstünde** gösterir. Konsol yine HDMI ile TV'ye bağlıdır, siz her zamanki gibi oynarsınız. Bilgisayar yalnızca altyazıyı okur.

Konsol modu yalnızca GTA VI içindir: bütün oyuncuların çevirileri aynı havuzda toplanır, onaylanan satırlar herkese anında gelir.
Monitörde oynuyorsanız ya da TV'nize Makine TV kurulamıyorsa aşağıdaki **"Monitörde oynayanlar"** bölümüne bakın.

```
PS5 / Xbox ──HDMI──▶ TV  ◀── Türkçe altyazı (Makine TV)
     │                          ▲
     └─ Remote Play ──▶ Bilgisayar (Makine Live) ──ev ağı──┘
```

## Gerekenler

- Windows bilgisayar (Makine Live açık kalır)
- Makine Live ([makineceviri.org/live](https://makineceviri.org/live/)) ve bir Makine Çeviri hesabı (Discord ya da e-posta)
- İçinde **Android TV / Google TV** olan bir TV (Vestel, Philips, Sony, TCL, Xiaomi…)
  ya da **LG TV** (webOS; geliştirici modu ile, aşağıda "LG TV'ler").
  Chromecast, Mi Box gibi ayrı bir TV kutusu altyazıyı yalnızca kendi görüntüsünün üstüne yazar; konsol TV'nin başka
  bir HDMI girişindeyse altyazı görünmez. Bu durumda **Telefonda** ya da **Bu ekranda**'yı kullanın.
  TV'niz bunlardan biri değilse ya da monitörde oynuyorsanız: altyazı **bilgisayar ekranında** ya da **telefonda** görünebilir.
- Bilgisayar, konsol ve TV **aynı ağda** (aynı modem)
- Görüntüyü bilgisayara getirmek için biri:
  - **PS5:** [PS Remote Play](https://www.playstation.com/remote-play/) uygulaması
  - **Xbox:** Xbox uygulaması (uzaktan oynatma)
  - ya da bir **yakalama kartı** (Elgato vb.)

## 1. Konsolu bilgisayara bağlayın

**PS5**
1. Ücretsiz ikinci bir PSN hesabı açın ve PS5'e kullanıcı olarak ekleyin.
2. PS5'te *Ayarlar › Sistem › Uzaktan Oynatma › Uzaktan Oynatmayı Etkinleştir*.
3. Bilgisayarda PS Remote Play'e **ikinci hesapla** girip PS5'e bağlanın.

> Neden ikinci hesap? Kendi hesabınızla bağlanırsanız kolunuz konsoldan kopar ve oyunu bilgisayardan oynamanız gerekir.
> İkinci hesapla siz TV'de oynamaya devam edersiniz, bilgisayar yalnızca izler.

**Xbox**
1. Xbox'ta *Ayarlar › Cihazlar ve bağlantılar › Uzak özellikler › Uzak özellikleri etkinleştir*.
2. Bilgisayarda Xbox uygulamasından konsolunuza **uzaktan oynatma** ile bağlanın.

**Yakalama kartı**
Kartın kendi uygulamasında (ör. Elgato 4K Capture Utility) önizlemeyi açın. OBS kullanıyorsanız
*Pencereli Projektör (Önizleme)* açın. PS5'te *Ayarlar › Sistem › HDMI › HDCP'yi etkinleştir* kapalı olmalıdır.

Remote Play / önizleme penceresi **açık kalmalı** (küçültmeyin), en az 960×540 olmalı. Üstünde başka pencere olması sorun değil.

## 2. Makine TV'yi TV'ye kurun (bir kez)

Makine Live'da **Konsol** sekmesi › TV satırındaki **Kur**:

1. TV'de *Ayarlar › Cihaz tercihleri › Hakkında › Yapı* satırına 7 kez basın (geliştirici seçenekleri açılır).
2. *Ayarlar › Cihaz tercihleri › Geliştirici seçenekleri*'nde **USB hata ayıklama**'yı (varsa **Ağ üzerinden hata ayıklama**'yı da) açın.
3. Makine Live TV'nin adresini kendisi bulur. **Kur**'a basın. TV'de izin sorusu çıkarsa **İzin ver**'i seçin.

Kurulum altyazının oyunun üstünde görünmesi için gereken izni de kendisi verir. Bittikten sonra hata ayıklamayı
kapatabilirsiniz.

*Elle kurmak isterseniz:* **APK'yı kaydet** ile dosyayı USB belleğe alın, TV'nin dosya yöneticisiyle kurun. Sonra
*Ayarlar › Uygulamalar › Özel uygulama erişimi › Diğer uygulamaların üzerinde göster* › Makine TV › izin verin.

## 3. Eşleştirin ve oynayın

1. TV'de **Makine TV**'yi açın. Ekranda 4 haneli bir **eşleşme kodu** ve TV'nin adresi görünür.
2. Makine Live › **Konsol** sekmesi › **Konsolda · PS5 / Xbox** › Altyazı nerede görünsün: **TV'de** › **TV bul** ›
   TV'yi seçin › kodu girin.
3. GTA VI'da altyazıları açın. Makine Live'da **Başlat**'a (F9) basın.
4. TV'yi konsolun girişine alın. Altyazının Türkçesi oyunun üstünde belirir.

Makine TV'de **Geri** tuşu uygulamayı kapatmaz; altyazılar arka planda gelmeye devam eder. TV açılınca Makine TV
kendiliğinden başlar (ayarlardan kapatılabilir). Yazı boyutu, konum ve arka plan TV'deki ayarlardan değişir.

## Monitörde oynayanlar

Makine TV bir TV uygulamasıdır; monitörlerde uygulama çalışmaz. Monitörde (ya da Makine TV kurulamayan bir TV'de)
oynuyorsanız Makine Live › **Konsol** › **Altyazı nerede görünsün?** satırından birini seçin:

**Bu ekranda (oyunu bilgisayarda oynayın)**
Konsolun görüntüsü bilgisayarda açılır, altyazı onun üstünde çıkar. Monitör bilgisayara bağlıdır.
- **Yakalama kartıyla (en az gecikme):** Konsol → yakalama kartı → bilgisayar. Kartın uygulamasında (ya da OBS'de
  *Pencereli Projektör*) önizlemeyi **tam ekran** açıp oyunu oradan oynayın. Kolunuz konsola bağlı kalır.
- **PS Remote Play / Xbox uygulamasıyla (kart gerekmez):** Bu kez **kendi hesabınızla** bağlanın ve oyunu Remote Play
  penceresinden **tam ekran** oynayın (kol bilgisayardan da kullanılabilir). Ev ağına göre biraz gecikme olabilir;
  kablolu bağlantı önerilir.

Altyazı Remote Play / önizleme penceresinin görüntüsüne göre yerleşir; pencere tam ekran değilse de görüntünün altında durur.

**Telefonda (konsol monitöre bağlıyken)**
Konsol doğrudan monitöre bağlı kalır, bilgisayar Remote Play ya da yakalama kartı penceresinden altyazıyı okur, Türkçesi
**telefonun ya da tabletin tarayıcısında** görünür. Uygulama kurmak gerekmez.
1. **Telefonda**'yı seçin. Konsol satırının altındaki kartta bir adres yazar (ör. `192.168.1.20:47712`).
2. Telefon bilgisayarla aynı ağdayken tarayıcıya bu adresi yazın. Son satır büyük, öncekiler küçük görünür.
3. Telefonu monitörün altına koyun. **A− / A+** yazı boyutunu, **Tam ekran** tarayıcı çubuklarını gizler.

Windows ilk seferde "Makine Live ağa erişmek istiyor" diye sorarsa **Özel ağlar**'a izin verin; yoksa telefon bağlanamaz.
Telefonun ekranı kararıyorsa ekran zaman aşımını uzatın.

## Yayın (OBS) ve ekran paylaşımı

Çeviri kutusu normalde ekran yakalamaya kapalıdır: Makine Live altyazıyı ekranın kopyasından okuduğu için kendi
kutusunu okumasın diye. Yayın yapıyor ya da ekranınızı arkadaşlarınızla paylaşıyorsanız:

- **Konsol modunda** kutu her zaman görünür (görüntü Remote Play penceresinin kendisinden okunur): OBS'nin ekran yakalaması,
  Discord / Teams ekran paylaşımı kutuyu da gösterir.
- **Bilgisayar oyunlarında** Makine Live › **Görünüm** › **Yayında ve ekran paylaşımında göster**'i açın. Makine Live bu
  durumda oyunun kendi penceresini okur. Bazı oyunlar (özel tam ekran, bazı Vulkan / OpenGL oyunları) pencereden
  okunamaz; o zaman kutu yine gizli kalır ve Kayıt'ta yazar. Oyunu **kenarlıksız / pencereli** modda açmak genelde çözer.
- **OBS'de en temiz yol:** **Görünüm** › **OBS tarayıcı kaynağı**'nı açın, adres kopyalanır
  (`http://127.0.0.1:47712/obs`). OBS'de *Kaynak ekle › Tarayıcı* › bu adresi yapıştırın, genişlik/yükseklik sahnenizle
  aynı olsun (ör. 1920×1080). Altyazı yayında ayrı, saydam bir katman olur; OBS'nin *Oyun Yakalama*'sıyla da çalışır.
  İsteğe bağlı: adresin sonuna `?size=32` (yazı boyutu), `?pos=ust` (üstte) ekleyebilirsiniz.

## LG TV'ler (webOS)

LG'de hiçbir uygulama TV'nin kendi HDMI ekranının üstüne yazamaz. Bu yüzden Makine TV, konsolun görüntüsünü
**kendi içinde** açar ve altyazıyı onun üstüne çizer. Oyunu oynarken TV'yi giriş menüsünden konsola almazsınız,
**Makine TV'yi açarsınız**; konsol görüntüsü uygulamanın içinde görünür.

**Kurulum (bir kez):**
1. TV'de LG Content Store'dan **Developer Mode** uygulamasını kurun. Ücretsiz bir LG geliştirici hesabıyla
   ([developer.lge.com](https://developer.lge.com)) giriş yapın.
2. Developer Mode'da **Dev Mode Status**'u açın (TV yeniden başlar), sonra **Key Server**'ı açın.
3. Makine Live › **Konsol** › TV satırında **Kur** › **LG (webOS)**. TV adresi kendiliğinden bulunur;
   Developer Mode ekranındaki 6 karakterlik **Passphrase**'i girip **Kur**'a basın.

**Kullanım:** Makine TV'yi açın; eşleşme kodu sağdaki panelde görünür. Bilgisayardan eşleştikten sonra panel kapanır ve
yalnızca oyun ile altyazı kalır. Kumanda: **OK** menü · **1–4** konsolun HDMI girişi · **Geri** TV'nin kendi HDMI ekranına
dön. Panelde yazı boyutu, konum ve arka plan ayarları da vardır. Makine TV açıkken ekran koruyucu devreye girmez.

**Bilmeniz gerekenler:**
- Geliştirici modu süreli bir oturumdur (birkaç haftada bir Developer Mode uygulamasından süreyi uzatmanız gerekir).
  Süre dolarsa uygulama TV'den kalkar; yeniden **Kur** yeterlidir.
- Görüntü uygulamanın içinden geldiği için TV'nin oyun modu devre dışı kalabilir; az da olsa gecikme hissedebilirsiniz.
- Makine TV'den çıkınca TV'nin kendi HDMI ekranı siyah kalırsa giriş menüsünden konsolun girişini yeniden seçin.
- TV ağda otomatik bulunamazsa panelde TV'nin adresi yazar: bilgisayarda **TV bul › Adresle bağlan**.

## Altyazı alanı

Varsayılan alan görüntünün alt ortasıdır. Oyunun altyazısı başka bir yerdeyse **F10** ile Remote Play penceresinin
üstünde altyazının durduğu yeri seçin. Alan pencereye göre oranla saklanır; pencereyi taşısanız ya da büyütseniz de kaymaz.

## Sorun giderme

| Durum | Ne yapmalı |
|---|---|
| "Konsol görüntüsü bulunamadı" | Remote Play / Xbox / yakalama penceresi açık mı? Listede olmayan bir program kullanıyorsanız **Pencere seç**. |
| "… penceresi küçültülmüş" | Pencereyi geri açın (küçültülmüş pencere okunamaz). |
| "TV bulunamadı" | TV'de Makine TV açık mı? TV ve bilgisayar aynı ağda mı? Misafir ağı / ayrı Wi-Fi bandı engelleyebilir. Olmazsa **Adresle bağlan** (adres Makine TV ekranında yazar). |
| "Kod kabul edilmedi" | **TV bul** ile yeniden eşleşin; kod TV ekranında yazar (TV'de "Yeni kod üret" kodu değiştirir). |
| TV'de altyazı yok, ama Makine Live çeviriyor | Makine TV'de "Katman izni gerekli" uyarısı var mı? **Kur** ile yeniden kurun ya da izni elle verin. "Bu cihaz altyazı katmanına izin vermiyor" yazıyorsa (düşük bellekli TV kutuları) **Telefonda**'yı kullanın. |
| Telefon adrese bağlanamıyor | Telefon bilgisayarla aynı Wi-Fi'da mı (misafir ağı değil)? Windows güvenlik duvarı sorusunda **Özel ağlar**'a izin verildi mi? |
| OBS'de / ekran paylaşımında altyazı yok | Yukarıdaki **Yayın (OBS) ve ekran paylaşımı** bölümüne bakın. |
| LG: "Parola yanlış" | Developer Mode ekranındaki Passphrase'i (6 karakter) girin; Key Server açık olmalı. |
| LG: oyun görüntüsü siyah | Makine TV menüsünde **Konsolun girişi**'ni (ya da kumandada 1–4) konsolun bağlı olduğu HDMI'ya ayarlayın. |
| Samsung TV | Makine TV şimdilik yok. Altyazı nerede görünsün: **Telefonda** ya da **Bu ekranda** (yukarıda "Monitörde oynayanlar"). |

Sorun bildirirken `%APPDATA%\MakineLive\logs\makine-live.log` dosyasını ekleyin:
[makineceviri.org/iletisim](https://makineceviri.org/iletisim) ya da Makine Çeviri Discord sunucusu.
