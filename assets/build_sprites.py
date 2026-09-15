#!/usr/bin/env python3
"""Turn the raw module cut-outs into the sprites the chain builder draws.

``assets/photos_cut/`` holds top-down photographs of each M5Stack Chain module
on transparency, straight out of the background remover: full camera
resolution, inconsistent sizes, and — on the DualKey — a lanyard and clip
hanging into frame above the module.  The builder wants small, tidy, uniform
PNGs, and pywebview only serves files under the web root, so the processed
copies live in ``fethr/ui/web/img/``.

Each sprite is

* cropped to :data:`CROPS` where a fixed box is needed (see the DualKey note),
* trimmed to its alpha bounding box so the module fills the frame,
* white-balanced, because the photographs were taken under warm light and the
  modules' white shells come out pink,
* feathered by :data:`FEATHER_PX` so a hard crop line does not read as a cut,
* resized to :data:`LONGEST_SIDE` px on its longest edge, and
* written as an optimised PNG.

Run it after replacing a photograph::

    python assets/build_sprites.py            # rebuild everything
    python assets/build_sprites.py --check    # fail if anything is out of date

It needs Pillow and NumPy, both of which fethr already depends on.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = ROOT / "assets" / "photos_cut"
OUTPUT_DIR = ROOT / "fethr" / "ui" / "web" / "img"

#: Every module the builder can place, in the order the picker lists them.
SPRITES = ("dualkey", "key", "joystick", "angle", "mono", "atom")

#: The sprite the shared white balance is measured from — see
#: :func:`white_balance_gain`.  The Mono panel is the only module that is a
#: large, flat, genuinely white surface.
REFERENCE = "mono"

#: Longest edge of a finished sprite, in pixels.  The canvas draws a module at
#: 72 css px and the DualKey at 144, so 256 survives a 2x display with room to
#: spare and still keeps every sprite under ~60 kB.
LONGEST_SIDE = 256

#: Radius of the blur applied to the alpha channel's edge.  Two pixels at
#: source resolution is invisible once the image is down to 256 px, but it is
#: enough to stop a straight crop reading as a scissor line.
FEATHER_PX = 2.0

#: Fixed crop boxes, in source pixels, applied before anything else.
#:
#: The DualKey photograph was taken with the module's wrist lanyard still
#: attached; the strap and its metal clip occupy the top-left of the frame and
#: survived the background removal, so an alpha-bounding-box trim alone keeps
#: them.  The box below starts just above the module's own top edge (the body
#: runs from y≈420 to the bottom of an 803x791 image) and drops the lot.
CROPS: dict[str, tuple[int, int, int, int]] = {
    "dualkey": (4, 414, 801, 791),
}

#: Minimum alpha for a pixel to count as "the module" when trimming.  The cut
#: outs have a soft halo of near-transparent background pixels; trimming on
#: ``> 0`` would keep all of it.
TRIM_THRESHOLD = 24


def load(name: str) -> Image.Image:
    """Open one source cut-out as RGBA."""
    path = SOURCE_DIR / f"{name}.png"
    if not path.is_file():
        raise FileNotFoundError(path)
    return Image.open(path).convert("RGBA")


def trim(image: Image.Image, threshold: int = TRIM_THRESHOLD) -> Image.Image:
    """Crop away rows and columns that are effectively transparent."""
    mask = image.getchannel("A").point(lambda v: 255 if v >= threshold else 0)
    box = mask.getbbox()
    return image.crop(box) if box else image


def measure_cast(image: Image.Image, cap: float = 1.7) -> np.ndarray:
    """Estimate the channel gains that would make ``image``'s whites white.

    The white-patch estimate: look at the brightest fifth of the opaque pixels
    — on a photograph of a white-shelled module that is the shell and its
    highlights, which we know should be neutral — and work out how much the two
    weaker channels have to be lifted to match the strongest one.  Gains are
    only ever ``>= 1`` and are capped at ``cap``, so a correction can brighten
    but never darken.
    """
    array = np.asarray(image.convert("RGBA"), dtype=np.float32)
    rgb, alpha = array[..., :3], array[..., 3]
    opaque = rgb[alpha > 200]
    if opaque.shape[0] < 256:
        return np.ones(3, dtype=np.float32)

    luma = opaque @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    highlights = opaque[luma >= np.percentile(luma, 80)]
    level = np.percentile(highlights, 90, axis=0)
    return np.clip(level.max() / np.maximum(level, 1.0), 1.0, cap).astype(np.float32)


#: Saturation band over which the correction fades out.  Below the first value
#: a pixel is treated as neutral and fully corrected; above the second it is
#: coloured plastic and left alone.  Measured on the *corrected* pixel: a warm
#: white shell only looks saturated until the cast is taken off it.
NEUTRAL_BAND = (0.15, 0.40)


def apply_gain(image: Image.Image, gain: np.ndarray) -> Image.Image:
    """Neutralise the light without repainting the plastic.

    Multiplying every pixel by the white-balance gain does fix the shells, but
    it also lifts blue past green in the red keycaps and turns M5Stack's red
    into pink — the cast is not the only reason those pixels are warm; they are
    warm because the caps are red.

    So the gain is applied in full only where the result comes out neutral, and
    faded out across :data:`NEUTRAL_BAND` as the corrected pixel gets more
    saturated.  White shells go white, red caps stay red, and the translucent
    housings in between get a proportionate share.
    """
    array = np.asarray(image.convert("RGBA"), dtype=np.float32)
    rgb = array[..., :3]
    corrected = np.clip(rgb * gain, 0, 255)

    top = corrected.max(axis=-1)
    saturation = (top - corrected.min(axis=-1)) / np.maximum(top, 1.0)
    low, high = NEUTRAL_BAND
    weight = np.clip((high - saturation) / (high - low), 0.0, 1.0)[..., None]

    array[..., :3] = rgb * (1.0 - weight) + corrected * weight
    return Image.fromarray(array.astype(np.uint8), "RGBA")


def feather(image: Image.Image, radius: float = FEATHER_PX) -> Image.Image:
    """Soften the alpha edge, including any introduced by a fixed crop.

    The blur is intersected with a slightly inset rectangle so the very edge of
    the canvas always fades out: blurring the alpha alone leaves a crop line
    fully opaque, because there is nothing transparent on the far side of it to
    blur into.
    """
    alpha = image.getchannel("A")
    inset = max(1, int(round(radius)))
    frame = Image.new("L", image.size, 0)
    ImageDraw.Draw(frame).rectangle(
        (inset, inset, image.width - 1 - inset, image.height - 1 - inset), fill=255
    )
    softened = Image.composite(alpha, Image.new("L", image.size, 0), frame)
    softened = softened.filter(ImageFilter.GaussianBlur(radius))
    # darker() = per-pixel minimum: feathering may only ever remove alpha.
    image.putalpha(ImageChops.darker(alpha, softened))
    return image


def fit(image: Image.Image, longest: int = LONGEST_SIDE) -> Image.Image:
    """Scale so the longest edge is ``longest`` px, never scaling up."""
    scale = longest / max(image.size)
    if scale >= 1.0:
        return image
    size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    return image.resize(size, Image.LANCZOS)


def white_balance_gain() -> np.ndarray:
    """One colour correction for the whole set, measured from :data:`REFERENCE`.

    Measuring each sprite on its own does not work here.  The Key and the
    DualKey are mostly translucent red keycap, so their brightest pixels are
    pale red rather than white; correcting *those* to neutral bleaches the red
    out of the caps.  Every one of these photographs came from the same session
    under the same lamp, so the honest thing is to measure the cast once, on the
    module that really is a flat white surface, and apply it to all of them —
    which also keeps the six sprites consistent with each other when they sit
    side by side on the canvas.
    """
    return measure_cast(trim(load(REFERENCE)))


def build_one(name: str, gain: np.ndarray | None = None) -> Image.Image:
    """Produce the finished sprite for ``name`` without writing it."""
    image = load(name)
    if name in CROPS:
        image = image.crop(CROPS[name])
    image = trim(image)
    image = apply_gain(image, white_balance_gain() if gain is None else gain)
    image = feather(image)
    return fit(image)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true",
        help="report what would change instead of writing (exit 1 if anything would)",
    )
    parser.add_argument(
        "--out", default=str(OUTPUT_DIR), metavar="DIR",
        help=f"output directory (default: {OUTPUT_DIR})",
    )
    args = parser.parse_args(argv)

    out_dir = Path(args.out)
    if not args.check:
        out_dir.mkdir(parents=True, exist_ok=True)

    gain = white_balance_gain()
    print(f"white balance  R×{gain[0]:.2f} G×{gain[1]:.2f} B×{gain[2]:.2f} "
          f"(from {REFERENCE}.png)")

    stale = 0
    for name in SPRITES:
        sprite = build_one(name, gain)
        target = out_dir / f"{name}.png"
        if args.check:
            fresh = target.is_file()
            print(f"{name:9} {sprite.width:4}x{sprite.height:<4} "
                  f"{'present' if fresh else 'MISSING'} {target}")
            stale += 0 if fresh else 1
            continue
        sprite.save(target, "PNG", optimize=True)
        print(f"{name:9} {sprite.width:4}x{sprite.height:<4} "
              f"{target.stat().st_size / 1024:6.1f} kB  {target}")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
