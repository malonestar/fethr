"""Host side of the fethr sidecar protocol (see ``hardware/PROTOCOL.md``).

Transport is the sidecar's USB CDC serial port: newline-delimited JSON, one
object per line, UTF-8, 115200 8N1.  Lines that do not start with ``{`` are the
device's human console and are ignored.

Every host command carries an ``id``; the device echoes it on the single reply,
which is how :meth:`SidecarDevice.request` matches replies to callers.  The
device may also emit unsolicited events (``layer``, ``hold``, ``tap``,
``chain``, ``battery``, ``boot``) at any time; those are forwarded to the
registered callback.

Threading
---------
One reader thread per connection (blocking ``readline``), plus one supervisor
thread that scans for the device and reconnects after an unplug.  Callers block
only inside :meth:`request`, on a per-request queue.
"""

from __future__ import annotations

import itertools
import json
import logging
import threading
import time
from typing import Any, Callable, Iterable

log = logging.getLogger(__name__)

__all__ = [
    "DEFAULT_CONFIG",
    "LAYER_NAMES",
    "PRODUCT_MATCH",
    "PROTO_ACTIONS",
    "STATE_VALUES",
    "SidecarDevice",
    "SidecarError",
    "VENDOR_ID",
    "is_sidecar_port",
    "parse_line",
    "scan_ports",
]

#: Espressif's USB vendor id — the DualKey is an ESP32-S3.
VENDOR_ID = 0x303A
#: Substring required in the USB product/description string.
PRODUCT_MATCH = "fethr"

#: Protocol version that added runtime layout and editable actions
#: (``PROTOCOL.md`` §"v2 additions": ``get_layers``, ``set_action``,
#: ``set_layer_meta``, ``swap_keys``, ``*_swap_xy``).  A device reporting less
#: than this can still be placed and oriented in the chain builder, but its key
#: map is whatever the firmware was built with.
PROTO_ACTIONS = 2

#: Fallback layer list for a disconnected device.  A connected one reports its
#: own in the ``hello`` reply, which is authoritative: the firmware ships the
#: FLOW layer only, and a build with ``FLOW_EXTRA_LAYERS`` on has more.
LAYER_NAMES = ("FETHR",)

#: Engine states the device understands (``PROTOCOL.md`` ``state``).  Mirrors
#: :class:`fethr.core.dictation.EngineState`; anything else is not sent.
STATE_VALUES = frozenset(
    {"idle", "recording", "transcribing", "cleaning", "pasted", "error"}
)

#: Host→device commands stay under the protocol's 512-byte line budget, but the
#: ``get_config`` reply is a whole config object and legitimately exceeds it.
#: We only use a ceiling to stop a wedged port feeding us unbounded garbage.
MAX_LINE_BYTES = 4096

#: Mirrors the firmware's compile-time defaults (``PROTOCOL.md`` §Config object).
#: Used to render the Sidecar page sensibly when nothing is plugged in.
DEFAULT_CONFIG: dict[str, Any] = {
    "layer_rgb": [[0, 90, 255]],
    "fn_rgb": {
        "DICT_RAW": [0, 90, 255],
        "DICT_CLEAN": [170, 0, 255],
        "REPASTE": [0, 200, 200],
        "MEDIA": [255, 110, 0],
        "UNDO": [0, 255, 90],
        "REDO": [0, 255, 180],
        "MOUSE_L": [255, 255, 255],
        "MOUSE_R": [255, 255, 255],
        "MOUSE_M": [255, 255, 255],
        "ENTER": [255, 255, 120],
    },
    "hold_rgb": [255, 0, 0],
    "led_idle_pct": 18,
    "led_flash_ms": 80,
    "node_leds": True,
    "mono_brightness": 5,
    "mono_rotation": 0,
    "mono_idle": "letter",
    "double_tap": True,
    "double_tap_ms": 350,
    "layer_hold_ms": 1000,
    "boot_layer": 0,
    "nav_y_sign": 1,
    "nav_x_sign": 1,
    "scroll_y_sign": 1,
    "scroll_x_sign": 1,
    "mouse_y_sign": 1,
    "led_index_key1": 0,
    # Protocol 2 only.  Listed here so the Sidecar page can render the chain
    # builder's orientation controls with nothing plugged in; a protocol 1
    # device rejects `set` on these, which is what
    # :func:`fethr.core.layout.filter_for_proto` is for.
    "swap_keys": False,
    "nav_swap_xy": False,
    "scroll_swap_xy": False,
}


class SidecarError(RuntimeError):
    """Raised when a command cannot be delivered or the device reports ``err``."""


# --------------------------------------------------------------------------
# pure helpers (unit tested without hardware)
# --------------------------------------------------------------------------


def parse_line(line: str | bytes) -> dict[str, Any] | None:
    """Parse one protocol line.

    Returns the decoded object, or ``None`` for console chatter, blank lines,
    malformed JSON, lines longer than :data:`MAX_LINE_BYTES`, and JSON that is
    not an object.

    >>> parse_line(b'{"ev":"ok"}\\r\\n')
    {'ev': 'ok'}
    >>> parse_line("chain: 5 nodes") is None
    True
    """
    if isinstance(line, (bytes, bytearray)):
        try:
            line = line.decode("utf-8", errors="replace")
        except Exception:  # pragma: no cover - decode never raises with replace
            return None
    text = line.strip()
    if not text or not text.startswith("{") or len(text) > MAX_LINE_BYTES:
        return None
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        return None
    return obj if isinstance(obj, dict) else None


def is_sidecar_port(info: Any) -> bool:
    """True if a ``pyserial`` ``ListPortInfo`` looks like a fethr sidecar.

    Matching is deliberately loose: the Espressif VID plus ``fethr`` anywhere in
    the product, description or manufacturer string.  A port carrying the right
    product string is accepted even if the VID is missing, because some Windows
    CDC enumerations report ``vid=None``.
    """
    text = " ".join(
        str(getattr(info, attr, "") or "")
        for attr in ("product", "description", "manufacturer", "interface")
    ).lower()
    named = PRODUCT_MATCH in text
    vid = getattr(info, "vid", None)
    # On Windows the built-in usbser driver reports the device as a generic
    # "USB Serial Device (COMn)" with product=None, so the product string is
    # only a bonus. Any Espressif-VID port is a candidate; connect() confirms
    # with a `hello` and moves on if the port is something else (e.g. the
    # AtomS3R companion plugged into USB, which answers nothing).
    return named or vid == VENDOR_ID


def scan_ports(lister: Callable[[], Iterable[Any]] | None = None) -> list[dict[str, Any]]:
    """Return candidate sidecar serial ports as plain dicts.

    Args:
        lister: callable returning ``ListPortInfo`` objects; defaults to
            ``serial.tools.list_ports.comports``.
    """
    if lister is None:
        try:
            from serial.tools import list_ports
        except ImportError:  # pragma: no cover - pyserial always installed
            return []
        lister = list_ports.comports
    found: list[dict[str, Any]] = []
    for info in lister():
        if not is_sidecar_port(info):
            continue
        found.append(
            {
                "port": getattr(info, "device", str(info)),
                "product": getattr(info, "product", None) or getattr(info, "description", ""),
                "vid": getattr(info, "vid", None),
                "pid": getattr(info, "pid", None),
                "serial_number": getattr(info, "serial_number", None),
            }
        )
    return found


# --------------------------------------------------------------------------
# device
# --------------------------------------------------------------------------


class SidecarDevice:
    """Connection manager and command channel for the sidecar.

    The object is always usable: with no device attached every accessor returns
    a "not connected" snapshot and commands raise :class:`SidecarError`, which
    is exactly what the UI needs to grey its controls out.
    """

    def __init__(
        self,
        on_event: Callable[[dict[str, Any]], None] | None = None,
        serial_factory: Callable[[str], Any] | None = None,
        port_lister: Callable[[], Iterable[Any]] | None = None,
        baudrate: int = 115200,
        reconnect_interval: float = 2.0,
        enabled: bool = True,
    ) -> None:
        self.on_event = on_event
        self.baudrate = baudrate
        self.reconnect_interval = reconnect_interval
        self._serial_factory = serial_factory or self._open_serial
        self._port_lister = port_lister
        self._enabled = bool(enabled)

        self._serial: Any = None
        self._port: str | None = None
        self._lock = threading.Lock()
        self._ids = itertools.count(1)
        self._pending: dict[int, Any] = {}
        self._reader: threading.Thread | None = None
        self._supervisor: threading.Thread | None = None
        self._stop = threading.Event()

        self.info: dict[str, Any] = {}
        self.config: dict[str, Any] = {}
        #: Cached ``get_layers`` reply; empty until asked, and on protocol 1
        #: devices it stays that way (see :meth:`get_layers`).
        self.layers: list[dict[str, Any]] = []
        self.last_error: str = ""

    # -- lifecycle -------------------------------------------------------

    @property
    def enabled(self) -> bool:
        """Whether the bridge is allowed to scan for or connect to a device."""
        return self._enabled

    def enable(self) -> None:
        """Turn the bridge on and start supervising immediately (no restart)."""
        self._enabled = True
        self.start()

    def disable(self) -> None:
        """Turn the bridge off: drop any connection and stop scanning."""
        self._enabled = False
        self.stop()

    def set_enabled(self, enabled: bool) -> None:
        """Convenience wrapper: :meth:`enable` or :meth:`disable` from a bool."""
        if enabled:
            self.enable()
        else:
            self.disable()

    def start(self) -> None:
        """Begin scanning for the device and keep it connected.

        A no-op while the bridge is disabled — never opens a serial port nor
        starts the supervisor thread in that case. Calling :meth:`enable`
        later starts it live, with no app restart required.
        """
        if not self._enabled or self._supervisor is not None:
            return
        self._stop.clear()
        self._supervisor = threading.Thread(
            target=self._supervise, name="fethr-sidecar-supervisor", daemon=True
        )
        self._supervisor.start()

    def stop(self) -> None:
        """Stop supervising and close the port."""
        self._stop.set()
        self.disconnect()
        self._supervisor = None

    def connect(self, port: str | None = None) -> bool:
        """Open ``port`` (or the first detected sidecar) and handshake.

        Returns:
            True if the port opened and ``hello`` was answered.
        """
        if not self._enabled:
            self.last_error = "sidecar disabled"
            return False
        if self.connected:
            return True
        if port is None:
            candidates = scan_ports(self._port_lister)
            if not candidates:
                self.last_error = "no sidecar detected"
                return False
            # Try every candidate: on Windows all Espressif boards look alike,
            # and only the sidecar answers `hello`.
            for cand in candidates:
                if self._connect_port(cand["port"]):
                    return True
            if not self.last_error:
                self.last_error = "no sidecar answered hello"
            return False
        return self._connect_port(port)

    def _connect_port(self, port: str) -> bool:
        """Open one port and handshake; False (and closed) if it is not a sidecar."""
        try:
            handle = self._serial_factory(port)
        except Exception as exc:
            self.last_error = f"{port}: {exc}"
            log.info("sidecar open failed: %s", self.last_error)
            return False

        with self._lock:
            self._serial = handle
            self._port = port
        self._reader = threading.Thread(
            target=self._read_loop, name="fethr-sidecar-reader", daemon=True
        )
        self._reader.start()

        try:
            self.info = self.request("hello", timeout=3.0)
            self.get_config()
        except SidecarError as exc:
            self.last_error = f"handshake failed: {exc}"
            self.disconnect()
            return False
        self.last_error = ""
        self._emit({"ev": "connected", "port": port, "fw": self.info.get("fw")})
        return True

    def disconnect(self) -> None:
        """Close the port and fail every in-flight request."""
        with self._lock:
            handle, self._serial, self._port = self._serial, None, None
            pending = list(self._pending.values())
            self._pending.clear()
            self.layers = []
        for slot in pending:
            try:
                slot.put_nowait({"ev": "err", "msg": "disconnected"})
            except Exception:
                pass
        if handle is not None:
            try:
                handle.close()
            except Exception:
                pass
            self._emit({"ev": "disconnected"})

    @property
    def connected(self) -> bool:
        """True while a port is open."""
        return self._serial is not None

    # -- commands --------------------------------------------------------

    def request(self, cmd: str, timeout: float = 2.0, **fields: Any) -> dict[str, Any]:
        """Send ``cmd`` and wait for the matching reply.

        Args:
            cmd: protocol command name (``hello``, ``set``, ``save``, …).
            timeout: seconds to wait for the reply.
            **fields: extra command fields (``path``, ``value``, ``index``, …).

        Returns:
            The reply object.

        Raises:
            SidecarError: on no connection, write failure, timeout, or an
                ``{"ev":"err"}`` reply from the device.
        """
        import queue as _queue

        with self._lock:
            handle = self._serial
            if handle is None:
                raise SidecarError("no sidecar connected")
            msg_id = next(self._ids)
            slot: Any = _queue.Queue(maxsize=1)
            self._pending[msg_id] = slot
            payload = json.dumps({"cmd": cmd, "id": msg_id, **fields}) + "\n"
            try:
                handle.write(payload.encode("utf-8"))
                flush = getattr(handle, "flush", None)
                if callable(flush):
                    flush()
            except Exception as exc:
                self._pending.pop(msg_id, None)
                raise SidecarError(f"write failed: {exc}") from exc

        try:
            reply = slot.get(timeout=timeout)
        except Exception as exc:
            raise SidecarError(f"timeout waiting for '{cmd}'") from exc
        finally:
            with self._lock:
                self._pending.pop(msg_id, None)

        if reply.get("ev") == "err":
            raise SidecarError(str(reply.get("msg", "device error")))
        return reply

    def status(self) -> dict[str, Any]:
        """Ask the device for a live status dump."""
        return self.request("status")

    def get_config(self) -> dict[str, Any]:
        """Fetch and cache the device configuration object (without envelope)."""
        reply = self.request("get_config", timeout=3.0)
        self.config = {k: v for k, v in reply.items() if k not in ("ev", "id")}
        return self.config

    def set_config(self, path: str, value: Any) -> dict[str, Any]:
        """Set one dotted config path in device RAM (call :meth:`save` to keep it)."""
        reply = self.request("set", path=path, value=value)
        _apply_local(self.config, path, value)
        return reply

    def save(self) -> dict[str, Any]:
        """Persist the device's current config to NVS."""
        return self.request("save")

    def reset_config(self) -> dict[str, Any]:
        """Restore factory defaults on the device and refresh the cache."""
        reply = self.request("reset_config")
        try:
            self.get_config()
        except SidecarError:
            pass
        return reply

    def set_layer(self, index: int) -> dict[str, Any]:
        """Switch the active layer.

        The valid range is whatever the device reported in ``hello``; a build
        with the extra layers compiled in has more than one.
        """
        count = len(self.snapshot()["layers"]) or 1
        if not 0 <= int(index) < count:
            raise SidecarError(f"layer out of range: {index}")
        return self.request("layer", index=int(index))

    def send_state(self, value: str) -> bool:
        """Tell the device what the dictation engine is doing.

        Fire and forget: the line is written and the ``{"ev":"ok"}`` reply is
        left to the reader thread, because the panel animation this drives is
        worthless if pushing it can stall the engine.  Safe to call at any
        time — with no device attached, an unknown value, or a dying port, it
        simply returns ``False``.

        Args:
            value: one of :data:`STATE_VALUES`.

        Returns:
            True if the line reached the port.
        """
        if value not in STATE_VALUES:
            log.debug("not sending unknown engine state %r", value)
            return False
        with self._lock:
            handle = self._serial
            if handle is None:
                return False
            payload = json.dumps({"cmd": "state", "value": value}) + "\n"
            try:
                handle.write(payload.encode("utf-8"))
                flush = getattr(handle, "flush", None)
                if callable(flush):
                    flush()
            except Exception as exc:
                log.debug("state push failed: %s", exc)
                return False
        return True

    def identify(self) -> dict[str, Any]:
        """Flash both key LEDs white so the user can spot the device."""
        return self.request("identify")

    # -- protocol 2: runtime layout and editable actions -------------------

    @property
    def proto(self) -> int:
        """Protocol version the device reported, or 0 when nothing is attached."""
        try:
            return int(self.info.get("proto") or 0)
        except (TypeError, ValueError):
            return 0

    @property
    def supports_actions(self) -> bool:
        """True when this device can have its key map edited over the wire."""
        return self.connected and self.proto >= PROTO_ACTIONS

    def _require_actions(self, what: str) -> None:
        if not self.connected:
            raise SidecarError("no sidecar connected")
        if self.proto < PROTO_ACTIONS:
            raise SidecarError(
                f"{what} needs firmware 0.2 (this device speaks protocol "
                f"{self.proto or 1})"
            )

    def get_layers(self) -> list[dict[str, Any]]:
        """Fetch every layer with its metadata and bound actions.

        Returns the ``layers`` array of the ``get_layers`` reply (see
        ``PROTOCOL.md``): name, colour, the three control modes, and the action
        bound to each slot.  Cached on :attr:`layers` for the UI.

        Raises:
            SidecarError: when nothing is attached or the device predates
                protocol 2 — the caller is expected to check
                :attr:`supports_actions` first and degrade rather than ask.
        """
        self._require_actions("reading the key map")
        reply = self.request("get_layers", timeout=3.0)
        layers = reply.get("layers")
        self.layers = list(layers) if isinstance(layers, list) else []
        return self.layers

    def set_action(self, layer: int, slot: str, action: dict[str, Any]) -> dict[str, Any]:
        """Bind one slot on one layer.

        ``action`` is validated against the protocol's vocabulary before it is
        sent (see :func:`fethr.core.actions.action_request`), so a typo comes
        back as a Python ``ValueError`` here instead of an ``err`` from the
        device three layers away.
        """
        from .actions import action_request

        self._require_actions("editing actions")
        return self.request("set_action", **action_request(layer, slot, action))

    def set_layer_meta(self, layer: int, **fields: Any) -> dict[str, Any]:
        """Rename or recolour a layer, or change its nav/scroll/angle modes.

        Any subset of ``name``, ``rgb``, ``nav_mode``, ``scroll_mode`` and
        ``angle_mode`` may be given; the device applies what it is sent, and
        validates the lot before writing any of it.

        The length rule on ``name`` is checked here too, and for the same reason
        the protocol gives for checking it there: 1–8 characters, refused rather
        than truncated.  Silently shortening what someone typed is a worse
        answer than telling them it does not fit.
        """
        from .actions import ANGLE_MODES, NAV_MODES, SCROLL_MODES

        self._require_actions("editing layers")
        payload: dict[str, Any] = {"layer": int(layer)}
        if "name" in fields:
            name = str(fields["name"]).strip()
            if not 1 <= len(name) <= 8:
                raise SidecarError("a layer name must be 1 to 8 characters")
            payload["name"] = name
        if "rgb" in fields:
            payload["rgb"] = [int(c) for c in fields["rgb"]][:3]
        for key, allowed in (("nav_mode", NAV_MODES), ("scroll_mode", SCROLL_MODES),
                             ("angle_mode", ANGLE_MODES)):
            if key in fields:
                value = str(fields[key])
                if value not in allowed:
                    raise SidecarError(f"unknown {key}: {value}")
                payload[key] = value
        if len(payload) == 1:
            raise SidecarError("set_layer_meta needs at least one field")
        return self.request("set_layer_meta", **payload)

    def mono(self, text: str) -> dict[str, Any]:
        """Scroll up to 32 characters across the 8x8 Mono panel."""
        return self.request("mono", text=str(text)[:32])

    # -- state -----------------------------------------------------------

    def snapshot(self) -> dict[str, Any]:
        """Return a JSON-friendly status blob for the UI.

        Always succeeds; when nothing is attached it reports
        ``connected: False`` plus the last connection error. When the master
        switch is off, ``state`` reads ``"off"`` regardless of anything else.
        """
        return {
            "connected": self.connected,
            "enabled": self._enabled,
            "state": "off" if not self._enabled else ("connected" if self.connected else "disconnected"),
            "port": self._port,
            "fw": self.info.get("fw"),
            "proto": self.info.get("proto"),
            "layers": list(self.info.get("layers") or LAYER_NAMES),
            "layer": self.info.get("layer", 0),
            "supports_actions": self.supports_actions,
            "companion": self.info.get("companion", False),
            "vbat_mv": self.info.get("vbat_mv"),
            "usb_mv": self.info.get("usb_mv"),
            "uptime_s": self.info.get("uptime_s"),
            "nodes": self.info.get("nodes") or [],
            "battery_low": bool(self.info.get("low")),
            "error": self.last_error,
        }

    def effective_config(self) -> dict[str, Any]:
        """Device config if connected, otherwise the firmware defaults."""
        return dict(self.config) if self.config else dict(DEFAULT_CONFIG)

    # -- internals -------------------------------------------------------

    def _open_serial(self, port: str) -> Any:
        import serial  # imported lazily so tests need no pyserial backend

        return serial.Serial(port, self.baudrate, timeout=1)

    def _supervise(self) -> None:
        """Reconnect loop: poll for the device whenever we are not connected."""
        while not self._stop.is_set():
            if not self.connected:
                self.connect()
            self._stop.wait(self.reconnect_interval)

    def _read_loop(self) -> None:
        """Blocking reader: one line at a time until the port dies."""
        handle = self._serial
        while handle is not None and not self._stop.is_set():
            try:
                raw = handle.readline()
            except Exception as exc:
                self.last_error = f"read failed: {exc}"
                break
            if raw in (None, b"", ""):
                if self._serial is not handle:
                    return
                continue
            obj = parse_line(raw)
            if obj is not None:
                self._dispatch(obj)
            handle = self._serial
        if self._serial is handle and handle is not None:
            self.disconnect()

    def _dispatch(self, obj: dict[str, Any]) -> None:
        """Route one decoded object to its waiting caller or to the event sink."""
        msg_id = obj.get("id")
        if msg_id is not None:
            with self._lock:
                slot = self._pending.pop(msg_id, None)
            if slot is not None:
                try:
                    slot.put_nowait(obj)
                except Exception:  # pragma: no cover - slot is size 1 and fresh
                    pass
                return
        self._absorb(obj)
        self._emit(obj)

    def _absorb(self, obj: dict[str, Any]) -> None:
        """Fold an event into the cached device info."""
        ev = obj.get("ev")
        if ev == "layer" and "index" in obj:
            self.info["layer"] = obj["index"]
        elif ev == "battery":
            self.info["vbat_mv"] = obj.get("vbat_mv")
            self.info["low"] = obj.get("low", False)
        elif ev == "chain":
            self.info["nodes"] = obj.get("nodes", [])
        elif ev in ("hello", "status", "boot"):
            self.info.update({k: v for k, v in obj.items() if k not in ("ev", "id")})

    def _emit(self, obj: dict[str, Any]) -> None:
        if self.on_event is None:
            return
        try:
            self.on_event(obj)
        except Exception:
            log.debug("sidecar event callback failed", exc_info=True)


def _apply_local(config: dict[str, Any], path: str, value: Any) -> None:
    """Mirror a ``set`` into the cached config, honouring indexed paths.

    ``layer_rgb.1`` indexes a list, ``fn_rgb.DICT_RAW`` a dict, ``mono_idle``
    is a plain key.  Unknown paths are ignored — the device is authoritative.
    """
    if not config:
        return
    parts = path.split(".")
    target: Any = config
    for part in parts[:-1]:
        if isinstance(target, list) and part.isdigit():
            index = int(part)
            if index >= len(target):
                return
            target = target[index]
        elif isinstance(target, dict) and part in target:
            target = target[part]
        else:
            return
    leaf = parts[-1]
    if isinstance(target, list) and leaf.isdigit():
        index = int(leaf)
        if index < len(target):
            target[index] = value
    elif isinstance(target, dict):
        target[leaf] = value


def wait_until(predicate: Callable[[], bool], timeout: float = 2.0,
               interval: float = 0.01) -> bool:
    """Poll ``predicate`` until true or ``timeout`` elapses (test/UI helper)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()
