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
| `hello` | — | `{"ev":"hello","fw":"0.1.0","proto":1,"layers":["FLOW"],"layer":0,"nodes":[...]}` |
| `status` | — | `{"ev":"status","layer":0,"vbat_mv":4012,"usb_mv":5010,"holds":[false,false],"nodes":[{"id":1,"type":"key"},{"id":2,"type":"joystick","role":"nav"},...],"uptime_s":123}` |
| `get_config` | — | `{"ev":"config", ...full config object below...}` |
| `set` | `"path"`, `"value"` (see config paths) | `{"ev":"ok","path":...}` or `{"ev":"err","msg":...}` |
| `save` | — | `{"ev":"ok"}` — persist current config to NVS |
| `reset_config` | — | `{"ev":"ok"}` — factory defaults (RAM + NVS) |
| `layer` | `"index"` (0 .. `layers.length - 1`) | `{"ev":"ok"}` + a `layer` event |
| `identify` | — | `{"ev":"ok"}`; flashes both key LEDs white 3× and shows "ID" on Mono |
| `mono` | `"text"` (≤32 chars) | `{"ev":"ok"}`; scrolls the text once (app-driven notifications) |
| `state` | `"value"` (see below) | `{"ev":"ok"}`; tells the device what the host's dictation engine is doing |

**Layer count is whatever `hello` reports.** The shipped firmware builds the FLOW
layer only; a build with `FLOW_EXTRA_LAYERS` on reports more. Do not assume four.

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

## Notes

- The device never blocks on serial; if the host is absent nothing changes.
- Config lives in NVS namespace `fethr` via `Preferences`; defaults are the
  compile-time values in `config.h` (the app's "Reset to defaults" = `reset_config`).
- `?` on the console still prints the human status dump.
