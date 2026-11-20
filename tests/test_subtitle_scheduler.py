"""
Altyazı zamanlaması birim testleri (subtitle_timing.py).
Çalıştırma: py tests/test_subtitle_scheduler.py
"""

import unittest

from _root import ROOT  # noqa: E402,F401
from makine_live.core.subtitle_timing import LineTracker, SubtitleScheduler, reading_min


def run(sched, t0, t1, step=0.05):
    """t0..t1 arasında tick at; (t, gösterilen id | None) zaman çizelgesi döndür."""
    out = []
    t = t0
    while t <= t1 + 1e-9:
        d = sched.tick(t)
        out.append((round(t, 3), d.id if d else None))
        t += step
    return out


def shown_interval(timeline, id_):
    ts = [t for t, i in timeline if i == id_]
    return (ts[0], ts[-1]) if ts else None


class SchedulerTests(unittest.TestCase):
    def test_late_translation_stays_as_long_as_source_was_visible(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0)
        s.source_gone(1, 4.0)                      # kaynak 4 sn ekranda kaldı
        s.translation_ready(1, "Seni bulacağım.", "api", 4.6)   # çeviri kaynak kalktıktan sonra geldi
        tl = run(s, 4.6, 12.0)
        start, end = shown_interval(tl, 1)
        self.assertAlmostEqual(start, 4.6, places=2)
        self.assertGreaterEqual(end - start, 4.0 - 0.06)   # en az kaynağın ekranda kaldığı süre
        self.assertLess(end - start, 4.2)

    def test_reading_minimum_for_short_flash(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0)
        s.source_gone(1, 0.3)                      # kaynak yalnızca 0.3 sn göründü
        text = "Jason'a seslenmek için X'e bas."
        s.translation_ready(1, text, "api", 0.9)
        start, end = shown_interval(run(s, 0.9, 6.0), 1)
        self.assertGreaterEqual(end - start, reading_min(text) - 0.06)

    def test_stays_while_source_visible(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0)
        s.translation_ready(1, "Uzun süre ekranda kalan satır.", "verified", 0.4)
        tl = run(s, 0.4, 20.0)
        self.assertTrue(all(i == 1 for _, i in tl))       # kaynak ekrandayken hiç kaybolmaz

    def test_next_line_waits_for_current_minimum(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0); s.source_gone(1, 3.0)
        s.translation_ready(1, "Birinci satır.", "api", 1.0)   # 1.0'da gösterilir, en az 3 sn
        s.source_new(2, 3.0); s.source_gone(2, 5.0)
        s.translation_ready(2, "İkinci satır.", "api", 3.3)
        tl = run(s, 1.0, 10.0)
        a = shown_interval(tl, 1)
        b = shown_interval(tl, 2)
        self.assertGreaterEqual(a[1] - a[0], 3.0 - 0.06)
        self.assertGreaterEqual(b[0], a[1])                    # sıra korunur, üst üste binmez
        self.assertGreaterEqual(b[1] - b[0], 2.0 - 0.06)

    def test_backlog_catches_up(self):
        s = SubtitleScheduler()
        # 4 hızlı satır; her biri 2.5 sn ekranda, çeviriler 1.2 sn gecikmeli
        for i in range(4):
            t = i * 2.5
            s.source_new(i + 1, t)
            s.source_gone(i + 1, t + 2.5)
            s.translation_ready(i + 1, f"Satır {i + 1} çevirisi biraz uzun bir cümle.", "api", t + 1.2)
        tl = run(s, 1.2, 25.0)
        ends = [shown_interval(tl, i + 1) for i in range(4)]
        self.assertTrue(all(ends))
        # Gecikme sınırlı kalır: son satır, kaynağı kalktıktan en geç ~3 sn sonra gösterilmiş olmalı
        self.assertLess(ends[3][0] - (7.5 + 2.5), 3.0)
        # Yakalama modunda bile hiçbir satır okuma süresinin %70'inden (en az 1 sn) kısa gösterilmez
        floor = max(1.0, 0.7 * reading_min("Satır 1 çevirisi biraz uzun bir cümle."))
        for (st, en) in ends:
            self.assertGreaterEqual(en - st, floor - 0.06)

    def test_rapid_burst_stays_in_sync(self):
        s = SubtitleScheduler()
        # 4 satır 0.5'er sn ekranda kalıp kayboluyor; çeviriler 0.6–1.0 sn gecikmeli
        for i in range(4):
            t = 1.0 + i * 0.5
            s.source_new(i + 1, t)
            s.source_gone(i + 1, t + 0.5)
            s.translation_ready(i + 1, f"Kısa satır {i + 1}, cevap ver!", "api", t + 0.6 + 0.1 * i)
        tl = run(s, 1.6, 15.0)
        last = shown_interval(tl, 4)
        self.assertIsNotNone(last)                      # en yeni satır hiçbir zaman atlanmaz
        self.assertLess(last[0] - 3.0, 4.0)             # kaynak kalktıktan en geç ~4 sn sonra ekranda

    def test_stale_line_skipped_when_newer_ready(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0); s.source_gone(1, 1.0)
        s.translation_ready(1, "A satırı.", "api", 0.5)
        s.tick(0.5)                                          # 1 gösteriliyor
        s.source_new(2, 1.0); s.source_gone(2, 1.4)
        s.translation_ready(2, "B satırı.", "api", 1.5)
        s.source_new(3, 1.4)
        s.translation_ready(3, "C satırı.", "api", 1.6)
        tl = run(s, 0.5, 12.0)
        # 2'nin kaynağı 1.4'te kalktı; sıra geldiğinde 6 sn'den eskiyse atlanır, değilse gösterilir —
        # her iki durumda da 3 sonunda gösterilmeli
        self.assertIsNotNone(shown_interval(tl, 3))

    def test_older_pending_translation_keeps_order(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0); s.source_gone(1, 0.8)
        s.source_new(2, 0.8)
        s.translation_ready(2, "İkinci (havuzdan hızlı).", "verified", 0.9)
        self.assertIsNone(s.tick(1.0))                      # 1'in çevirisi bekleniyor
        s.translation_ready(1, "Birinci (API).", "api", 1.3)
        d = s.tick(1.35)
        self.assertEqual(d.id, 1)

    def test_failed_translation_does_not_block(self):
        s = SubtitleScheduler()
        s.source_new(1, 0.0); s.source_gone(1, 0.5)
        s.source_new(2, 0.5)
        s.translation_ready(2, "İkinci.", "api", 0.9)
        s.translation_failed(1)
        self.assertEqual(s.tick(1.0).id, 2)


class TrackerTests(unittest.TestCase):
    def feed(self, tr, seq, t0=0.0, dt=0.1):
        events = []
        t = t0
        for text in seq:
            events += [(round(t, 2),) + e for e in tr.update(text, t)]
            t += dt
        return events

    def test_stable_line_dispatched_once(self):
        tr = LineTracker()
        ev = self.feed(tr, ["I was going to find you, Shaun."] * 20)
        self.assertEqual([e[1] for e in ev], ["new", "dispatch"])
        self.assertLessEqual(ev[1][0], 0.15)                   # ~2 tur

    def test_jitter_and_glued_words_same_line(self):
        tr = LineTracker()
        variants = ["I was going to find you, Shaun. I promise. We have to leave now.",
                    "Iwas going to find you, Shaun.I promise. We have to leave now.",
                    "I was going to find yon, Shaun. I promise. We have to leave now.",
                    "I was goingto find you, Shaun. I promise. We have to leave now"]
        ev = self.feed(tr, (variants * 10)[:40])
        kinds = [e[1] for e in ev]
        self.assertEqual(kinds.count("new"), 1)
        self.assertEqual(kinds.count("gone"), 0)
        self.assertEqual(kinds.count("dispatch"), 1)

    def test_brief_ocr_dropout_is_not_gone(self):
        tr = LineTracker()
        ev = self.feed(tr, ["Where were you last night?"] * 5 + [""] + ["Where were you last night?"] * 5)
        self.assertNotIn("gone", [e[1] for e in ev])

    def test_gone_after_two_misses(self):
        tr = LineTracker()
        ev = self.feed(tr, ["Where were you last night?"] * 5 + ["", "", ""])
        gone = [e for e in ev if e[1] == "gone"]
        self.assertEqual(len(gone), 1)
        self.assertAlmostEqual(gone[0][3], 0.4, places=2)        # son görüldüğü an

    def test_typewriter_dispatches_final_text(self):
        tr = LineTracker()
        full = "Deviant located. Proceeding to intercept."
        seq = [full[:n] for n in range(6, len(full) + 1, 6)] + [full] * 4
        ev = self.feed(tr, seq)
        disp = [e for e in ev if e[1] == "dispatch"]
        self.assertEqual(disp[-1][3], full)
        self.assertEqual([e[1] for e in ev].count("new"), 1)

    def test_new_line_replaces_old(self):
        tr = LineTracker()
        ev = self.feed(tr, ["Press X to call Jason."] * 4 + ["Ethan, where are you?"] * 4)
        kinds = [e[1] for e in ev]
        self.assertEqual(kinds, ["new", "dispatch", "gone", "new", "dispatch"])


if __name__ == "__main__":
    unittest.main(verbosity=1)
