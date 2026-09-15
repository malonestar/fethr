"""Geometry and orientation for the chain builder.

The Sidecar page's **Layout** tab lets you lay your own sidecar out on a grid:
where each M5Stack Chain module physically sits on the desk and which way round
it is mounted.  Two things come out of that.

Placement
    Purely cosmetic, and purely the user's: positions and rotations are saved in
    ``settings.sidecar.layout`` so the picture survives a restart.  The *order*
    of the chain is not editable — it is physical, and the device reports it.

Orientation
    Not cosmetic.  A joystick screwed on sideways still has to send "up" when
    you push it away from you, and the firmware's axis settings are how that is
    expressed.  :func:`orientation_settings` is the single place that turns a
    rotation into those settings, so the rule is in one testable function
    instead of scattered through the page's JavaScript.

Everything here is pure: no device, no I/O, no UI.  ``fethr.ui.window.Api``
calls it on behalf of the page.
"""

from __future__ import annotations

from typing import Any, Iterable

__all__ = [
    "CANVAS_CELLS",
    "GRID_PX",
    "LAYOUT_VERSION",
    "MODULES",
    "PROTO2_PATHS",
    "ModuleSpec",
    "auto_arrange",
    "clamp_cell",
    "empty_layout",
    "filter_for_proto",
    "layout_key",
    "losses_for_proto",
    "normalise_layout",
    "orientation_settings",
    "rotations_for",
    "slots_for",
]

#: Bumped when a stored layout can no longer be read as-is.  A document with an
#: unknown version is discarded rather than guessed at — the layout is a
#: convenience, and rebuilding it is one click of Auto-arrange.
LAYOUT_VERSION = 1

#: Snap grid, in CSS pixels.  The page uses the same number for its background
#: grid, so it lives here rather than in two places.  Small enough that a whole
#: chain fits beside the inspector without panning: a module is three cells, so
#: this is a 54 px picture of a 24 mm block.
GRID_PX = 18

#: Canvas size in grid cells.  A module is 3 cells wide on a 5-cell pitch, so
#: this is six modules across and four down.
CANVAS_CELLS = (28, 16)


class ModuleSpec:
    """Static facts about one kind of module.

    Attributes:
        label: the module's full name, as the picker lists it.
        short: what the badge under a node on the canvas says.  Six modules in
            a row do not have room for "Chain Joystick" twice.
        sprite: file name under ``fethr/ui/web/img/``.
        cells: footprint as ``(width, height)`` in grid cells.
        rotations: the rotations this module may be mounted at.
        slots: bindable action slots (``PROTOCOL.md`` §Actions), in editor order.
        chained: True for modules that sit on the Chain bus and so take part in
            the IN → OUT connector.  The DualKey is the bus master and the Atom
            hangs off the spare port, so neither is chained.
    """

    __slots__ = ("label", "short", "sprite", "cells", "rotations", "slots", "chained")

    def __init__(
        self,
        label: str,
        short: str,
        sprite: str,
        cells: tuple[int, int],
        rotations: tuple[int, ...],
        slots: tuple[str, ...] = (),
        chained: bool = True,
    ) -> None:
        self.label = label
        self.short = short
        self.sprite = sprite
        self.cells = cells
        self.rotations = rotations
        self.slots = slots
        self.chained = chained

    def as_dict(self) -> dict[str, Any]:
        """JSON-friendly copy for the page."""
        return {
            "label": self.label,
            "short": self.short,
            "sprite": self.sprite,
            "cells": list(self.cells),
            "rotations": list(self.rotations),
            "slots": list(self.slots),
            "chained": self.chained,
        }


#: Module types the builder knows, keyed by the ``type`` the device reports in
#: its ``nodes`` list (plus ``dualkey`` and ``atom``, which are not bus nodes).
MODULES: dict[str, ModuleSpec] = {
    # Two keys wide, and it can only be the right way up or upside down: the
    # USB socket has to face somewhere, and 90 degrees would put the keys on
    # their side.  180 is "USB away from you", which is `swap_keys`.
    "dualkey": ModuleSpec("DualKey", "DualKey", "dualkey.png", (6, 3), (0, 180),
                          ("key1", "key2"), chained=False),
    "key": ModuleSpec("Chain Key", "Key", "key.png", (3, 3), (0, 90, 180, 270),
                      ("chain_key", "chain_key_double")),
    "joystick": ModuleSpec("Chain Joystick", "Joystick", "joystick.png", (3, 3),
                           (0, 90, 180, 270), ("nav_click",)),
    "angle": ModuleSpec("Chain Angle", "Angle", "angle.png", (3, 3), (0, 90, 180, 270)),
    "mono": ModuleSpec("Chain Mono", "Mono", "mono.png", (3, 3), (0, 90, 180, 270)),
    "atom": ModuleSpec("AtomS3R companion", "Companion", "atom.png", (3, 3),
                       (0, 90, 180, 270), chained=False),
}

#: Config paths that only exist from protocol 2 onwards, with the value a
#: protocol 1 device behaves as if it had.  Sending one of these to a 0.1.x
#: sidecar earns an ``err``, so :func:`filter_for_proto` drops them — and
#: dropping one that was only going to be set to its default costs nothing,
#: which is why a half turn works on old firmware and a quarter turn does not.
PROTO2_DEFAULTS: dict[str, Any] = {
    "swap_keys": False, "nav_swap_xy": False, "scroll_swap_xy": False,
}

#: Just the names, for membership tests.
PROTO2_PATHS = frozenset(PROTO2_DEFAULTS)


# --------------------------------------------------------------------------
# rotation -> device settings
# --------------------------------------------------------------------------


def orientation_settings(
    module: str, rotation: int, role: str | None = None
) -> dict[str, Any]:
    """Return the device config a module mounted at ``rotation`` needs.

    Args:
        module: a key of :data:`MODULES`.
        rotation: degrees **clockwise**, as the canvas draws it (CSS
            ``rotate()``).  Values outside 0/90/180/270 are reduced modulo 360;
            anything the module cannot be mounted at raises ``ValueError``.
        role: for a joystick, ``"nav"`` (default) or ``"scroll"`` — the device
            reports which is which in its ``nodes`` list, and they have separate
            axis settings.

    Returns:
        ``{config path: value}``, ready to hand to ``set`` one pair at a time.
        Empty for modules whose mounting the firmware does not need to know
        about (the Angle knob has no axes; the Atom is a display).

    The joystick truth table
    ------------------------
    Let the module's own axes be *ax* (positive = the stick pushed toward the
    module's own right) and *ay* (positive = pushed toward the module's own
    top), both as they read with the module upright.  The firmware computes::

        u, v   = (ay, ax) if nav_swap_xy else (ax, ay)
        right  = u * nav_x_sign > 0
        up     = v * nav_y_sign > 0

    Rotating the module clockwise by θ on the desk rotates its axes with it, so
    the screen-relative deflection the user intends is::

        screen_x =  ax·cos θ + ay·sin θ
        screen_y = -ax·sin θ + ay·cos θ

    Solving ``u·x_sign = screen_x`` and ``v·y_sign = screen_y`` at each quarter
    turn gives:

    =========  ============  ==========  ==========
    rotation   swap_xy       x_sign      y_sign
    =========  ============  ==========  ==========
    0          False         +1          +1
    90         True          +1          -1
    180        False         -1          -1
    270        True          -1          +1
    =========  ============  ==========  ==========

    which reads as: half a turn flips both signs and swaps nothing; a quarter
    turn swaps the axes and flips exactly one of them.

    The Mono panel is simpler: ``mono_rotation`` *is* the mounting angle, and
    the firmware rotates its framebuffer by it so the layer letter reads upright
    on a module lying on its side.

    >>> orientation_settings("joystick", 90)
    {'nav_swap_xy': True, 'nav_x_sign': 1, 'nav_y_sign': -1}
    >>> orientation_settings("dualkey", 180)
    {'swap_keys': True}
    """
    spec = MODULES.get(module)
    if spec is None:
        raise ValueError(f"unknown module: {module}")
    rotation = int(rotation) % 360
    if rotation not in spec.rotations:
        raise ValueError(f"{module} cannot be mounted at {rotation}°")

    if module == "dualkey":
        # 180° is USB-up, which puts the physical keys the other way round.
        return {"swap_keys": rotation == 180}

    if module == "joystick":
        prefix = "scroll" if (role or "nav") == "scroll" else "nav"
        swap, x_sign, y_sign = _AXIS_TABLE[rotation]
        return {
            f"{prefix}_swap_xy": swap,
            f"{prefix}_x_sign": x_sign,
            f"{prefix}_y_sign": y_sign,
        }

    if module == "mono":
        return {"mono_rotation": rotation}

    # Angle: a knob has no handedness the firmware can act on — turning the
    # module turns the detents with it.  Key / Atom: nothing to orient.
    return {}


#: rotation -> (swap_xy, x_sign, y_sign).  Derived in :func:`orientation_settings`.
_AXIS_TABLE: dict[int, tuple[bool, int, int]] = {
    0: (False, 1, 1),
    90: (True, 1, -1),
    180: (False, -1, -1),
    270: (True, -1, 1),
}


def filter_for_proto(values: dict[str, Any], proto: int) -> dict[str, Any]:
    """Drop config paths a device on protocol ``proto`` would reject.

    A 0.1.x sidecar has signs but no ``swap_xy`` and no ``swap_keys``, so a
    quarter-turn joystick cannot be fully expressed there.  What *can* be sent
    still is — a half turn works everywhere.
    """
    if proto >= 2:
        return dict(values)
    return {k: v for k, v in values.items() if k not in PROTO2_PATHS}


def losses_for_proto(values: dict[str, Any], proto: int) -> list[str]:
    """Paths ``proto`` cannot store *and* whose absence changes the behaviour.

    :func:`filter_for_proto` drops every protocol 2 path; most of the time the
    value being dropped is the firmware's own default, so nothing is actually
    lost and the user should not be told otherwise.  This is the list worth
    warning about.

    >>> losses_for_proto(orientation_settings("joystick", 180), proto=1)
    []
    >>> losses_for_proto(orientation_settings("joystick", 90), proto=1)
    ['nav_swap_xy']
    """
    if proto >= 2:
        return []
    return sorted(
        path for path, value in values.items()
        if path in PROTO2_DEFAULTS and value != PROTO2_DEFAULTS[path]
    )


def rotations_for(module: str) -> tuple[int, ...]:
    """Rotations ``module`` may be mounted at (``()`` if it is unknown)."""
    spec = MODULES.get(module)
    return spec.rotations if spec else ()


def slots_for(module: str, role: str | None = None) -> tuple[str, ...]:
    """Bindable action slots for one placed module.

    Only the joystick depends on ``role``: the device gives one of them the
    nav job and the other the scroll wheel, and they have separate slots
    (``nav_click`` / ``scroll_click``) as well as separate axis settings.
    """
    spec = MODULES.get(module)
    if spec is None:
        return ()
    if module == "joystick" and (role or "nav") == "scroll":
        return ("scroll_click",)
    return spec.slots


# --------------------------------------------------------------------------
# the stored layout
# --------------------------------------------------------------------------


def layout_key(module: str, bus_id: Any) -> str:
    """Stable identity for one placed node: module type plus bus id.

    Positions have to survive re-enumeration, and the device's ``nodes`` list is
    the only identity available — there are no serial numbers on the bus.  Two
    joysticks therefore keep their places as long as they keep their ids, which
    is what unplugging and replugging the *same* chain gives you.
    """
    return f"{module}:{bus_id}"


def empty_layout() -> dict[str, Any]:
    """A layout with nothing but the DualKey at the origin."""
    return {
        "version": LAYOUT_VERSION,
        "nodes": {
            layout_key("dualkey", 0): {
                "module": "dualkey", "bus_id": 0, "x": 0, "y": 0, "rotation": 0,
            }
        },
    }


def clamp_cell(x: int, y: int, module: str) -> tuple[int, int]:
    """Keep a node's top-left corner inside the canvas."""
    cols, rows = CANVAS_CELLS
    spec = MODULES.get(module)
    w, h = spec.cells if spec else (3, 3)
    return (
        max(0, min(int(x), cols - w)),
        max(0, min(int(y), rows - h)),
    )


def normalise_layout(raw: Any) -> dict[str, Any]:
    """Return a clean layout document, whatever ``raw`` turns out to be.

    Settings files are user-editable and survive upgrades, so this tolerates
    every kind of junk: a non-dict, a future version, unknown module types,
    string coordinates, rotations the module cannot be mounted at.  Anything it
    cannot make sense of is dropped rather than repaired, and a document with
    no DualKey gets one, because the builder always has an origin.
    """
    if not isinstance(raw, dict) or raw.get("version") != LAYOUT_VERSION:
        return empty_layout()

    incoming = raw.get("nodes")
    if not isinstance(incoming, dict):
        incoming = {}

    nodes: dict[str, dict[str, Any]] = {}
    for key, node in incoming.items():
        if not isinstance(node, dict):
            continue
        module = str(node.get("module", ""))
        spec = MODULES.get(module)
        if spec is None:
            continue
        try:
            x, y = clamp_cell(int(node.get("x", 0)), int(node.get("y", 0)), module)
            rotation = int(node.get("rotation", 0)) % 360
        except (TypeError, ValueError):
            continue
        if rotation not in spec.rotations:
            rotation = spec.rotations[0]
        bus_id = node.get("bus_id", 0)
        if not isinstance(bus_id, (int, str)):
            continue
        nodes[str(key)] = {
            "module": module, "bus_id": bus_id,
            "x": x, "y": y, "rotation": rotation,
        }

    if not any(n["module"] == "dualkey" for n in nodes.values()):
        nodes.update(empty_layout()["nodes"])
    return {"version": LAYOUT_VERSION, "nodes": nodes}


# --------------------------------------------------------------------------
# auto-arrange
# --------------------------------------------------------------------------

#: Grid cells between one module's left edge and the next one's.  Two cells
#: wider than a module, which is the room the IN/OUT badges and the connector
#: need between neighbours.
PITCH = 5


def auto_arrange(
    nodes: Iterable[dict[str, Any]], companion: bool = False
) -> dict[str, Any]:
    """Lay the device's reported chain out in reading order.

    The DualKey goes top-left, the bus nodes run to its right in the order the
    device enumerated them, and a run too long for one row U-turns onto the next
    rather than running off the canvas.  A companion Atom, which hangs off the
    DualKey's *other* Chain port, is placed to its left — which is also why the
    row length is computed rather than fixed: a companion costs a module's worth
    of width.

    Args:
        nodes: the device's ``nodes`` list — dicts with ``type`` and ``id``.
        companion: whether an AtomS3R is linked.

    Returns:
        A layout document, ready for :func:`normalise_layout`.
    """
    layout = empty_layout()
    placed = layout["nodes"]
    dual = placed[layout_key("dualkey", 0)]
    dual_w = MODULES["dualkey"].cells[0]

    # Room on the left for the companion, so the DualKey is not flush with the
    # edge when one is attached.
    dual["x"], dual["y"] = (PITCH if companion else 0), 0
    if companion:
        placed[layout_key("atom", "companion")] = {
            "module": "atom", "bus_id": "companion",
            "x": 0, "y": 0, "rotation": 0,
        }

    row, column = 0, 0
    # PITCH - 3 cells of clear air after the DualKey, the same gap the nodes
    # keep from each other: any less and neighbouring IN/OUT badges collide.
    first_x = dual["x"] + dual_w + (PITCH - 3)
    per_row = max(1, (CANVAS_CELLS[0] - first_x + (PITCH - 3)) // PITCH)
    for node in nodes:
        module = str(node.get("type", ""))
        if module not in MODULES or not MODULES[module].chained:
            continue
        if column == per_row:
            column, row = 0, row + 1
        # Odd rows run right-to-left, so the chain doubles back instead of
        # jumping across the canvas.
        slot = column if row % 2 == 0 else (per_row - 1 - column)
        x = first_x + slot * PITCH
        y = dual["y"] + row * PITCH
        x, y = clamp_cell(x, y, module)
        placed[layout_key(module, node.get("id", column))] = {
            "module": module, "bus_id": node.get("id", column),
            "x": x, "y": y, "rotation": 0,
        }
        column += 1

    return normalise_layout(layout)
