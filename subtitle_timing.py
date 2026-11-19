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
    Çevirisi hazır satırları kutuda gösterme sırası ve süresi:
      - Her çeviri en az, kaynak satırın oyunda ekranda kaldığı süre kadar görünür
        (çevirinin gösterildiği andan itibaren sayılır); kaynak hâlâ ekrandaysa kalır.
      - Hiçbir satır okuma süresinden kısa görünmez: 1.2 sn + 45 ms/karakter (en fazla 7 sn).
      - Gecikme birikecekse (sıradaki satır 2 sn'den fazla geride gösterilecekse) yakalama moduna geçilir: kaynak
        süresi kadar bekletme kalkar, okuma süresi %70'ine iner (en az 1 sn). Arkasında daha yeni hazır
        satır varken kaynağı 3 sn'den önce kalkmış satır atlanır; kutu oyunla senkron kalır.
"""

from __future__ import annotations

import difflib
import re
from dataclasses import dataclass, field

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
    best_text: str = ""          # en çok boşluk içeren varyant (OCR'ın en sık hatası boşluk yutmak)


class LineTracker:
    SAME_LINE = 0.80        # bu benzerliğin üstü aynı satır (uzun satırlarda OCR titremesi)
    STABLE_SIM = 0.97       # ardışık iki tur bu kadar benzerse "sabit" sayılır
    STABLE_POLLS = 2        # ~200 ms (100 ms tur)
    MAX_WAIT = 0.6          # titreme sürse bile bu kadar sonra çevir
    MISS_POLLS = 2          # bu kadar boş tur + ...
    MISS_TIME = 0.25        # ... bu kadar süre görünmezse satır kalktı
    REDISPATCH_SIM = 0.90   # çeviri gönderildikten sonra metin bu kadar değişirse yeniden çevir

    def __init__(self):
        self._next_id = 1
        self.cur: _Line | None = None
        self._misses = 0

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
            c.stable_n = c.stable_n + 1 if similarity(c.prev_key, key) >= self.STABLE_SIM else 1
            c.prev_key = key
            # Daktilo: en uzun/son hâli tut
            if len(key) >= len(c.key) * 0.9:
                c.text, c.key = text, key
            best_key = compact_key(c.best_text)
            if len(key) > len(best_key) * 1.05 + 1 or (
                    len(key) >= len(best_key) * 0.95 and text.count(" ") > c.best_text.count(" ")):
                c.best_text = text
            c.last_seen = now
        else:
            if c:
                events.append(("gone", c.id, c.last_seen))
            c = self.cur = _Line(self._next_id, text, key, now, now, key, best_text=text)
            self._next_id += 1
            events.append(("new", c.id, now))

        ready = c.stable_n >= self.STABLE_POLLS or now - c.first_seen >= self.MAX_WAIT
        if ready:
            if c.dispatched_key is None:
                c.dispatched_key = c.key
                events.append(("dispatch", c.id, c.best_text or c.text))
            elif c.key != c.dispatched_key and (
                    similarity(c.key, c.dispatched_key) < self.REDISPATCH_SIM
                    or len(c.key) > len(c.dispatched_key) * 1.1 + 3):
                c.dispatched_key = c.key
                events.append(("dispatch", c.id, c.best_text or c.text))
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
                 backlog_lag: float = 2.0, wait_older: float = 2.5, skip_after: float = 3.0,
                 catchup_ratio: float = 0.7, catchup_floor: float = 1.0):
        self.min_base, self.per_char, self.min_cap = min_base, per_char, min_cap
        self.backlog_lag = backlog_lag      # tahmini gecikme bunu aşınca yakalama moduna geç
        self.wait_older = wait_older        # yeni satır hazırken eski (çevirisi gelmemiş) satırı en fazla bekle
        self.skip_after = skip_after        # kaynağı bu kadar önce kalkmış satır, arkada yenisi varsa atlanır
        # Yakalama modunda bir satır en az: max(catchup_floor, catchup_ratio × okuma süresi)
        self.catchup_ratio, self.catchup_floor = catchup_ratio, catchup_floor
        self.entries: dict[int, _Entry] = {}
        self.current: int | None = None
        self.log: list = []                 # (t, olay, id) — testler ve hata ayıklama için

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
        rmin = self.reading_min(e.text)
        normal_end = e.shown_at + max(e.src_duration(), rmin)
        waiting = self._waiting()
        # Tahmini gecikme: bu satır normal süresini doldurursa sıradaki satır oyundakinden ne kadar geride kalır
        if waiting and normal_end - waiting[0].first_seen > self.backlog_lag:
            # Yakalama: kaynak süresi kadar bekletme yok, okuma süresi de kısalır
            return e.shown_at + max(self.catchup_floor, self.catchup_ratio * rmin)
        return normal_end

    def tick(self, now: float) -> Display | None:
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
        # Ekrandan çoktan kalkmış, arkasında yenisi hazır olan satırı atla
        while len(ready) > 1:
            head = ready[0]
            if not head.visible and now - head.last_seen > self.skip_after:
                self.log.append((now, "skip", head.id))
                del self.entries[head.id]
                ready.pop(0)
            else:
                break
        return ready[0]
