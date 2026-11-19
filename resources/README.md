# resources/

| Dosya | Kaynak | Lisans |
|---|---|---|
| `en_words.txt` | [wordninja](https://github.com/keredson/wordninja) kelime frekans listesinin ilk 60 000 kelimesi + kesme işaretli kısaltmalar (`can't`, `it's` …). Sıklık sırasına göre, satır başına bir kelime. | MIT (c) 2017 Derek Anderson |

`en_words.txt`, OCR'ın İngilizce metinde yapıştırdığı kelimeleri ayırmak için kullanılır (`src/text_fix.cpp`).
Derlemede `build/release/resources/` içine kopyalanır ve `.makine` paketine girer.
