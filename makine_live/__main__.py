# -*- coding: utf-8 -*-
"""py -m makine_live: Makine Live uygulaması (tek dosyalık exe'nin de giriş noktası)."""

import sys

from makine_live.app import main

if __name__ == "__main__":
    sys.exit(main())
