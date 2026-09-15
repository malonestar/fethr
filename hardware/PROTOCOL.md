# fethr sidecar ⇄ host protocol (v1)

Transport: the sidecar's USB CDC serial port (same cable as the HID device),
115200 8N1. **Newline-delimited JSON**, one object per line, UTF-8, max 512 bytes
per line. Lines that do not start with `{` are the human console (`?` etc.) and
are ignored by the app; lines the device prints for humans never start with `{`.

Host identifies the device by USB VID `0x303A` (Espressif) and product string
`fethr sidecar` (set via `USB.productName()`), then sends `hello`.

Every host command gets exactly one reply object with the same `"id"` if one was
supplied. Device may also emit unsolicited **events** at any time.

## Host → device commands

| cmd | fields | reply |
|---|---|---|
| `hello` | — | `{"ev":"hello","fw":"0.1.0","proto":1,"layers":["FLOW","MEDIA","EDIT","MOUSE"],"layer":0,"nodes":[...]}` |
| `status` | — | `{"ev":"status","layer":0,"vbat_mv":4012,"usb_mv":5010,"holds":[false,false],"nodes":[{"id":1,"type":"key"},{"id":2,"type":"joystick","role":"nav"},...],"companion":{"linked":false},"uptime_s":123}` |
| `get_config` | — | `{"ev":"config", ...full config object below...}` |
| `set` | `"path"`, `"value"` (see config paths) | `{"ev":"ok","path":...}` or `{"ev":"err","msg":...}` |
| `save` | — | `{"ev":"ok"}` — persist current config to NVS |
| `reset_config` | — | `{"ev":"ok"}` — factory defaults (RAM + NVS) |
| `layer` | `"index"` (0 .. `layers.length - 1`) | `{"ev":"ok"}` + a `layer` event |
| `identify` | — | `{"ev":"ok"}`; flashes both key LEDs white 3× and shows "ID" on Mono |
| `mono` | `"text"` (≤32 chars) | `{"ev":"ok"}`; scrolls the text once (app-driven notifications) |
| `state` | `"value"` (see below) | `{"ev":"ok"}`; tells the device what the host's dictation engine is doing |

**Layer count is whatever `hello` reports.** The current shipped image builds all four
(`FLOW_EXTRA_LAYERS 1`); a build with the flag off reports `["FLOW"]` alone, and a
future one could report more. Do not assume four — read the array.

## Host engine state (`state`)

```json
{"cmd":"state","value":"transcribing"}
```

`value` ∈ `idle` | `recording` | `transcribing` | `cleaning` | `pasted` | `error`.
Anything else → `{"ev":"err","msg":"unknown state"}`. The host is expected to send
one on every engine state change; the device keeps no timeout of its own.

| value | device |
|---|---|
| `transcribing`, `cleaning` | Mono shows a 4-frame rotating-dot spinner (~120 ms/frame) for as long as the state is current; both key LEDs breathe amber |
| `pasted` | Mono shows ✓ for 700 ms; one short green flash on both key LEDs |
| `error` | Mono shows ✗ for 900 ms; two red flashes |
| `idle`, `recording` | clears the above; recording is already driven by the physical hold, so it is otherwise a no-op. A running ✓/✗ is left to expire, so `pasted` immediately followed by `idle` still shows its checkmark |

A physical hold outranks host state on the panel: while Key1/Key2 is held the mic
glyph wins, whatever the host last said.

### The checkmark moved

Up to 0.1.0 the panel showed ✓ as soon as a dictation key was **released**. That is
the wrong moment — the text has not landed yet. Now:

* **host attached** → releasing the key shows nothing; the ✓ arrives with
  `state: pasted`;
* **no host** (the device used as a plain USB keyboard) → the old release-time ✓,
  because it is the only feedback available.

"Attached" means a `hello` **or** a `state` command within the last 60 s. `state`
counts deliberately: a host says `hello` once per connection, and dictating pushes
states constantly, so the window stays fresh for as long as the app is actually
driving the device.

## Config object (`get_config` reply / `set` paths)

```json
{
  "layer_rgb":   [[0,90,255]],
  "fn_rgb": {"DICT_RAW":[0,90,255],"DICT_CLEAN":[170,0,255],"REPASTE":[0,200,200],
             "MEDIA":[255,110,0],"UNDO":[0,255,90],"REDO":[0,255,180],
             "MOUSE_L":[255,255,255],"MOUSE_R":[255,255,255],"MOUSE_M":[255,255,255],
             "ENTER":[255,255,120]},
  "hold_rgb":    [255,0,0],
  "led_idle_pct": 18,
  "led_flash_ms": 80,
  "node_leds":   true,
  "mono_brightness": 5,
  "mono_rotation": 0,
  "mono_idle":   "letter",
  "double_tap":  true,
  "double_tap_ms": 350,
  "layer_hold_ms": 1000,
  "boot_layer":  0,
  "nav_y_sign": 1, "nav_x_sign": 1, "scroll_y_sign": 1, "scroll_x_sign": 1, "mouse_y_sign": 1,
  "led_index_key1": 0
}
```

`layer_rgb` has one entry per built layer. `fn_rgb` always carries the full palette,
including the functions only the optional layers use.

`set` uses dotted/indexed paths: `{"cmd":"set","path":"layer_rgb.0","value":[255,120,0]}`,
`{"cmd":"set","path":"fn_rgb.DICT_RAW","value":[0,120,255]}`,
`{"cmd":"set","path":"mono_idle","value":"blank"}`. `mono_idle` ∈ `blank|letter`.
`mono_rotation` ∈ 0|90|180|270. Signs ∈ -1|1. Changes apply immediately in RAM;
`save` persists. Unknown path → `err`.

## Device → host events (unsolicited)

| ev | fields | when |
|---|---|---|
| `layer` | `index`, `name` | layer changed (by key or by command) |
| `hold` | `key` (1|2), `active` (bool), `fn` | a hold action starts/stops (the app can show "recording") |
| `tap` | `key` ("1","2","chain","chain2","nav","scroll") | any tap fired |
| `chain` | `nodes` | (re)enumeration finished |
| `battery` | `vbat_mv`, `low` (bool) | every 30 s |
| `boot` | `fw` | once at startup |

`companion` reports the optional [companion display](COMPANION.md) — an AtomS3R on the
DualKey's spare Chain port. It is not a Chain node, so it can never appear in `nodes`
and gets its own field. The field is always present, including in builds with the link
compiled out (`FLOW_COMPANION 0`), where it reads `false`: a host should not have to
tell "no companion" from "no field".

## Companion link

The same wire format runs on a second, entirely separate UART between the sidecar and
the optional companion display: newline-delimited JSON, 115200 8N1, lines that do not
start with `{` ignored. It is **not** the host protocol — a different, smaller set of
messages — but it is deliberately the same shape, and the `ev` names mean what they
mean above. [COMPANION.md](COMPANION.md) covers the wiring and the user-facing
behaviour; this section is the wire contract.

Neither end is a "host": both are microcontrollers on a straight-through cable, so
TX/RX orientation is unknown. **The sidecar probes**, alternating its two candidate pin
orders every 1.5 s until a line comes back; the companion's pins are fixed. The sidecar
also picks the port pair its Chain bus did *not* claim, so no configuration exists.

### Companion → sidecar

Lines are capped at **384 bytes** on this link (not the host protocol's 512) and, as
there, are dropped rather than truncated. The `hello` event is the only message that
gets anywhere near it.

| cmd | fields | effect |
|---|---|---|
| `hello` | `who`, `fw` | Sidecar replies with a `hello` event, then pushes the current `state`, `chain` and `battery` so the companion can draw a correct first frame |
| `ping` | — | Keepalive. No reply; silence for 10 s drops the link back to probing |
| `layer_next` | — | Cycle to the next layer (no-op with one layer built) |
| `layer_prev` | — | Cycle to the previous layer |
| `layer` | `index` | Jump to a layer; out of range is ignored |
| `identify` | — | Flash both key LEDs white 3× and show "ID" on Mono, exactly as the host command does |

Commands get no `ok`/`err` reply. The companion is a display, not a settings client:
its only feedback is the `layer` event a successful layer change produces anyway, and a
command it cannot honour is one it should not have sent.

### Sidecar → companion

| ev | fields | when |
|---|---|---|
| `hello` | `fw`, `layers`, `layer`, `rgb`, `legend` | Every 1.5 s while probing, and as the reply to `cmd:hello`. `layers` is the full name list, so the companion can label a layer it has never seen change; `rgb` is the current layer's **runtime** colour, so recolouring from the app follows through |
| `layer` | `index`, `name`, `rgb`, `legend` | Layer changed — from the Chain Key, the host, or the companion's own button |
| `state` | `value` | The engine state changed. Same value set as the host's `state` command |
| `hold` | `key` (1\|2), `active` | A dictation key went down or up |
| `tap` | `key`, `fn` | Any tap fired. Same `key` vocabulary as the host's `tap` event (`"1"`, `"2"`, `"chain"`, `"chain2"`, `"nav"`, `"scroll"`) |
| `knob` | `detent`, `of` | The angle knob crossed a detent. Rate-limited (see below) |
| `chain` | `count` | (Re)enumeration finished. A count, not the node list — the companion only shows a number |
| `battery` | `vbat_mv` | Every 30 s |

### `legend` — the key legend

```json
{"ev":"layer","index":1,"name":"MEDIA","rgb":[255,110,0],
 "legend":["K1: play/pause","K2: next track","K3: mute","stick arrows / vol"]}
```

Exactly **four** strings, each at most **20 characters**, in slot order: Key1, Key2,
Chain Key, then one line describing the nav stick and the angle knob as
`stick <role> / <role>`. The sidecar builds all four from `LAYERS[]` in `layerLegend()`
(`layers.cpp`) — the first three from the bound action, the fourth from `nav_mode` and
`angle_mode` — and the first three are built to fit **16** characters so a companion can
choose a larger font when they are short.

This exists so that **the companion holds no copy of the keymap**. It renders the four
strings it is given, which is why a binding changed in `layers.cpp` cannot leave a stale
legend on a screen: there is no second table to update. A consumer should treat a
`legend` with anything other than four string elements as absent and keep the previous
one, rather than display a half-updated legend.

### `tap` — and why `fn` is not an FnId

```json
{"ev":"tap","key":"chain","fn":"repaste"}
```

`fn` is the sidecar's **human label** for the action that fired (`actionLabel()` in
`layers.cpp`) — `"play/pause"`, `"mute"`, `"undo"`, `"right click"` — and deliberately
**not** the `FN_*` identifier the host protocol's `hold` event carries. The two are not
interchangeable: every control on the MEDIA layer is `FN_MEDIA`, so the identifier
cannot distinguish play/pause from mute, and picking an icon is exactly that
distinction. The field is omitted when the action has no label. These strings are prose,
not identifiers: unlike `FN_*`, renaming one breaks nothing.

### `knob`

```json
{"ev":"knob","detent":13,"of":24}
```

`detent` is `0 .. of-1`. Emitted on a detent change **whatever the layer does with the
knob**, including `ANGLE_OFF` — a knob that turns and shows nothing reads as a broken
screen rather than as an unbound control.

Rate-limited to **10 per second**, by coalescing rather than dropping: a movement inside
the gap replaces the pending value and `companionTick()` sends it when the gap expires,
so the position the knob came to rest at is always the last one on the wire. A queued
detent is discarded if the link drops, because it would otherwise arrive ahead of the
`hello` that says which layer it belongs to.

## Notes

- The device never blocks on serial; if the host is absent nothing changes.
- Config lives in NVS namespace `fethr` via `Preferences`; defaults are the
  compile-time values in `config.h` (the app's "Reset to defaults" = `reset_config`).
- `?` on the console still prints the human status dump.
