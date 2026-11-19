# Makine Live ile GTA 6'yı Türkçe oynamak

GTA 6'nın (Grand Theft Auto VI) İngilizce altyazılarını oynarken Türkçe okumak için hazırlanmış adım adım bir rehber.
PS5 ya da Xbox'ta oynayıp altyazıyı TV'de görmek, monitörde oynamak, telefonu ikinci ekran olarak kullanmak ve yayın
yapmak için ayrı ayrı anlatım var. Size uyan bölümü okumanız yeterli.

> Makine Live, [Makine Çeviri](https://makineceviri.org) topluluğunun bir projesidir. Rockstar Games ya da Take-Two
> ile bağlantısı yoktur ve resmî bir Rockstar ürünü değildir. Oyunun dosyalarına dokunmaz, oyunu değiştirmez; yalnızca
> ekranda görünen altyazıyı okur.

---

## İçindekiler

1. [Makine Live nedir?](#1-makine-live-nedir)
2. [Hangi yol bana uygun?](#2-hangi-yol-bana-uygun)
3. [Neler gerekir?](#3-neler-gerekir)
4. [Kurulum ve ilk giriş](#4-kurulum-ve-ilk-giriş)
5. [Senaryo A: Konsolda oyna, altyazıyı TV'de gör](#5-senaryo-a-konsolda-oyna-altyazıyı-tvde-gör)
6. [Senaryo B: Konsoldan monitörde (bilgisayar ekranında) oyna](#6-senaryo-b-konsoldan-monitörde-bilgisayar-ekranında-oyna)
7. [Senaryo C: Telefon / tablet ikinci ekran ve yayıncılar için OBS](#7-senaryo-c-telefon--tablet-ikinci-ekran-ve-yayıncılar-için-obs)
8. [Bilgisayarda oynanan oyunlar için kısa not](#8-bilgisayarda-oynanan-oyunlar-için-kısa-not)
9. [Klavye kısayolları](#9-klavye-kısayolları)
10. [Sorun giderme](#10-sorun-giderme)
11. [Gizlilik](#11-gizlilik)
12. [Şu an neler denendi, neler denenmedi?](#12-şu-an-neler-denendi-neler-denenmedi)

---

## 1. Makine Live nedir?

Makine Live, ekrandaki İngilizce altyazıyı okuyup Türkçesini oyunun üstünde gösteren bir Windows uygulamasıdır.
Çeviriler bir **topluluk çeviri havuzunda** toplanır: aynı replik birkaç farklı oyuncudan uyuşan çeviri alınca onaylanır
ve bundan sonra herkese anında gelir. Siz oynadıkça havuz büyür, havuz büyüdükçe çeviriler hızlanır.

Konsol modu **yalnızca GTA VI içindir**. Oyun seçmeniz gerekmez; konsoldan okunan her satır GTA VI havuzuna gider.

---

## 2. Hangi yol bana uygun?

| Nerede oynuyorsunuz? | Altyazıyı nerede görmek istiyorsunuz? | Okuyacağınız bölüm |
|---|---|---|
| PS5 / Xbox, **TV'de** | TV'de, oyunun üstünde | [Senaryo A](#5-senaryo-a-konsolda-oyna-altyazıyı-tvde-gör) |
| PS5 / Xbox, **monitörde** (görüntü bilgisayarda) | Aynı ekranda, oyunun üstünde | [Senaryo B](#6-senaryo-b-konsoldan-monitörde-bilgisayar-ekranında-oyna) |
| PS5 / Xbox, TV'nize Makine TV kurulamıyor | Telefonda ya da tablette | [Senaryo C](#7-senaryo-c-telefon--tablet-ikinci-ekran-ve-yayıncılar-için-obs) |
| Yayın yapıyorsunuz (OBS, Discord) | Yayında da görünsün | [Senaryo C › Yayıncılar](#yayıncılar-için-obs-ve-ekran-paylaşımı) |

Hangi yolu seçerseniz seçin, **bilgisayar şarttır**: konsolun görüntüsünün bir kopyası bilgisayarda açılır, Makine Live
altyazıyı oradan okur.

```
PS5 / Xbox ──HDMI──▶ TV   ◀── Türkçe altyazı (Makine TV)
     │                              ▲
     └─ Remote Play ──▶ Bilgisayar (Makine Live) ──ev ağı──┘
```

---

## 3. Neler gerekir?

**Her durumda**
- Windows 10 ya da Windows 11 yüklü bir bilgisayar. Oynarken Makine Live açık kalır.
- İnternet bağlantısı (çeviriler sunucudan gelir).
- Bir Makine Çeviri hesabı. Discord ya da e-posta ile açılır, şifre gerekmez.

**Konsolda oynuyorsanız, görüntüyü bilgisayara getirmek için bunlardan biri**
- **PS5:** [PS Remote Play](https://www.playstation.com/remote-play/) uygulaması (ücretsiz). Makine Live, açık kaynaklı
  PS Remote Play istemcisi **chiaki**'yi de tanır.
- **Xbox:** Windows için **Xbox uygulaması** (uzaktan oynatma). **Greenlight** da tanınır.
- ya da bir **yakalama kartı**: Elgato 4K Capture Utility, OBS'nin pencereli projektörü ya da Windows'un **Kamera**
  uygulamasıyla açılan önizleme. Listede olmayan başka bir program da kullanılabilir (**Pencere seç**, aşağıda).

**Altyazıyı TV'de görmek istiyorsanız**
- **Android TV / Google TV** işletim sistemli bir TV ya da **LG webOS** TV. Ayrıntılar
  [Makine TV'yi kurun](#a3-makine-tvyi-tvye-kurun-bir-kez) bölümünde.
- Bilgisayar, konsol ve TV **aynı ev ağında** (aynı modeme bağlı) olmalı.

---

## 4. Kurulum ve ilk giriş

### İndirme

1. **https://makineceviri.org/live/** adresinden `MakineLive.exe` dosyasını indirin.
2. Kurulum yoktur: dosyayı istediğiniz bir klasöre koyup çift tıklamanız yeterli. İlk açılış birkaç saniye sürebilir.

### Windows "bilgisayarınızı korudu" derse

Windows, yeni ve henüz çok indirilmemiş programlarda mavi bir **SmartScreen** penceresi gösterebilir. Bu, programın
zararlı olduğu anlamına gelmez; Windows programı henüz tanımıyor demektir.

1. Penceredeki **Ek bilgi** bağlantısına tıklayın.
2. Altta beliren **Yine de çalıştır** düğmesine basın.

Bunu yalnızca ilk açılışta yapmanız gerekir.

### İlk giriş

Makine Live, **Makine Çeviri hesabınızla** çalışır. Şifre yoktur.

1. Makine Live'ı açın ve **Makine Çeviri hesabıyla giriş yap**'a basın. Uygulama ekranda bir **kod** gösterir
   (ör. `ABCD-EFGH`) ve tarayıcınızda **makineceviri.org/live/baglan/** sayfasını açar.
2. Sayfada **Giriş yap**'a basıp Makine Çeviri hesabınızla girin: **Discord** ile ya da **e-postanıza gelen kodla**.
   Hesabınız yoksa aynı pencerede birkaç saniyede açılır.
3. Sayfadaki kodun uygulamada gördüğünüz kodla aynı olduğuna bakın (kodu yazmanız gerekmez) ve
   **Makine Live'ı bağla**'ya basın.
4. Uygulamaya dönün; birkaç saniye içinde ana panel açılır. Sayfa açılmadıysa uygulamada **Sayfayı yeniden aç**'a basın.

Oturumunuz bu bilgisayarda, Windows hesabınıza bağlı olarak şifreli saklanır. Sonraki açılışlarda yeniden giriş
yapmanız gerekmez. Çıkmak isterseniz **Hesap** sekmesinde **Çıkış yap** düğmesi var.

> Eski kapalı beta test hesapları artık gerekmiyor; Makine Çeviri hesabınız yeterli.

### Bilmeniz gereken birkaç şey

- **API anahtarı gerekmez.** Çeviri Makine Çeviri sunucusunda yapılır. Dil sabittir: İngilizceden Türkçeye.
- **Günlük ücretsiz çeviri sınırı vardır.** Sınır dolsa bile havuzdaki onaylı çeviriler gelmeye devam eder.
- Yeni bir sürüm çıktığında uygulama size sorar (**Şimdi güncelle**). Güncelleme zorunluysa "Güncelleme gerekli"
  ekranı çıkar; **Şimdi güncelle** dosyayı indirip uygulamayı kendisi yeniden açar.

### Panele kısa bir bakış

Ana panelin üstünde algılanan oyun ya da konsol kartı, büyük **Başlat** düğmesi (**F9**) ve bir **Önizleme** alanı
bulunur. Altta beş sekme vardır:

| Sekme | Ne işe yarar? |
|---|---|
| **Canlı** | Çevrilen satırların geçmişi, istatistikler, gecikme grafiği |
| **Çeviri** | Dil bilgisi ve **Okunacak bölge** (**Seç** / **F10**) |
| **Görünüm** | Altyazı kutusunun stili, yazı boyutu, konumu; yayın ayarları |
| **Konsol** | PS5 / Xbox için her şey: görüntü kaynağı, altyazının nerede görüneceği, TV |
| **Hesap** | Kullanıcı adı, sürüm, **Çıkış yap** |

---

## 5. Senaryo A: Konsolda oyna, altyazıyı TV'de gör

Konsol her zamanki gibi HDMI ile TV'ye bağlı kalır, siz koltuktan oynarsınız. Bilgisayar arka planda Remote Play
penceresinden altyazıyı okur, Türkçesi TV'deki **Makine TV** uygulamasıyla oyunun üstünde görünür.

Kurulum dört adımdır; ilk üçünü yalnızca bir kez yaparsınız.

### A1. Konsol görüntüsünü bilgisayara getirin

**PS5 (PS Remote Play)**

1. PlayStation Network'te ücretsiz **ikinci bir hesap** açın ve bu hesabı PS5'e kullanıcı olarak ekleyin.
2. PS5'te *Ayarlar › Sistem › Uzaktan Oynatma › Uzaktan Oynatmayı Etkinleştir*'i açın.
3. Bilgisayarda PS Remote Play'i açın, **ikinci hesapla** giriş yapıp PS5'inize bağlanın.

> **Neden ikinci hesap?** Kendi hesabınızla bağlanırsanız kolunuz konsoldan kopar ve oyunu bilgisayardan oynamanız
> gerekir. İkinci hesapla siz TV'de oynamaya devam edersiniz, bilgisayar yalnızca izler.

**Xbox (Xbox uygulaması)**

1. Xbox'ta *Ayarlar › Cihazlar ve bağlantılar › Uzak özellikler › Uzak özellikleri etkinleştir*'i açın.
2. Bilgisayarda Xbox uygulamasını açın ve konsolunuza **uzaktan oynatma** ile bağlanın.

**Yakalama kartı (isteğe bağlı)**

Konsol → yakalama kartı → TV şeklinde bağlıysanız kartın kendi uygulamasında (ör. Elgato 4K Capture Utility)
önizlemeyi açın. OBS kullanıyorsanız *Pencereli Projektör (Önizleme)* de olur. USB kartlarda Windows'un **Kamera**
uygulaması da çalışır.

PS5'te yakalama kartı kullanacaksanız *Ayarlar › Sistem › HDMI › HDCP'yi Etkinleştir* kapalı olmalıdır; açıkken kart
siyah görüntü alır.

**Pencereyle ilgili üç kural**
- Remote Play / önizleme penceresi **açık kalmalı**. Görev çubuğuna küçültülmüş pencere okunamaz.
- Pencerenin görüntü alanı **en az 960 × 540** piksel olmalı. Daha küçükse yazı okunamayacak kadar küçülür ve
  Makine Live sizi uyarır.
- Pencerenin **üstünde başka pencere olması sorun değil** ve öndeki pencere olması da gerekmez. Makine Live pencerenin
  kendi içeriğini okur.

### A2. Makine Live'da Konsol sekmesini ayarlayın

1. **Konsol** sekmesini açın.
2. **Oyun nerede?** satırında **Konsolda · PS5 / Xbox**'ı seçin.
3. **Konsol görüntüsü** kartına bakın. Remote Play / Xbox / yakalama penceresi açıksa kartta adı ve boyutu yazar
   (ör. "PS Remote Play · PS5 · 1280×720").
   - "Konsol görüntüsü bulunamadı" yazıyorsa pencerenin açık olduğundan emin olun.
   - Listede olmayan bir program kullanıyorsanız **Pencere seç**'e basın, 3 saniye içinde o programın penceresine
     geçin. Öndeki pencere konsol görüntüsü olarak kaydedilir.
4. **Altyazı nerede görünsün?** satırında **TV'de**'yi seçin.

Konsol modunda oyun seçimi yoktur; kartta ve kayıtlarda oyun her zaman Grand Theft Auto VI olarak görünür.

### A3. Makine TV'yi TV'ye kurun (bir kez)

**Makine TV**, TV'de çalışan küçük bir uygulamadır. Bilgisayardan gelen Türkçe satırı oyunun üstüne çizer. Kurulum
bilgisayardaki Makine Live'dan, ev ağı üzerinden yapılır: **Konsol** sekmesi › TV kartındaki **Kur** düğmesi.
Açılan **Makine TV'yi kur** penceresinin üstünde TV türünü seçersiniz: **Android / Google TV** ya da **LG (webOS)**.

#### Android TV / Google TV

Sony, Philips, TCL, Hisense, Xiaomi gibi markaların **Android TV ya da Google TV işletim sistemli** modelleri bu
gruptadır. Makine TV Android 8.0 ve üzerini destekler. Menü adları markaya göre biraz değişebilir.

> **TV kutuları ve çubuklar hakkında:** Makine TV altyazıyı, kurulduğu cihazın **kendi görüntüsünün** üstüne çizer.
> TV'nin içinde Android TV / Google TV varsa bu görüntü konsolun HDMI girişini de kapsar. Ama Chromecast, Mi Box gibi
> TV'ye takılan **ayrı bir kutu** kullanıyorsanız, kutu yalnızca kendi HDMI girişinin üstüne çizebilir; konsol başka
> bir HDMI girişindeyse altyazı konsolun görüntüsünde görünmez. Bu durumda **Telefonda** seçeneğini kullanın
> ([Senaryo C](#7-senaryo-c-telefon--tablet-ikinci-ekran-ve-yayıncılar-için-obs)).

**1. Geliştirici seçeneklerini açın**

- **Android TV:** *Ayarlar › Cihaz tercihleri › Hakkında* bölümünde **Yapı** satırına art arda **7 kez** basın.
- **Google TV:** *Ayarlar › Sistem › Hakkında* bölümünde **Android TV OS derlemesi** satırına art arda **7 kez** basın.

"Artık bir geliştiricisiniz" benzeri bir mesaj çıkar. Bu yalnızca gizli bir ayar menüsünü görünür yapar; TV'nize bir şey
olmaz.

**2. Hata ayıklamayı açın**

*Geliştirici seçenekleri* menüsünde (Android TV'de *Ayarlar › Cihaz tercihleri* altında, Google TV'de *Ayarlar › Sistem*
altında) **USB hata ayıklama**'yı açın. Varsa **Ağ üzerinden hata ayıklama**'yı da açın.

> Bu ayar (ADB), bilgisayarın uygulamayı TV'ye ev ağı üzerinden yükleyebilmesi içindir. Makine Live TV'ye 5555
> numaralı kapıdan bağlanır. Kurulum bittikten sonra hata ayıklamayı kapatabilirsiniz; Makine TV çalışmaya devam eder.

**3. Bilgisayardan kurun**

1. Makine Live › **Konsol** › **Kur** › **Android / Google TV**.
2. Makine Live ağda hata ayıklaması açık TV'yi arar ve **TV adresi** kutusunu kendisi doldurur. Bulamazsa adresi elle
   yazın (TV'de *Ayarlar › Ağ* bölümünde görünür, ör. `192.168.1.40`).
3. **Kur**'a basın.
4. İlk seferde TV ekranında **hata ayıklamaya izin verilsin mi** diye bir soru çıkar. Kumandayla **İzin ver**'i seçin.
5. Makine Live sırayla uygulamayı yükler, kurar, altyazının oyunun üstünde görünmesi için gereken **katman iznini**
   verir ve Makine TV'yi açar. Pencerede "Kuruldu. Şimdi TV bul ile eşleşin." yazınca bitti demektir.

**Kur olmazsa: APK'yı USB bellekle kurun**

1. **Makine TV'yi kur** penceresinde **APK'yı kaydet**'e basıp `makine-tv.apk` dosyasını bir USB belleğe kaydedin.
2. Belleği TV'ye takın ve TV'nin dosya yöneticisiyle dosyayı açıp kurun. TV, dosya yöneticisine bilinmeyen
   uygulama yükleme izni vermenizi isteyebilir.
3. Katman iznini elle verin: *Ayarlar › Uygulamalar › Özel uygulama erişimi › Diğer uygulamaların üzerinde göster*
   › **Makine TV** › açık. Makine TV'de "Katman izni gerekli" kartı çıkarsa **İzin ekranını aç** düğmesi sizi bu
   ayara götürmeye çalışır.

Bazı TV'lerde bu izin menüsü gizlidir. Bulamazsanız hata ayıklamayı açıp **Kur**'u deneyin; Kur bu izni menüye
gerek kalmadan verir.

> Bazı yeni Google TV'lerde yalnızca eşleştirme kodu isteyen **Kablosuz hata ayıklama** seçeneği bulunur. **Kur**
> düğmesi bu yöntemle bağlanamayabilir; o durumda **APK'yı kaydet** yolunu kullanın.

**Düşük bellekli cihazlar:** Android, bazı düşük bellekli cihazlarda uygulamaların görüntünün üstüne çizmesine hiç
izin vermez. Makine TV'de "Bu cihaz altyazı katmanına izin vermiyor" yazıyorsa **Telefonda** seçeneğini kullanın.

#### LG TV'ler (webOS)

LG TV'lerde hiçbir uygulama TV'nin kendi HDMI ekranının üstüne yazamaz. Bu yüzden Makine TV, LG'de konsolun
görüntüsünü **kendi içinde** açar ve altyazıyı onun üstüne çizer. Yani oynarken TV'yi giriş menüsünden konsola
almazsınız, **Makine TV'yi açarsınız**; konsolun görüntüsü uygulamanın içinde görünür.

LG'ye kurulum, LG'nin geliştiricilere sunduğu **Developer Mode** uygulamasıyla yapılır.

**1. Developer Mode'u hazırlayın**

1. TV'de **LG Content Store**'dan **Developer Mode** uygulamasını kurun.
2. Uygulamayı açın ve ücretsiz bir **LG geliştirici hesabıyla** ([developer.lge.com](https://developer.lge.com))
   giriş yapın. Hesabınız yoksa bu siteden açabilirsiniz.
3. **Dev Mode Status**'u açın. TV yeniden başlar.
4. Developer Mode uygulamasını tekrar açın ve **Key Server**'ı açın.
5. Ekranda 6 karakterlik bir **Passphrase** görünür. Bunu not edin.

**2. Bilgisayardan kurun**

1. Makine Live › **Konsol** › **Kur** › **LG (webOS)**.
2. **TV adresi** kendiliğinden bulunur (bulunamazsa elle yazın).
3. **Passphrase** kutusuna Developer Mode ekranındaki 6 karakteri yazın.
4. **Kur**'a basın. Makine TV kurulur ve TV'de açılır.

**3. Konsolun girişini seçin**

Makine TV açıkken kumandada konsolun bağlı olduğu HDMI girişinin numarasına basın (**1**, **2**, **3** ya da **4**).
Aynı ayar menüde **Konsolun girişi** satırında da var.

**Kumanda**

| Tuş | Ne yapar? |
|---|---|
| **OK** (ya da yukarı / aşağı) | Menüyü açar; menüde seçili ayarı değiştirir |
| **1–4** | Konsolun HDMI girişini seçer |
| **Geri** | Menü açıksa kapatır; değilse Makine TV'den çıkar ve TV'nin kendi HDMI ekranına döner |

Menüde **Konsolun girişi**, **Yazı boyutu**, **Konum**, **Arka plan**, **Örnek altyazı göster** ve **Yeni kod üret**
bulunur. Bilgisayar bağlanınca menü kendiliğinden kapanır, ekranda yalnızca oyun ve altyazı kalır. Makine TV açıkken
TV'nin ekran koruyucusu devreye girmez.

**LG'de bilmeniz gerekenler**
- Geliştirici modu **süreli bir oturumdur**. Süreyi Developer Mode uygulamasından uzatabilirsiniz. Süre dolarsa
  Makine TV TV'den kalkar; Developer Mode'u yeniden açıp Makine Live'da **Kur**'a basmanız yeterlidir.
- Görüntü uygulamanın içinden geldiği için TV'nin oyun modu devre dışı kalabilir ve az da olsa gecikme olabilir.
  Bu kısım henüz gerçek oyunla denenmedi (aşağıda [bölüm 12](#12-şu-an-neler-denendi-neler-denenmedi)).
- Makine TV'den çıkınca TV'nin HDMI ekranı siyah kalırsa TV'nin giriş menüsünden konsolun girişini yeniden seçin.
- TV ağda otomatik bulunamazsa Makine TV ekranında TV'nin adresi yazar: bilgisayarda **TV bul › Adresle bağlan**.

#### Samsung, Apple TV, Fire TV ve diğerleri

Makine TV şu an yalnızca **Android TV / Google TV** ve **LG webOS** için var. **Samsung (Tizen)**, **Apple TV**,
**Amazon Fire TV**, **Roku** ve akıllı olmayan TV'ler için bir Makine TV uygulaması yok. Bu TV'lerde altyazıyı yine
okuyabilirsiniz:

- **Telefonda:** Altyazı telefonunuzun ya da tabletinizin tarayıcısında görünür; uygulama kurmak gerekmez.
  Telefonu TV'nin önüne ya da yanınıza koyarsınız. [Senaryo C](#7-senaryo-c-telefon--tablet-ikinci-ekran-ve-yayıncılar-için-obs)
- **Bu ekranda:** Oyunu bilgisayarın ekranında (Remote Play ya da yakalama kartı penceresinden) oynarsınız, altyazı
  onun üstünde çıkar. [Senaryo B](#6-senaryo-b-konsoldan-monitörde-bilgisayar-ekranında-oyna)

### A4. TV ile eşleştirin

1. TV'de **Makine TV**'yi açın. Ekranda büyük harflerle 4 haneli bir **EŞLEŞME KODU**, altında "PC bekleniyor" ve
   TV'nin adresi görünür.
2. Makine Live › **Konsol** sekmesinde **TV bul**'a basın.
3. Makine Live ev ağındaki Makine TV'leri arar. Tek TV varsa doğrudan kod penceresi açılır; birden fazlaysa
   **Hangi TV?** listesinden TV'nizi seçin.
4. "TV ekranındaki 4 haneli eşleşme kodunu girin." kutusuna kodu yazıp **Bağlan**'a basın.
5. TV'de durum "Bağlı · <bilgisayarınızın adı>" olur, Makine Live'da TV kartında "bağlı · altyazılar TV'de
   gösteriliyor" yazar.

Eşleşme bir kez yapılır. Makine Live TV'yi ve kodu hatırlar; sonraki oturumlarda kendiliğinden bağlanır.

**Neden kod var?** Kod, aynı ağdaki başka bir bilgisayarın TV'nize yazı göndermesini engeller. Yanlış kod girilirse TV
her denemede kısa bir süre bekletir; art arda 5 yanlış denemeden sonra yeni bir kod üretir ve bir dakika boyunca hiçbir
kodu kabul etmez. Bu durumda TV'deki **yeni** kodla tekrar eşleşin.

**Bağlantı koparsa:** TV kapanır, Wi-Fi gider gelir ya da modem TV'ye yeni bir adres verirse Makine Live birkaç
saniyede bir yeniden bağlanmayı dener ve gerekirse TV'yi ağda yeniden bulur. Bir şey yapmanız gerekmez. Bilgisayardan
bir süre hiçbir şey gelmezse TV ekrandaki altyazıyı kendisi temizler.

### A5. Oynayın

1. GTA VI'da **altyazıları açın** (oyunun ayarlarından).
2. Makine Live'da **Başlat**'a ya da **F9**'a basın.
3. TV'yi konsolun girişine alın (LG'de bunun yerine Makine TV'yi açın).
4. Oyunda İngilizce bir altyazı çıktığında Türkçesi kısa süre sonra TV'de, oyunun üstünde belirir.

**Android TV'de Makine TV ayarları**

TV'de Makine TV'yi açtığınızda sağ tarafta kumandayla değiştirebileceğiniz ayarlar bulunur:

| Ayar | Seçenekler |
|---|---|
| **Yazı boyutu** | Küçük · Orta · Büyük |
| **Konum** | Alt · Üst |
| **Arka plan** | Şeridin koyuluğu: %60 · %78 · %90 |
| **Örnek altyazı göster** | Ayarları denemek için örnek bir satır gösterir |
| **Açılışta başlat** | TV açılınca Makine TV kendiliğinden çalışır (varsayılan: Açık) |
| **Yeni kod üret** | Eşleşme kodunu değiştirir; bilgisayardan yeni kodla yeniden eşleşmeniz gerekir |

Makine TV'de **Geri** tuşu uygulamayı kapatmaz; altyazılar arka planda gelmeye devam eder.

**Şeridin rengi:** Altyazı şeridinin solunda **yeşil** bir çizgi varsa o çeviri toplulukça onaylanmıştır. **Sarı**
çizgi, çevirinin havuzda olduğunu ama henüz onaylanmadığını gösterir.

**Hazır çeviriler:** Panelde "Hazır çeviriler bulundu" sorusu çıkarsa **Evet, indir** deyin. Onaylı çeviriler
bilgisayarınıza iner; bu satırlar internete hiç gitmeden, anında gösterilir ve günlük sınırınızdan düşmez.

### A6. Altyazı alanını ayarlayın (F10)

Makine Live varsayılan olarak görüntünün **alt ortasını** okur; GTA VI altyazıları için çoğu zaman bu yeterlidir.
Altyazı başka bir yerdeyse ya da okunan alanda başka yazılar (ör. ekran ipuçları) varsa alanı daraltın:

1. Remote Play / önizleme penceresi açıkken **F10**'a basın (ya da **Çeviri** sekmesinde **Okunacak bölge** › **Seç**).
2. Remote Play penceresinin üstünde altyazının durduğu alanı fareyle çerçeveleyin.

Alan pencerenin görüntüsüne **oranla** kaydedilir: pencereyi taşısanız ya da büyütseniz de alan kaymaz. Seçtiğiniz alan
Remote Play görüntüsünün içinde olmalıdır.

---

## 6. Senaryo B: Konsoldan monitörde (bilgisayar ekranında) oyna

Monitörlerde uygulama çalışmadığı için Makine TV kurulamaz. Bunun yerine konsolun görüntüsünü bilgisayarda açıp oyunu
oradan oynarsınız; altyazı o görüntünün üstünde çıkar.

### Görüntüyü bilgisayarda açın

- **Yakalama kartıyla (en az gecikme):** Konsol → yakalama kartı → bilgisayar. Kartın uygulamasında (ya da OBS'de
  *Pencereli Projektör*) önizlemeyi **tam ekran** açıp oyunu oradan oynayın. Kolunuz konsola bağlı kalır.
- **PS Remote Play / Xbox uygulamasıyla (kart gerekmez):** Bu kez **kendi hesabınızla** bağlanın ve oyunu Remote Play
  penceresinden **tam ekran** oynayın. Kol bilgisayara da bağlanabilir.

### Makine Live'ı ayarlayın

1. **Konsol** sekmesi › **Oyun nerede?** › **Konsolda · PS5 / Xbox**.
2. **Altyazı nerede görünsün?** › **Bu ekranda**. Kartta "Bu bilgisayarın ekranı" yazar.
3. **Başlat** (**F9**).

Altyazı kutusu ekrana göre değil, **Remote Play / önizleme penceresinin 16:9 görüntüsüne göre** yerleşir: pencere tam
ekran değilse de kutu görüntünün altında (ya da seçtiğiniz konumda) durur, siyah bantların üstüne kaymaz.

### Kutuyu yerleştirin ve kilitleyin (F8)

- Kutunun görünümü **Görünüm** sekmesinden değişir: **Stil** (Altyazı · Yalın · Sinema), **Yazı boyutu**,
  **Bant opaklığı**, **En kısa gösterim** ve **Konum** (Alt · Üst · Serbest).
- **Kilitle** düğmesi ya da **F8** kutunun taşınabilir mi yoksa kilitli mi olduğunu değiştirir. Kilit açıkken kutuyu
  fareyle sürükleyebilirsiniz; bıraktığınız yer kaydedilir ve **Konum** **Serbest** olur.
- Oynamaya başlamadan önce kutuyu **kilitleyin** (F8): kilitliyken fare tıklamaları kutudan geçip oyuna gider.
- Panelde **Örnek göster** düğmesiyle kutunun nasıl göründüğünü oyun olmadan da deneyebilirsiniz.

### Gecikme ve okuma için ipuçları

- **Kablolu bağlantı** kullanın: Remote Play'de hem konsolu hem bilgisayarı mümkünse Ethernet kablosuyla modeme bağlayın.
  Ev ağına göre biraz gecikme olabilir; yakalama kartı bu gecikmeyi ortadan kaldırır.
- Remote Play penceresinin görüntü alanı **en az 960 × 540** olmalı; daha küçük pencerede okuma durur ve Makine Live
  "penceresi küçük" uyarısı verir. Tam ekran en iyisidir.
- Remote Play uygulamasının ayarlarında görüntü kalitesi seçilebiliyorsa yüksek çözünürlük seçin; yazı ne kadar netse
  okuma o kadar güvenilir olur.
- Pencereyi **küçültmeyin**. Küçültülmüş pencere okunamaz.
- Onaylı çeviriler en hızlı gelenlerdir. "Hazır çeviriler bulundu" sorusuna **Evet, indir** demek bunları
  bilgisayarınıza indirir.

> Monitörünüz bilgisayara değil doğrudan konsola bağlıysa, bilgisayar yine Remote Play ya da yakalama kartı penceresinden
> okur; altyazıyı da **Telefonda** seçeneğiyle telefonda görürsünüz (sıradaki bölüm).

---

## 7. Senaryo C: Telefon / tablet ikinci ekran ve yayıncılar için OBS

### Telefonda ya da tablette altyazı

TV'nize Makine TV kurulamıyorsa ya da konsol doğrudan monitöre bağlıysa altyazıyı telefonun ya da tabletin
tarayıcısında okuyabilirsiniz. Uygulama kurmak gerekmez.

1. Bilgisayarda Remote Play / Xbox / yakalama penceresi açık olsun ([A1](#a1-konsol-görüntüsünü-bilgisayara-getirin)).
2. **Konsol** sekmesi › **Oyun nerede?** › **Konsolda · PS5 / Xbox**.
3. **Altyazı nerede görünsün?** › **Telefonda**. Kartta bir adres yazar (ör. `192.168.1.20:47712`).
   **Kopyala** ile adresi panoya alabilirsiniz.
4. Telefon ya da tablet **bilgisayarla aynı Wi-Fi'dayken** tarayıcıya bu adresi yazın.
5. **Başlat** (**F9**). Son satır büyük, önceki birkaç satır üstünde küçük görünür. Kartta kaç cihazın bağlı olduğu
   yazar ("1 cihaz bağlı").

Sayfada **A−** / **A+** yazı boyutunu değiştirir, **Tam ekran** tarayıcı çubuklarını gizler. Telefonu TV'nin ya da
monitörün altına koyabilirsiniz.

- Windows ilk seferde Makine Live'ın ağa erişmek istediğini sorabilir. **Özel ağlar**'a izin verin; yoksa telefon
  bağlanamaz.
- Misafir Wi-Fi ağları cihazların birbirini görmesini çoğu zaman engeller; telefonu ana ağa bağlayın.
- Telefonun ekranı kararıyorsa ekran zaman aşımını uzatın.

**Telefonda** seçeneği konsol modunda çalışır.

### Yayıncılar için: OBS ve ekran paylaşımı

Makine Live'ın çeviri kutusu normalde ekran yakalamaya **kapalıdır**: Makine Live altyazıyı ekranın kopyasından okuduğu
için kendi kutusunu okumasın diye. Yayın yapıyor ya da ekranınızı paylaşıyorsanız:

- **Konsol modunda** kutu her zaman yakalamaya açıktır, çünkü görüntü Remote Play penceresinin kendisinden okunur.
  OBS'nin ekran yakalaması ve Discord / Teams ekran paylaşımı kutuyu da gösterir.
- **Bilgisayar oyunlarında** **Görünüm** sekmesinde **Yayında ve ekran paylaşımında göster**'i açın. Makine Live bu
  durumda oyunun kendi penceresini okur. Bazı oyunlar (özel tam ekran, bazı Vulkan / OpenGL oyunları) pencereden
  okunamaz; o zaman kutu gizli kalır ve Makine Live bir süre sonra yeniden dener. Oyunu **kenarlıksız** ya da
  **pencereli** modda açmak genelde çözer.

**OBS'de en temiz yol: tarayıcı kaynağı**

1. **Görünüm** sekmesinde **OBS tarayıcı kaynağı**'nı açın. Adres kendiliğinden panoya kopyalanır
   (`http://127.0.0.1:47712/obs`); gerekirse yanındaki **Kopyala** ile tekrar alın.
2. OBS'de *Kaynak ekle › Tarayıcı* seçin ve bu adresi yapıştırın.
3. Genişlik ve yüksekliği sahnenizle aynı yapın (ör. 1920 × 1080).

Altyazı yayında ayrı, saydam bir katman olur; OBS'nin *Oyun Yakalama*'sıyla da çalışır. İsterseniz adresin sonuna
ayar ekleyebilirsiniz: `?size=32` yazı boyutu, `?opacity=78` arka plan koyuluğu, `?pos=ust` altyazıyı üste alır.
Birden fazlasını `&` ile birleştirin: `http://127.0.0.1:47712/obs?size=32&pos=ust`.

> 47712 numaralı kapı doluysa Makine Live sıradaki boş kapıyı kullanır; doğru adres her zaman panelde yazar.

---

## 8. Bilgisayarda oynanan oyunlar için kısa not

Makine Live bilgisayarda oynadığınız oyunlarla da çalışır. **Konsol** sekmesinde **Oyun nerede?** › **Bu bilgisayarda**
seçili olmalı.

- **Oyun algılama:** Makine Live yalnızca bilgisayarınızda kurulu **oyunları** tanır: Steam, Epic, GOG, Ubisoft
  Connect, EA, Xbox / Game Pass ve Windows'un oyun listesi. Tarayıcı, müzik çalar ya da Discord asla okunmaz.
  Algılanan oyun panelin üstündeki kartta görünür.
- Oyununuz tanınmadıysa oyunu açın, kartta **Oyun ekle**'ye basın ve geri sayım bitmeden oyunun penceresine geçin.
  Yanlış bir program oyun sanıldıysa **Oyun değil**'e basın. Yenile düğmesi kütüphaneyi yeniden tarar.
- **F9** okumayı başlatır / durdurur, **F10** okunacak bölgeyi seçer. Okuma yalnızca oyun öndeyken ve seçili bölgenin
  üstünde başka pencere yokken yapılır.
- Kutunun görünümü **Görünüm** sekmesinden, yeri **F8** (kilit açıkken sürükleyerek) değişir.

---

## 9. Klavye kısayolları

Kısayollar oyun öndeyken de çalışır.

| Tuş | Ne yapar? |
|---|---|
| **F9** | Okumayı başlatır / durdurur (**Başlat** / **Durdur**) |
| **F10** | Okunacak bölgeyi seçer. Konsol modunda Remote Play / önizleme penceresinin üstünde seçilir |
| **F8** | Altyazı kutusunu kilitler ya da kilidini açar. Kilit açıkken kutu sürüklenir; kilitliyken tıklamalar oyuna geçer |

**Makine TV kumandası:** Android TV'de ayarlar ok tuşları ve OK ile seçilir, **Geri** uygulamayı arka plana alır.
LG'de **OK** menü, **1–4** HDMI girişi, **Geri** çıkış ([LG bölümü](#lg-tvler-webos)).

---

## 10. Sorun giderme

| Ne görüyorsunuz? | Ne yapmalı? |
|---|---|
| "Konsol görüntüsü bulunamadı" | Remote Play / Xbox / yakalama penceresi açık mı? Listede olmayan bir program kullanıyorsanız **Pencere seç**. |
| "… penceresi küçültülmüş" | Pencereyi geri açın. Küçültülmüş pencere okunamaz; başka pencerelerin arkasında kalması ise sorun değil. |
| "… penceresi küçük (… × …)" | Pencereyi en az 960 × 540 yapın ya da tam ekran açın. |
| Görüntü siyah, hiçbir şey okunmuyor | Yakalama kartında PS5'in HDCP ayarını kapatın (*Ayarlar › Sistem › HDMI*). Korumalı içerik (ör. bazı video uygulamaları) okunamaz. |
| "TV bulunamadı" | TV'de Makine TV açık mı? TV ve bilgisayar aynı modeme bağlı mı? Misafir ağı ya da ayrı Wi-Fi bandı engelleyebilir. Olmazsa **Adresle bağlan** ile Makine TV ekranında yazan adresi girin. Makine TV kurulu değilse **TV'ye kur**. |
| "Kod kabul edilmedi" | **TV bul** ile yeniden eşleşin ve TV ekranındaki **güncel** kodu girin. 5 yanlış denemeden sonra TV kodu yeniler ve bir dakika hiçbir kodu kabul etmez; bir dakika bekleyip yeni kodu girin. |
| "TV'ye ulaşılamıyor · yeniden deneniyor" | TV açık ve Makine TV çalışıyor mu? Makine Live kendisi yeniden bağlanır; TV'nin adresi değiştiyse ağda yeniden arar. |
| Makine Live çeviriyor ama TV'de altyazı yok | Makine TV'de "Katman izni gerekli" uyarısı var mı? **Kur** ile yeniden kurun ya da izni elle verin. "Bu cihaz altyazı katmanına izin vermiyor" yazıyorsa **Telefonda**'yı kullanın. Ayrı bir TV kutusu kullanıyorsanız [bu nota](#android-tv--google-tv) bakın. |
| Android TV: "TV bağlantıyı onaylamadı" | Kur sırasında TV ekranında çıkan soruya **İzin ver** deyip tekrar deneyin. |
| Android TV: "TV'ye bağlanılamadı" | TV'de **USB hata ayıklama** (varsa **Ağ üzerinden hata ayıklama**) açık mı, adres doğru mu? Olmazsa **APK'yı kaydet**. |
| LG: "Parola (Passphrase) yanlış" | Developer Mode ekranında yazan 6 karakteri girin. **Key Server** açık olmalı. |
| LG: "TV bağlantıyı kabul etmedi" | Developer Mode'da **Key Server**'ı kapatıp açın ve ekrandaki yeni Passphrase ile tekrar deneyin. |
| LG: Makine TV kayboldu | Geliştirici modu süresi dolmuş olabilir. Developer Mode'u açın ve Makine Live'da yeniden **Kur**'a basın. |
| LG: oyun görüntüsü siyah | Kumandada **1–4** ile (ya da menüde **Konsolun girişi**) konsolun bağlı olduğu HDMI girişini seçin. |
| Telefon adrese bağlanamıyor | Telefon bilgisayarla aynı Wi-Fi'da mı (misafir ağı değil)? Windows'un sorusunda **Özel ağlar**'a izin verildi mi? |
| OBS'de / ekran paylaşımında altyazı yok | [Yayıncılar için](#yayıncılar-için-obs-ve-ekran-paylaşımı) bölümüne bakın; en kolayı **OBS tarayıcı kaynağı**. |
| Altyazı okunuyor ama yanlış yer okunuyor | **F10** ile yalnızca altyazının durduğu alanı seçin. |
| "Bakım çalışması" ekranı | Sunucuda kısa bir bakım yapılıyor. Uygulama 30 saniyede bir kendisi sorar ve bakım bitince açılır; **Tekrar dene** ile hemen de sorabilirsiniz. |
| "Günlük çeviri sınırı doldu" | O gün için yeni çeviri hakkınız bitti. Havuzdaki onaylı çeviriler gelmeye devam eder; sınır ertesi gün yenilenir. |
| "Sunucuya ulaşılamıyor" | İnternet bağlantınızı kontrol edin. Makine Live kendisi yeniden dener. |
| "Oturumunuz kapatıldı" | Yeniden giriş yapın ([İlk giriş](#ilk-giriş)). |

### Destek isterken

Sorununuzu **[makineceviri.org](https://makineceviri.org)** üzerinden bildirebilirsiniz. Bildirime şu dosyayı
eklemeniz işimizi çok kolaylaştırır:

```
%APPDATA%\MakineLive\logs\makine-live.log
```

Bu yolu Windows'ta *Başlat* menüsüne ya da dosya gezgininin adres çubuğuna yapıştırıp Enter'a basarak dosyaya
ulaşabilirsiniz. Dosyada paneldeki kayıtlar ve olası hata ayrıntıları bulunur.

---

## 11. Gizlilik

- Makine Live OCR'ı (yazı okumayı) **bilgisayarınızda** yapar. Sunucuya ekran görüntüsü değil, yalnızca okunan
  altyazı satırı çeviri için gider.
- **Makine TV sunucuya hiç bağlanmaz ve hesap bilgisi taşımaz.** Yalnızca ev ağınızda, eşleştiğiniz bilgisayardan
  gelen altyazı satırını gösterir. Eşleşme kodu, aynı ağdaki başka bir bilgisayarın TV'nize yazmasını engeller.
- Telefon ve OBS sayfaları da yalnızca altyazıyı gösterir; hesap ya da oturum bilgisi içermez. Bu sayfalar yalnızca siz
  açtığınızda çalışır.
- Oturumunuz bilgisayarınızda, Windows hesabınıza bağlı olarak şifreli saklanır. **Çıkış yap** onu siler.

---

## 12. Şu an neler denendi, neler denenmedi?

Makine Live'ın konsol / TV modu yeni. Neyin gerçekten denendiğini açıkça yazmak istedik:

| Konu | Durum |
|---|---|
| Android TV 8, 9, 12 ve Google TV 14, 16 üzerinde kurulum, katman izni, eşleşme, yanlış kodun reddi ve altyazı katmanı | Google'ın resmî TV emülatörlerinde denendi |
| Bilgisayarda Remote Play benzeri bir pencereden okuma → çeviri → TV'ye gönderme (pencerenin üstünde başka pencere varken) | Uçtan uca denendi |
| LG TV'ye bilgisayardan kurulum, uygulamanın açılması, bilgisayarın TV'yi ağda bulması | Gerçek bir LG TV'de (webOS 3.4) denendi |
| LG'de konsolun HDMI görüntüsünün uygulama içinde açılması, oyun modu ve gecikme | Henüz denenmedi |
| Gerçek bir PS5 / Xbox ve gerçek bir Android TV ile oynanış | Henüz denenmedi |

Bu yüzden ilk günlerde bazı TV'lerde beklenmedik davranışlar görebilirsiniz. Karşılaşırsanız günlük dosyasıyla birlikte
bildirmeniz bize çok yardımcı olur.

---

*Makine Live, Makine Çeviri topluluğu tarafından geliştirilmektedir. Grand Theft Auto, Rockstar Games'in ticari
markasıdır; bu rehber ve Makine Live Rockstar Games ya da Take-Two Interactive ile bağlantılı değildir.*
