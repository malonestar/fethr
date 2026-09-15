"""The key-mapping vocabulary and the shape of what goes on the wire.

``hardware/PROTOCOL.md`` §"v2 additions" is the contract; these tests pin the
host's copy of it, including the rule that nothing the device would reject is
ever sent.
"""

from __future__ import annotations

import pytest

from fethr.core.actions import (
    ACTION_TYPES,
    CONSUMER_KEYS,
    FN_CLASSES,
    KEYBOARD_KEYS,
    MODIFIERS,
    MOUSE_BUTTONS,
    SLOTS,
    action_request,
    empty_action,
    fn_palette_key,
    keys_for_type,
    normalise_action,
    vocabulary,
)
from fethr.core.device import DEFAULT_CONFIG


# ------------------------------------------------------------- vocabulary --


def test_keyboard_vocabulary_covers_the_protocol_list():
    for name in ("a", "z", "0", "9", "f1", "f24", "enter", "esc", "tab", "space",
                 "backspace", "delete", "insert", "home", "end", "pageup",
                 "pagedown", "up", "down", "left", "right", "capslock",
                 "printscreen", "-", "=", "[", "]", ";", "'", ",", ".", "/",
                 "\\", "`"):
        assert name in KEYBOARD_KEYS, name
    assert len(KEYBOARD_KEYS) == len(set(KEYBOARD_KEYS)), "no duplicates"


def test_each_type_draws_from_the_right_key_list():
    assert keys_for_type("key_tap") is KEYBOARD_KEYS
    assert keys_for_type("consumer_tap") is CONSUMER_KEYS
    assert keys_for_type("mouse_double") is MOUSE_BUTTONS
    assert keys_for_type("none") == ()


def test_function_classes_map_onto_the_device_palette():
    """Every fn class but ``custom`` must name a real ``fn_rgb`` entry."""
    palette = DEFAULT_CONFIG["fn_rgb"]
    for fn in FN_CLASSES:
        key = fn_palette_key(fn)
        if fn == "custom":
            assert key is None
        else:
            assert key in palette, fn


def test_vocabulary_is_json_friendly_and_complete():
    vocab = vocabulary()
    assert [t["id"] for t in vocab["types"]] == list(ACTION_TYPES)
    assert [m["id"] for m in vocab["modifiers"]] == list(MODIFIERS)
    assert set(vocab["slots"]) == set(SLOTS)
    assert vocab["nav_modes"][0]["id"] == "arrows"


# -------------------------------------------------------------- normalise --


def test_empty_action_is_unbound():
    assert empty_action() == {"type": "none", "key": "", "mods": [], "fn": "custom"}


def test_a_none_action_drops_whatever_it_was_carrying():
    assert normalise_action({"type": "none", "key": "f8", "mods": ["ctrl"]}) == empty_action()


def test_modifiers_are_de_duplicated_in_the_order_they_were_given():
    action = normalise_action(
        {"type": "key_tap", "key": "z", "mods": ["shift", "ctrl", "shift"], "fn": "undo"}
    )
    assert action["mods"] == ["shift", "ctrl"]


def test_modifiers_are_dropped_for_types_that_cannot_use_them():
    action = normalise_action({"type": "mouse_btn", "key": "left", "mods": ["ctrl"]})
    assert action["mods"] == []


def test_an_unknown_function_class_falls_back_to_custom():
    assert normalise_action({"type": "key_tap", "key": "a", "fn": "sparkle"})["fn"] == "custom"


@pytest.mark.parametrize(
    "action, message",
    [
        ({"type": "teleport", "key": "a"}, "unknown action type"),
        ({"type": "key_tap", "key": "meta"}, "cannot use key"),
        ({"type": "consumer_tap", "key": "f8"}, "cannot use key"),
        ({"type": "mouse_btn", "key": "scroll"}, "cannot use key"),
        ({"type": "key_tap", "key": "a", "mods": ["hyper"]}, "unknown modifier"),
        ({"type": "key_tap", "key": "a", "mods": "ctrl"}, "mods must be a list"),
    ],
)
def test_bad_actions_are_rejected_here_not_by_the_device(action, message):
    with pytest.raises(ValueError, match=message):
        normalise_action(action)


def test_a_non_object_is_not_an_action():
    with pytest.raises(ValueError):
        normalise_action(["key_tap", "a"])


# --------------------------------------------------------- request shape --


def test_set_action_request_shape():
    """Exactly the fields PROTOCOL.md's `set_action` names, and nothing else."""
    request = action_request(
        1, "key1", {"type": "key_hold", "key": "f9", "mods": ["ctrl"], "fn": "dict_clean"}
    )
    assert request == {
        "layer": 1,
        "slot": "key1",
        "action": {"type": "key_hold", "key": "f9", "mods": ["ctrl"], "fn": "dict_clean"},
    }
    assert set(request["action"]) == {"type", "key", "mods", "fn"}


def test_request_accepts_a_layer_index_as_a_string():
    assert action_request("0", "chain_key", empty_action())["layer"] == 0


def test_request_rejects_an_unknown_slot():
    with pytest.raises(ValueError, match="unknown slot"):
        action_request(0, "key3", empty_action())


def test_request_rejects_a_negative_layer():
    with pytest.raises(ValueError, match="layer out of range"):
        action_request(-1, "key1", empty_action())
