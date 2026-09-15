"""Windows master-volume control and input-device enumeration.

Everything here degrades gracefully: on a non-Windows box, or when COM/pycaw
is unavailable, :func:`available` returns False and the getters return ``None``
so the Audio page can render a disabled panel instead of crashing.

COM note: pywebview dispatches JS API calls on worker threads, so every call
initialises COM for the calling thread and tears it down again.  That is a few
hundred microseconds and keeps us free of apartment-threading surprises.
"""

from __future__ import annotations

import logging
from contextlib import contextmanager
from typing import Any, Iterator

log = logging.getLogger(__name__)

__all__ = [
    "available",
    "default_input_device",
    "get_mute",
    "get_volume",
    "list_input_devices",
    "set_mute",
    "set_volume",
    "toggle_mute",
]

try:  # pragma: no cover - Windows only
    import comtypes
    from ctypes import POINTER, cast

    from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

    _PYCAW_OK = True
except Exception:  # pragma: no cover - any non-Windows/COM failure
    comtypes = None  # type: ignore[assignment]
    _PYCAW_OK = False


def available() -> bool:
    """True if master-volume control is usable on this machine."""
    return _PYCAW_OK


@contextmanager
def _endpoint() -> Iterator[Any]:
    """Yield an ``IAudioEndpointVolume`` for the default output device."""
    if not _PYCAW_OK:
        raise RuntimeError("pycaw unavailable")
    comtypes.CoInitialize()  # type: ignore[union-attr]
    try:
        speakers = AudioUtilities.GetSpeakers()
        interface = speakers.Activate(
            IAudioEndpointVolume._iid_, comtypes.CLSCTX_ALL, None  # type: ignore[union-attr]
        )
        yield cast(interface, POINTER(IAudioEndpointVolume))
    finally:
        try:
            comtypes.CoUninitialize()  # type: ignore[union-attr]
        except Exception:
            pass


def get_volume() -> int | None:
    """Return master volume as 0–100, or ``None`` if unavailable."""
    try:
        with _endpoint() as volume:
            return int(round(volume.GetMasterVolumeLevelScalar() * 100))
    except Exception as exc:
        log.debug("get_volume failed: %s", exc)
        return None


def set_volume(percent: int) -> bool:
    """Set master volume from a 0–100 percentage.  Returns success."""
    level = max(0, min(100, int(percent))) / 100.0
    try:
        with _endpoint() as volume:
            volume.SetMasterVolumeLevelScalar(level, None)
        return True
    except Exception as exc:
        log.debug("set_volume failed: %s", exc)
        return False


def get_mute() -> bool | None:
    """Return the master mute flag, or ``None`` if unavailable."""
    try:
        with _endpoint() as volume:
            return bool(volume.GetMute())
    except Exception as exc:
        log.debug("get_mute failed: %s", exc)
        return None


def set_mute(muted: bool) -> bool:
    """Set the master mute flag.  Returns success."""
    try:
        with _endpoint() as volume:
            volume.SetMute(bool(muted), None)
        return True
    except Exception as exc:
        log.debug("set_mute failed: %s", exc)
        return False


def toggle_mute() -> bool | None:
    """Flip master mute and return the new state (``None`` on failure)."""
    current = get_mute()
    if current is None:
        return None
    return not current if set_mute(not current) else current


# --------------------------------------------------------------------------
# input devices
# --------------------------------------------------------------------------


def list_input_devices() -> list[dict[str, Any]]:
    """Enumerate microphones as ``{index, name, channels, default}`` dicts.

    Returns an empty list when PortAudio is missing (e.g. a headless CI box).
    """
    try:
        import sounddevice as sd
    except Exception:
        return []
    try:
        devices = sd.query_devices()
        default_index = sd.default.device[0] if sd.default.device else None
    except Exception as exc:
        log.debug("query_devices failed: %s", exc)
        return []
    out: list[dict[str, Any]] = []
    for index, device in enumerate(devices):
        if int(device.get("max_input_channels", 0)) <= 0:
            continue
        out.append(
            {
                "index": index,
                "name": device.get("name", f"device {index}"),
                "channels": int(device["max_input_channels"]),
                "default": index == default_index,
            }
        )
    return out


def default_input_device() -> str | None:
    """Return the name of the system default microphone, if any."""
    for device in list_input_devices():
        if device["default"]:
            return str(device["name"])
    return None
