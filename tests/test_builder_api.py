"""The JavaScript-facing side of the chain builder (``fethr.ui.window.Api``).

No window, no webview and no hardware: the Api only ever touches the app
object, so a stand-in with a settings document and a fake device exercises all
of it.  The behaviour that matters most here is the protocol 1 fallback — an
older sidecar must never be *asked* for something it does not implement.
"""

from __future__ import annotations

import pytest

from fethr.core.device import SidecarError
from fethr.core.layout import LAYOUT_VERSION, empty_layout, normalise_layout
from fethr.core.settings import Settings, load_settings, save_settings
from fethr.ui.window import Api


class FakeDevice:
    """A sidecar that records what it was asked and answers plausibly."""

    def __init__(self, proto: int = 2, connected: bool = True, fail: str = "") -> None:
        self._proto = proto
        self.connected = connected
        self.requests: list[tuple[str, tuple, dict]] = []
        self.config: dict = {}
        self.fail = fail

    # -- the bits Api uses
    @property
    def proto(self) -> int:
        return self._proto if self.connected else 0

    @property
    def supports_actions(self) -> bool:
        return self.connected and self.proto >= 2

    def set_config(self, path, value):
        self.requests.append(("set", (path, value), {}))
        if self.fail == "set":
            raise SidecarError("unknown path")
        self.config[path] = value
        return {"ev": "ok"}

    def get_layers(self):
        self.requests.append(("get_layers", (), {}))
        if self.fail == "get_layers":
            raise SidecarError("timeout waiting for 'get_layers'")
        return [{
            "index": 0, "name": "FETHR", "rgb": [0, 90, 255],
            "nav_mode": "arrows", "scroll_mode": "wheel_pan", "angle_mode": "volume",
            "actions": {"key1": {"type": "key_hold", "key": "f8", "mods": [],
                                 "fn": "dict_raw"}},
        }]

    def set_action(self, layer, slot, action):
        self.requests.append(("set_action", (layer, slot, action), {}))
        return {"ev": "ok"}

    def set_layer_meta(self, layer, **fields):
        # The real one validates; this mirrors only the rule the Api's tests
        # care about, so a bad name still comes back as an error.
        if "name" in fields and not 1 <= len(str(fields["name"]).strip()) <= 8:
            raise SidecarError("a layer name must be 1 to 8 characters")
        self.requests.append(("set_layer_meta", (layer,), fields))
        return {"ev": "ok"}


class FakeApp:
    """The slice of :class:`fethr.__main__.FethrApp` the Api talks to."""

    def __init__(self, device: FakeDevice, settings_file) -> None:
        self.device = device
        self.settings = Settings()
        self.settings.legacy_imported = "already-done"
        self.settings_file = settings_file

    def set_sidecar_layout(self, layout):
        clean = normalise_layout(layout)
        self.settings.sidecar.layout = clean
        save_settings(self.settings, self.settings_file)
        return clean


@pytest.fixture
def api(tmp_path):
    def make(**kwargs):
        app = FakeApp(FakeDevice(**kwargs), tmp_path / "settings.json")
        return Api(app), app
    return make


# ------------------------------------------------------------ static model --


def test_builder_model_carries_the_protocol_facts(api):
    bridge, _ = api()
    model = bridge.builder_model()
    assert model["grid_px"] > 0
    assert len(model["canvas_cells"]) == 2
    assert model["layout_version"] == LAYOUT_VERSION
    assert model["proto_actions"] == 2
    assert set(model["modules"]) >= {"dualkey", "key", "joystick", "angle", "mono", "atom"}
    assert model["modules"]["dualkey"]["rotations"] == [0, 180]
    assert model["role_slots"] == {"nav": ["nav_click"], "scroll": ["scroll_click"]}
    assert model["vocabulary"]["types"][0]["id"] == "none"


# ----------------------------------------------------- protocol 1 fallback --


def test_protocol_1_device_is_never_asked_for_its_layers(api):
    """`get_layers` does not exist before firmware 0.2 — do not send it."""
    bridge, app = api(proto=1)
    reply = bridge.device_get_layers()

    assert app.device.requests == [], "the device must not be asked at all"
    assert reply["ok"] is True          # not an error, just an older device
    assert reply["editable"] is False
    assert reply["layers"] == []
    assert reply["proto"] == 1
    assert "0.2" in reply["hint"]


def test_disconnected_device_is_never_asked_either(api):
    bridge, app = api(connected=False)
    reply = bridge.device_get_layers()
    assert app.device.requests == []
    assert reply["editable"] is False
    assert reply["hint"]


def test_protocol_2_device_returns_its_key_map(api):
    bridge, app = api(proto=2)
    reply = bridge.device_get_layers()
    assert [name for name, *_ in app.device.requests] == ["get_layers"]
    assert reply["editable"] is True
    assert reply["layers"][0]["name"] == "FETHR"


def test_a_failing_get_layers_is_reported_not_raised(api):
    bridge, _ = api(proto=2, fail="get_layers")
    reply = bridge.device_get_layers()
    assert reply["ok"] is False
    assert "timeout" in reply["error"]
    assert reply["editable"] is False


def test_protocol_1_orientation_drops_what_it_cannot_store(api):
    bridge, app = api(proto=1)
    result = bridge.device_apply_orientation("joystick", 90, "nav")
    assert result["ok"] is True
    assert result["applied"] == {"nav_x_sign": 1, "nav_y_sign": -1}
    assert result["dropped"] == ["nav_swap_xy"]
    assert [path for _, (path, _value), _ in app.device.requests] == [
        "nav_x_sign", "nav_y_sign"
    ]


# ------------------------------------------------------------ orientation --


def test_orientation_is_pushed_one_path_at_a_time(api):
    bridge, app = api(proto=2)
    result = bridge.device_apply_orientation("joystick", 270, "scroll")
    assert result["ok"] is True
    assert app.device.config == {
        "scroll_swap_xy": True, "scroll_x_sign": -1, "scroll_y_sign": 1,
    }
    assert result["dropped"] == []


def test_orientation_stops_and_reports_on_a_refusal(api):
    bridge, _ = api(proto=2, fail="set")
    result = bridge.device_apply_orientation("dualkey", 180)
    assert result["ok"] is False
    assert "unknown path" in result["error"]
    assert result["applied"] == {}


def test_an_impossible_rotation_is_an_error_not_a_crash(api):
    bridge, app = api()
    result = bridge.layout_orientation("dualkey", 90)
    assert result["ok"] is False
    assert app.device.requests == []


# ---------------------------------------------------------- set_action ----


def test_set_action_sends_the_protocol_shape(api):
    bridge, app = api(proto=2)
    result = bridge.device_set_action(
        0, "key1", {"type": "key_hold", "key": "f8", "mods": [], "fn": "dict_raw"})
    assert result["ok"] is True
    name, args, _ = app.device.requests[-1]
    assert name == "set_action"
    assert args == (0, "key1",
                    {"type": "key_hold", "key": "f8", "mods": [], "fn": "dict_raw"})


def test_set_action_normalises_before_sending(api):
    """A modifier on a mouse binding is dropped rather than sent and refused."""
    bridge, app = api(proto=2)
    bridge.device_set_action(
        0, "nav_click", {"type": "mouse_btn", "key": "middle", "mods": ["ctrl"]})
    assert app.device.requests[-1][1][2]["mods"] == []


def test_set_action_rejects_an_unknown_key_without_asking_the_device(api):
    bridge, app = api(proto=2)
    result = bridge.device_set_action(0, "key1", {"type": "key_tap", "key": "any"})
    assert result["ok"] is False
    assert "cannot use key" in result["error"]
    assert app.device.requests == []


def test_set_layer_meta_passes_its_fields_through(api):
    bridge, app = api(proto=2)
    assert bridge.device_set_layer_meta(1, {"name": "MEDIA"})["ok"] is True
    assert app.device.requests[-1][2] == {"name": "MEDIA"}


def test_an_over_long_layer_name_is_refused_not_truncated(api):
    """PROTOCOL.md: names are 1-8 characters, and too long is an error."""
    bridge, _ = api(proto=2)
    result = bridge.device_set_layer_meta(0, {"name": "DICTATION"})
    assert result["ok"] is False
    assert "1 to 8" in result["error"]


# ------------------------------------------------------------ persistence --


def test_layout_round_trips_through_the_settings_file(api, tmp_path):
    bridge, app = api()
    arranged = bridge.layout_auto_arrange(
        [{"id": 1, "type": "key"}, {"id": 2, "type": "joystick"}], companion=False)
    arranged["nodes"]["key:1"]["rotation"] = 90
    arranged["nodes"]["key:1"]["x"] = 11

    saved = bridge.set_sidecar_layout(arranged)
    assert saved["ok"] is True
    assert bridge.get_sidecar_layout() == arranged

    reloaded = load_settings(app.settings_file)
    assert reloaded.sidecar.layout == arranged


def test_a_never_arranged_install_reads_back_an_empty_canvas(api):
    bridge, _ = api()
    assert bridge.get_sidecar_layout() == empty_layout()


def test_a_hand_edited_settings_file_cannot_break_the_canvas(api):
    bridge, app = api()
    app.settings.sidecar.layout = {"version": LAYOUT_VERSION, "nodes": {
        "key:1": {"module": "key", "bus_id": 1, "x": -40, "y": 9999, "rotation": 45},
    }}
    layout = bridge.get_sidecar_layout()
    node = layout["nodes"]["key:1"]
    assert node["x"] == 0 and node["y"] >= 0
    assert node["rotation"] in (0, 90, 180, 270)
    assert any(n["module"] == "dualkey" for n in layout["nodes"].values())
