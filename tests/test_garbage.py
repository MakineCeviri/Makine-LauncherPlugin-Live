"""
OCR süzgeçleri (makine_live/core/garbage.py): sunucunun çöp kuralıyla aynı sonuç (spec/garbage_vectors.json) ve
gerçek oyun kayıtlarından (GTA VI, Kasım 2026) satır başına arayüz / gürültü süzgeci.
Çalıştırma: py tests/test_garbage.py
"""

import json
import os
import unittest

from _root import ROOT
from makine_live.core.garbage import filter_ocr, garbage_reason


class SharedVectors(unittest.TestCase):
    def test_same_verdict_as_server(self):
        with open(os.path.join(ROOT, "spec", "garbage_vectors.json"), encoding="utf-8") as fh:
            vectors = json.load(fh)
        wrong = []
        for v in vectors:
            reason = garbage_reason(v["source"], v.get("translation", ""), v.get("source_lang", "en"))
            if bool(reason) != bool(v["garbage"]):
                wrong.append(f'{v["source"]!r}: beklenen {v["garbage"]}, gelen {reason!r} ({v.get("note", "")})')
        self.assertEqual(wrong, [])


class OcrFilter(unittest.TestCase):
    def assertKept(self, raw, expected):
        self.assertEqual(filter_ocr(raw), expected)

    def assertDropped(self, raw):
        self.assertEqual(filter_ocr(raw), "", raw)

    def test_dialogue_passes(self):
        self.assertKept("Hey! Get out the damn road!", "Hey! Get out the damn road!")
        self.assertKept("Oh, shit. holy shit!\nWhat the actual fuck?", "Oh, shit. holy shit! What the actual fuck?")
        self.assertKept("Uh.. is that a shark?", "Uh.. is that a shark?")
        self.assertKept("Aaaah! Help me!", "Aaaah! Help me!")
        self.assertKept("I was going to\ngo.", "I was going to go.")       # altyazı iki kutuya bölündü

    def test_game_hud_is_not_sent(self):
        for raw in ("RES", "RES 0", "SECURITY TRACKER", "ACCESS SECURIT TRACKER", "STRENGTH RATING",
                    "VAPID JASON'S '70 G AN A DO", "ERTIBLE 3:06pm", "KR 0", "OT9"):
            self.assertDropped(raw)

    def test_hud_next_to_subtitle_is_removed(self):
        self.assertKept("VAPID JASON'S '70\nCome over here, silly!", "Come over here, silly!")
        self.assertKept("5:30 pm\nHey! Ah, c'mere, I got something for you.", "Hey! Ah, c'mere, I got something for you.")
        self.assertKept("VRAM Budget 4672 MB / 7003 MB\nYou must be busy with all of this.",
                        "You must be busy with all of this.")

    def test_button_glyphs_and_speaker_names(self):
        self.assertKept("Ya Ya You must be busy with it all.", "You must be busy with it all.")
        self.assertKept("a a a Don't let me keep you.", "Don't let me keep you.")
        self.assertKept("TOMIKO Listen to me carefully.", "Listen to me carefully.")
        self.assertKept("TOMIKO\nListen to me carefully.", "Listen to me carefully.")
        self.assertKept("It is unfortunate when a man dies.", "It is unfortunate when a man dies.")
        self.assertKept("Oh oh, we're in trouble now.", "Oh oh, we're in trouble now.")

    def test_ocr_noise_is_not_sent(self):
        for raw in ("ee id n na do rs error a ra", "ikpuew oas on er ay non ue", "a os se ep on o a sr og ed",
                    "e e s e s a a se aai se", "wd 00:9 ee re e se", "Oyuna Devam Et",
                    "an no re ea a s na r areas", "o i ae ya one re e"):
            self.assertDropped(raw)

    def test_noise_line_does_not_spoil_dialogue(self):
        self.assertKept("ee id n na do rs error a ra\nWait, a bomb- what bomb?", "Wait, a bomb- what bomb?")


if __name__ == "__main__":
    unittest.main(verbosity=1)
