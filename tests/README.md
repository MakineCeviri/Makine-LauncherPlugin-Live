# Testler

Bütün testler bu klasördedir ve depo kökünden ya da bu klasörden çalıştırılabilir. Uygulamanın paketini
(`makine_live`) ve `build/release`'i `_root.py` bulur.

```powershell
py -m pip install -r requirements.txt     # bir kez
py tests/test_normalization.py
```

| Test | Ne sınar | Gerektirir |
|---|---|---|
| `test_plugin.py` | DLL ABI: başlatma, ayarlar ve DPAPI, gerçek OCR, eksik çalışma dosyası hatası | derlenmiş DLL (`cmake --build build`) + OCR dosyaları |
| `test_dll_server.py` | DLL'in `makine` motoru: jetonla `/api/live/translate`, 401, jeton iptali (küçük sahte sunucuyla) | derlenmiş DLL |
| `test_normalization.py` | Python normalizasyonu ↔ `spec/normalization_vectors.json` | — |
| `test_game_library.py` | yalnızca oyunların tanınması (sahte Steam / Epic klasörleri, `appinfo.vdf`) | — |
| `test_local_pool.py` | yerel havuz: arama, benzer satır, eşitleme farkları, kalıcılık | — |
| `test_subtitle_scheduler.py` | satır takibi ve altyazı zamanlayıcısı | — |
| `test_garbage.py` | OCR süzgeci: sunucuyla aynı çöp kararı (`spec/garbage_vectors.json`), oyun arayüzü / gürültü / tuş simgeleri | — |
| `test_console_mode.py` | konsol modu: 16:9 alan, oransal bölge, kaynak eşleme, gerçek pencereyle bulma | Tk |
| `test_tv_link.py` | PC → Makine TV: keşif, eşleşme kodu, satır, en yeni kazanır, yeniden bağlanma (sahte TV) | — |
| `test_subtitle_web.py` | telefon / OBS sayfaları ve SSE akışı (gerçek HTTP) | — |
| `test_discord_presence.py` | Discord'da gösterme: el sıkışma, etkinlik, hız sınırı, Discord kapalıyken bekleme | — |
| `test_telemetry.py` | Kullanım bildirimi: okuma süresi / satır farkı, ekibe gidecek günlüğün sonu (gzip), sistem bilgisi | — |
| `test_app_update.py` | Kendi kendini güncelleme: .old'a çevirme, yerine koyma, yazılamazsa geri alma (komut dosyası yok) | — |
| `test_webos_service.py` | LG servisi gerçek Node ile (sahte `webos-service`, `webos_mock/`): keşif, eşleşme, kod kilidi | Node.js |
| `test_webos_install.py` | LG geliştirici modu kurulumu, sahte TV'ye: anahtar, parola, SSH, kurulum yanıtları | paramiko |
| `test_webos_install_cpp.py` | Aynı sahte TV'ye eklentinin C++ kurulumu (`src/webos_install.cpp`, libssh2): AES/3DES PEM, ssh-rsa'lı eski ve rsa-sha2'li yeni SSH, SFTP'li/SFTP'siz yükleme, sessiz akış, ret | paramiko + `live_webos_test.exe` (`ctest -R webos`) |
| `text_fix_test.cpp` | C++ boşluk düzeltici (`SpacingFixer`) | `ctest --test-dir build` |
| `cpp/test_logic.cpp` `pool_list` | oyun havuzları listesi: durum (missing / outdated / current / empty / error), sıra, 200 sınırı; `local_pool`: havuz özeti (`games.json`) | `ctest --test-dir build -R pool` |
| `pools_e2e.py` | Eklenti gerçek `makine-plugin-host.exe`'de, sahte sunucuya: `pools` (kütüphane, yoklama, indirme, yeni onayların kendiliğinden inmesi), bakımda durup bitince yeniden başlama, `log_sent` reddedilince günlüğün yüklenmesi | `build/package` + host (`MAKINE_PLUGIN_HOST`) |
| `cpp/test_fit.cpp` | uzun çeviri parçaları, altyazı bulucunun kanıtı, geçici büyüme | `ctest --test-dir build -R "fit|finder|temp_grow"` |
| `cpp/finder_gate.cpp` | altyazı bulucu, tam ekran sahnelerde (kesinlik, duyarlılık, kilit süresi, örnek maliyeti) | `python tools/make_scene_frames.py --bench ../Makine-LauncherPlugin-Live-ocrbench` → `ctest -R finder_gate` |

Diğerleri:

- `webos_preview.html` — LG arayüzünü TV olmadan görmek için: Edge/Chrome `--allow-file-access-from-files` ile
  `tests/webos_preview.html?scene=menu|line|long|note`.
- `test_ui.py`, `test_winhttp.py` — eski deneme araçları (DLL'i elle denemek; DeepL'e ham WinHTTP isteği,
  anahtar `DEEPL_API_KEY` ortam değişkeninden).
- Makine TV'nin gerçek Android TV / Google TV emülatörlerinde uçtan uca denemesi: `py tools/tv_emulator_check.py`.

Sunucunun testleri sunucu deposundadır; ortak kurallar `spec/` vektörleriyle iki tarafta da sınanır.
