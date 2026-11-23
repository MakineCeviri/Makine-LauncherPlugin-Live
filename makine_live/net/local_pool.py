"""
Makine Live — yerel havuz: bir oyunun ONAYLI çevirilerinin bilgisayardaki kopyası.

[TEST→C++] Saf mantık + dosya. Ağ çağrısı yapmaz; indirme pool_client.ServerClient.pool_sync ile yapılır.

Akış:
  1. Oyun algılanınca sunucuya kaç onaylı satır olduğu sorulur (sync&head=1). Varsa kullanıcıya sorulur:
     "… için hazır çeviriler bulundu. Oynayacağınız oyun bu mu?" Kullanıcı onaylarsa liste indirilir.
     Onaylanan oyunlar hatırlanır; sonraki açılışlarda sormadan eşitlenir.
  2. Satır önce burada aranır (tam anahtar, sonra sunucuyla aynı kuralla ≥%90 benzer anahtar). Bulunursa ağa çıkılmaz.
  3. Burada olmayan satırlar (onaylanmamış ya da havuzda hiç olmayan) her zaman sunucuya (translate.php) gider.
     Yalnızca onaylı satırlar indiği için yerel kopya güvenilir ve doğrulaması kolaydır.
  4. Oyun açıkken liste periyodik olarak sürümüyle (rev) sorulur. Bir satır onaylanır, düzenlenir, onayı geri alınır
     ya da silinirse sürüm değişir ve liste yeniden iner (eklenen / değişen / kalkan sayıları kayda yazılır).

Dosyalar: %APPDATA%\\MakineLive\\pool\\<slug>.json  ve  pool\\games.json (onaylanan oyunlar).
Anahtar: pool_client.compact_pool_key = sunucudaki dialogue_key (normalize edilmiş metnin harf ve rakamları).
"""

from __future__ import annotations

import json
import os
import re
import threading
import time
from typing import Dict, List, Optional, Tuple

from makine_live.net.pool_client import compact_pool_key

# Sunucudaki benzer satır kuralıyla aynı (Makine-WEB src/live/text.ts; docs/MIMARI.md §4.3)
FUZZY_MIN_KEY_LEN = 12
FUZZY_MIN_SIMILARITY = 0.9
_DIGITS = re.compile(r"\d+")


def bounded_levenshtein(a: str, b: str, limit: int) -> int:
    """Levenshtein mesafesi; limit aşılınca erken çıkar (limit + 1 döner)."""
    if abs(len(a) - len(b)) > limit:
        return limit + 1
    if len(a) > len(b):
        a, b = b, a
    prev = list(range(len(a) + 1))
    for i, cb in enumerate(b, 1):
        cur = [i] + [0] * len(a)
        best = i
        for j, ca in enumerate(a, 1):
            v = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
            cur[j] = v
            if v < best:
                best = v
        if best > limit:
            return limit + 1
        prev = cur
    return prev[-1] if prev[-1] <= limit else limit + 1


def levenshtein(a: str, b: str) -> int:
    """
    Levenshtein mesafesi, Myers'ın bit-paralel algoritmasıyla (Hyyrö 2003): kısa metnin her harfi bir bit, uzun metnin
    her harfi için birkaç tamsayı işlemi. bounded_levenshtein'in iç içe döngüsünden ~50 kat hızlı; yerel havuzda binlerce
    aday tek tek karşılaştırılırken (benzer satır araması) çeviri iş parçacıkları GIL'i tutup paneli dondurmasın diye.
    """
    if len(a) > len(b):
        a, b = b, a
    m = len(a)
    if m == 0:
        return len(b)
    peq: dict = {}
    for i, ch in enumerate(a):
        peq[ch] = peq.get(ch, 0) | (1 << i)
    full = (1 << m) - 1
    last = 1 << (m - 1)
    pv, mv, score = full, 0, m
    for ch in b:
        eq = peq.get(ch, 0)
        xv = eq | mv
        xh = (((eq & pv) + pv) ^ pv) | eq
        ph = mv | ~(xh | pv)
        mh = pv & xh
        if ph & last:
            score += 1
        elif mh & last:
            score -= 1
        ph = (ph << 1) | 1
        mh <<= 1
        pv = (mh | ~(xv | ph)) & full
        mv = ph & xv & full
    return score


def bigram_mask(key: str) -> int:
    """
    Anahtarın ikili harflerinin (bigram) 512 bitlik izi. Bir düzenleme en fazla iki ikiliyi bozar, yani
    (iz(a) & ~iz(b)).bit_count() <= 2 * mesafe(a, b). Çakışmalar yalnızca farkı küçültür: süzgeç hiçbir benzer adayı atmaz.
    """
    m = 0
    for i in range(len(key) - 1):
        m |= 1 << ((ord(key[i]) * 31 + ord(key[i + 1])) % 509)
    return m


def keys_similar(a: str, b: str) -> bool:
    if a == b:
        return True
    la, lb = len(a), len(b)
    if min(la, lb) < FUZZY_MIN_KEY_LEN:
        return False
    longest = max(la, lb)
    limit = int(longest * (1 - FUZZY_MIN_SIMILARITY) + 1e-9)
    if abs(la - lb) > limit:
        return False
    if _DIGITS.findall(a) != _DIGITS.findall(b):
        return False                         # "floor 3" / "floor 4" asla eşleşmez
    return levenshtein(a, b) <= limit


class GamePool:
    """Bir oyunun onaylı satırları (bellekte dizinli)."""

    def __init__(self, slug: str, name: str = "", rev: str = "", items: Optional[List[list]] = None,
                 synced_at: float = 0.0):
        self.slug, self.name, self.rev, self.synced_at = slug, name, rev, synced_at
        self.items: Dict[int, Tuple[str, str, str]] = {}        # id -> (anahtar, kaynak, çeviri)
        self._by_key: Dict[str, int] = {}
        self._by_len: Dict[int, List[int]] = {}
        self._fuzzy: Dict[str, Optional[int]] = {}               # benzer satır aramasının sonuçları (liste değişmez)
        self._masks: Dict[int, int] = {}                          # adayların ikili harf izi (ilk aramada hesaplanır)
        for it in items or []:
            try:
                did, key, src, tr = int(it[0]), str(it[1]), str(it[2]), str(it[3])
            except (ValueError, IndexError, TypeError):
                continue
            if not key or not tr:
                continue
            self.items[did] = (key, src, tr)
            self._by_key.setdefault(key, did)
            self._by_len.setdefault(len(key), []).append(did)

    def __len__(self):
        return len(self.items)

    def lookup(self, text: str) -> Optional[Tuple[int, str, bool]]:
        """(diyalog id, çeviri, tam eşleşme mi) ya da None."""
        key = compact_pool_key(text)
        if not key:
            return None
        did = self._by_key.get(key)
        if did is not None:
            return did, self.items[did][2], True
        n = len(key)
        if n < FUZZY_MIN_KEY_LEN:
            return None
        if key in self._fuzzy:                   # OCR aynı satırı tekrar tekrar okur: sonuç (bulunamadı dahil) hatırlanır
            best = self._fuzzy[key]
            return None if best is None else (best, self.items[best][2], False)
        d = int(n * (1 - FUZZY_MIN_SIMILARITY))
        digits = _DIGITS.findall(key)
        mask = bigram_mask(key)
        masks = self._masks
        best, best_dist = None, None
        for length in range(n - d - 1, n + d + 2):
            for cand in self._by_len.get(length, ()):
                k = self.items[cand][0]
                # keys_similar ile aynı kural, adayın uzunluğu ve rakamları önce (ucuz), mesafe bir kez
                limit = int(max(n, len(k)) * (1 - FUZZY_MIN_SIMILARITY) + 1e-9)
                if len(k) < FUZZY_MIN_KEY_LEN or abs(n - len(k)) > limit:
                    continue
                km = masks.get(cand)
                if km is None:
                    km = masks[cand] = bigram_mask(k)
                if (mask & ~km).bit_count() > 2 * limit or _DIGITS.findall(k) != digits:
                    continue
                dist = levenshtein(key, k)
                if dist <= limit and (best_dist is None or dist < best_dist):
                    best, best_dist = cand, dist
        if len(self._fuzzy) >= 5000:
            self._fuzzy.clear()
        self._fuzzy[key] = best
        if best is None:
            return None
        return best, self.items[best][2], False

    def to_json(self) -> dict:
        return {"game": self.slug, "name": self.name, "rev": self.rev, "synced_at": self.synced_at,
                "items": [[i, k, s, t] for i, (k, s, t) in sorted(self.items.items())]}


class LocalPool:
    """Oyun başına yerel havuz dosyaları + onaylanan oyunlar + gösterim sayaçları."""

    def __init__(self, data_dir: str):
        self.dir = os.path.join(data_dir, "pool")
        self._lock = threading.Lock()
        self._pools: Dict[str, GamePool] = {}
        self._hits: Dict[str, Dict[int, int]] = {}
        self._games = self._read_json(os.path.join(self.dir, "games.json")) or {}
        if not isinstance(self._games.get("confirmed"), dict):
            self._games = {"confirmed": {}}
        self.declined = set()                   # bu oturumda "Hayır" denen oyunlar

    # ── dosyalar ──
    @staticmethod
    def _read_json(path):
        try:
            with open(path, encoding="utf-8") as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return None

    def _write_json(self, path, data):
        try:
            os.makedirs(self.dir, exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(data, fh, ensure_ascii=False, separators=(",", ":"))
            os.replace(tmp, path)
        except OSError:
            pass

    def _file(self, slug):
        return os.path.join(self.dir, re.sub(r"[^a-z0-9-]", "", slug) + ".json")

    # ── onay ──
    def is_confirmed(self, slug: str) -> bool:
        return slug in self._games["confirmed"]

    def confirm(self, slug: str, name: str = ""):
        with self._lock:
            self._games["confirmed"][slug] = {"name": name, "t": time.time()}
            self.declined.discard(slug)
            data = json.loads(json.dumps(self._games))
        self._write_json(os.path.join(self.dir, "games.json"), data)

    def forget(self, slug: str):
        """Oyunun yerel havuzunu kapat ve sil (yanlış oyun onaylandıysa)."""
        with self._lock:
            self._games["confirmed"].pop(slug, None)
            self._pools.pop(slug, None)
            data = json.loads(json.dumps(self._games))
        self._write_json(os.path.join(self.dir, "games.json"), data)
        try:
            os.remove(self._file(slug))
        except OSError:
            pass

    # ── havuz ──
    def get(self, slug: str) -> Optional[GamePool]:
        """Bellekte ya da diskte varsa oyunun havuzu (yalnızca onaylanmış oyunlar)."""
        if not slug or not self.is_confirmed(slug):
            return None
        with self._lock:
            p = self._pools.get(slug)
        if p is not None:
            return p
        d = self._read_json(self._file(slug))
        if not isinstance(d, dict):
            d = {}       # henüz indirilmedi: boş havuz (her satırda diske bakılmasın; eşitleme doldurur)
        p = GamePool(slug, d.get("name", ""), d.get("rev", ""), d.get("items"), d.get("synced_at", 0.0))
        with self._lock:
            self._pools.setdefault(slug, p)
            return self._pools[slug]

    def rev(self, slug: str) -> str:
        p = self.get(slug)
        return p.rev if p else ""

    def apply_sync(self, slug: str, resp: dict) -> Dict[str, int]:
        """Sunucu yanıtını uygula. Dönüş: {"added", "changed", "removed", "total", "unchanged"}."""
        old = self.get(slug)
        if resp.get("unchanged") and old is not None:
            old.synced_at = time.time()
            return {"added": 0, "changed": 0, "removed": 0, "total": len(old), "unchanged": 1}
        new = GamePool(slug, resp.get("name", "") or (old.name if old else ""), resp.get("rev", ""), resp.get("items"),
                       time.time())
        before = old.items if old else {}
        added = sum(1 for i in new.items if i not in before)
        removed = sum(1 for i in before if i not in new.items)
        changed = sum(1 for i, v in new.items.items() if i in before and before[i][2] != v[2])
        with self._lock:
            self._pools[slug] = new
        self._write_json(self._file(slug), new.to_json())
        return {"added": added, "changed": changed, "removed": removed, "total": len(new), "unchanged": 0}

    def lookup(self, slug: str, text: str) -> Optional[Tuple[int, str, bool]]:
        p = self.get(slug)
        if p is None:
            return None
        hit = p.lookup(text)
        if hit:
            with self._lock:
                h = self._hits.setdefault(slug, {})
                h[hit[0]] = h.get(hit[0], 0) + 1
        return hit

    def take_hits(self, slug: str) -> Dict[int, int]:
        with self._lock:
            return self._hits.pop(slug, {})

    def return_hits(self, slug: str, hits: Dict[int, int]):
        """Gönderilemeyen sayaçları geri koy (ağ hatası)."""
        with self._lock:
            h = self._hits.setdefault(slug, {})
            for k, v in hits.items():
                h[k] = h.get(k, 0) + v
