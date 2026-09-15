#!/usr/bin/env python3
"""Regenerate ``fethr/assets/fethr.ico`` and ``fethr.png`` from code.

The tray draws its icon programmatically anyway (see
``fethr.tray.make_feather_image``); this script just bakes the same mark into
files so Windows has a real .ico for shortcuts and the installer.

    python scripts/make_assets.py
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from fethr.tray import make_feather_image  # noqa: E402

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def main() -> int:
    """Write the PNG and multi-resolution ICO into ``fethr/assets``."""
    assets = ROOT / "fethr" / "assets"
    assets.mkdir(parents=True, exist_ok=True)

    png = make_feather_image(256)
    png.save(assets / "fethr.png")

    png.save(
        assets / "fethr.ico",
        format="ICO",
        sizes=[(size, size) for size in ICO_SIZES],
    )
    print(f"wrote {assets / 'fethr.png'} and {assets / 'fethr.ico'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
