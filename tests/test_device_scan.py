"""Sidecar scan policy: which serial ports the bridge opens, and when.

The point of the policy is to stay off other people's ESP32 boards: an
Espressif-VID port is only a *candidate*, and opening one mid-flash breaks
esptool / PlatformIO uploads.  Every test drives :meth:`SidecarDevice.tick`
directly over a scripted port list, and records each open, so "never touched
that port" is something the suite can assert rather than hope for.
"""

from __future__ import annotations

import json
import queue
import time
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from fethr.core.device import BUSY_RETRY_TICKS, DEFAULT_CONFIG, SidecarDevice

# ----------------------------------------------------------------- fakes --


class SerialException(IOError):
    """Stand-in for ``serial.SerialException`` (also an ``IOError`` subclass)."""


class Port:
    """Minimal ``ListPortInfo``: an Espressif board with no product string,
    which is exactly how Windows' usbser driver reports every one of them."""

    def __init__(self, device: str) -> None:
        self.device = device
        self.product = None
        self.description = f"USB Serial Device ({device})"
        self.manufacturer = "Microsoft"
        self.interface = ""
        self.vid = 0x303A
        self.pid = 0x1001
        self.serial_number = None


def _sidecar_reply(command: dict) -> list[dict]:
    msg_id, cmd = command.get("id"), command.get("cmd")
    if cmd == "hello":
        return [{"ev": "hello", "id": msg_id, "fw": "0.2.2", "proto": 2,
                 "layers": ["FETHR"], "layer": 0, "nodes": []}]
    if cmd == "get_config":
        return [{"ev": "config", "id": msg_id, **DEFAULT_CONFIG}]
    return [{"ev": "ok", "id": msg_id}]


class Wire:
    """A serial handle.  ``sidecar=False`` is a dev board: it accepts bytes and
    never answers, so the handshake can only time out."""

    def __init__(self, sidecar: bool) -> None:
        self.sidecar = sidecar
        self.outgoing: queue.Queue[bytes] = queue.Queue()
        self.closed = False

    def write(self, data: bytes) -> int:
        if self.closed:
            raise OSError("port is closed")
        if self.sidecar:
            for reply in _sidecar_reply(json.loads(data.decode("utf-8"))):
                self.outgoing.put((json.dumps(reply) + "\n").encode("utf-8"))
        return len(data)

    def readline(self) -> bytes:
        if self.closed:
            raise OSError("port is closed")
        try:
            return self.outgoing.get(timeout=0.01)
        except queue.Empty:
            return b""

    def close(self) -> None:
        self.closed = True


class Bench:
    """The desk: which ports exist, what each one is, and every open attempt."""

    BUSY = "busy"
    BOARD = "board"
    SIDECAR = "sidecar"

    def __init__(self) -> None:
        self.present: list[str] = []
        self.kind: dict[str, str] = {}
        self.opens: list[str] = []
        self.remembered: list[str | None] = []

    def plug(self, port: str, kind: str) -> None:
        self.kind[port] = kind
        if port not in self.present:
            self.present.append(port)

    def unplug(self, port: str) -> None:
        if port in self.present:
            self.present.remove(port)

    def lister(self):
        return [Port(p) for p in self.present]

    def open(self, port: str):
        self.opens.append(port)
        kind = self.kind.get(port)
        if kind == self.BUSY:
            # pyserial's real Windows message for a port held by esptool.
            raise SerialException(
                f"could not open port '{port}': PermissionError(13, "
                "'Access is denied.', None, 5)")
        return Wire(sidecar=kind == self.SIDECAR)

    def device(self, mode: str = "auto", preferred: str | None = None) -> SidecarDevice:
        return SidecarDevice(
            serial_factory=self.open,
            port_lister=self.lister,
            connect_mode=mode,
            preferred_port=preferred,
            on_port_remembered=self.remembered.append,
            hello_timeout=0.05,
        )


@pytest.fixture
def bench():
    return Bench()


def ticks(device: SidecarDevice, n: int) -> None:
    for _ in range(n):
        device.tick()


# ------------------------------------------------------------- manual mode --


def test_manual_mode_never_opens_a_port_without_a_remembered_one(bench):
    bench.plug("COM9", Bench.BOARD)
    device = bench.device(mode="manual")
    try:
        ticks(device, 3)
        bench.plug("COM12", Bench.SIDECAR)   # even a real sidecar plugged in later
        ticks(device, 3)
        assert bench.opens == []
        assert device.connected is False
    finally:
        device.stop()


def test_manual_mode_reattaches_only_the_remembered_port_when_it_reappears(bench):
    bench.plug("COM9", Bench.BOARD)
    device = bench.device(mode="manual", preferred="COM7")
    try:
        ticks(device, 2)                      # sidecar unplugged: nothing to do
        assert bench.opens == []
        bench.plug("COM7", Bench.SIDECAR)     # replugged into the same socket
        device.tick()
        assert device.connected is True
        assert device.snapshot()["port"] == "COM7"
        assert bench.opens == ["COM7"]        # the dev board on COM9 untouched
    finally:
        device.stop()


def test_manual_mode_gives_up_on_a_silent_remembered_port(bench):
    """The remembered port gets a short grace (firmware may still be booting),
    then is left alone until it re-enumerates — never polled forever."""
    bench.plug("COM7", Bench.BOARD)           # the name now belongs to a dev board
    device = bench.device(mode="manual", preferred="COM7")
    try:
        ticks(device, BUSY_RETRY_TICKS + 6)
        assert bench.opens == ["COM7"] * (BUSY_RETRY_TICKS + 1)
        bench.unplug("COM7")
        device.tick()
        bench.plug("COM7", Bench.SIDECAR)     # re-enumerated: eligible again
        device.tick()
        assert device.connected is True
    finally:
        device.stop()


# --------------------------------------------------------------- auto mode --


def test_auto_mode_probes_a_new_port_once_and_not_again_while_present(bench):
    device = bench.device(mode="auto")
    try:
        device.tick()
        bench.plug("COM9", Bench.BOARD)
        ticks(device, 5)
        assert bench.opens == ["COM9"]
        assert device.connected is False
    finally:
        device.stop()


def test_auto_mode_first_tick_treats_every_present_port_as_new(bench):
    bench.plug("COM9", Bench.BOARD)
    bench.plug("COM10", Bench.BOARD)
    device = bench.device(mode="auto")
    try:
        ticks(device, 4)
        assert sorted(bench.opens) == ["COM10", "COM9"]
    finally:
        device.stop()


def test_auto_mode_reprobes_a_port_only_after_it_reenumerates(bench):
    bench.plug("COM9", Bench.BOARD)
    device = bench.device(mode="auto")
    try:
        ticks(device, 3)
        assert bench.opens == ["COM9"]
        bench.unplug("COM9")
        device.tick()
        bench.plug("COM9", Bench.BOARD)
        ticks(device, 3)
        assert bench.opens == ["COM9", "COM9"]
    finally:
        device.stop()


def test_a_port_that_answers_hello_becomes_the_preferred_port(bench):
    bench.plug("COM9", Bench.BOARD)
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="auto")
    try:
        device.tick()
        assert device.connected is True
        assert device.preferred_port == "COM12"
        assert bench.remembered == ["COM12"]
        assert device.snapshot()["last_port"] == "COM12"
    finally:
        device.stop()


def test_the_remembered_port_is_tried_first(bench):
    bench.plug("COM3", Bench.BOARD)
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="auto", preferred="COM12")
    try:
        device.tick()
        assert bench.opens == ["COM12"]       # COM3 never needed
        assert bench.remembered == []         # same port: nothing to persist
    finally:
        device.stop()


def test_a_busy_port_is_skipped_without_waiting_and_not_retried_that_tick(bench):
    bench.plug("COM5", Bench.BUSY)
    device = bench.device(mode="auto")
    try:
        started = time.monotonic()
        device.tick()
        assert time.monotonic() - started < 0.5  # no hello wait on a busy port
        assert bench.opens == ["COM5"]
        assert "busy" in device.last_error
    finally:
        device.stop()


def test_a_busy_port_does_not_block_the_sidecar_in_the_same_tick(bench):
    bench.plug("COM5", Bench.BUSY)
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="auto")
    try:
        device.tick()
        assert device.connected is True
        assert bench.opens.count("COM5") == 1
    finally:
        device.stop()


def test_a_busy_port_gets_a_few_ticks_of_grace_then_is_left_alone(bench):
    """Windows refuses opens for a moment after enumeration, so busy is retried
    briefly — but a board esptool is flashing stays busy and is then dropped."""
    bench.plug("COM5", Bench.BUSY)
    device = bench.device(mode="auto")
    try:
        ticks(device, BUSY_RETRY_TICKS + 8)
        assert bench.opens == ["COM5"] * (BUSY_RETRY_TICKS + 1)
    finally:
        device.stop()


def test_a_port_that_stops_being_busy_inside_the_grace_connects(bench):
    bench.plug("COM12", Bench.BUSY)
    device = bench.device(mode="auto")
    try:
        device.tick()
        bench.kind["COM12"] = Bench.SIDECAR
        device.tick()
        assert device.connected is True
    finally:
        device.stop()


def test_disabled_bridge_ticks_do_nothing(bench):
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="auto")
    device._enabled = False
    assert device.tick() is False
    assert bench.opens == []


# ------------------------------------------------------- explicit Connect --


def test_find_and_connect_scans_every_port_regardless_of_mode(bench):
    bench.plug("COM3", Bench.BOARD)
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="manual")
    try:
        ticks(device, 3)
        assert bench.opens == []              # manual: the supervisor stayed off
        assert device.find_and_connect() is True
        assert set(bench.opens) == {"COM3", "COM12"}
        assert device.preferred_port == "COM12"
        assert bench.remembered == ["COM12"]
    finally:
        device.stop()


def test_find_and_connect_retries_ports_the_supervisor_gave_up_on(bench):
    bench.plug("COM12", Bench.BOARD)          # not answering yet
    device = bench.device(mode="auto")
    try:
        ticks(device, 2)
        assert bench.opens == ["COM12"]
        bench.kind["COM12"] = Bench.SIDECAR   # firmware finally up
        ticks(device, 2)
        assert device.connected is False      # the supervisor will not re-ask...
        assert device.find_and_connect() is True  # ...but the button does
    finally:
        device.stop()


def test_find_and_connect_reports_why_nothing_answered(bench):
    bench.plug("COM3", Bench.BOARD)
    bench.plug("COM5", Bench.BUSY)
    device = bench.device(mode="manual")
    try:
        assert device.find_and_connect() is False
        assert "COM5: busy" in device.last_error
        assert "COM3" in device.last_error
    finally:
        device.stop()


def test_connect_without_a_port_is_find_and_connect(bench):
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="manual")
    try:
        assert device.connect() is True
        assert device.preferred_port == "COM12"
    finally:
        device.stop()


# ---------------------------------------------------- mode / port control --


def test_switching_to_auto_live_probes_the_ports_already_present(bench):
    bench.plug("COM12", Bench.SIDECAR)
    device = bench.device(mode="manual")
    try:
        ticks(device, 2)
        assert bench.opens == []
        device.set_connect_mode("auto")
        device.tick()
        assert device.connected is True
    finally:
        device.stop()


def test_switching_to_manual_live_stops_probing_new_ports(bench):
    device = bench.device(mode="auto")
    try:
        device.tick()
        device.set_connect_mode("manual")
        bench.plug("COM9", Bench.BOARD)
        ticks(device, 3)
        assert bench.opens == []
    finally:
        device.stop()


def test_unknown_connect_mode_falls_back_to_auto(bench):
    assert bench.device(mode="sometimes").connect_mode == "auto"


def test_forget_port_clears_it_and_tells_the_app(bench):
    device = bench.device(mode="manual", preferred="COM7")
    device.forget_port()
    assert device.preferred_port is None
    assert bench.remembered == [None]
    device.forget_port()                      # idempotent: no second callback
    assert bench.remembered == [None]


def test_snapshot_carries_mode_port_and_a_human_line(bench):
    device = bench.device(mode="manual")
    snap = device.snapshot()
    assert snap["connect_mode"] == "manual"
    assert snap["last_port"] is None
    assert snap["scan_line"] == "manual — click Connect"
    device.preferred_port = "COM10"
    assert "COM10" in device.snapshot()["scan_line"]
    device.set_connect_mode("auto")
    assert device.snapshot()["scan_line"] == "auto — watching COM10 and newly plugged-in boards"


# --------------------------------------------------------------- app wiring --


def test_app_persists_the_remembered_port(tmp_path: Path):
    from fethr.__main__ import FethrApp
    from fethr.core.settings import Settings, load_settings

    app = FethrApp.__new__(FethrApp)          # no window, tray or engine
    app.settings = Settings()
    app.settings.legacy_imported = "n/a"
    app.settings_file = tmp_path / "settings.json"
    app.window = MagicMock()

    app._on_port_remembered("COM12")
    assert load_settings(app.settings_file).sidecar.last_port == "COM12"
    app.window.push.assert_called_once()

    app._on_port_remembered("COM12")          # unchanged: no extra write/push
    app.window.push.assert_called_once()


def test_app_applies_mode_and_port_to_the_live_device(bench):
    from fethr.__main__ import FethrApp
    from fethr.core.settings import Settings

    app = FethrApp.__new__(FethrApp)
    app.settings = Settings()
    app.device = bench.device(mode="auto")
    app.settings.sidecar.connect_mode = "manual"
    app.settings.sidecar.last_port = "COM7"
    app._apply_device_settings()
    assert app.device.connect_mode == "manual"
    assert app.device.preferred_port == "COM7"


def test_api_connect_button_runs_a_full_scan(bench):
    from fethr.ui.window import Api

    bench.plug("COM3", Bench.BOARD)
    bench.plug("COM12", Bench.SIDECAR)
    app = MagicMock()
    app.device = bench.device(mode="manual")
    try:
        result = Api(app).device_connect()
        assert result["ok"] is True
        assert result["status"]["port"] == "COM12"
        status = Api(app).device_status()
        assert status["connect_mode"] == "manual"
        assert status["last_port"] == "COM12"
        assert status["scan_line"] == "manual — connected on COM12"
    finally:
        app.device.stop()
