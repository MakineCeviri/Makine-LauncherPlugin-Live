"""Testler tests/ klasöründen çalışır. Bu modül kök klasörü (pool_client, tv_link, build/release…)
içe aktarma yoluna ekler ve Windows konsolunda Türkçe çıktının hata vermemesini sağlar.

    from _root import ROOT
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
