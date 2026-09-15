"""The settings window: a pywebview shell around ``ui/web``.

Threading model
---------------
Windows gives pywebview no choice: the WebView2 control and its message pump
must live on the process's **main thread**, and :func:`webview.start` blocks
there for the lifetime of the app.  Everything else therefore runs around it::

    main thread     : webview.start()  ← blocks until quit
    fethr-tray      : pystray icon + menu (its own Win32 message loop)
    fethr-eventbus  : EventBus dispatcher, pushes events into the page
    fethr-sidecar-* : serial reader + reconnect supervisor
    keyboard hooks  : the dictation hot path (owned by the `keyboard` package)
    fethr-finish    : one short-lived thread per utterance

Consequences that matter:

* The window is created **hidden at start-up**, not on demand — creating a
  window from a worker thread after ``start()`` is fragile on WebView2, while
  ``show()``/``hide()`` from any thread is not.
* Closing the window hides it; only the tray's Quit really exits.
* :meth:`WindowManager.push` marshals JSON into the page with ``evaluate_js``
  and swallows errors, because events can arrive before the document is ready
  or after it has been torn down.
* Every :class:`Api` method is called on a pywebview worker thread, so they
  must be thread-safe and must never block for long.
"""

from __future__ import annotations

import json
import logging
import os
import subprocess
import sys
import threading
import time
import traceback
import webbrowser
from pathlib import Path
from typing import Any

from .. import __version__
from ..core import audio as audio_core
from ..core.device import DEFAULT_CONFIG, SidecarError
from ..core.dictation import silent_wav
from ..core.settings import settings_dir

log = logging.getLogger(__name__)

__all__ = ["Api", "WindowManager", "WEB_ROOT"]

WEB_ROOT = Path(__file__).resolve().parent / "web"
ASSETS_DIR = Path(__file__).resolve().parents[2] / "assets"


class Api:
    """The object exposed to JavaScript as ``window.pywebview.api``.

    Every method returns JSON-serialisable data and never raises: failures come
    back as ``{"ok": False, "error": "..."}`` so the page can show a toast
    instead of dying silently in the bridge.
    """

    def __init__(self, app: Any) -> None:
        self._app = app

    # -- settings --------------------------------------------------------

    def get_settings(self) -> dict[str, Any]:
        """Return the full settings document plus static app metadata."""
        settings = self._app.settings
        return {
            "settings": settings.to_dict(),
            "version": __version__,
            "settings_path": str(self._app.settings_file),
            "platform": sys.platform,
            "audio_available": audio_core.available(),
        }

    def set_settings(self, patch: dict[str, Any]) -> dict[str, Any]:
        """Apply a nested settings patch, persist it, and re-arm the engine."""
        try:
            self._app.apply_settings(patch or {})
            return {"ok": True, "settings": self._app.settings.to_dict()}
        except Exception as exc:
            log.exception("set_settings failed")
            return {"ok": False, "error": str(exc)}

    def dismiss_first_run(self) -> dict[str, Any]:
        """Clear the first-run banner permanently."""
        return self.set_settings({"first_run": False})

    def open_settings_folder(self) -> dict[str, Any]:
        """Reveal ``%APPDATA%\\fethr`` in the system file manager."""
        folder = settings_dir()
        try:
            if os.name == "nt":
                os.startfile(str(folder))  # noqa: S606 - user-initiated
            elif sys.platform == "darwin":
                subprocess.Popen(["open", str(folder)])
            else:
                subprocess.Popen(["xdg-open", str(folder)])
            return {"ok": True, "path": str(folder)}
        except Exception as exc:
            return {"ok": False, "error": str(exc), "path": str(folder)}

    def open_url(self, url: str) -> dict[str, Any]:
        """Open ``url`` in the user's default browser (http/https only)."""
        if not str(url).lower().startswith(("http://", "https://")):
            return {"ok": False, "error": "only http(s) links are allowed"}
        try:
            webbrowser.open(url)
            return {"ok": True}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    # -- engine ----------------------------------------------------------

    def get_engine_state(self) -> dict[str, Any]:
        """Return the engine snapshot used by the status pill and last-result box."""
        return self._app.engine.snapshot()

    def get_status(self) -> dict[str, Any]:
        """Return the cached server/sidecar status shown in the top bar."""
        return self._app.status_snapshot()

    def probe_servers(self) -> dict[str, Any]:
        """Force an immediate reachability probe of both servers."""
        return self._app.probe_now()

    def test_transcribe(self) -> dict[str, Any]:
        """Send the bundled sample clip to the ASR server and report the text."""
        return self._app.engine.test_transcribe(_sample_wav())

    def test_beep(self) -> dict[str, Any]:
        """Play the record-start/record-stop tone pair."""
        self._app.engine.test_beep()
        return {"ok": True}

    def repaste(self) -> dict[str, Any]:
        """Re-inject the last transcript at the current cursor."""
        return {"ok": self._app.engine.repaste()}

    # -- sidecar ---------------------------------------------------------

    def set_sidecar_enabled(self, enabled: bool) -> dict[str, Any]:
        """Flip the sidecar master switch live and persist the setting.

        Starts or stops the bridge's supervisor thread in place — no app
        restart needed — and returns the fresh status/settings so the page
        can update itself without a reload.
        """
        try:
            self._app.set_sidecar_enabled(bool(enabled))
            return {
                "ok": True,
                "status": self._app.status_snapshot(),
                "settings": self._app.settings.to_dict(),
            }
        except Exception as exc:
            log.exception("set_sidecar_enabled failed")
            return {"ok": False, "error": str(exc)}

    def device_status(self) -> dict[str, Any]:
        """Connection state, firmware, battery, uptime and enumerated nodes."""
        snapshot = self._app.device.snapshot()
        if snapshot["connected"]:
            try:
                snapshot.update(
                    {k: v for k, v in self._app.device.status().items()
                     if k not in ("ev", "id")}
                )
            except SidecarError as exc:
                snapshot["error"] = str(exc)
        return snapshot

    def device_get_config(self) -> dict[str, Any]:
        """Return the device config (firmware defaults when disconnected)."""
        device = self._app.device
        if device.connected:
            try:
                return {"ok": True, "connected": True, "config": device.get_config()}
            except SidecarError as exc:
                return {"ok": False, "connected": True, "error": str(exc),
                        "config": device.effective_config()}
        return {"ok": True, "connected": False, "config": dict(DEFAULT_CONFIG)}

    def device_set(self, path: str, value: Any) -> dict[str, Any]:
        """Set one config path in device RAM."""
        return self._device_call(lambda d: d.set_config(path, value))

    def device_save(self) -> dict[str, Any]:
        """Persist the device's config to NVS."""
        return self._device_call(lambda d: d.save())

    def device_reset(self) -> dict[str, Any]:
        """Restore the device's factory defaults."""
        return self._device_call(lambda d: d.reset_config())

    def device_identify(self) -> dict[str, Any]:
        """Flash the key LEDs white three times."""
        return self._device_call(lambda d: d.identify())

    def device_layer(self, index: int) -> dict[str, Any]:
        """Switch the device to layer ``index`` (0–3)."""
        return self._device_call(lambda d: d.set_layer(int(index)))

    def device_reconnect(self) -> dict[str, Any]:
        """Drop the port and immediately rescan for the sidecar."""
        device = self._app.device
        device.disconnect()
        ok = device.connect()
        return {"ok": ok, "error": "" if ok else device.last_error,
                "status": device.snapshot()}

    def _device_call(self, action: Any) -> dict[str, Any]:
        try:
            reply = action(self._app.device)
            return {"ok": True, "reply": reply}
        except SidecarError as exc:
            return {"ok": False, "error": str(exc)}
        except Exception as exc:  # pragma: no cover - defensive
            log.exception("device call failed")
            return {"ok": False, "error": str(exc)}

    # -- audio -----------------------------------------------------------

    def audio_get(self) -> dict[str, Any]:
        """Master volume, mute state and the microphone list."""
        return {
            "available": audio_core.available(),
            "volume": audio_core.get_volume(),
            "muted": audio_core.get_mute(),
            "devices": audio_core.list_input_devices(),
            "selected": self._app.settings.audio.input_device,
        }

    def audio_set_volume(self, percent: int) -> dict[str, Any]:
        """Set the Windows master volume (0–100)."""
        return {"ok": audio_core.set_volume(int(percent))}

    def audio_set_mute(self, muted: bool) -> dict[str, Any]:
        """Set the Windows master mute flag."""
        return {"ok": audio_core.set_mute(bool(muted)), "muted": bool(muted)}

    def audio_toggle_mute(self) -> dict[str, Any]:
        """Flip master mute and return the resulting state."""
        state = audio_core.toggle_mute()
        return {"ok": state is not None, "muted": state}

    # -- window ----------------------------------------------------------

    def hide_window(self) -> dict[str, Any]:
        """Hide the window; fethr keeps running in the tray."""
        self._app.hide_window()
        return {"ok": True}

    def quit_app(self) -> dict[str, Any]:
        """Shut the whole application down."""
        threading.Thread(target=self._app.quit, name="fethr-quit", daemon=True).start()
        return {"ok": True}


def _sample_wav() -> bytes:
    """Return the bundled test clip, falling back to one second of silence.

    A real ``jfk.wav`` next to the repo (the classic whisper.cpp sample) makes
    the self-test prove decoding as well as connectivity; without it we still
    exercise the full network round-trip with silence.
    """
    here = Path(__file__).resolve()
    candidates = [
        here.parent.parent / "assets" / "test.wav",
        here.parents[3] / "test" / "jfk.wav",
        here.parents[4] / "test" / "jfk.wav",
    ]
    for candidate in candidates:
        try:
            if candidate.is_file():
                return candidate.read_bytes()
        except OSError:  # pragma: no cover - unreadable path
            continue
    return silent_wav(1.0)


class WindowManager:
    """Creates, shows and feeds the single pywebview window."""

    def __init__(self, app: Any, api: Api) -> None:
        self._app = app
        self._api = api
        self._window: Any = None
        self._quitting = False
        self._ready = threading.Event()

    # -- lifecycle -------------------------------------------------------

    def create(self, hidden: bool = True) -> Any:
        """Create the (single) window object without starting the GUI loop."""
        import webview

        if self._window is not None:
            return self._window
        ui = self._app.settings.ui
        self._window = webview.create_window(
            "fethr",
            str(WEB_ROOT / "index.html"),
            js_api=self._api,
            width=ui.window_width,
            height=ui.window_height,
            min_size=(800, 560),
            hidden=hidden,
            background_color="#11151c",
            text_select=True,
        )
        self._window.events.closing += self._on_closing
        self._window.events.loaded += self._on_loaded
        return self._window

    def start(self, func: Any = None, debug: bool = False) -> None:
        """Run the GUI loop.  Blocks the calling (main) thread until quit."""
        import webview

        webview.start(func, debug=debug)

    def show(self) -> None:
        """Bring the window to the front, creating it if necessary."""
        if self._window is None:
            return
        try:
            self._window.show()
            self._window.restore()
        except Exception:
            log.debug("window show failed", exc_info=True)

    def hide(self) -> None:
        """Hide the window without destroying its state."""
        if self._window is None:
            return
        try:
            self._window.hide()
        except Exception:
            log.debug("window hide failed", exc_info=True)

    def destroy(self) -> None:
        """Really close the window and let :meth:`start` return."""
        self._quitting = True
        if self._window is None:
            return
        try:
            self._window.destroy()
        except Exception:
            log.debug("window destroy failed", exc_info=True)
        self._window = None

    def wait_ready(self, timeout: float = 10.0) -> bool:
        """Block until the page has fired ``loaded`` (used by the smoke test)."""
        return self._ready.wait(timeout)

    # -- page bridge -----------------------------------------------------

    def push(self, event: dict[str, Any]) -> None:
        """Deliver an event object to the page as ``window.fethrEvent(ev)``."""
        if self._window is None or not self._ready.is_set():
            return
        try:
            payload = json.dumps(event)
        except (TypeError, ValueError):
            return
        try:
            self._window.evaluate_js(
                f"window.fethrEvent && window.fethrEvent({payload})"
            )
        except Exception:
            log.debug("push failed", exc_info=True)

    # -- events ----------------------------------------------------------

    def _on_closing(self) -> bool:
        """Hide instead of closing, unless the app is genuinely quitting."""
        if self._quitting:
            return True
        threading.Thread(target=self.hide, name="fethr-hide", daemon=True).start()
        return False

    def _on_loaded(self) -> None:
        self._ready.set()
        self._apply_window_icon()

    def _apply_window_icon(self) -> None:
        """Give the native window the fethr icon (title bar + taskbar).

        pywebview has no icon parameter on Windows, so this goes straight to
        Win32: ``WM_SETICON`` on the host form's handle. Best-effort; any
        failure leaves the default icon and is logged at debug level.
        """
        if sys.platform != "win32" or self._window is None:
            return
        try:
            import ctypes

            hwnd = int(self._window.native.Handle.ToInt64())
            path = str(ASSETS_DIR / "fethr.ico")
            user32 = ctypes.windll.user32
            IMAGE_ICON, LR_LOADFROMFILE, WM_SETICON = 1, 0x10, 0x80
            for which, px in ((0, 16), (1, 32)):  # ICON_SMALL, ICON_BIG
                hicon = user32.LoadImageW(None, path, IMAGE_ICON, px, px, LR_LOADFROMFILE)
                if hicon:
                    user32.SendMessageW(hwnd, WM_SETICON, which, hicon)
        except Exception:
            log.debug("window icon not applied", exc_info=True)


def _smoke_screenshots(app: Any) -> None:
    """Optional visual capture during ``--smoke``.

    When ``FETHR_SMOKE_SHOTS`` names a directory, walk every sidebar page and
    save a PNG of the window for each (used for UI review on machines without
    a person in front of them). Silently skipped when unset or when
    ``PIL.ImageGrab`` is unavailable.
    """
    import os

    out_dir = os.environ.get("FETHR_SMOKE_SHOTS")
    if not out_dir:
        return
    try:
        from PIL import ImageGrab
    except Exception:  # pragma: no cover - Pillow without ImageGrab
        return
    os.makedirs(out_dir, exist_ok=True)
    win = app.window._window  # pywebview Window
    for page in ("dictation", "sidecar", "audio", "about"):
        win.evaluate_js(
            "document.querySelector('.nav-item[data-page=\"%s\"]').click()" % page
        )
        time.sleep(1.2)
        # pywebview reports logical pixels; ImageGrab works in physical ones.
        scale = 1.0
        try:
            import ctypes

            hwnd = int(win.native.Handle.ToInt64())
            scale = ctypes.windll.user32.GetDpiForWindow(hwnd) / 96.0
        except Exception:
            pass
        bbox = tuple(int(round(v * scale)) for v in
                     (win.x, win.y, win.x + win.width, win.y + win.height))
        ImageGrab.grab(bbox=bbox).save(os.path.join(out_dir, f"{page}.png"))


def run_smoke(app: Any, seconds: float = 5.0) -> None:
    """Smoke-test body: show the window, hold it open, then shut down.

    Runs on a pywebview worker thread started by :func:`webview.start`.  The
    outcome is recorded on the app (``smoke_ok`` / ``smoke_error``) because the
    GUI loop, not this function, owns the process exit code.
    """
    try:
        if not app.window.wait_ready(timeout=20.0):
            raise RuntimeError("the page did not finish loading within 20 s")
        app.show_window()
        time.sleep(seconds)
        _smoke_screenshots(app)
        app.window.push({"type": "smoke", "ok": True})
        app.smoke_ok = True
    except Exception:
        app.smoke_error = traceback.format_exc()
    finally:
        app.quit()
