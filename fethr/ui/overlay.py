"""The live-transcript overlay: a small floating pill near the bottom of the screen.

While the dictation key is held the streaming core publishes ``live`` events;
this module shows them in a second pywebview window so the user can watch the
words land (and watch the cleanup pass correct them) before the paste.

The one rule that matters
-------------------------
**The overlay must never take keyboard focus.**  The paste goes to whichever
window has focus, so an overlay that activates itself would swallow the very
text it is previewing.  That is enforced in layers:

* ``focus=False`` at creation — pywebview sets ``WS_EX_NOACTIVATE`` on the form.
* After the page loads we add ``WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`` (and drop
  ``WS_EX_APPWINDOW``) ourselves, and re-assert them on every show, because
  WinForms can recompute the extended style when it touches the form.
  ``WS_EX_TOOLWINDOW`` also keeps it off the taskbar and out of Alt+Tab.
* We **never call pywebview's** ``show()`` for it: on WinForms that is
  ``Form.Show(); Form.Activate()`` — an explicit activation.  Showing and hiding
  go straight to Win32 instead: ``SetWindowPos(HWND_TOPMOST, …, SWP_NOACTIVATE |
  SWP_ASYNCWINDOWPOS)`` to place it, then ``ShowWindowAsync(SW_SHOWNOACTIVATE)``
  / ``ShowWindowAsync(SW_HIDE)``.  (It has to be ShowWindow, not
  ``SWP_SHOWWINDOW``: only ShowWindow sends ``WM_SHOWWINDOW``, and without it
  WinForms never tells WebView2 it is visible, so the page does not paint.)
  The async variants also mean the EventBus thread never waits on the GUI
  thread.
* ``--smoke`` with ``FETHR_SMOKE_SHOTS`` set compares ``GetForegroundWindow()``
  before and after showing the overlay and logs an ERROR if it changed.

Long dictations
---------------
The pill grows with the transcript.  The page measures the natural height of
its flowing text and reports it (debounced ~80 ms) through a one-method js_api,
``report_height``; :func:`overlay_size` clamps it to between one line and 40%
of the work area, and the window is moved/resized with the same
``SetWindowPos(SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)`` call used to show it, with
its bottom edge fixed :data:`OVERLAY_MARGIN` px above the taskbar so it grows
upward.  Past the cap the page scrolls, following the newest words.

The show/hide policy and the placement maths are pure functions
(:class:`OverlayLogic`, :func:`overlay_position`, :func:`overlay_size`) so
they are unit-tested without a display.
"""

from __future__ import annotations

import inspect
import json
import logging
import sys
import threading
import time
from typing import Any

from .window import WEB_ROOT

log = logging.getLogger(__name__)

__all__ = [
    "HIDE",
    "OVERLAY_HTML",
    "OverlayLogic",
    "OverlayManager",
    "SCHEDULE",
    "SHOW",
    "overlay_position",
    "overlay_size",
    "smoke_capture",
    "supported_kwargs",
]

OVERLAY_HTML = WEB_ROOT / "overlay.html"

#: Logical (96-dpi) size the hidden window is created at (placeholder only:
#: :func:`overlay_size` decides the real size every time it is shown).
OVERLAY_WIDTH = 560
OVERLAY_HEIGHT = 110
#: Gap between the pill's bottom edge and the work area's bottom (taskbar).
OVERLAY_MARGIN = 90
#: Width cap (logical px) and share of the work area it may use.
OVERLAY_MAX_WIDTH = 720
OVERLAY_WIDTH_FRACTION = 0.6
#: One line of text plus the pill's padding/border (logical px): the pill is
#: never shorter than this.  It grows with the text up to this share of the
#: work-area height, then scrolls inside.
OVERLAY_MIN_HEIGHT = 48
OVERLAY_HEIGHT_FRACTION = 0.4
#: How long the pill lingers after the text has landed.
HIDE_DELAY_S = 0.6

SHOW = "show"
HIDE = "hide"
SCHEDULE = "schedule"

#: Engine states that end an utterance.
_TERMINAL_STATES = frozenset({"pasted", "idle", "error"})


# --------------------------------------------------------------------------
# pure logic
# --------------------------------------------------------------------------


class OverlayLogic:
    """Decide when the overlay appears and disappears.

    Feed it every engine event with the current setting and a clock reading;
    it answers with :data:`SHOW`, :data:`HIDE`, :data:`SCHEDULE` (a delayed
    hide was armed — see :attr:`deadline`) or ``None`` (nothing to do).

    Hiding happens :data:`HIDE_DELAY_S` after the *later* of the terminal state
    (``pasted``/``idle``/``error``) and the ``final`` live event: every such
    event pushes the deadline out, never in.  A new ``recording`` cancels a
    pending hide so back-to-back dictations do not flicker.
    """

    def __init__(self, delay: float = HIDE_DELAY_S) -> None:
        self.delay = float(delay)
        self.visible = False
        self.deadline: float | None = None

    def feed(self, event: dict[str, Any], enabled: bool, now: float) -> str | None:
        if not enabled:
            return self.disable()
        kind = event.get("type")
        if kind == "state":
            state = event.get("state")
            if state == "recording":
                self.deadline = None
                if not self.visible:
                    self.visible = True
                    return SHOW
                return None
            if state in _TERMINAL_STATES and self.visible:
                return self._arm(now)
            return None
        if kind == "live" and event.get("final") and self.visible:
            return self._arm(now)
        return None

    def disable(self) -> str | None:
        """The setting was switched off: hide at once if showing."""
        self.deadline = None
        if self.visible:
            self.visible = False
            return HIDE
        return None

    def due(self, now: float) -> bool:
        """True (once) when a scheduled hide has come due; flips to hidden."""
        if self.visible and self.deadline is not None and now >= self.deadline:
            self.visible = False
            self.deadline = None
            return True
        return False

    def _arm(self, now: float) -> str:
        target = now + self.delay
        self.deadline = target if self.deadline is None else max(self.deadline, target)
        return SCHEDULE


def overlay_position(
    work_area: tuple[int, int, int, int],
    width: int,
    height: int,
    margin: int = OVERLAY_MARGIN,
) -> tuple[int, int]:
    """Top-left corner that centres the pill ``margin`` px above the work area's bottom.

    ``work_area`` is ``(left, top, right, bottom)`` — the primary monitor minus
    the taskbar, as ``SPI_GETWORKAREA`` reports it — and every number is in the
    same pixel space (physical, on Windows).  The result is clamped so the
    pill stays on screen even on a tiny or oddly-shaped work area.
    """
    left, top, right, bottom = (int(v) for v in work_area)
    x = left + (right - left - int(width)) // 2
    y = bottom - int(margin) - int(height)
    x = max(left, min(x, right - int(width)))
    y = max(top, min(y, bottom - int(height)))
    return x, y


def overlay_size(
    work_area: tuple[int, int, int, int],
    content_height: float | None,
    scale: float = 1.0,
) -> tuple[int, int]:
    """Physical ``(width, height)`` of the pill for a given content height.

    ``content_height`` is what the page asked for, in CSS (logical) px — the
    natural height of the flowing text plus the pill's own padding — or
    ``None`` before the page has reported anything.  ``work_area`` is in
    physical px and ``scale`` converts logical to physical.

    * width  = ``min(OVERLAY_MAX_WIDTH, 60% of the work-area width)``
    * height = the content height, never below one line
      (:data:`OVERLAY_MIN_HEIGHT`) and never above 40% of the work-area
      height; past that cap the page scrolls inside.
    """
    left, top, right, bottom = (int(v) for v in work_area)
    scale = float(scale) if scale and scale > 0 else 1.0
    area_w = max(1, right - left)
    area_h = max(1, bottom - top)
    width = min(int(round(OVERLAY_MAX_WIDTH * scale)),
                int(area_w * OVERLAY_WIDTH_FRACTION))
    cap = max(1, int(area_h * OVERLAY_HEIGHT_FRACTION))
    floor = min(int(round(OVERLAY_MIN_HEIGHT * scale)), cap)
    wanted = floor if content_height is None else int(round(float(content_height) * scale))
    height = max(floor, min(wanted, cap))
    return max(1, width), height


def supported_kwargs(func: Any, wanted: dict[str, Any]) -> dict[str, Any]:
    """Keep only the keyword arguments ``func`` actually accepts."""
    try:
        params = inspect.signature(func).parameters
    except (TypeError, ValueError):
        return dict(wanted)
    if any(p.kind is inspect.Parameter.VAR_KEYWORD for p in params.values()):
        return dict(wanted)
    return {k: v for k, v in wanted.items() if k in params}


# --------------------------------------------------------------------------
# Win32 helpers
# --------------------------------------------------------------------------

_GWL_EXSTYLE = -20
_WS_EX_TOOLWINDOW = 0x00000080
_WS_EX_APPWINDOW = 0x00040000
_WS_EX_NOACTIVATE = 0x08000000
_HWND_TOPMOST = -1
_SWP_NOSIZE, _SWP_NOMOVE, _SWP_NOACTIVATE = 0x0001, 0x0002, 0x0010
_SWP_SHOWWINDOW, _SWP_ASYNCWINDOWPOS = 0x0040, 0x4000
_SW_HIDE, _SW_SHOWNOACTIVATE = 0, 4
_SPI_GETWORKAREA = 0x0030
_DWMWA_WINDOW_CORNER_PREFERENCE, _DWMWCP_ROUND = 33, 2
_DWMWA_BORDER_COLOR = 34


def _user32() -> Any:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    ptr = ctypes.c_ssize_t
    user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.GetWindowLongPtrW.restype = ptr
    user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ptr]
    user32.SetWindowLongPtrW.restype = ptr
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wintypes.UINT]
    user32.ShowWindowAsync.argtypes = [wintypes.HWND, ctypes.c_int]
    return user32


def _work_area() -> tuple[int, int, int, int]:
    """Primary monitor work area (physical px), or a 1080p guess."""
    try:
        import ctypes
        from ctypes import wintypes

        rect = wintypes.RECT()
        if ctypes.windll.user32.SystemParametersInfoW(
                _SPI_GETWORKAREA, 0, ctypes.byref(rect), 0):
            return rect.left, rect.top, rect.right, rect.bottom
        sm = ctypes.windll.user32.GetSystemMetrics
        return 0, 0, sm(0), sm(1)  # SM_CXSCREEN, SM_CYSCREEN
    except Exception:
        return 0, 0, 1920, 1040


# --------------------------------------------------------------------------
# manager
# --------------------------------------------------------------------------


class OverlayApi:
    """The overlay page's js_api: exactly one method, nothing else exposed.

    pywebview walks the public attributes of a js_api object, so this is a
    tiny dedicated class rather than the manager itself.
    """

    def __init__(self, manager: "OverlayManager") -> None:
        self._manager = manager

    def report_height(self, height: Any) -> bool:
        """The page's natural content height in CSS px (debounced by the page)."""
        return self._manager.set_content_height(height)


class OverlayManager:
    """Owns the overlay window: create hidden, feed, show/hide without focus."""

    def __init__(self, app: Any) -> None:
        self._app = app
        self._window: Any = None
        self._hwnd: int | None = None
        self._ready = threading.Event()
        self._quitting = False
        self._lock = threading.Lock()
        self._logic = OverlayLogic()
        self._timer: threading.Timer | None = None
        self._api = OverlayApi(self)
        #: Last height the page asked for (CSS px), None = nothing reported.
        self._content_height: float | None = None
        #: Work area captured at show time (re-read on every show).
        self._area: tuple[int, int, int, int] | None = None
        self._last_rect: tuple[int, int, int, int] | None = None

    # -- lifecycle -------------------------------------------------------

    def create(self) -> Any:
        """Create the hidden window.  Must run before :func:`webview.start`."""
        import webview

        if self._window is not None:
            return self._window
        wanted = {
            "width": OVERLAY_WIDTH,
            "height": OVERLAY_HEIGHT,
            "resizable": False,
            # pywebview's default min_size is (200, 100) and WinForms enforces
            # it even against SetWindowPos, which would pad a one-line pill.
            "min_size": (200, OVERLAY_MIN_HEIGHT),
            "frameless": True,
            "easy_drag": False,
            "on_top": True,
            "hidden": True,
            "focus": False,        # pywebview's name for it
            "focusable": False,    # other forks' name, filtered out if absent
            "background_color": "#11151c",
            "text_select": False,
            "zoomable": False,
            "js_api": self._api,   # report_height: the page asks to grow
            # ``transparent`` is deliberately NOT requested: on WinForms it only
            # clears the WebView2 background and leaves the form's default grey
            # BackColor behind it.  Rounded corners come from DWM instead.
        }
        kwargs = supported_kwargs(webview.create_window, wanted)
        log.debug("overlay create_window kwargs: %s", sorted(kwargs))
        self._window = webview.create_window("fethr live", str(OVERLAY_HTML), **kwargs)
        self._window.events.loaded += self._on_loaded
        self._window.events.closing += self._on_closing
        return self._window

    def destroy(self) -> None:
        self._quitting = True
        self._cancel_timer()
        window, self._window = self._window, None
        if window is None:
            return
        try:
            window.destroy()
        except Exception:
            log.debug("overlay destroy failed", exc_info=True)

    def wait_ready(self, timeout: float = 10.0) -> bool:
        return self._ready.wait(timeout)

    @property
    def visible(self) -> bool:
        return self._logic.visible

    # -- event entry points ----------------------------------------------

    def handle(self, event: dict[str, Any]) -> None:
        """Route one engine event: update visibility and forward to the page."""
        if not isinstance(event, dict) or event.get("type") not in ("state", "live"):
            return
        with self._lock:
            action = self._logic.feed(event, self._enabled(), time.monotonic())
            showing = self._logic.visible
            deadline = self._logic.deadline
            if action == SHOW:
                # A new session starts one line tall; the page reports again
                # as soon as words land.
                self._content_height = None
        # Push before showing, so a new session's reset reaches the page
        # before the pill is on screen with the previous utterance on it.
        if showing:
            self.push(event)
        if action == SHOW:
            self.show()
        elif action == HIDE:
            self._cancel_timer()
            self.hide()
        elif action == SCHEDULE and deadline is not None:
            self._arm_timer(deadline)
        elif action is None and event.get("state") == "recording":
            self._cancel_timer()

    def apply_settings(self) -> None:
        """Re-read ``settings.ui.live_overlay`` (called after every settings change)."""
        if self._enabled():
            return
        with self._lock:
            action = self._logic.disable()
        if action == HIDE:
            self._cancel_timer()
            self.hide()

    def _enabled(self) -> bool:
        try:
            return bool(self._app.settings.ui.live_overlay)
        except Exception:
            return False

    # -- window ops ------------------------------------------------------

    def push(self, event: dict[str, Any]) -> None:
        """Deliver ``event`` to the page as ``window.fethrEvent(ev)``."""
        window = self._window
        if window is None or not self._ready.is_set():
            return
        try:
            payload = json.dumps(event)
        except (TypeError, ValueError):
            return
        script = f"window.fethrEvent && window.fethrEvent({payload})"
        try:
            runner = getattr(window, "run_js", None) or window.evaluate_js
            runner(script)
        except Exception:
            log.debug("overlay push failed", exc_info=True)

    def set_content_height(self, height: Any) -> bool:
        """Record the page's natural height (CSS px); resize if showing.

        Called from pywebview's js_api thread.  Returns True when the value
        was accepted (the page ignores the result).
        """
        try:
            value = float(height)
        except (TypeError, ValueError):
            return False
        if not value == value or value <= 0 or value > 100_000:  # NaN / junk
            return False
        with self._lock:
            self._content_height = value
            showing = self._logic.visible
        if showing:
            self._place()
        return True

    def _geometry(self, refresh_area: bool) -> tuple[int, int, int, int]:
        """``(x, y, w, h)`` in physical px for the current content height."""
        if refresh_area or self._area is None:
            self._area = _work_area()
        scale = self._scale()
        w, h = overlay_size(self._area, self._content_height, scale)
        x, y = overlay_position(self._area, w, h, int(round(OVERLAY_MARGIN * scale)))
        return x, y, w, h

    def _place(self, refresh_area: bool = False) -> None:
        """Move/resize the pill without activating it (bottom edge anchored)."""
        if self._window is None or not self._ready.is_set():
            return
        if sys.platform == "win32" and self._hwnd:
            try:
                rect = self._geometry(refresh_area)
                if rect == self._last_rect and not refresh_area:
                    return
                self._last_rect = rect
                x, y, w, h = rect
                _user32().SetWindowPos(
                    self._hwnd, _HWND_TOPMOST, x, y, w, h,
                    _SWP_NOACTIVATE | _SWP_ASYNCWINDOWPOS,
                )
            except Exception:
                log.debug("overlay resize failed", exc_info=True)
            return
        try:  # non-Windows best effort; pywebview takes logical px
            x, y, w, h = self._geometry(refresh_area)
            self._window.resize(w, h)
            self._window.move(x, y)
        except Exception:
            log.debug("overlay resize failed", exc_info=True)

    def show(self) -> None:
        """Place the pill bottom-centre and show it WITHOUT activating it."""
        if self._window is None or not self._ready.is_set():
            return
        if sys.platform == "win32" and self._hwnd:
            try:
                self._apply_styles()
                # Re-read the work area on every show: the taskbar may have
                # moved or the display changed since the last utterance.
                x, y, w, h = self._geometry(refresh_area=True)
                self._last_rect = (x, y, w, h)
                user32 = _user32()
                user32.SetWindowPos(
                    self._hwnd, _HWND_TOPMOST, x, y, w, h,
                    _SWP_NOACTIVATE | _SWP_ASYNCWINDOWPOS,
                )
                # ShowWindow (not SWP_SHOWWINDOW) because only ShowWindow
                # sends WM_SHOWWINDOW, which is what tells WinForms — and
                # through it the WebView2 control — that it is visible.
                # Without it the form shows but the page never paints.
                user32.ShowWindowAsync(self._hwnd, _SW_SHOWNOACTIVATE)
                return
            except Exception:
                log.debug("native overlay show failed", exc_info=True)
                return  # never fall back to pywebview.show(): it activates
        # Non-Windows fallback (best effort; focus behaviour is platform's).
        try:
            x, y, w, h = self._geometry(refresh_area=True)
            self._window.resize(w, h)
            self._window.move(x, y)
            self._window.show()
        except Exception:
            log.debug("overlay show failed", exc_info=True)

    def hide(self) -> None:
        if self._window is None:
            return
        if sys.platform == "win32" and self._hwnd:
            try:
                _user32().ShowWindowAsync(self._hwnd, _SW_HIDE)
                return
            except Exception:
                log.debug("native overlay hide failed", exc_info=True)
        try:
            self._window.hide()
        except Exception:
            log.debug("overlay hide failed", exc_info=True)

    # -- timers ----------------------------------------------------------

    def _arm_timer(self, deadline: float) -> None:
        self._cancel_timer()
        delay = max(0.0, deadline - time.monotonic()) + 0.01
        timer = threading.Timer(delay, self._on_timer)
        timer.daemon = True
        timer.name = "fethr-overlay-hide"
        self._timer = timer
        timer.start()

    def _cancel_timer(self) -> None:
        timer, self._timer = self._timer, None
        if timer is not None:
            timer.cancel()

    def _on_timer(self) -> None:
        with self._lock:
            due = self._logic.due(time.monotonic())
            deadline = self._logic.deadline
        if due:
            self.hide()
        elif self._logic.visible and deadline is not None:
            self._arm_timer(deadline)  # pushed out while we slept

    # -- native bits -----------------------------------------------------

    def _on_loaded(self) -> None:
        if sys.platform == "win32" and self._window is not None:
            try:
                self._hwnd = int(self._window.native.Handle.ToInt64())
            except Exception:
                log.debug("overlay hwnd unavailable", exc_info=True)
            self._apply_styles()
            self._apply_dwm()
        self._ready.set()

    def _on_closing(self) -> bool:
        # Frameless and never focused, so only a programmatic close gets here.
        return True if self._quitting else False

    def _apply_styles(self) -> None:
        if not self._hwnd:
            return
        try:
            user32 = _user32()
            style = user32.GetWindowLongPtrW(self._hwnd, _GWL_EXSTYLE)
            wanted = (style | _WS_EX_TOOLWINDOW | _WS_EX_NOACTIVATE) & ~_WS_EX_APPWINDOW
            if wanted != style:
                user32.SetWindowLongPtrW(self._hwnd, _GWL_EXSTYLE, wanted)
        except Exception:
            log.debug("overlay ex-style not applied", exc_info=True)

    def _apply_dwm(self) -> None:
        """Windows 11 rounded corners + a subtle border.  No-op elsewhere."""
        try:
            import ctypes

            dwm = ctypes.windll.dwmapi
            corner = ctypes.c_int(_DWMWCP_ROUND)
            dwm.DwmSetWindowAttribute(self._hwnd, _DWMWA_WINDOW_CORNER_PREFERENCE,
                                      ctypes.byref(corner), ctypes.sizeof(corner))
            border = ctypes.c_uint(0x0040322A)  # COLORREF of #2a3240
            dwm.DwmSetWindowAttribute(self._hwnd, _DWMWA_BORDER_COLOR,
                                      ctypes.byref(border), ctypes.sizeof(border))
        except Exception:
            log.debug("overlay DWM attributes not applied", exc_info=True)

    def _scale(self) -> float:
        try:
            import ctypes

            return ctypes.windll.user32.GetDpiForWindow(self._hwnd) / 96.0 or 1.0
        except Exception:
            return 1.0

    def native_rect(self) -> tuple[int, int, int, int] | None:
        """Screen rectangle in physical px (for the smoke capture)."""
        if not self._hwnd:
            return None
        try:
            import ctypes
            from ctypes import wintypes

            rect = wintypes.RECT()
            ctypes.windll.user32.GetWindowRect(self._hwnd, ctypes.byref(rect))
            return rect.left, rect.top, rect.right, rect.bottom
        except Exception:
            return None


#: Fake session used by the ``--smoke`` capture.
SMOKE_EVENTS: tuple[dict[str, Any], ...] = (
    {"type": "state", "state": "recording", "detail": ""},
    {"type": "live", "final": False, "mode": "clean", "transcript": {
        "sentences": [
            {"id": 0, "raw": "so um the overlay shows what you say", "clean":
             "The overlay shows what you say."},
            {"id": 1, "raw": "while you are still holding the key", "clean": None},
        ],
        "pending": "and the words",
        "tentative": "keep coming in",
    }},
)

_LONG_SENTENCES: tuple[tuple[str, str | None], ...] = (
    ("okay so this is a long dictation to check the overlay grows",
     "Okay, so this is a long dictation to check that the overlay grows."),
    ("when you keep talking for a minute or more the pill should get taller",
     "When you keep talking for a minute or more, the pill should get taller."),
    ("it grows upward from the bottom edge which stays ninety pixels above the taskbar",
     "It grows upward from the bottom edge, which stays ninety pixels above the taskbar."),
    ("once it reaches about forty percent of the screen height it stops growing",
     "Once it reaches about forty percent of the screen height, it stops growing."),
    ("after that the text scrolls inside and the newest words stay in view",
     "After that the text scrolls inside, and the newest words stay in view."),
    ("long links like https://github.com/malonestar/fethr/blob/main/fethr/ui/web/"
     "overlay.html?query=a-very-long-unbroken-token must wrap instead of overflowing",
     None),
    ("the sentences flow together as one paragraph so the wrapping looks natural "
     "rather than one block per sentence", None),
    ("the state dot sits in its own column in the top right corner so it never "
     "covers any of the words no matter how many lines there are", None),
    ("when the cleanup pass rewrites a sentence only that sentence fades and the "
     "rest of the paragraph stays exactly where it was", None),
    ("if you scroll up to reread something the overlay leaves you there until the "
     "final text arrives and then it snaps back down to the end", None),
)

#: A ~200-word session for the ``overlay_long.png`` capture (the tall state).
SMOKE_LONG_EVENTS: tuple[dict[str, Any], ...] = (
    {"type": "state", "state": "recording", "detail": ""},
    {"type": "live", "final": False, "mode": "clean", "transcript": {
        "sentences": [{"id": i, "raw": raw, "clean": clean}
                      for i, (raw, clean) in enumerate(_LONG_SENTENCES)],
        "pending": "and this is the part that is still being transcribed right now",
        "tentative": "with a few tentative words at the very end",
    }},
)

#: Three times that: past the 40% cap, so the page must scroll to the end.
SMOKE_OVERFLOW_EVENTS: tuple[dict[str, Any], ...] = (
    {"type": "state", "state": "recording", "detail": ""},
    {"type": "live", "final": False, "mode": "clean", "transcript": {
        "sentences": [{"id": i, "raw": raw, "clean": clean}
                      for i, (raw, clean) in enumerate(_LONG_SENTENCES * 3)],
        "pending": "and this is the part that is still being transcribed right now",
        "tentative": "THE NEWEST WORDS MUST BE VISIBLE",
    }},
)


def _foreground() -> int:
    try:
        import ctypes

        return int(ctypes.windll.user32.GetForegroundWindow() or 0)
    except Exception:
        return 0


def smoke_capture(manager: OverlayManager, out_path: str,
                  events: tuple[dict[str, Any], ...] = SMOKE_EVENTS) -> bool:
    """Show the overlay with a fake transcript and save a PNG of it."""
    if not manager.wait_ready(10.0):
        return False
    try:
        from PIL import ImageGrab
    except Exception:  # pragma: no cover
        return False
    with manager._lock:
        manager._content_height = None       # as a fresh SHOW would
    for event in events:
        manager.push(event)
    with manager._lock:
        manager._logic.visible = True
        manager._logic.deadline = None
    fg_before = _foreground()
    manager.show()
    try:
        # Let the page report its height (80 ms debounce + js_api hop) and
        # the async SetWindowPos land; stop early once the rect settles.
        time.sleep(0.6)
        rect, stable = manager.native_rect(), 0
        for _ in range(20):
            time.sleep(0.1)
            now = manager.native_rect()
            stable = stable + 1 if now == rect else 0
            rect = now
            if stable >= 4:
                break
        try:
            measured = manager._window.evaluate_js(
                "(function(){var t=document.getElementById('text');"
                "return {wants: window.fethrMeasure ? window.fethrMeasure() : null,"
                "gap_to_bottom: Math.round(t.scrollHeight - t.scrollTop - t.clientHeight),"
                "h_overflow: t.scrollWidth - t.clientWidth};})()")
        except Exception:
            measured = None
        log.info("overlay smoke %s: page %s, reported %s css px, window rect %s",
                 out_path, measured, manager._content_height, rect)
        fg_after = _foreground()
        if fg_after != fg_before or (fg_after and fg_after == manager._hwnd):
            log.error("overlay changed the foreground window: %s -> %s (overlay %s)",
                      fg_before, fg_after, manager._hwnd)
        else:
            log.info("overlay shown; foreground window unchanged (%s)", fg_after)
        rect = manager.native_rect()
        if rect is None:
            return False
        try:
            ImageGrab.grab(bbox=rect).save(out_path)
        except OSError as exc:
            log.warning("could not capture the overlay: %s", exc)
            return False
        return True
    finally:
        with manager._lock:
            manager._logic.visible = False
        manager.hide()
