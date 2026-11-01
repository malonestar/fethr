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
    "resolve_input_device",
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


#: Host-API preference on Windows.  PortAudio lists every microphone once per
#: host API (MME, DirectSound, WASAPI, WDM-KS), and MME truncates names at 31
#: characters, so the raw list shows one mic four times, two of them garbled.
#: WASAPI carries the full device name and is what Windows itself uses.
_HOSTAPI_PREFERENCE = ("Windows WASAPI", "Windows DirectSound", "MME", "Windows WDM-KS")

#: PortAudio pseudo-devices that are not microphones.
_PSEUDO_DEVICE_MARKERS = ("sound mapper", "primary sound capture", "primary sound driver")


def _pick_devices(devices: list[dict[str, Any]], hostapis: list[dict[str, Any]],
                  default_index: int | None) -> list[dict[str, Any]]:
    """Pure half of :func:`list_input_devices` (unit-tested with canned data).

    Keeps input-capable devices from the single best host API that has any,
    drops PortAudio's mapper/primary pseudo-devices, and de-duplicates by name.
    """
    api_names = [str(h.get("name", "")) for h in hostapis]
    inputs = [
        (i, d) for i, d in enumerate(devices)
        if int(d.get("max_input_channels", 0) or 0) > 0
        and not any(m in str(d.get("name", "")).lower() for m in _PSEUDO_DEVICE_MARKERS)
    ]
    if not inputs:
        return []
    present = {api_names[int(d.get("hostapi", -1))] if 0 <= int(d.get("hostapi", -1)) < len(api_names)
               else "" for _, d in inputs}
    chosen = next((a for a in _HOSTAPI_PREFERENCE if a in present), None)
    if chosen is not None:
        inputs = [(i, d) for i, d in inputs
                  if 0 <= int(d.get("hostapi", -1)) < len(api_names)
                  and api_names[int(d["hostapi"])] == chosen]
    # Which name is the system default?  The default index may point at an
    # MME entry; match by name so the WASAPI twin gets the badge.
    default_name = ""
    if default_index is not None and 0 <= default_index < len(devices):
        default_name = str(devices[default_index].get("name", "")).strip()
    out: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, device in inputs:
        name = str(device.get("name", f"device {index}")).strip()
        if name in seen:
            continue
        seen.add(name)
        is_default = bool(default_name) and (
            name == default_name or name.startswith(default_name) or default_name.startswith(name)
        )
        out.append({
            "index": index,
            "name": name,
            "channels": int(device["max_input_channels"]),
            "default": is_default,
        })
    return out


def list_input_devices() -> list[dict[str, Any]]:
    """Enumerate microphones as ``{index, name, channels, default}`` dicts.

    One entry per physical microphone, full name, best host API (see
    :data:`_HOSTAPI_PREFERENCE`).  Returns an empty list when PortAudio is
    missing (e.g. a headless CI box).
    """
    try:
        import sounddevice as sd
    except Exception:
        return []
    try:
        devices = [dict(d) for d in sd.query_devices()]
        hostapis = [dict(h) for h in sd.query_hostapis()]
        default_index = sd.default.device[0] if sd.default.device else None
    except Exception as exc:
        log.debug("query_devices failed: %s", exc)
        return []
    return _pick_devices(devices, hostapis, default_index)


def resolve_input_device(name: str | None) -> int | str | None:
    """Turn a saved microphone name into something ``sounddevice`` can open.

    ``sounddevice`` matches a string against every host API's device list and
    raises when more than one entry matches — which a full WASAPI name does,
    because its MME twin is a prefix of it.  So resolve to the index of the
    entry :func:`list_input_devices` would show, and fall back to the raw name
    (then to the system default) if it is no longer present.
    """
    if not name:
        return None
    for device in list_input_devices():
        if device["name"] == name:
            return int(device["index"])
    log.info("microphone %r not found; using the system default", name)
    return None


def default_input_device() -> str | None:
    """Return the name of the system default microphone, if any."""
    for device in list_input_devices():
        if device["default"]:
            return str(device["name"])
    return None
