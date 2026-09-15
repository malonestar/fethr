"""System-tray icon: the app's only permanently visible surface.

The tray runs on its own daemon thread because pywebview owns the main thread
on Windows (see :mod:`fethr.ui.window`).  pystray's Win32 backend creates its
own hidden window and message loop, so a non-main thread is fine.

The icon itself is drawn with Pillow at start-up — ``assets/fethr.ico`` is used
when present, otherwise an identical feather is generated in memory, so a bare
``git clone`` with no binary assets still looks right.  The feather is tinted by
engine state, which makes "am I recording?" answerable from the corner of the
eye.
"""

from __future__ import annotations

import logging
import threading
from pathlib import Path
from typing import Any, Callable

from PIL import Image, ImageDraw

from .core.dictation import EngineState

log = logging.getLogger(__name__)

__all__ = ["FethrTray", "STATE_COLOURS", "make_feather_image"]

ASSETS = Path(__file__).resolve().parent / "assets"

#: Feather tint per engine state — blue idle, red live, amber working.
STATE_COLOURS: dict[str, tuple[int, int, int]] = {
    EngineState.IDLE.value: (77, 163, 255),
    EngineState.RECORDING.value: (255, 71, 87),
    EngineState.TRANSCRIBING.value: (255, 184, 77),
    EngineState.CLEANING.value: (124, 224, 208),
    EngineState.PASTED.value: (61, 220, 151),
    EngineState.ERROR.value: (255, 71, 87),
}

#: Kept ASCII on purpose: these strings end up in the tray tooltip and in log
#: lines, and Windows consoles still default to cp1252.
STATE_LABELS: dict[str, str] = {
    EngineState.IDLE.value: "Idle",
    EngineState.RECORDING.value: "Recording...",
    EngineState.TRANSCRIBING.value: "Transcribing...",
    EngineState.CLEANING.value: "Cleaning up...",
    EngineState.PASTED.value: "Pasted",
    EngineState.ERROR.value: "Error",
}


def make_feather_image(size: int = 64, colour: tuple[int, int, int] = (77, 163, 255)) -> Image.Image:
    """Draw the fethr mark: a tilted feather with separated barbs and a quill.

    The feather is built upright at 4x resolution — blade outline, barb slits,
    then the shaft continuing past the blade into the quill — and only then
    rotated and downsampled, so the shaft and quill stay one continuous stroke.

    Args:
        size: square edge length in pixels.
        colour: RGB tint for the blade.

    Returns:
        An RGBA :class:`PIL.Image.Image` with a transparent background.
    """
    scale = 4  # supersample, then downscale for clean edges
    edge = size * scale
    width, height = int(edge * 0.36), int(edge * 0.96)
    blade_bottom = int(height * 0.76)
    centre = width // 2
    shaft_colour = tuple(max(0, int(c * 0.52)) for c in colour)

    work = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    pen = ImageDraw.Draw(work)

    # Blade outline: pointed at the tip, widest just below the middle.
    def half_width(t: float) -> float:
        """Half the blade width at fraction ``t`` from tip (0) to base (1)."""
        return (t ** 0.85) * ((1.0 - t) ** 0.30)

    peak = max(half_width(i / 200) for i in range(201)) or 1.0
    max_half = width * 0.47
    left: list[tuple[int, int]] = []
    right: list[tuple[int, int]] = []
    steps = 120
    for i in range(steps + 1):
        t = i / steps
        y = int(t * blade_bottom)
        half = max_half * half_width(t) / peak
        left.append((int(centre - half), y))
        right.append((int(centre + half), y))
    pen.polygon(left + right[::-1], fill=(*colour, 255))

    # Barb slits: thin transparent strokes sweeping outward and up from the
    # shaft, which is what separates a feather from a leaf.
    slit = max(2, int(edge * 0.022))
    gap = max(2, int(width * 0.10))
    for i in range(8):
        t = 0.18 + i * 0.098
        y = int(blade_bottom * t)
        rise = int(blade_bottom * 0.13)
        pen.line([(centre + gap, y), (width, y - rise)], fill=(0, 0, 0, 0), width=slit)
        pen.line([(centre - gap, y), (0, y - rise)], fill=(0, 0, 0, 0), width=slit)

    # Shaft + quill: one stroke from the tip all the way to the nib.
    pen.line(
        [(centre, int(blade_bottom * 0.04)), (centre, height - 1)],
        fill=(*shaft_colour, 255),
        width=max(2, int(edge * 0.022)),
    )

    work = work.rotate(35, resample=Image.BICUBIC, expand=True)
    canvas = Image.new("RGBA", (edge, edge), (0, 0, 0, 0))
    canvas.alpha_composite(
        work, ((edge - work.width) // 2, (edge - work.height) // 2)
    )
    return canvas.resize((size, size), Image.LANCZOS)


#: Engine state -> bundled tray asset (built by assets/build_logo.py). States not
#: listed here fall back to the idle mark; a missing file falls back to the
#: programmatic feather so the tray always has *something*.
STATE_ASSETS = {
    "recording": "tray_recording.png",
    "transcribing": "tray_busy.png",
    "cleaning": "tray_busy.png",
    "error": "tray_recording.png",
}


def load_icon(state: str = EngineState.IDLE.value, size: int = 64) -> Image.Image:
    """Return the tray image for ``state``, preferring the bundled assets."""
    colour = STATE_COLOURS.get(state, STATE_COLOURS[EngineState.IDLE.value])
    for name in (STATE_ASSETS.get(state, "tray_idle.png"), "fethr.png", "fethr.ico"):
        path = ASSETS / name
        if path.is_file():
            try:
                return Image.open(path).convert("RGBA").resize(
                    (size, size), Image.LANCZOS)
            except Exception:  # pragma: no cover - corrupt asset
                log.debug("could not load %s", path, exc_info=True)
    return make_feather_image(size, colour)


class FethrTray:
    """pystray wrapper driven by callbacks supplied by the app controller."""

    def __init__(
        self,
        on_open: Callable[[], None],
        on_quit: Callable[[], None],
        get_state: Callable[[], str],
        get_cleanup: Callable[[], bool],
        set_cleanup: Callable[[bool], None],
        set_layer: Callable[[int], None],
        get_layer: Callable[[], int | None],
        get_sidecar_enabled: Callable[[], bool],
        set_sidecar_enabled: Callable[[bool], None],
    ) -> None:
        self._on_open = on_open
        self._on_quit = on_quit
        self._get_state = get_state
        self._get_cleanup = get_cleanup
        self._set_cleanup = set_cleanup
        self._set_layer = set_layer
        self._get_layer = get_layer
        self._get_sidecar_enabled = get_sidecar_enabled
        self._set_sidecar_enabled = set_sidecar_enabled
        self._icon: Any = None
        self._thread: threading.Thread | None = None
        self._state = EngineState.IDLE.value

    # -- lifecycle -------------------------------------------------------

    def start(self) -> None:
        """Create the icon and run its message loop on a daemon thread."""
        import pystray

        self._icon = pystray.Icon(
            "fethr", load_icon(), "fethr - local dictation", self._build_menu(pystray)
        )
        self._thread = threading.Thread(
            target=self._icon.run, name="fethr-tray", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        """Remove the icon."""
        if self._icon is not None:
            try:
                self._icon.stop()
            except Exception:  # pragma: no cover - already gone
                pass
            self._icon = None

    # -- updates ---------------------------------------------------------

    def set_state(self, state: str) -> None:
        """Re-tint the icon and refresh the status line for ``state``."""
        if state == self._state or self._icon is None:
            return
        self._state = state
        try:
            self._icon.icon = load_icon(state)
            self._icon.title = f"fethr — {STATE_LABELS.get(state, state)}"
            self._icon.update_menu()
        except Exception:  # pragma: no cover - platform quirk
            log.debug("tray update failed", exc_info=True)

    def refresh(self) -> None:
        """Re-evaluate the dynamic menu entries (cleanup toggle, layer)."""
        if self._icon is not None:
            try:
                self._icon.update_menu()
            except Exception:  # pragma: no cover
                pass

    # -- menu ------------------------------------------------------------

    def _build_menu(self, pystray: Any) -> Any:
        """Assemble the menu.

        pystray inspects each callback's signature and rejects anything with
        more than two parameters, so the per-layer handlers are real closures
        rather than lambdas with default arguments.
        """
        item, menu = pystray.MenuItem, pystray.Menu

        def layer_item(index: int, name: str) -> Any:
            def activate(_icon: Any, _item: Any) -> None:
                self._set_layer(index)

            def is_active(_item: Any) -> bool:
                return self._get_layer() == index

            return item(f"{index}  {name}", activate, checked=is_active, radio=True)

        def state_text(_item: Any) -> str:
            return f"State: {STATE_LABELS.get(self._get_state(), 'Idle')}"

        def toggle_cleanup(_icon: Any, _item: Any) -> None:
            self._set_cleanup(not self._get_cleanup())

        def toggle_sidecar(_icon: Any, _item: Any) -> None:
            self._set_sidecar_enabled(not self._get_sidecar_enabled())

        return menu(
            item("Open fethr", lambda _icon, _item: self._on_open(), default=True),
            menu.SEPARATOR,
            item(state_text, None, enabled=False),
            item("Cleanup pass", toggle_cleanup, checked=lambda _item: self._get_cleanup()),
            item(
                "Sidecar enabled",
                toggle_sidecar,
                checked=lambda _item: self._get_sidecar_enabled(),
            ),
            item(
                "Sidecar layer",
                menu(*(layer_item(index, name)
                       for index, name in enumerate(("FLOW", "MEDIA", "EDIT", "MOUSE")))),
                visible=lambda _item: self._get_sidecar_enabled(),
            ),
            menu.SEPARATOR,
            item("Quit", lambda _icon, _item: self._on_quit()),
        )
