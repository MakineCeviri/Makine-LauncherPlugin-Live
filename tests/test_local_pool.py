"""
Yerel havuz birim testleri (local_pool.py). Ağ kullanılmaz.

    py tests/test_local_pool.py
"""

import random
import sys
import tempfile
import time

from _root import ROOT  # noqa: E402,F401
from local_pool import GamePool, LocalPool, bounded_levenshtein, keys_similar
from pool_client import compact_pool_key

FAILS = 0


def check(cond, label):
    global FAILS
    print(("  ok   " if cond else "  HATA ") + label)
    if not cond:
        FAILS += 1


def lev(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def row(i, src, tr):
    return [i, compact_pool_key(src), src, tr]


def main():
    print("[1] Mesafe ve benzerlik (sunucudaki keys_similar ile aynı kurallar)")
    rnd = random.Random(7)
    ok = True
    for _ in range(300):
        a = "".join(rnd.choice("abcde") for _ in range(rnd.randint(0, 14)))
        b = "".join(rnd.choice("abcde") for _ in range(rnd.randint(0, 14)))
        d = lev(a, b)
        for limit in (0, 2, 5, 20):
            got = bounded_levenshtein(a, b, limit)
            ok &= (got == d) if d <= limit else (got == limit + 1)
    check(ok, "sınırlı Levenshtein, tam hesapla aynı")
    check(keys_similar("isanybodythereplease", "isanybodythereplaese"), "tek harf hatası benzer")
    check(not keys_similar("yes", "yet"), "kısa anahtarlar eşleşmez")
    check(not keys_similar("gotofloor3rightnow", "gotofloor4rightnow"), "rakamı farklı satırlar eşleşmez")

    print("[2] Arama")
    p = GamePool("heavy-rain", "Heavy Rain", "r1", [
        row(1, "Is anybody there? Please help me.", "Orada kimse var mı? Lütfen yardım edin."),
        row(2, "Press X to call Jason", "Jason'ı çağırmak için X'e bas"),
        row(3, "Go to floor 3.", "3. kata çık."),
        [4, "", "bozuk", "satır"],
    ])
    check(len(p) == 3, "bozuk satır atlandı")
    h = p.lookup("Is anybody there?Please help me")
    check(h is not None and h[0] == 1 and h[2], "noktalama/boşluk farkı: tam anahtar eşleşmesi")
    h = p.lookup("Is anybody there? Plaese help me.")
    check(h is not None and h[0] == 1 and not h[2], "OCR harf hatası: benzer satır eşleşmesi")
    check(p.lookup("Go to floor 4.") is None, "rakam farklı: eşleşmez")
    check(p.lookup("Something never seen before in this game") is None, "havuzda olmayan satır: yok (sunucuya gider)")
    big = GamePool("g", "", "", [row(i, f"Line number {i} of the long dialogue tree here", f"S{i}") for i in range(20000)])
    t0 = time.perf_counter()
    for _ in range(20):
        big.lookup("A sentence that is not in the pool at all, really")
    ms = (time.perf_counter() - t0) * 1000 / 20
    check(ms < 150, f"20 000 satırda bulunamayan satır araması {ms:.1f} ms")

    print("[3] Eşitleme, kalıcılık, onay")
    d = tempfile.mkdtemp(prefix="makine_pool_")
    lp = LocalPool(d)
    check(lp.get("heavy-rain") is None and lp.lookup("heavy-rain", "Press X to call Jason") is None,
          "onaylanmamış oyunun yerel havuzu kullanılmaz")
    lp.confirm("heavy-rain", "Heavy Rain")
    st = lp.apply_sync("heavy-rain", {"rev": "r1", "name": "Heavy Rain", "items": [
        row(1, "Is anybody there? Please help me.", "Orada kimse var mı?"),
        row(2, "Press X to call Jason", "Jason'ı çağır")]})
    check(st == {"added": 2, "changed": 0, "removed": 0, "total": 2, "unchanged": 0}, f"ilk indirme {st}")
    check(lp.apply_sync("heavy-rain", {"unchanged": True, "rev": "r1"})["unchanged"] == 1, "sürüm aynı: değişiklik yok")
    st = lp.apply_sync("heavy-rain", {"rev": "r2", "items": [
        row(2, "Press X to call Jason", "Jason'ı çağırmak için X"),
        row(5, "Shaun? Where are you?", "Shaun? Neredesin?")]})
    check(st == {"added": 1, "changed": 1, "removed": 1, "total": 2, "unchanged": 0},
          f"onaylanan / düzenlenen / silinen satırlar ({st})")
    check(lp.lookup("heavy-rain", "Is anybody there? Please help me.") is None, "silinen satır artık yerelde yok")
    lp2 = LocalPool(d)
    h = lp2.lookup("heavy-rain", "Press X to call Jason")
    check(lp2.is_confirmed("heavy-rain") and lp2.rev("heavy-rain") == "r2" and h and h[1] == "Jason'ı çağırmak için X",
          "yeniden açılışta onay, sürüm ve satırlar diskten geldi")
    lp2.lookup("heavy-rain", "Press X to call Jason")
    hits = lp2.take_hits("heavy-rain")
    check(hits == {2: 2} and lp2.take_hits("heavy-rain") == {}, "gösterim sayaçları toplanıp alındı")
    lp2.return_hits("heavy-rain", hits)
    check(lp2.take_hits("heavy-rain") == {2: 2}, "gönderilemeyen sayaçlar geri kondu")
    lp2.forget("heavy-rain")
    check(not lp2.is_confirmed("heavy-rain") and LocalPool(d).get("heavy-rain") is None, "Oyun değil: yerel havuz silindi")

    print(f"\n{'BAŞARILI' if FAILS == 0 else f'{FAILS} HATA'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
