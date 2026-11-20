# Güvenlik

## Açık bildirmek

Makine Live'da (DLL, masaüstü uygulaması, Makine TV ya da `makineceviri.org/api/live/` sunucusu) bir güvenlik açığı
bulduysanız lütfen herkese açık bir issue **açmayın**. Bunun yerine **iletisim@makineceviri.org** adresine
"Makine Live güvenlik" konusuyla yazın. Mümkünse şunları ekleyin:

- etkilenen bileşen ve sürüm (uygulamada **Hesap** sekmesi ya da `manifest.json`),
- açığı yeniden üretme adımları,
- olası etkisi.

Bildirimi en kısa sürede yanıtlar, düzeltme yayımlanana kadar ayrıntıları gizli tutarız. Düzeltmeden sonra, isterseniz
sürüm notlarında adınızla teşekkür ederiz.

## Desteklenen sürümler

Yalnızca son yayımlanan sürüm desteklenir. Uygulama açılışta sürümünü sunucuya sorar; güvenlik düzeltmeleri gerekirse
**zorunlu güncelleme** olarak yayımlanır.

## Tasarımda alınan önlemler

**İstemci**
- Depoda anahtar, şifre, jeton ya da imza anahtarı yoktur. Kullanıcının kendi API anahtarı gerekmez; çeviriyi sunucu yapar.
- Oturum jetonu (`%APPDATA%\MakineLive\session.dat`) ve DLL ayarlarındaki `authToken` / `apiKey` Windows DPAPI ile, geçerli
  kullanıcıya bağlı olarak şifrelenir; başka bir kullanıcı ya da bilgisayarda çözülemez.
- Ekran görüntüsü ve OCR bilgisayarda kalır. Sunucuya yalnızca okunan metin satırı ve oyunun adı gider.
- Cihaz kimliği Windows `MachineGuid` değerinin uygulamaya özgü SHA-256 özetidir; ham değer gönderilmez.
- Bütün sunucu trafiği HTTPS'tir; sertifika doğrulaması kapatılmaz. Güncellemeler indirildikten sonra SHA-256 ile doğrulanır.
- Altyazı kutusu `WDA_EXCLUDEFROMCAPTURE` ile ekran yakalamasından ve ekran paylaşımından gizlenir (kullanıcı açıkça
  "yayında göster" demedikçe).

**Yerel ağ**
- Telefon / OBS altyazı sayfası (`makine_live/tv/subtitle_web.py`) yalnızca açıkken çalışır: OBS için yalnızca `127.0.0.1`'e, telefon
  seçiliyse ev ağına bağlanır. Sabit yollar dışında dosya sunmaz, dış kaynak yüklemez (CSP `'self'`), hesap ya da jeton taşımaz.
- PC → Makine TV bağlantısı TV'de görünen 4 haneli eşleşme kodu olmadan kurulamaz. Yanlış kodda TV bekletir, 5 yanlıştan
  sonra kodu yeniler ve 60 sn hiçbir kodu kabul etmez; karşılaştırma sabit sürelidir. Makine TV sunucuya bağlanmaz ve
  jeton taşımaz.
- "TV'ye kur" yalnızca kullanıcının TV'de açtığı hata ayıklama / geliştirici modu üzerinden çalışır.

**Sunucu** (kodu ayrı ve özel depoda)
- Havuz ve çeviri uçları geçerli uygulama jetonu ister; jetonların yalnızca SHA-256 özeti saklanır.
- Her hesap bir satıra tek oy verir; havuz yalnızca onaylı çevirileri dağıtır. İstemcinin kendi çevirisini oy olarak
  göndermesi yalnızca ekip hesaplarına açıktır, böylece havuza sahte hesaplarla çeviri sokulamaz.
- IP adresleri oy sayımında yalnızca HMAC özeti olarak kullanılır. İstekler hız sınırlıdır.

## Geliştiriciler için

- Anahtarları hiçbir dosyaya yazmayın; testlerde ortam değişkeninden okuyun (örnek: `DEEPL_API_KEY`).
- Makine TV imza anahtarı depoda değildir (`MAKINE_TV_KEYSTORE`, `MAKINE_TV_KEYSTORE_PASS`); `*.jks`, `*.keystore`, `.env`
  ve derleme çıktıları `.gitignore`'dadır.
