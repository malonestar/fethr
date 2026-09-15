#!/usr/bin/env python3
"""Run the app against a fake sidecar and photograph every page.

``python -m fethr --smoke`` already walks the UI and can save PNGs, but with no
hardware attached the Sidecar page is mostly empty banners — which is exactly
the state the chain builder has least to say in.  This harness stands a
protocol 2 sidecar up in-process (a full chain, a companion display, one
layer with real bindings), points the bridge at it, and runs the same smoke
walk, so the captures show the builder doing its job.

It is a review tool, not part of the app: nothing in ``fethr/`` imports it,
and it patches only the two seams the serial bridge already exposes.

    python assets/capture_builder.py                 # -> assets/shots_builder/
    python assets/capture_builder.py --out other/

Every screenshot it takes is of a window on your desktop, so leave the machine
alone for the ~20 seconds it runs.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from fethr.core import device as device_module  # noqa: E402
from fethr.core.device import DEFAULT_CONFIG  # noqa: E402

#: The chain on the desk this harness pretends to be.
NODES = [
    {"id": 1, "type": "key"},
    {"id": 2, "type": "joystick", "role": "nav"},
    {"id": 3, "type": "joystick", "role": "scroll"},
    {"id": 4, "type": "angle"},
    {"id": 5, "type": "mono"},
]

LAYERS = [
    {
        "index": 0, "name": "FLOW", "rgb": [0, 90, 255],
        "nav_mode": "arrows", "scroll_mode": "wheel_pan", "angle_mode": "volume",
        "actions": {
            "key1": {"type": "key_hold", "key": "f8", "mods": [], "fn": "dict_raw"},
            "key2": {"type": "key_hold", "key": "f9", "mods": [], "fn": "dict_clean"},
            "chain_key": {"type": "key_tap", "key": "f7", "mods": [], "fn": "repaste"},
            "chain_key_double": {"type": "key_tap", "key": "z", "mods": ["ctrl"],
                                 "fn": "undo"},
            "nav_click": {"type": "key_tap", "key": "enter", "mods": [], "fn": "enter"},
            "scroll_click": {"type": "mouse_btn", "key": "middle", "mods": [],
                             "fn": "mouse_m"},
        },
    },
    {
        "index": 1, "name": "MEDIA", "rgb": [255, 110, 0],
        "nav_mode": "arrows", "scroll_mode": "wheel_arrows", "angle_mode": "volume",
        "actions": {
            "key1": {"type": "consumer_tap", "key": "play_pause", "mods": [],
                     "fn": "media"},
            "key2": {"type": "consumer_tap", "key": "next", "mods": [], "fn": "media"},
            "chain_key": {"type": "consumer_tap", "key": "mute", "mods": [],
                          "fn": "media"},
            "chain_key_double": {"type": "none", "key": "", "mods": [], "fn": "custom"},
            "nav_click": {"type": "consumer_tap", "key": "stop", "mods": [],
                          "fn": "media"},
            "scroll_click": {"type": "mouse_btn", "key": "middle", "mods": [],
                             "fn": "mouse_m"},
        },
    },
]


class FakeSidecar:
    """A serial port that answers the protocol from a dict of state."""

    def __init__(self) -> None:
        self.outgoing: queue.Queue[bytes] = queue.Queue()
        self.config = {**DEFAULT_CONFIG, "layer_rgb": [[0, 90, 255], [255, 110, 0]]}
        self.closed = False

    def write(self, data: bytes) -> int:
        command = json.loads(data.decode("utf-8"))
        for reply in self._answer(command):
            self.outgoing.put((json.dumps(reply) + "\n").encode("utf-8"))
        return len(data)

    def readline(self) -> bytes:
        try:
            return self.outgoing.get(timeout=0.05)
        except queue.Empty:
            return b""

    def close(self) -> None:
        self.closed = True

    def _answer(self, command: dict) -> list[dict]:
        msg_id = command.get("id")
        cmd = command.get("cmd")
        if cmd == "hello":
            return [{"ev": "hello", "id": msg_id, "fw": "0.2.0", "proto": 2,
                     "layers": [layer["name"] for layer in LAYERS], "layer": 0,
                     "nodes": NODES, "companion": {"linked": True}}]
        if cmd == "status":
            return [{"ev": "status", "id": msg_id, "layer": 0, "vbat_mv": 4038,
                     "usb_mv": 5017, "uptime_s": 4271, "nodes": NODES,
                     "companion": {"linked": True}}]
        if cmd == "get_config":
            return [{"ev": "config", "id": msg_id, **self.config}]
        if cmd == "get_layers":
            return [{"ev": "layers", "id": msg_id, "layers": LAYERS}]
        if cmd == "set":
            self.config[command["path"]] = command["value"]
        return [{"ev": "ok", "id": msg_id}]


def install_fake() -> None:
    """Point the serial bridge at :class:`FakeSidecar` instead of a real port."""
    device_module.scan_ports = lambda lister=None: [
        {"port": "COM-FAKE", "product": "fethr sidecar", "vid": 0x303A,
         "pid": 0x1001, "serial_number": "CAPTURE"}
    ]
    device_module.SidecarDevice._open_serial = lambda self, port: FakeSidecar()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=str(ROOT / "assets" / "shots_builder"),
                        metavar="DIR", help="where to write the PNGs")
    args = parser.parse_args(argv)

    install_fake()
    os.environ["FETHR_SMOKE_SHOTS"] = args.out

    from fethr.__main__ import FethrApp

    settings = json.loads((ROOT / "assets" / "smoke_settings.json").read_text("utf-8"))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "settings.json"
        path.write_text(json.dumps(settings), encoding="utf-8")
        app = FethrApp(settings_file=path)
        code = app.run(smoke=True)

    print(f"captures in {args.out}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
