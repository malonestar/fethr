"""Build every fethr logo asset from ONE geometry definition.

Outputs (next to this file):
  logo.svg              mark + wordmark lockup (dark background)
  mark.svg              mark only, transparent
  fethr.ico             16/24/32/48/64/128/256 multi-size (mark on transparent)
  fethr.png             256 px mark, transparent
  tile.png              256 px rounded dark tile with mark (window / store icon)
  tray_idle.png         32 px mark, feather blue
  tray_recording.png    32 px mark, red
  tray_busy.png         32 px mark, amber
  mono_glyph.txt        8x8 bitmap rows for the Chain Mono panel

Run:  python build_logo.py
"""
from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent

BLUE = (77, 163, 255)      # #4DA3FF feather blue
TEAL = (46, 211, 183)      # #2ED3B7 accent
RED = (255, 71, 87)
AMBER = (255, 176, 32)
INK = (15, 17, 21)         # #0F1115 charcoal
WHITE = (245, 247, 250)

# ------------------------------------------------------------------ geometry
# Everything is defined in a 256x256 box. The mark is a quill-less feather:
# a leaf whose right edge is a smooth bulge and whose left edge is a near-
# straight spine, cut by three diagonal chevron gaps, on a curved stem.

TIP = (150.0, 26.0)
BASE = (118.0, 168.0)
STEM_END = (96.0, 236.0)
STEM_W = 9.0


def _bezier(p0, p1, p2, p3, n=48):
    pts = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        x = u**3 * p0[0] + 3 * u**2 * t * p1[0] + 3 * u * t**2 * p2[0] + t**3 * p3[0]
        y = u**3 * p0[1] + 3 * u**2 * t * p1[1] + 3 * u * t**2 * p2[1] + t**3 * p3[1]
        pts.append((x, y))
    return pts


def leaf_outline():
    """Closed polygon of the feather body (right bulge, left spine)."""
    right = _bezier(TIP, (196.0, 60.0), (190.0, 150.0), BASE)      # bulging edge
    left = _bezier(BASE, (110.0, 120.0), (118.0, 60.0), TIP)       # near-straight spine
    return right + left[1:]


def chevron_bands():
    """Three diagonal gaps (as thick line segments) rising to the right."""
    bands = []
    for cy in (78.0, 106.0, 134.0):
        # line from lower-left to upper-right across the body
        bands.append(((96.0, cy + 30.0), (200.0, cy - 18.0)))
    return bands


BAND_W = 9.0


def stem_path():
    return _bezier(BASE, (116.0, 200.0), (104.0, 214.0), STEM_END)


def stem_polygon(width=STEM_W):
    """Offset the stem centreline into a closed polygon (smooth edges when
    rasterised, unlike ImageDraw.line which leaves wedge gaps on curves)."""
    pts = stem_path()
    left, right = [], []
    h = width / 2.0
    for i, (x, y) in enumerate(pts):
        x0, y0 = pts[max(i - 1, 0)]
        x1, y1 = pts[min(i + 1, len(pts) - 1)]
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / n * h, dx / n * h
        left.append((x + nx, y + ny))
        right.append((x - nx, y - ny))
    # round cap at the end
    ex, ey = pts[-1]
    x0, y0 = pts[-2]
    ang = math.atan2(ey - y0, ex - x0)
    cap = [(ex + h * math.cos(ang + a), ey + h * math.sin(ang + a))
           for a in [(-math.pi / 2) + (math.pi * k / 8) for k in range(9)]]
    return left + cap + right[::-1]


# ------------------------------------------------------------------ raster
def render_mark(size: int, color, bg=None, tile=False, ss=8) -> Image.Image:
    """Render the mark at `size` px using `ss`x supersampling."""
    S = size * ss
    k = S / 256.0
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    if tile:
        r = int(56 * k)
        d.rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=(*INK, 255))
    elif bg is not None:
        d.rectangle([0, 0, S, S], fill=(*bg, 255))

    # Slightly shrink + centre the mark inside a tile for breathing room.
    if tile:
        k2 = k * 0.78
        off = (S - 256 * k2) / 2.0
    else:
        k2 = k
        off = 0.0

    def P(p):
        return (p[0] * k2 + off, p[1] * k2 + off)

    d.polygon([P(p) for p in leaf_outline()], fill=(*color, 255))

    # Chevron gaps: erase (transparent) on plain marks, ink on tiles.
    gap = (0, 0, 0, 0) if not tile and bg is None else (*(INK if tile else bg), 255)
    for a, b in chevron_bands():
        d.line([P(a), P(b)], fill=gap, width=max(1, int(BAND_W * k2)))

    # Re-clip the bands to the leaf: draw the leaf outline mask and AND it.
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).polygon([P(p) for p in leaf_outline()], fill=255)
    stem = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(stem).polygon([P(p) for p in stem_polygon()], fill=(*color, 255))
    # keep body only where the leaf is; then overlay stem
    body = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if tile or bg is not None:
        # keep the background outside the leaf
        outside = Image.new("RGBA", (S, S), (*(INK if tile else bg), 255))
        if tile:
            outside = Image.new("RGBA", (S, S), (0, 0, 0, 0))
            ImageDraw.Draw(outside).rounded_rectangle(
                [0, 0, S - 1, S - 1], radius=int(56 * k), fill=(*INK, 255))
        body = outside
    body.paste(img, (0, 0), mask)
    body.alpha_composite(stem)

    return body.resize((size, size), Image.LANCZOS)


def build_raster():
    render_mark(256, BLUE).save(HERE / "fethr.png")
    render_mark(256, BLUE, tile=True).save(HERE / "tile.png")
    render_mark(32, BLUE).save(HERE / "tray_idle.png")
    render_mark(32, RED).save(HERE / "tray_recording.png")
    render_mark(32, AMBER).save(HERE / "tray_busy.png")

    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render_mark(s, BLUE) for s in sizes]
    frames[-1].save(HERE / "fethr.ico", format="ICO",
                    sizes=[(s, s) for s in sizes], append_images=frames[:-1])


# ------------------------------------------------------------------ svg
def _path_from_points(pts, close=True):
    d = "M %.1f %.1f " % pts[0] + " ".join("L %.1f %.1f" % p for p in pts[1:])
    return d + (" Z" if close else "")


def mark_svg_group(color_hex: str, gap_hex: str | None) -> str:
    leaf = _path_from_points(leaf_outline())
    stem = _path_from_points(stem_path(), close=False)
    bands = "".join(
        f'<line x1="{a[0]:.1f}" y1="{a[1]:.1f}" x2="{b[0]:.1f}" y2="{b[1]:.1f}"/>'
        for a, b in chevron_bands())
    # Gaps are cut with a mask so the mark stays transparent-safe.
    return f"""
  <defs>
    <mask id="cuts" maskUnits="userSpaceOnUse" x="0" y="0" width="256" height="256">
      <rect width="256" height="256" fill="white"/>
      <g stroke="black" stroke-width="{BAND_W}" stroke-linecap="butt">{bands}</g>
    </mask>
  </defs>
  <path d="{leaf}" fill="{color_hex}" mask="url(#cuts)"/>
  <path d="{stem}" fill="none" stroke="{color_hex}" stroke-width="{STEM_W}" stroke-linecap="round"/>"""


def build_svg():
    blue = "#4DA3FF"
    mark = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">{mark_svg_group(blue, None)}\n</svg>\n'
    (HERE / "mark.svg").write_text(mark, encoding="utf-8")
    # pywebview only serves files under the web root, so the UI gets its own copy.
    web_root = HERE.parent / "fethr" / "ui" / "web"
    if web_root.is_dir():
        (web_root / "mark.svg").write_text(mark, encoding="utf-8")

    lockup = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 260" width="800" height="260">
  <rect width="800" height="260" rx="28" fill="#0F1115"/>
  <g transform="translate(60 2)">{mark_svg_group(blue, None)}
  </g>
  <text x="330" y="176" font-family="Montserrat, 'Segoe UI', Inter, system-ui, sans-serif"
        font-weight="600" font-size="150" letter-spacing="-4" fill="#F5F7FA">fethr</text>
</svg>
"""
    (HERE / "logo.svg").write_text(lockup, encoding="utf-8")


# ------------------------------------------------------------------ mono glyph
MONO_ROWS = [
    ".....##.",
    "....###.",
    "...####.",
    "...####.",
    "..####..",
    "..###...",
    ".##.....",
    "#.......",
]


def build_mono_glyph():
    """8x8 feather silhouette for the Chain Mono panel (hand-drawn: a 256 px
    mark does not survive an 8 px downsample)."""
    rows = MONO_ROWS
    hexrows = ", ".join("0x%02X" % int(r.replace("#", "1").replace(".", "0"), 2) for r in rows)
    (HERE / "mono_glyph.txt").write_text(
        "\n".join(rows) + f"\n\n/* glyphs.cpp: */ {{{hexrows}}}\n", encoding="utf-8")


if __name__ == "__main__":
    build_raster()
    build_svg()
    build_mono_glyph()
    print("built:", ", ".join(p.name for p in sorted(HERE.glob("*")) if p.suffix in
                              {".png", ".ico", ".svg", ".txt"}))
