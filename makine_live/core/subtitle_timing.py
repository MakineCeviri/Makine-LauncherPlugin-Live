"""
Makine Live — altyazı zamanlaması (TEST HARNESS, C++'a taşınacak saf mantık).

İki saf sınıf; ikisi de zamanı dışarıdan alır (monotonic saniye), Tk'ye/ağa dokunmaz:

LineTracker
    Her OCR turunda ekrandaki metni alır ve olay üretir:
      ("new", id, t)          ekranda yeni bir altyazı satırı belirdi
      ("gone", id, last_seen) satır ekrandan kalktı (son görüldüğü an)
      ("dispatch", id, text)  satır sabitlendi → çevir (daktilo efekti bitti, OCR titremesi tolere edilir)
    Aynı satırın titreyen OCR varyantları ("Iwas going" / "I was going", tek harf hataları) ve daktilo
    gibi büyüyen metin aynı satır sayılır; böylece uzun satırlar her turda "yeni satır" sanılmaz.

SubtitleScheduler
    Çevirisi hazır satırları kutuda gösterme sırası ve süresi. Amaç kutunun oyunla aynı hızda ilerlemesi:
      - Kaynak hâlâ ekrandaysa çeviri kalır.
      - Arkada hazır yeni satır YOKSA: kaynağın ekranda kaldığı süre kadar, en az okuma süresi (1.2 sn + 45 ms/karakter,
        en fazla 7 sn) kadar görünür.
      - Arkada hazır yeni satır VARSA: yalnızca kaynağın oyunda ekranda kaldığı süre kadar (en az 0.8 sn). Okuma süresi
        beklenmez; eskiden beklendiği için hızlı sahnelerde her satır gecikme biriktiriyordu. Sıradaki satır 2 sn'den
        fazla geride kalacaksa süre fark kadar kısalır (yakalama).
      - Kaynağı 3 sn önce kalkmış satır, arkasında yenisi hazırsa atlanır; 4 sn önce kalkmış ve hâlâ gösterilmemiş
        satır hiç gösterilmez (geç gelen çeviri oyundaki başka bir sahnenin üstüne düşmesin).
"""

from __future__ import annotations

from collections import deque

import difflib
import re
from dataclasses import dataclass

_WS = re.compile(r"\s+")


def compact_key(text: str) -> str:
    """Boşluktan bağımsız karşılaştırma anahtarı (OCR'ın kaybettiği boşluklar eşleşmeyi bozmasın)."""
    return _WS.sub("", text).lower()


def similarity(a: str, b: str) -> float:
    if a == b:
        return 1.0
    if not a or not b:
        return 0.0
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def reading_min(text: str, base: float = 1.2, per_char: float = 0.045, cap: float = 7.0) -> float:
    return min(cap, base + per_char * len(text or ""))


# ─────────────────────────────────────────────────────────────
# LineTracker
# ─────────────────────────────────────────────────────────────

@dataclass
class _Line:
    id: int
    text: str
    key: str
    first_seen: float
    last_seen: float
    prev_key: str
    stable_n: int = 1
    dispatched_key: str | None = None
    dispatched_text: str | None = None
    best_text: str = ""          # en çok boşluk içeren varyant (OCR'ın en sık hatası boşluk yutmak)
    grew_at: float = -1.0        # metnin en son büyüdüğü an (daktilo efekti sürüyor mu)
    max_len: int = 0             # şimdiye kadarki en uzun okuma (titreme büyüme sayılmasın)
    partial: bool = False        # daktilo bitmeden (MAX_GROW_WAIT) gönderildi: bitince tamamı da gönderilir


class LineTracker:
    SAME_LINE = 0.80        # bu benzerliğin üstü aynı satır (uzun satırlarda OCR titremesi)
    STABLE_SIM = 0.97       # ardışık iki tur bu kadar benzerse "sabit" sayılır
    STABLE_POLLS = 2        # ~200 ms (100 ms tur)
    MAX_WAIT = 0.6          # titreme sürse bile bu kadar sonra çevir
    GROW_QUIET = 0.25       # ... ama metin bu kadar süredir büyümüyorsa (daktilo efekti bitmiş)
    MAX_GROW_WAIT = 3.0     # daktilo efekti bundan uzun sürerse yarım hâliyle gönderilir, bitince tamamı
    MISS_POLLS = 2          # bu kadar boş tur + ...
    MISS_TIME = 0.25        # ... bu kadar süre görünmezse satır kalktı
    REDISPATCH_SIM = 0.90   # çeviri gönderildikten sonra metin bu kadar değişirse yeniden çevir

    def __init__(self):
        self._next_id = 1
        self.cur: _Line | None = None
        self._misses = 0

    @staticmethod
    def grows(longest: str, max_len: int, key: str) -> bool:
        """
        Daktilo efekti: yeni okuma şimdiye kadarki en uzun okumadan uzun ve onun (son iki harfi hariç) devamı.
        Bir görünüp bir kaybolan nokta ("that" / "that.") büyüme sayılmaz: en uzun okumayı aşmaz.
        """
        if len(key) <= max_len:
            return False
        head = longest[:-2] if len(longest) > 4 else longest
        return key.startswith(head)

    @staticmethod
    def same_line(a: str, b: str) -> bool:
        if a == b:
            return True
        short, long_ = (a, b) if len(a) <= len(b) else (b, a)
        # Daktilo efekti: metin baştan büyüyor
        if len(short) >= 4 and long_.startswith(short[: max(4, int(len(short) * 0.9))]):
            return True
        return similarity(a, b) >= LineTracker.SAME_LINE

    def update(self, text: str, now: float) -> list:
        """text: o turda geçerli sayılan OCR metni ('' = ekranda geçerli satır yok)."""
        events = []
        text = (text or "").strip()
        if not text:
            self._misses += 1
            c = self.cur
            if c and self._misses >= self.MISS_POLLS and now - c.last_seen >= self.MISS_TIME:
                events.append(("gone", c.id, c.last_seen))
                self.cur = None
            return events

        self._misses = 0
        key = compact_key(text)
        c = self.cur
        if c and self.same_line(c.key, key):
            # Büyüyen metin "sabit" sayılmaz: uzun satırda harf harf büyürken ardışık okumalar %97'den benzer
            # çıkıyor ve yarım satır gönderiliyordu (havuza yarım satırlar da bundan düşüyordu).
            if self.grows(c.key, c.max_len, key):
                c.stable_n = 1
                c.grew_at = now
            else:
                c.stable_n = c.stable_n + 1 if similarity(c.prev_key, key) >= self.STABLE_SIM else 1
            c.prev_key = key
            c.max_len = max(c.max_len, len(key))
            # Daktilo: en uzun/son hâli tut
            if len(key) >= len(c.key) * 0.9:
                c.text, c.key = text, key
            best_key = compact_key(c.best_text)
            if (len(key) > len(best_key) * 1.05 + 1
                    or (len(key) > len(best_key) and key.startswith(best_key[:-2]))     # devamı geldi ("right n" → "now.")
                    or (len(key) >= len(best_key) * 0.95 and text.count(" ") > c.best_text.count(" "))):
                c.best_text = text
            c.last_seen = now
        else:
            if c:
                events.append(("gone", c.id, c.last_seen))
            c = self.cur = _Line(self._next_id, text, key, now, now, key, best_text=text, max_len=len(key))
            self._next_id += 1
            events.append(("new", c.id, now))

        age = now - c.first_seen
        settled = c.stable_n >= self.STABLE_POLLS or (age >= self.MAX_WAIT and now - c.grew_at >= self.GROW_QUIET)
        if settled or age >= self.MAX_GROW_WAIT:
            out = c.best_text or c.text
            if c.dispatched_key is None:
                c.dispatched_key, c.dispatched_text = c.key, out
                c.partial = not settled
                events.append(("dispatch", c.id, out))
            elif c.key != c.dispatched_key and out != c.dispatched_text and (
                    (c.partial and settled)
                    or similarity(c.key, c.dispatched_key) < self.REDISPATCH_SIM
                    or len(c.key) > len(c.dispatched_key) * 1.1 + 3):
                c.dispatched_key, c.dispatched_text = c.key, out
                c.partial = c.partial and not settled
                events.append(("dispatch", c.id, out))
        return events

    def reset(self, now: float) -> list:
        """Takip durdu (hedef pencere önde değil vb.): ekrandaki satır kalkmış sayılır."""
        events = []
        if self.cur:
            events.append(("gone", self.cur.id, self.cur.last_seen))
            self.cur = None
        self._misses = 0
        return events


# ─────────────────────────────────────────────────────────────
# SubtitleScheduler
# ─────────────────────────────────────────────────────────────

@dataclass
class _Entry:
    id: int
    first_seen: float
    last_seen: float | None = None   # None: kaynak hâlâ ekranda
    text: str | None = None
    kind: str = "api"
    ready_at: float | None = None
    shown_at: float | None = None

    @property
    def visible(self) -> bool:
        return self.last_seen is None

    def src_duration(self) -> float:
        return 0.0 if self.last_seen is None else max(0.0, self.last_seen - self.first_seen)


@dataclass(frozen=True)
class Display:
    id: int
    text: str
    kind: str


class SubtitleScheduler:
    def __init__(self, min_base: float = 1.2, per_char: float = 0.045, min_cap: float = 7.0,
                 backlog_lag: float = 2.0, wait_older: float = 1.6, skip_after: float = 3.0,
                 drop_after: float = 4.0, switch_floor: float = 0.8, forget_after: float = 10.0):
        self.min_base, self.per_char, self.min_cap = min_base, per_char, min_cap
        self.backlog_lag = backlog_lag      # sıradaki satır bundan fazla geride kalacaksa süre kısalır
        self.wait_older = wait_older        # yeni satır hazırken eski (çevirisi gelmemiş) satırı en fazla bekle
        self.skip_after = skip_after        # kaynağı bu kadar önce kalkmış satır, arkada yenisi varsa atlanır
        self.drop_after = drop_after        # kaynağı bu kadar önce kalkmış, gösterilmemiş satır hiç gösterilmez
        self.switch_floor = switch_floor    # arkada yeni satır varken bile en az bu kadar görünür
        self.forget_after = forget_after    # çevirisi hiç gelmemiş, kaynağı kalkmış girdi bu kadar sonra silinir
        self.entries: dict[int, _Entry] = {}
        self.current: int | None = None
        self.log: deque = deque(maxlen=500)  # (t, olay, id) — testler ve hata ayıklama için

    # ── Olaylar ──

    def source_new(self, id: int, t: float):
        self.entries[id] = _Entry(id, first_seen=t)

    def source_gone(self, id: int, last_seen: float):
        e = self.entries.get(id)
        if e and e.last_seen is None:
            e.last_seen = last_seen

    def translation_ready(self, id: int, text: str, kind: str, t: float):
        e = self.entries.get(id)
        if not e or not text:
            return
        e.text, e.kind = text, kind
        if e.ready_at is None:
            e.ready_at = t

    def translation_failed(self, id: int):
        e = self.entries.get(id)
        if e and e.shown_at is None and e.text is None:
            del self.entries[id]

    def flash(self, id: int, text: str, kind: str, t: float):
        """Kaynaksız tek seferlik satır (örnek / geçmişten yeniden göster): okuma süresi kadar."""
        self.entries[id] = _Entry(id, first_seen=t, last_seen=t, text=text, kind=kind, ready_at=t)

    def clear(self):
        self.entries.clear()
        self.current = None

    # ── Zamanlama ──

    def reading_min(self, text: str) -> float:
        return reading_min(text, self.min_base, self.per_char, self.min_cap)

    def _waiting(self):
        """Gösterilmemiş, çevirisi hazır satırlar (eskiden yeniye)."""
        return [e for i, e in sorted(self.entries.items()) if e.shown_at is None and e.text]

    def lag(self, now: float) -> float:
        w = self._waiting()
        return now - w[0].first_seen if w else 0.0

    def end_time(self, e: _Entry, now: float) -> float:
        if e.visible:
            return float("inf")
        src = e.src_duration()
        waiting = [w for w in self._waiting() if w.id > e.id]
        if not waiting:
            # Sırada bir şey yok: okunabilsin diye en az okuma süresi kadar
            return e.shown_at + max(src, self.reading_min(e.text))
        # Sırada yeni satır var: oyunun hızında ilerle (kaynak ne kadar göründüyse o kadar). Sıradaki satır
        # oyundakinden backlog_lag'den fazla geride kalacaksa fark kadar kısalt.
        base = src
        lag = e.shown_at + src - waiting[0].first_seen
        if lag > self.backlog_lag:
            base = src - (lag - self.backlog_lag)
        return e.shown_at + max(self.switch_floor, base)

    def _forget_stale(self, now: float):
        """Çevirisi hiç gelmemiş (çöp, hata, OCR titremesi) ve kaynağı çoktan kalkmış girdiler birikmesin."""
        for i in [i for i, e in self.entries.items()
                  if e.shown_at is None and not e.text and e.last_seen is not None and now - e.last_seen > self.forget_after]:
            del self.entries[i]

    def tick(self, now: float) -> Display | None:
        if len(self.entries) > 8:
            self._forget_stale(now)
        if self.current is not None:
            e = self.entries.get(self.current)
            if e is None or now >= self.end_time(e, now):
                if e is not None:
                    self.log.append((now, "end", e.id))
                    del self.entries[e.id]
                self.current = None
            else:
                return Display(e.id, e.text, e.kind)

        nxt = self._pick_next(now)
        if nxt is None:
            return None
        nxt.shown_at = now
        self.current = nxt.id
        self.log.append((now, "show", nxt.id))
        # Bu satırdan eski, henüz gösterilmemiş satırlar artık bayat
        for i in [i for i, e in self.entries.items() if i < nxt.id and e.shown_at is None]:
            self.log.append((now, "drop", i))
            del self.entries[i]
        return Display(nxt.id, nxt.text, nxt.kind)

    def _pick_next(self, now: float) -> _Entry | None:
        ordered = [e for _, e in sorted(self.entries.items()) if e.shown_at is None]
        ready = [e for e in ordered if e.text]
        if not ready:
            return None
        first_ready = ready[0]
        # Daha eski bir satırın çevirisi gelmek üzereyse kısa süre bekle (sıra korunur)
        older_pending = [e for e in ordered if e.id < first_ready.id and not e.text]
        if older_pending and now - older_pending[0].first_seen < self.wait_older:
            return None
        # Ekrandan çoktan kalkmış satırı atla: arkasında yenisi hazırsa skip_after, değilse drop_after sonra
        while ready:
            head = ready[0]
            late = None if head.visible else now - head.last_seen
            if late is not None and (late > self.drop_after or (len(ready) > 1 and late > self.skip_after)):
                self.log.append((now, "skip", head.id))
                del self.entries[head.id]
                ready.pop(0)
            else:
                break
        return ready[0] if ready else None
