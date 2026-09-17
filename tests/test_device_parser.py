"""Sidecar protocol: line parsing, port detection and the request/reply channel.

Driven entirely by a fake serial port, so the suite runs with no device (and no
pyserial backend) attached.
"""

from __future__ import annotations

import json
import queue
import threading
import time
from unittest.mock import MagicMock

import pytest

from fethr.core.device import (
    DEFAULT_CONFIG,
    SidecarDevice,
    SidecarError,
    is_sidecar_port,
    parse_line,
    scan_ports,
    wait_until,
)


# ------------------------------------------------------------ parse_line --


@pytest.mark.parametrize(
    "line, expected",
    [
        (b'{"ev":"ok","id":3}\r\n', {"ev": "ok", "id": 3}),
        ('{"ev":"layer","index":2,"name":"EDIT"}', {"ev": "layer", "index": 2, "name": "EDIT"}),
        ("  {\"ev\":\"boot\",\"fw\":\"0.1.0\"}  ", {"ev": "boot", "fw": "0.1.0"}),
    ],
)
def test_parse_line_accepts_protocol_objects(line, expected):
    assert parse_line(line) == expected


@pytest.mark.parametrize(
    "line",
    [
        b"",
        "\n",
        "chain: 5 nodes enumerated",        # human console output
        "? for status",
        '{"ev":"broken"',                    # truncated JSON
        "[1, 2, 3]",                         # JSON, but not an object
        '{"x":"' + "y" * 5000 + '"}',        # past MAX_LINE_BYTES
    ],
)
def test_parse_line_rejects_everything_else(line):
    assert parse_line(line) is None


def test_parse_line_survives_undecodable_bytes():
    assert parse_line(b"\xff\xfe garbage") is None


# -------------------------------------------------------- port detection --


class FakePort:
    """Stand-in for ``serial.tools.list_ports.ListPortInfo``."""

    def __init__(self, device, product="", description="", vid=0x303A, pid=0x1001):
        self.device = device
        self.product = product
        self.description = description
        self.manufacturer = "Espressif"
        self.interface = ""
        self.vid = vid
        self.pid = pid
        self.serial_number = "ABC123"


def test_is_sidecar_port_matches_vid_and_product():
    assert is_sidecar_port(FakePort("COM7", product="fethr sidecar")) is True


def test_is_sidecar_port_matches_on_description_too():
    assert is_sidecar_port(FakePort("COM7", description="fethr sidecar (COM7)")) is True


def test_is_sidecar_port_tolerates_a_missing_vid():
    """Windows CDC enumerations sometimes report no VID."""
    assert is_sidecar_port(FakePort("COM7", product="fethr sidecar", vid=None)) is True


def test_is_sidecar_port_accepts_any_espressif_port_as_candidate():
    """Windows' usbser driver reports 'USB Serial Device' with product=None for
    every Espressif board, so the VID alone must qualify a port; connect()
    sorts the sidecar from other boards with the `hello` handshake."""
    assert is_sidecar_port(FakePort("COM10", description="USB Serial Device (COM10)",
                                    product=None)) is True
    assert is_sidecar_port(FakePort("COM4", product="ESP32-S3 USB JTAG")) is True


def test_is_sidecar_port_name_is_authoritative_even_on_a_foreign_vid():
    assert is_sidecar_port(FakePort("COM9", product="fethr sidecar", vid=0x2341)) is True


def test_is_sidecar_port_rejects_unnamed_foreign_vid():
    assert is_sidecar_port(FakePort("COM3", product="USB Serial", vid=0x2341)) is False


def test_scan_ports_returns_only_matches():
    ports = [
        FakePort("COM3", product="USB Serial", vid=0x2341),
        FakePort("COM7", product="fethr sidecar"),
    ]
    found = scan_ports(lambda: ports)
    assert [p["port"] for p in found] == ["COM7"]
    assert found[0]["vid"] == 0x303A


# -------------------------------------------------------------- fake wire --


class FakeSerial:
    """A serial port that answers protocol commands from a scripted responder."""

    def __init__(self, responder=None):
        self.outgoing: queue.Queue[bytes] = queue.Queue()
        self.written: list[dict] = []
        self.closed = False
        self._responder = responder or default_responder

    # -- host side
    def write(self, data: bytes) -> int:
        if self.closed:
            raise OSError("port is closed")
        command = json.loads(data.decode("utf-8"))
        self.written.append(command)
        for reply in self._responder(command):
            self.outgoing.put((json.dumps(reply) + "\n").encode("utf-8"))
        return len(data)

    def readline(self) -> bytes:
        if self.closed:
            raise OSError("port is closed")
        try:
            return self.outgoing.get(timeout=0.02)
        except queue.Empty:
            return b""

    def close(self) -> None:
        self.closed = True

    # -- device side (tests inject unsolicited traffic with this)
    def emit(self, obj) -> None:
        line = obj if isinstance(obj, str) else json.dumps(obj)
        self.outgoing.put((line + "\n").encode("utf-8"))


def default_responder(command):
    """Minimal firmware emulation: hello, get_config and a generic ok."""
    msg_id = command.get("id")
    cmd = command.get("cmd")
    if cmd == "hello":
        return [{"ev": "hello", "id": msg_id, "fw": "0.1.0", "proto": 1,
                 "layers": ["FETHR"], "layer": 0,
                 "nodes": [{"id": 1, "type": "key"}]}]
    if cmd == "get_config":
        return [{"ev": "config", "id": msg_id, **DEFAULT_CONFIG}]
    if cmd == "status":
        return [{"ev": "status", "id": msg_id, "layer": 0, "vbat_mv": 4012,
                 "usb_mv": 5010, "uptime_s": 123, "nodes": []}]
    if cmd == "set" and command.get("path") == "nope":
        return [{"ev": "err", "id": msg_id, "msg": "unknown path"}]
    if cmd == "silent":
        return []
    return [{"ev": "ok", "id": msg_id}]


@pytest.fixture
def wired():
    """A connected :class:`SidecarDevice` backed by a :class:`FakeSerial`."""
    fake = FakeSerial()
    events: list[dict] = []
    device = SidecarDevice(
        on_event=events.append,
        serial_factory=lambda port: fake,
        port_lister=lambda: [FakePort("COM7", product="fethr sidecar")],
    )
    assert device.connect() is True
    yield device, fake, events
    device.stop()


# ------------------------------------------------------------- behaviour --


def test_disconnected_device_is_still_usable():
    device = SidecarDevice(port_lister=list)
    snapshot = device.snapshot()
    assert snapshot["connected"] is False
    assert snapshot["layers"] == ["FETHR"]
    assert device.effective_config() == DEFAULT_CONFIG
    with pytest.raises(SidecarError):
        device.identify()


def test_connect_performs_the_handshake(wired):
    device, fake, events = wired
    assert device.connected is True
    assert device.info["fw"] == "0.1.0"
    assert device.config["mono_idle"] == "letter"
    assert [c["cmd"] for c in fake.written] == ["hello", "get_config"]
    assert any(e["ev"] == "connected" for e in events)


def test_every_command_carries_a_unique_id(wired):
    device, fake, _ = wired
    device.identify()
    device.save()
    ids = [c["id"] for c in fake.written]
    assert len(ids) == len(set(ids))


def test_replies_are_matched_to_their_caller(wired):
    device, _, _ = wired
    assert device.status()["vbat_mv"] == 4012


def test_error_replies_raise(wired):
    device, _, _ = wired
    with pytest.raises(SidecarError, match="unknown path"):
        device.set_config("nope", 1)


def test_request_times_out_without_a_reply(wired):
    device, _, _ = wired
    with pytest.raises(SidecarError, match="timeout"):
        device.request("silent", timeout=0.15)


def test_unsolicited_events_reach_the_callback_and_update_state(wired):
    device, fake, events = wired
    fake.emit({"ev": "layer", "index": 2, "name": "EDIT"})
    fake.emit({"ev": "battery", "vbat_mv": 3711, "low": True})
    fake.emit("? press for status")  # console noise must be dropped

    assert wait_until(lambda: device.info.get("vbat_mv") == 3711, timeout=1.0)
    assert device.snapshot()["layer"] == 2
    assert device.snapshot()["battery_low"] is True
    kinds = [e["ev"] for e in events]
    assert "layer" in kinds and "battery" in kinds
    assert all(k != "?" for k in kinds)


def test_set_config_mirrors_into_the_local_cache(wired):
    device, _, _ = wired
    device.set_config("layer_rgb.0", [1, 2, 3])
    device.set_config("fn_rgb.DICT_RAW", [9, 9, 9])
    device.set_config("mono_idle", "blank")
    assert device.config["layer_rgb"][0] == [1, 2, 3]
    assert device.config["fn_rgb"]["DICT_RAW"] == [9, 9, 9]
    assert device.config["mono_idle"] == "blank"


def test_set_layer_validates_against_the_reported_layers(wired):
    """The device ships one layer; asking for a second is a host-side error."""
    device, fake, _ = wired
    assert device.snapshot()["layers"] == ["FETHR"]
    device.set_layer(0)
    with pytest.raises(SidecarError):
        device.set_layer(1)
    assert [c["cmd"] for c in fake.written].count("layer") == 1


def test_disconnect_releases_waiting_callers(wired):
    device, _, _ = wired
    errors: list[Exception] = []

    def call() -> None:
        try:
            device.request("silent", timeout=3.0)
        except SidecarError as exc:
            errors.append(exc)

    worker = threading.Thread(target=call, daemon=True)
    worker.start()
    assert wait_until(lambda: bool(device._pending), timeout=1.0)
    device.disconnect()
    worker.join(timeout=2.0)
    assert errors and "disconnected" in str(errors[0])
    assert device.connected is False


def test_reconnect_after_unplug(wired):
    device, fake, _ = wired
    device.disconnect()
    assert device.connected is False
    time.sleep(0.1)      # let the old reader thread notice and exit
    fake.closed = False  # the "device" comes back on the same port
    assert device.connect() is True
    assert device.info["fw"] == "0.1.0"


# -------------------------------------------------------- host state push --


def test_send_state_writes_one_unacknowledged_line(wired):
    """``state`` is fire-and-forget: a bare line, no ``id``, no waiting."""
    device, fake, _ = wired
    assert device.send_state("transcribing") is True
    assert fake.written[-1] == {"cmd": "state", "value": "transcribing"}
    assert "id" not in fake.written[-1]


@pytest.mark.parametrize(
    "value", ["idle", "recording", "transcribing", "cleaning", "pasted", "error"]
)
def test_send_state_accepts_every_engine_state(wired, value):
    """Every :class:`EngineState` value must be one the device understands."""
    device, fake, _ = wired
    assert device.send_state(value) is True
    assert fake.written[-1]["value"] == value


def test_send_state_covers_the_engine_states_exactly():
    from fethr.core.device import STATE_VALUES
    from fethr.core.dictation import EngineState

    assert {s.value for s in EngineState} == set(STATE_VALUES)


def test_send_state_rejects_an_unknown_value(wired):
    device, fake, _ = wired
    before = len(fake.written)
    assert device.send_state("thinking") is False
    assert len(fake.written) == before


def test_send_state_is_a_noop_when_disconnected():
    """Called constantly by the engine, so it must never raise."""
    device = SidecarDevice(port_lister=list)
    assert device.connected is False
    assert device.send_state("recording") is False


def test_send_state_survives_a_dying_port(wired):
    device, fake, _ = wired
    fake.closed = True                      # unplugged between two engine events
    assert device.send_state("pasted") is False


def test_engine_state_events_are_pushed_to_the_device(wired):
    """The app's event relay must forward engine states to the bridge."""
    from fethr.__main__ import FethrApp

    device, fake, _ = wired

    app = FethrApp.__new__(FethrApp)        # no window, tray or engine needed
    app.device = device
    app.tray = MagicMock()
    app.window = MagicMock()

    app._on_engine_event({"type": "state", "state": "transcribing", "detail": ""})
    assert fake.written[-1] == {"cmd": "state", "value": "transcribing"}
    app.tray.set_state.assert_called_once_with("transcribing")
    app.window.push.assert_called_once()

    before = len(fake.written)
    app._on_engine_event({"type": "transcript", "final": "hello"})
    assert len(fake.written) == before      # only state events go to the device


# ------------------------------------------------------ master enable switch --


def test_disabled_bridge_never_scans_ports(monkeypatch):
    """The master switch must gate scanning itself, not just connecting.

    With no explicit ``port_lister``, a real scan would import pyserial and
    call ``serial.tools.list_ports.comports`` — a disabled bridge must never
    reach that call at all.
    """
    from serial.tools import list_ports

    comports = MagicMock(return_value=[])
    monkeypatch.setattr(list_ports, "comports", comports)

    device = SidecarDevice(enabled=False)
    assert device.enabled is False

    device.start()
    assert device._supervisor is None  # no supervisor thread was ever started

    assert device.connect() is False
    assert device.snapshot()["connected"] is False
    assert device.snapshot()["state"] == "off"
    comports.assert_not_called()
    device.stop()


def test_enable_at_runtime_starts_the_supervisor_and_connects():
    """Flipping the switch on live must scan and connect, no restart needed."""
    fake = FakeSerial()
    events: list[dict] = []
    device = SidecarDevice(
        on_event=events.append,
        serial_factory=lambda port: fake,
        port_lister=lambda: [FakePort("COM7", product="fethr sidecar")],
        enabled=False,
        reconnect_interval=0.02,
    )
    assert device.enabled is False
    assert device._supervisor is None

    device.enable()
    try:
        assert device.enabled is True
        assert device._supervisor is not None
        assert device._supervisor.is_alive()
        assert wait_until(lambda: device.connected, timeout=1.0)
    finally:
        device.stop()


def test_disable_at_runtime_disconnects_and_stops_scanning():
    """Flipping the switch off must drop the connection and stop the supervisor."""
    fake = FakeSerial()
    events: list[dict] = []
    device = SidecarDevice(
        on_event=events.append,
        serial_factory=lambda port: fake,
        port_lister=lambda: [FakePort("COM7", product="fethr sidecar")],
        enabled=True,
        reconnect_interval=0.02,
    )
    device.start()
    assert wait_until(lambda: device.connected, timeout=1.0)
    assert device._supervisor is not None

    device.disable()
    assert device.enabled is False
    assert device.connected is False
    assert device._supervisor is None
    assert device.snapshot()["state"] == "off"
    assert any(e.get("ev") == "disconnected" for e in events)
