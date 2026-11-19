"""
Checks pool_client.normalize_dialogue_text against the shared vectors in
spec/normalization_vectors.json (same file is used by the PHP server test).
Run: py tests/test_normalization.py
"""

import json
import os
import unittest

from _root import ROOT  # noqa: E402
from pool_client import normalize_dialogue_text

VECTORS = os.path.join(ROOT, "spec", "normalization_vectors.json")


class NormalizationVectors(unittest.TestCase):
    def test_vectors(self):
        with open(VECTORS, encoding="utf-8") as f:
            vectors = json.load(f)
        self.assertGreaterEqual(len(vectors), 15)
        for v in vectors:
            with self.subTest(input=v["input"]):
                self.assertEqual(normalize_dialogue_text(v["input"]), v["expected"])

    def test_idempotent(self):
        with open(VECTORS, encoding="utf-8") as f:
            vectors = json.load(f)
        for v in vectors:
            with self.subTest(input=v["input"]):
                self.assertEqual(normalize_dialogue_text(v["expected"]), v["expected"])


if __name__ == "__main__":
    unittest.main()
