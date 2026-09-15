"""Second pass over the builder sprites: neutral product white, clean red.

The photos were shot under warm desk light, so the white shells came out
pink-grey and the translucent red keycaps salmon. This pass:

1. estimates the shell colour per image from its brightest low-saturation
   pixels and computes channel gains that make that patch neutral,
2. applies the gain fully to neutral pixels and fades it out as saturation
   rises (so the red keycap keeps its hue instead of drifting towards orange),
3. lifts the shell to product white (target luminance 236),
4. adds a touch of saturation to coloured pixels and a light unsharp mask.

Run after build_sprites.py:  python polish_sprites.py
Writes in place to fethr/ui/web/img/*.png (originals kept as *.orig.png once).
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

HERE = Path(__file__).resolve().parent
IMG = HERE.parent / "fethr" / "ui" / "web" / "img"
NAMES = ["dualkey", "key", "joystick", "angle", "mono", "atom"]

TARGET_WHITE = 236.0     # product-photo white on the shell
SAT_BOOST = 1.10          # colour pixels only


def shell_gain(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """Channel gains from the brightest, least-saturated opaque pixels."""
    lum = rgb.mean(axis=2)
    sat = rgb.max(axis=2) - rgb.min(axis=2)
    opaque = alpha > 200
    cand = opaque & (sat < 40)
    if cand.sum() < 200:            # e.g. the Atom (mostly black screen)
        cand = opaque & (sat < 70)
    thresh = np.percentile(lum[cand], 80)
    patch = rgb[cand & (lum >= thresh)]
    mean = patch.mean(axis=0)
    return mean.mean() / mean       # scales each channel to the grey mean


def polish(path: Path) -> None:
    orig = path.with_suffix(".orig.png")
    if not orig.exists():
        path.replace(orig)
    im = Image.open(orig).convert("RGBA")
    arr = np.asarray(im).astype(np.float32)
    rgb, a = arr[..., :3], arr[..., 3]

    gain = shell_gain(rgb, a)
    sat = (rgb.max(axis=2) - rgb.min(axis=2)) / 255.0      # 0..1
    blend = np.clip(1.0 - sat * 2.2, 0.0, 1.0)[..., None]  # 1 on whites, 0 on strong colour
    corrected = rgb * (1.0 + (gain - 1.0) * blend)

    # lift the shell to product white
    lum = corrected.mean(axis=2)
    shell = (a > 200) & (sat < 0.16)
    if shell.sum() > 200:
        cur = np.percentile(lum[shell], 85)
        lift = TARGET_WHITE / max(cur, 1.0)
        lift = float(np.clip(lift, 0.9, 1.35))
        corrected = corrected * (1.0 + (lift - 1.0) * blend)

    # saturation touch on colour pixels
    grey = corrected.mean(axis=2, keepdims=True)
    boost = 1.0 + (SAT_BOOST - 1.0) * (1.0 - blend)
    corrected = grey + (corrected - grey) * boost

    out = np.dstack([np.clip(corrected, 0, 255), a]).astype(np.uint8)
    res = Image.fromarray(out, "RGBA").filter(
        ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=3))
    res.save(path, optimize=True)
    print(f"{path.name:12s} gain R{gain[0]:.2f} G{gain[1]:.2f} B{gain[2]:.2f}")


if __name__ == "__main__":
    for n in NAMES:
        polish(IMG / f"{n}.png")
