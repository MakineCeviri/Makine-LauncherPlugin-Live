# -*- coding: utf-8 -*-
"""Uygulamanın sürümü: manifest.json'dan (exe'de runtime/manifest.json, geliştirmede deponun kökü)."""

import json
import os

from makine_live.paths import ROOT, RUNTIME_DIR


def _app_version() -> str:
    for p in (os.path.join(RUNTIME_DIR, "manifest.json"), os.path.join(ROOT, "manifest.json")):
        try:
            with open(p, encoding="utf-8") as fh:
                return str(json.load(fh).get("version", "dev"))
        except (OSError, ValueError):
            continue
    return "dev"


APP_VERSION = _app_version()          # sürüm karşılaştırması bununla (sunucuya X-Makine-Version)
APP_CHANNEL = "beta"
APP_VERSION_LABEL = f"{APP_VERSION} {APP_CHANNEL}"   # ekranda: "0.1.0 beta"


APP_VERSION = _app_version()          # sürüm karşılaştırması bununla (sunucuya X-Makine-Version)
APP_CHANNEL = "beta"
APP_VERSION_LABEL = f"{APP_VERSION} {APP_CHANNEL}"   # ekranda: "0.3.1 beta"
