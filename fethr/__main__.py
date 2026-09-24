"""Entry point: wire settings, engine, sidecar, tray and window together.

``python -m fethr`` (or the installed ``fethr`` command) starts everything in a
single process.  See :mod:`fethr.ui.window` for the threading model.
"""

from __future__ import annotations

import argparse
import logging
import sys
import threading
import traceback
from functools import partial
from pathlib import Path
from typing import Any

from . import __version__
from .core.dictation import DictationEngine, EngineState, EventBus
from .core.device import SidecarDevice
from .core.settings import Settings, load_settings, save_settings, settings_path
from .tray import FethrTray
from .ui.overlay import OverlayManager
from .ui.window import Api, WindowManager, run_smoke

log = logging.getLogger("fethr")

__all__ = ["FethrApp", "main"]

#: How often the top-bar status dots re-probe the two servers.
PROBE_INTERVAL_S = 30.0


class FethrApp:
    """Owns every long-lived object and mediates between them.

    The app is the only place that knows about all four subsystems, which keeps
    ``core`` free of UI imports and the UI free of protocol details.
    """

    def __init__(self, settings_file: Path | None = None) -> None:
        self.settings_file: Path = Path(settings_file) if settings_file else settings_path()
        self.settings: Settings = load_settings(self.settings_file)

        self.bus = EventBus()
        self.engine = DictationEngine(self.settings, self.bus)
        self.device = SidecarDevice(
            on_event=self._on_device_event, enabled=self.settings.sidecar_enabled
        )

        self.api = Api(self)
        self.window = WindowManager(self, self.api)
        self.overlay = OverlayManager(self)
        self.tray = FethrTray(
            on_open=self.show_window,
            on_quit=self.quit,
            get_state=lambda: self.engine.state.value,
            get_cleanup=lambda: self.settings.dictation.cleanup_enabled,
            set_cleanup=self._set_cleanup,
            set_layer=self._set_layer,
            get_layer=lambda: self.device.snapshot()["layer"] if self.device.connected else None,
            get_sidecar_enabled=lambda: self.settings.sidecar_enabled,
            set_sidecar_enabled=self.set_sidecar_enabled,
        )

        self._probe_cache: dict[str, Any] = {
            "asr": {"ok": False, "latency_ms": None, "detail": "not probed"},
            "cleanup": {"ok": False, "latency_ms": None, "detail": "not probed"},
        }
        self._stop = threading.Event()
        self._quitting = threading.Event()
        self.smoke_ok = False
        self.smoke_error = ""

        self.bus.subscribe(self._on_engine_event)

    # -- lifecycle -------------------------------------------------------

    def run(self, smoke: bool = False, debug: bool = False) -> int:
        """Start every subsystem and block on the GUI loop.

        Args:
            smoke: run the automated start-up check instead of waiting for the
                user (show the window for five seconds, then exit).
            debug: open the WebView developer tools.

        Returns:
            A process exit code — 0 on a clean run.
        """
        self.engine.start()
        if self.settings.sidecar.auto_connect:
            self.device.start()
        self.tray.start()

        threading.Thread(target=self._probe_loop, name="fethr-probe", daemon=True).start()

        self.window.create(hidden=self.settings.ui.start_minimised and not smoke)
        # Second, so the settings window stays pywebview's main window.
        self.overlay.create()
        try:
            self.window.start(partial(run_smoke, self, 5.0) if smoke else None, debug=debug)
        finally:
            self.shutdown()

        if smoke and not self.smoke_ok:
            sys.stderr.write(self.smoke_error or "smoke run ended without success\n")
            return 1
        return 0

    def quit(self) -> None:
        """Tear the app down; unblocks :meth:`run`."""
        if self._quitting.is_set():
            return
        self._quitting.set()
        # Overlay first: pywebview only returns from start() once EVERY
        # window is gone, and the overlay has no close button of its own.
        self.overlay.destroy()
        self.window.destroy()

    def shutdown(self) -> None:
        """Stop threads and persist settings.  Idempotent."""
        if self._stop.is_set():
            return
        self._stop.set()
        self.overlay.destroy()
        self.engine.stop()
        self.device.stop()
        self.tray.stop()
        try:
            save_settings(self.settings, self.settings_file)
        except OSError:
            log.warning("could not persist settings", exc_info=True)
        self.bus.close()

    # -- window ----------------------------------------------------------

    def show_window(self) -> None:
        """Reveal the settings window (tray click, or first run)."""
        self.window.show()

    def hide_window(self) -> None:
        """Hide the settings window; the app stays live in the tray."""
        self.window.hide()

    # -- settings --------------------------------------------------------

    def apply_settings(self, patch: dict[str, Any]) -> Settings:
        """Merge ``patch`` into settings, persist, and re-arm the engine."""
        self.settings.update(patch)
        save_settings(self.settings, self.settings_file)
        self.engine.apply_settings(self.settings)
        self.overlay.apply_settings()
        self.tray.refresh()
        return self.settings

    def _set_cleanup(self, enabled: bool) -> None:
        self.apply_settings({"dictation": {"cleanup_enabled": bool(enabled)}})
        self.window.push({"type": "settings", "settings": self.settings.to_dict()})

    def _set_layer(self, index: int) -> None:
        try:
            self.device.set_layer(index)
        except Exception as exc:
            log.info("layer switch failed: %s", exc)
        self.tray.refresh()

    def set_sidecar_layout(self, layout: dict[str, Any]) -> dict[str, Any]:
        """Store the chain builder's canvas and persist it.

        Assigned whole rather than merged through :meth:`apply_settings`: the
        layout is one document whose keys are whichever modules are plugged in,
        so there is nothing to patch field by field.  It is normalised on the
        way in, which means a hand-edited settings.json cannot put the builder
        into a state it has no way to draw.
        """
        from .core.layout import normalise_layout

        clean = normalise_layout(layout)
        self.settings.sidecar.layout = clean
        save_settings(self.settings, self.settings_file)
        return clean

    def set_sidecar_enabled(self, enabled: bool) -> None:
        """Flip the sidecar master switch live, from the tray or the UI.

        Persists the setting, starts/stops the bridge's supervisor thread
        in place (no app restart) and refreshes every surface that shows
        sidecar state.
        """
        enabled = bool(enabled)
        self.apply_settings({"sidecar_enabled": enabled})
        self.device.set_enabled(enabled)
        self.tray.refresh()
        self.window.push({"type": "settings", "settings": self.settings.to_dict()})
        self.window.push({"type": "status", "status": self.status_snapshot()})

    # -- status ----------------------------------------------------------

    def status_snapshot(self) -> dict[str, Any]:
        """Everything the top status bar renders."""
        return {
            "asr": self._probe_cache["asr"],
            "cleanup": self._probe_cache["cleanup"],
            "sidecar": self.device.snapshot(),
            "engine": self.engine.snapshot(),
        }

    def probe_now(self) -> dict[str, Any]:
        """Probe both servers immediately and refresh the cache."""
        self._probe_cache["asr"] = self.engine.probe_asr().as_dict()
        self._probe_cache["cleanup"] = self.engine.probe_cleanup().as_dict()
        status = self.status_snapshot()
        self.window.push({"type": "status", "status": status})
        return status

    def _probe_loop(self) -> None:
        """Re-probe the servers every :data:`PROBE_INTERVAL_S` seconds."""
        while not self._stop.is_set():
            try:
                self.probe_now()
            except Exception:
                log.debug("probe failed", exc_info=True)
            self._stop.wait(PROBE_INTERVAL_S)

    # -- event plumbing --------------------------------------------------

    def _on_engine_event(self, event: dict[str, Any]) -> None:
        """Relay engine events to the tray icon, the sidecar and the page.

        The device gets every engine state so its panel can show a spinner
        while the server is working and a checkmark when the text has actually
        landed — neither of which it can infer from the key it sent.  The push
        is fire-and-forget and a no-op when nothing is plugged in.
        """
        if event.get("type") == "state":
            state = str(event.get("state", EngineState.IDLE.value))
            self.tray.set_state(state)
            self.device.send_state(state)
        overlay = getattr(self, "overlay", None)  # absent on bare test doubles
        if overlay is not None:
            overlay.handle(event)
        self.window.push(event)

    def _on_device_event(self, event: dict[str, Any]) -> None:
        """Relay sidecar events to the page (and refresh the tray's layer radio)."""
        self.window.push({"type": "device", "event": event})
        if event.get("ev") in ("layer", "connected", "disconnected"):
            self.tray.refresh()


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="fethr",
        description="Local-first dictation: hold a key, speak, release.",
    )
    parser.add_argument("--version", action="version", version=f"fethr {__version__}")
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="start-up self-check: launch everything, show the window for five "
             "seconds, then exit (0 = healthy).",
    )
    parser.add_argument("--debug", action="store_true", help="open the WebView dev tools")
    parser.add_argument(
        "--settings", metavar="PATH", default=None,
        help="use an alternative settings.json (testing / portable installs)",
    )
    parser.add_argument(
        "--log-level", default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="console log verbosity (default: INFO)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """Console-script entry point.  Returns the process exit code."""
    args = _parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
    try:
        app = FethrApp(settings_file=Path(args.settings) if args.settings else None)
        return app.run(smoke=args.smoke, debug=args.debug)
    except Exception:
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
