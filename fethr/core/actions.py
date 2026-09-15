"""The key-mapping vocabulary, and the shape of a ``set_action`` command.

``hardware/PROTOCOL.md`` §"v2 additions" defines what a sidecar running protocol
2 will accept in an action: a type, a key *name* (never a scancode), a set of
modifiers, and a function class that decides the key's colour and its one-line
legend.  This module is the host's copy of that vocabulary.

It exists so the builder's key picker and the validation of what it sends come
from the same list.  A name this module does not know never reaches the wire —
the device would answer ``err`` and the page would have to explain a failure it
could have prevented.

Everything here is pure data and pure functions; :mod:`fethr.core.device` does
the talking.
"""

from __future__ import annotations

from typing import Any

__all__ = [
    "ACTION_TYPES",
    "ANGLE_MODES",
    "CONSUMER_KEYS",
    "FN_CLASSES",
    "KEYBOARD_KEYS",
    "MODIFIERS",
    "MOUSE_BUTTONS",
    "NAV_MODES",
    "SCROLL_MODES",
    "SLOTS",
    "action_request",
    "empty_action",
    "fn_palette_key",
    "keys_for_type",
    "normalise_action",
    "vocabulary",
]

#: Action types, in the order the editor's dropdown lists them.
ACTION_TYPES: tuple[str, ...] = (
    "none",
    "key_hold",
    "key_tap",
    "consumer_tap",
    "mouse_btn",
    "mouse_hold",
    "mouse_double",
)

#: Types whose ``key`` comes from :data:`KEYBOARD_KEYS` and that accept modifiers.
_KEYBOARD_TYPES = frozenset({"key_hold", "key_tap"})
#: Types whose ``key`` comes from :data:`MOUSE_BUTTONS`.
_MOUSE_TYPES = frozenset({"mouse_btn", "mouse_hold", "mouse_double"})

MODIFIERS: tuple[str, ...] = ("ctrl", "shift", "alt", "gui")

#: Keyboard key names.  Punctuation is the literal character, as the protocol
#: says, which is why the tail of this tuple looks like line noise.
KEYBOARD_KEYS: tuple[str, ...] = (
    *(chr(c) for c in range(ord("a"), ord("z") + 1)),
    *(str(d) for d in range(10)),
    *(f"f{n}" for n in range(1, 25)),
    "enter", "esc", "tab", "space", "backspace", "delete", "insert",
    "home", "end", "pageup", "pagedown",
    "up", "down", "left", "right",
    "capslock", "printscreen",
    "-", "=", "[", "]", ";", "'", ",", ".", "/", "\\", "`",
)

CONSUMER_KEYS: tuple[str, ...] = (
    "play_pause", "next", "prev", "stop", "mute",
    "vol_up", "vol_down", "brightness_up", "brightness_down",
)

MOUSE_BUTTONS: tuple[str, ...] = ("left", "right", "middle")

#: Function classes.  ``custom`` is the escape hatch: neutral colour, and the
#: legend falls back to the key name.
FN_CLASSES: tuple[str, ...] = (
    "dict_raw", "dict_clean", "repaste", "media", "undo", "redo",
    "mouse_l", "mouse_r", "mouse_m", "enter", "custom",
)

#: Bindable slots (``PROTOCOL.md`` ``get_layers`` → ``actions``).
SLOTS: tuple[str, ...] = (
    "key1", "key2", "chain_key", "chain_key_double", "nav_click", "scroll_click",
)

NAV_MODES: tuple[str, ...] = ("arrows", "mouse", "off")
SCROLL_MODES: tuple[str, ...] = ("wheel_pan", "wheel_arrows", "off")
ANGLE_MODES: tuple[str, ...] = ("volume", "wheel", "off")

#: Human labels for the editor.  Only where the identifier is not self-explaining.
LABELS: dict[str, str] = {
    "none": "Nothing",
    "key_hold": "Key — hold",
    "key_tap": "Key — tap",
    "consumer_tap": "Media key",
    "mouse_btn": "Mouse button",
    "mouse_hold": "Mouse button — hold",
    "mouse_double": "Mouse — double click",
    "key1": "Key 1",
    "key2": "Key 2",
    "chain_key": "Tap",
    "chain_key_double": "Double-tap",
    "nav_click": "Click",
    "scroll_click": "Click",
    "gui": "Win",
    "play_pause": "Play / pause",
    "next": "Next track",
    "prev": "Previous track",
    "vol_up": "Volume up",
    "vol_down": "Volume down",
    "brightness_up": "Brightness up",
    "brightness_down": "Brightness down",
    "wheel_pan": "Wheel, X pans",
    "wheel_arrows": "Wheel, X arrows",
}


def label_for(name: str) -> str:
    """A human label for an identifier, falling back to a tidied-up version."""
    if name in LABELS:
        return LABELS[name]
    return name.replace("_", " ").capitalize() if len(name) > 1 else name


def fn_palette_key(fn: str) -> str | None:
    """The ``fn_rgb`` key a function class takes its colour from.

    The config object spells these in upper case (``DICT_RAW``); the action
    vocabulary spells them in lower (``dict_raw``).  ``custom`` deliberately has
    no palette entry — it is the neutral one.
    """
    upper = str(fn).upper()
    return None if upper == "CUSTOM" else upper


def keys_for_type(action_type: str) -> tuple[str, ...]:
    """The key vocabulary a given action type draws from."""
    if action_type in _KEYBOARD_TYPES:
        return KEYBOARD_KEYS
    if action_type == "consumer_tap":
        return CONSUMER_KEYS
    if action_type in _MOUSE_TYPES:
        return MOUSE_BUTTONS
    return ()


def empty_action() -> dict[str, Any]:
    """An unbound action."""
    return {"type": "none", "key": "", "mods": [], "fn": "custom"}


def normalise_action(action: Any) -> dict[str, Any]:
    """Coerce ``action`` into something the device will accept.

    Raises:
        ValueError: if the type is unknown, the key is not in that type's
            vocabulary, or a modifier is not one of :data:`MODIFIERS`.  A
            ``none`` action is allowed to carry leftover fields; they are
            dropped rather than rejected, so clearing a slot always works.
    """
    if not isinstance(action, dict):
        raise ValueError("action must be an object")

    action_type = str(action.get("type", "none"))
    if action_type not in ACTION_TYPES:
        raise ValueError(f"unknown action type: {action_type}")
    if action_type == "none":
        return empty_action()

    key = str(action.get("key", ""))
    allowed = keys_for_type(action_type)
    if key not in allowed:
        raise ValueError(f"{action_type} cannot use key {key!r}")

    raw_mods = action.get("mods") or []
    if not isinstance(raw_mods, (list, tuple)):
        raise ValueError("mods must be a list")
    mods = [str(m) for m in raw_mods]
    unknown = [m for m in mods if m not in MODIFIERS]
    if unknown:
        raise ValueError(f"unknown modifier(s): {', '.join(unknown)}")
    if mods and action_type not in _KEYBOARD_TYPES:
        # The protocol restricts modifiers to the keyboard types; silently
        # dropping them beats an err the user cannot explain.
        mods = []

    fn = str(action.get("fn", "custom"))
    if fn not in FN_CLASSES:
        fn = "custom"

    # Keep the protocol's field order, and de-duplicate modifiers without
    # losing the order the user ticked them in.
    return {
        "type": action_type,
        "key": key,
        "mods": list(dict.fromkeys(mods)),
        "fn": fn,
    }


def action_request(layer: int, slot: str, action: Any) -> dict[str, Any]:
    """Build the fields of a ``set_action`` command.

    Returns the command's arguments (not the envelope: ``cmd`` and ``id`` are
    :meth:`fethr.core.device.SidecarDevice.request`'s business)::

        {"layer": 0, "slot": "key1", "action": {...}}

    Raises:
        ValueError: on an unknown slot, a negative layer, or an action
            :func:`normalise_action` rejects.
    """
    slot = str(slot)
    if slot not in SLOTS:
        raise ValueError(f"unknown slot: {slot}")
    index = int(layer)
    if index < 0:
        raise ValueError(f"layer out of range: {layer}")
    return {"layer": index, "slot": slot, "action": normalise_action(action)}


def vocabulary() -> dict[str, Any]:
    """Everything the key picker needs, in one JSON-friendly blob.

    The page builds its dropdowns from this rather than keeping a second copy
    of the protocol in JavaScript.
    """
    return {
        "types": [{"id": t, "label": label_for(t), "keys": list(keys_for_type(t))}
                  for t in ACTION_TYPES],
        "modifiers": [{"id": m, "label": label_for(m)} for m in MODIFIERS],
        "fn_classes": [{"id": f, "label": label_for(f), "palette": fn_palette_key(f)}
                       for f in FN_CLASSES],
        "key_labels": {k: label_for(k) for k in CONSUMER_KEYS},
        "slots": {s: label_for(s) for s in SLOTS},
        "nav_modes": [{"id": m, "label": label_for(m)} for m in NAV_MODES],
        "scroll_modes": [{"id": m, "label": label_for(m)} for m in SCROLL_MODES],
        "angle_modes": [{"id": m, "label": label_for(m)} for m in ANGLE_MODES],
    }
