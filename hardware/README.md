# The fethr sidecar

A desk device for dictation: hold a mechanical key to talk, tap another to paste
again, with two joysticks, a knob and an 8×8 LED panel for feedback. It is built
from [M5Stack Chain](https://docs.m5stack.com/en/chain/Chain_DualKey) modules and
runs fethr's own firmware. To the PC it is an ordinary USB keyboard and mouse — no
driver, and no app needed for the keys themselves to work.

**0.1.0 ships one layer: FLOW, the dictation layer.** The firmware also contains
MEDIA, EDIT and MOUSE layers; they are written and they compile, but they have not
been exercised on hardware, so they sit behind a build flag
(`FLOW_EXTRA_LAYERS` in `firmware/pio/include/config.h`). With a single layer the
Chain Key's long hold has nothing to cycle to and does nothing at all; its tap and
double-tap work as normal.

* **Flashing** (prebuilt images, no toolchain): [FLASH.md](FLASH.md)
* **Serial protocol** between the device and the app: [PROTOCOL.md](PROTOCOL.md)
* **Companion display** — the optional AtomS3R second screen: [COMPANION.md](COMPANION.md)
* **Build notes, pinned versions, what is and isn't verified against real
  hardware**: [firmware/flow_sidecar/FIRMWARE_NOTES.md](firmware/flow_sidecar/FIRMWARE_NOTES.md)

## Modules

| Part | Role | Notes |
|---|---|---|
| [Chain DualKey](https://docs.m5stack.com/en/chain/Chain_DualKey) (ESP32-S3) | Brain, USB HID, 2 keys, 2 RGB LEDs | The only programmable unit; two Chain ports |
| [Chain Key](https://docs.m5stack.com/en/chain/Chain_Key) | Third key: re-paste / undo | STM32 node, 2 RGB |
| [Chain Joystick](https://docs.m5stack.com/en/chain/Chain_Joystick) ×2 | Nav stick (arrows + Enter), scroll stick (wheel/pan + middle click) | Hall-effect, ±4095 mapped, click button |
| [Chain Angle](https://docs.m5stack.com/en/chain/Chain_Angle) | Knob → volume | 12-bit pot, 280°, **no button** |
| [Chain Mono](https://docs.m5stack.com/en/chain/Chain_Mono) | 8×8 LED panel: recording indicator and status | Output only |

Only the DualKey is required. Every other node is optional — the firmware
enumerates what is present and the features belonging to a missing node drop out.

There is also an optional **companion display** — an [AtomS3R](https://docs.m5stack.com/en/core/AtomS3R)
on an [Atomic ToChain Base](https://docs.m5stack.com/en/accessory/Atomic_ToChain_Base),
plugged into the DualKey's *other* Chain port. It is not a Chain node: it is a second
controller on its own UART. On a colour LCD it shows a **legend of what every key does
on the current layer** — built by the DualKey from its own layer table, so it cannot go
stale — plus the dictation state, and its button cycles layers. See
[COMPANION.md](COMPANION.md).

## Wiring

All Chain nodes go on **one** of the DualKey's two HY2.0-4P ports. The firmware
auto-probes both pairs (`G5/G6` and `G47/G48`, in both TX/RX orders) at boot and
settles on whichever answers, so either port works and you do not have to know which
is which. The triangle moulded into each Chain Bridge connector must point **away**
from the DualKey, outward along the chain.

Node order does not matter: the firmware enumerates by device type, and of the two
joysticks the one nearest the DualKey becomes the nav stick. A comfortable physical
order is DualKey → Key → Joystick → Joystick → Angle → Mono, with the panel at the
far end facing you.

The **other** port is left free for the optional companion display, which takes
whichever pair the Chain bus did not claim. Driving G7/G8 — the side-switch sense
lines — breaks the board's ability to power off cleanly, so the firmware never
touches them.

## What the FLOW layer does

| Input | Action | What the app does |
|---|---|---|
| Key 1 **hold** | F8 down / up | raw dictation, pasted at the cursor |
| Key 2 **hold** | F9 down / up | dictation plus the LLM cleanup pass |
| Chain Key **tap** | F7 | paste the last transcript again |
| Chain Key **double-tap** | Ctrl+Z | undo that paste |
| Nav stick | arrow keys with auto-repeat; click = Enter | |
| Scroll stick | wheel ∝ deflection, X pans; click = middle click | |
| Angle knob | volume up/down, 24 detents | |

**Key LEDs**: each key idles in its *function's* colour (blue = raw dictation,
violet = cleaned), breathes red while held, and flashes briefly on a tap. They
breathe amber while the app is transcribing, flash green when the text lands and
red twice on an error.

**Mono panel**: a microphone while you hold a key, a spinner while the server is
working, ✓ when the transcript is pasted, ✗ on failure, a volume bar when you turn
the knob, arrows and a scroll animation for the sticks, and the layer letter when
idle.

The ✓ is worth a note. Before 0.1.0 it appeared when you *released* the key, which
is not the moment the text arrives. Now the app tells the device what it is doing
(`state` in PROTOCOL.md) and the ✓ means the paste actually happened. With no app
attached — the device used as a plain keyboard — the old release-time ✓ comes back,
since it is then the only feedback there is.

The bindings are a data table in `firmware/pio/src/layers.cpp`; the shape and every
tunable are in `firmware/pio/include/config.h`. Colours, brightness and timings are
editable at run time from the app's Sidecar page and persist in the device's NVS —
no reflash.

## Why custom firmware

M5Stack's factory firmware configures itself over a Wi-Fi AP and offers about a
dozen fixed *paired* presets — Copy/Paste, Undo/Redo, Volume, Arrows, media keys.
There is no way to send an arbitrary keycode, no true press-on-down /
release-on-up hold, no mouse, and no cross-node control of the Mono panel. F7/F8/F9
and hold semantics are therefore unreachable with it. The full analysis, including
the Chain bus protocol and the pin map, is in
[research/chain_firmware_research.md](research/chain_firmware_research.md).

The stock firmware is still useful for one thing: before flashing, its config page
draws the chain topology, which is a two-minute check that every node enumerates.

## Layout

```
hardware/
  README.md                     this file
  FLASH.md                      flashing, for someone with no toolchain
  PROTOCOL.md                   the device <-> app serial protocol
  COMPANION.md                  the optional AtomS3R companion display
  flash.ps1                     one-command flasher (Windows)
  bin/fethr-sidecar-0.1.0/           prebuilt DualKey images + SHA256SUMS
  bin/fethr-companion-atoms3r-0.1.0/ prebuilt AtomS3R images + SHA256SUMS
  research/
    chain_firmware_research.md  stock verdict, bus protocol, pins, API, gotchas
  firmware/
    pio/                        SOURCE OF TRUTH (PlatformIO project)
      platformio.ini
      include/  config.h actions.h glyphs.h sidecar.h settings.h serial_proto.h
                companion.h
      src/      main.cpp chain.cpp mono.cpp leds.cpp layers.cpp actions.cpp
                glyphs.cpp settings.cpp serial_proto.cpp companion.cpp
      tools/    sync_ino.ps1    regenerates the Arduino sketch folder
    companion-atoms3r/          the companion display (separate PlatformIO project)
      platformio.ini
      include/  companion_proto.h
      src/      main.cpp
    flow_sidecar/               GENERATED flat copy for the Arduino IDE
      flow_sidecar.ino + the same .h/.cpp files
      FIRMWARE_NOTES.md         hand-maintained: build, flash, API verification,
                                first-power-on checklist
```

The two firmwares are separate projects on purpose: different boards, different
libraries (M5Chain vs M5Unified), different USB modes. They share only the wire
format in PROTOCOL.md.

Edit under `firmware/pio/` and regenerate the Arduino copy:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\pio\tools\sync_ino.ps1
```

## Known unknowns

None of this has run on assembled hardware yet. Everything below is coded
defensively and is a one-line fix, most of them without a reflash — the app can
`set` them and `save` them to the device.

* Which NeoPixel index sits under Key 1 (`led_index_key1`).
* Joystick axis polarity (`nav_x_sign`, `nav_y_sign`, `scroll_x_sign`,
  `scroll_y_sign`) and cursor Y direction (`mouse_y_sign`).
* WS2812 colour order — `NEO_GRB` comes from M5Stack's LED example, not a
  datasheet. If red and green look swapped, change it in `leds.cpp`.
* The battery voltage divider: the ×1.51 factor is from M5Stack's power example.
  The serial `?` dump prints the measured millivolts so it can be calibrated
  against a meter.
* Whether the chained nodes' own click-feedback pulses visibly fight the colours
  the firmware writes. `node_leds: false` turns our writes off if so.
* Each Chain port's TX/RX assignment — M5Stack's own documents contradict each
  other. Both the bus probe and the companion link try both orders, so neither
  has to be right.
* The companion display, in its entirety — nothing about it has been on hardware.
  [COMPANION.md](COMPANION.md) has the list.

FIRMWARE_NOTES.md §6 has the full list with what the firmware does about each.

## Later

* BLE mode — the DualKey supports it (battery powered, pairs to a phone as a
  keyboard); the firmware is USB-only.
* The other three layers, once FLOW has been through real use.
* Remapping keys from the app. Today the protocol edits appearance and feel;
  which key fires what is compile-time data.
* The second Chain port as a second *bus*, for more nodes. It currently carries the
  companion display, which is a plain UART link rather than a Chain bus.
