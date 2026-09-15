"""Chain builder geometry: the rotation truth table, layouts and actions.

The rotation rule is the part of the builder that is not cosmetic — get it
wrong and a joystick sends "down" when you push it up — so it lives in Python
(:mod:`fethr.core.layout`) rather than in the page's JavaScript, and it is
tested here against the derivation written out in its docstring.
"""

from __future__ import annotations

import pytest

from fethr.core import actions
from fethr.core.layout import (
    CANVAS_CELLS,
    LAYOUT_VERSION,
    MODULES,
    auto_arrange,
    clamp_cell,
    empty_layout,
    filter_for_proto,
    layout_key,
    losses_for_proto,
    normalise_layout,
    orientation_settings,
    rotations_for,
    slots_for,
)


# ------------------------------------------------- the rotation truth table --

#: rotation -> (swap_xy, x_sign, y_sign). Half a turn flips both signs and
#: swaps nothing; a quarter turn swaps the axes and flips exactly one.
AXES = {
    0: (False, 1, 1),
    90: (True, 1, -1),
    180: (False, -1, -1),
    270: (True, -1, 1),
}


@pytest.mark.parametrize("rotation, expected", sorted(AXES.items()))
def test_nav_joystick_truth_table(rotation, expected):
    swap, x_sign, y_sign = expected
    assert orientation_settings("joystick", rotation) == {
        "nav_swap_xy": swap, "nav_x_sign": x_sign, "nav_y_sign": y_sign,
    }


@pytest.mark.parametrize("rotation, expected", sorted(AXES.items()))
def test_scroll_joystick_uses_its_own_axis_settings(rotation, expected):
    swap, x_sign, y_sign = expected
    assert orientation_settings("joystick", rotation, role="scroll") == {
        "scroll_swap_xy": swap, "scroll_x_sign": x_sign, "scroll_y_sign": y_sign,
    }


def test_a_half_turn_never_swaps_and_a_quarter_turn_always_does():
    """The shape of the table, stated once more as a property."""
    for rotation in (0, 180):
        assert orientation_settings("joystick", rotation)["nav_swap_xy"] is False
    for rotation in (90, 270):
        assert orientation_settings("joystick", rotation)["nav_swap_xy"] is True


def test_quarter_turns_flip_exactly_one_sign():
    for rotation in (90, 270):
        values = orientation_settings("joystick", rotation)
        negatives = [v for k, v in values.items() if k.endswith("_sign") and v == -1]
        assert len(negatives) == 1


def test_opposite_quarter_turns_are_mirror_images():
    ninety = orientation_settings("joystick", 90)
    seventy = orientation_settings("joystick", 270)
    assert ninety["nav_x_sign"] == -seventy["nav_x_sign"]
    assert ninety["nav_y_sign"] == -seventy["nav_y_sign"]


def test_dualkey_half_turn_swaps_the_keys():
    assert orientation_settings("dualkey", 0) == {"swap_keys": False}
    assert orientation_settings("dualkey", 180) == {"swap_keys": True}


def test_dualkey_cannot_be_mounted_on_its_side():
    with pytest.raises(ValueError):
        orientation_settings("dualkey", 90)


def test_mono_rotation_is_passed_straight_through():
    for rotation in (0, 90, 180, 270):
        assert orientation_settings("mono", rotation) == {"mono_rotation": rotation}


@pytest.mark.parametrize("module", ["key", "angle", "atom"])
def test_modules_without_orientable_hardware_need_no_settings(module):
    assert orientation_settings(module, 90) == {}


def test_rotation_is_reduced_modulo_a_full_turn():
    assert orientation_settings("joystick", 450) == orientation_settings("joystick", 90)


def test_unknown_module_is_rejected():
    with pytest.raises(ValueError, match="unknown module"):
        orientation_settings("toaster", 0)


def test_every_module_allows_at_least_one_rotation():
    for module in MODULES:
        assert rotations_for(module)
        assert orientation_settings(module, rotations_for(module)[0]) is not None


# ----------------------------------------------------------- proto filtering --


def test_protocol_1_cannot_express_a_quarter_turn_but_keeps_what_it_can():
    wanted = orientation_settings("joystick", 90)
    allowed = filter_for_proto(wanted, proto=1)
    assert "nav_swap_xy" not in allowed
    assert allowed == {"nav_x_sign": 1, "nav_y_sign": -1}


def test_protocol_1_still_manages_a_half_turn():
    """Both signs survive, and the swap it cannot store was False anyway."""
    wanted = orientation_settings("joystick", 180)
    assert filter_for_proto(wanted, proto=1) == {"nav_x_sign": -1, "nav_y_sign": -1}
    assert losses_for_proto(wanted, proto=1) == []


def test_protocol_1_loses_the_swap_a_quarter_turn_needs():
    assert losses_for_proto(orientation_settings("joystick", 90), proto=1) == ["nav_swap_xy"]


def test_protocol_1_drops_swap_keys():
    assert filter_for_proto(orientation_settings("dualkey", 180), proto=1) == {}
    assert losses_for_proto(orientation_settings("dualkey", 180), proto=1) == ["swap_keys"]
    assert losses_for_proto(orientation_settings("dualkey", 0), proto=1) == []


def test_protocol_2_keeps_everything():
    wanted = orientation_settings("joystick", 270)
    assert filter_for_proto(wanted, proto=2) == wanted
    assert losses_for_proto(wanted, proto=2) == []


# ------------------------------------------------------------ stored layouts --


def test_empty_layout_has_only_the_dualkey():
    layout = empty_layout()
    assert layout["version"] == LAYOUT_VERSION
    assert list(layout["nodes"]) == ["dualkey:0"]


def test_normalise_round_trips_a_good_document():
    layout = {
        "version": LAYOUT_VERSION,
        "nodes": {
            "dualkey:0": {"module": "dualkey", "bus_id": 0, "x": 2, "y": 1, "rotation": 180},
            "joystick:3": {"module": "joystick", "bus_id": 3, "x": 9, "y": 5, "rotation": 270},
        },
    }
    assert normalise_layout(layout) == layout


def test_normalise_survives_a_settings_file_round_trip(tmp_path):
    """The layout has to come back out of settings.json exactly as it went in."""
    from fethr.core.settings import Settings, load_settings, save_settings

    settings = Settings()
    settings.legacy_imported = "already-done"
    settings.sidecar.layout = auto_arrange(
        [{"id": 1, "type": "key"}, {"id": 2, "type": "joystick", "role": "nav"}],
        companion=True,
    )
    path = tmp_path / "settings.json"
    save_settings(settings, path)

    reloaded = load_settings(path)
    assert reloaded.sidecar.layout == settings.sidecar.layout
    assert normalise_layout(reloaded.sidecar.layout) == settings.sidecar.layout


def test_update_assigns_the_layout_whole_instead_of_flattening_it():
    """`sidecar.layout` is a document, not a section of settings."""
    from fethr.core.settings import Settings

    settings = Settings()
    document = empty_layout()
    settings.update({"sidecar": {"layout": document}})
    assert settings.sidecar.layout == document


@pytest.mark.parametrize(
    "junk",
    [
        None, [], "nope", {},
        {"version": 999, "nodes": {}},                    # a future version
        {"version": LAYOUT_VERSION, "nodes": ["key:1"]},  # nodes as a list
        {"version": LAYOUT_VERSION},                      # no nodes at all
    ],
)
def test_normalise_replaces_anything_it_cannot_read(junk):
    assert normalise_layout(junk) == empty_layout()


def test_normalise_drops_unknown_modules_and_keeps_the_rest():
    layout = normalise_layout({
        "version": LAYOUT_VERSION,
        "nodes": {
            "dualkey:0": {"module": "dualkey", "bus_id": 0, "x": 0, "y": 0, "rotation": 0},
            "toaster:1": {"module": "toaster", "bus_id": 1, "x": 0, "y": 0, "rotation": 0},
            "key:1": "not even an object",
        },
    })
    assert set(layout["nodes"]) == {"dualkey:0"}


def test_normalise_repairs_an_impossible_rotation():
    layout = normalise_layout({
        "version": LAYOUT_VERSION,
        "nodes": {
            "dualkey:0": {"module": "dualkey", "bus_id": 0, "x": 0, "y": 0, "rotation": 90},
        },
    })
    assert layout["nodes"]["dualkey:0"]["rotation"] == 0


def test_normalise_always_leaves_an_origin():
    layout = normalise_layout({
        "version": LAYOUT_VERSION,
        "nodes": {"key:1": {"module": "key", "bus_id": 1, "x": 4, "y": 0, "rotation": 0}},
    })
    assert any(n["module"] == "dualkey" for n in layout["nodes"].values())


def test_clamp_keeps_a_node_on_the_canvas():
    cols, rows = CANVAS_CELLS
    assert clamp_cell(-5, -5, "key") == (0, 0)
    assert clamp_cell(9999, 9999, "dualkey") == (cols - 6, rows - 3)


def test_layout_key_is_type_plus_bus_id():
    assert layout_key("joystick", 4) == "joystick:4"


# ------------------------------------------------------------ auto-arrange --

CHAIN = [
    {"id": 1, "type": "key"},
    {"id": 2, "type": "joystick", "role": "nav"},
    {"id": 3, "type": "joystick", "role": "scroll"},
    {"id": 4, "type": "angle"},
    {"id": 5, "type": "mono"},
]


def test_auto_arrange_places_every_reported_module():
    layout = auto_arrange(CHAIN)
    assert set(layout["nodes"]) == {
        "dualkey:0", "key:1", "joystick:2", "joystick:3", "angle:4", "mono:5",
    }


def test_auto_arrange_runs_left_to_right_then_turns_back():
    layout = auto_arrange(CHAIN)["nodes"]
    row_one = [layout[k]["x"] for k in ("key:1", "joystick:2", "joystick:3", "angle:4")]
    assert row_one == sorted(row_one), "the first four run left to right"
    assert layout["mono:5"]["y"] > layout["angle:4"]["y"], "the fifth drops a row"
    assert layout["mono:5"]["x"] == layout["angle:4"]["x"], "and doubles back under it"


def test_auto_arrange_makes_room_for_a_companion():
    with_atom = auto_arrange(CHAIN, companion=True)["nodes"]
    assert "atom:companion" in with_atom
    assert with_atom["atom:companion"]["x"] < with_atom["dualkey:0"]["x"]
    assert "atom:companion" not in auto_arrange(CHAIN)["nodes"]


def test_auto_arrange_ignores_things_that_are_not_bus_modules():
    layout = auto_arrange([{"id": 1, "type": "key"}, {"id": 2, "type": "mystery"}])
    assert set(layout["nodes"]) == {"dualkey:0", "key:1"}


def test_auto_arrange_keeps_everything_inside_the_canvas():
    cols, rows = CANVAS_CELLS
    for key, node in auto_arrange(CHAIN, companion=True)["nodes"].items():
        width, height = MODULES[node["module"]].cells
        assert 0 <= node["x"] <= cols - width, key
        assert 0 <= node["y"] <= rows - height, key


def test_auto_arrange_output_is_already_normalised():
    layout = auto_arrange(CHAIN, companion=True)
    assert normalise_layout(layout) == layout


# ------------------------------------------------------------------ slots --


def test_joystick_slots_follow_the_role_the_device_gave_it():
    assert slots_for("joystick") == ("nav_click",)
    assert slots_for("joystick", "nav") == ("nav_click",)
    assert slots_for("joystick", "scroll") == ("scroll_click",)


def test_every_declared_slot_is_one_the_protocol_knows():
    for module in MODULES:
        for role in (None, "nav", "scroll"):
            for slot in slots_for(module, role):
                assert slot in actions.SLOTS, f"{module}/{role}: {slot}"
