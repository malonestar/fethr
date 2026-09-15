# fethr Sidecar Firmware 0.1.0 — Build, Flash, Verify

Firmware for the M5Stack **Chain DualKey** (ESP32-S3FN8) acting as a USB-HID
macro-keyboard sidecar for the fethr dictation app.

> **This file is hand-maintained.** Everything *else* in `firmware/flow_sidecar/` with a
> `.h` / `.cpp` / `.ino` extension is **generated** — see §0.

> **Version numbering.** 0.1.0 is the first public release and matches the app's
> version. Sections below still refer to "v2" and "v2.1" where they describe when a
> feature arrived during development — those were internal numbers for what shipped
> as 0.1.0, and they are kept because the reasoning is attached to them.

> **Scope.** 0.1.0 builds the **FLOW layer only** (`FLOW_EXTRA_LAYERS 0` in
> `config.h`). MEDIA, EDIT and MOUSE are complete and compile — set the flag to 1 —
> but have never run on hardware. With `LAYER_COUNT == 1` the Chain Key's long hold
> is inert: no LED flash, no panel scroll, no layer change, and the tap and
> double-tap still fire on release. Everything that walks the layer table reads
> `LAYER_COUNT`, so the flag is the whole change.

If you just want to flash the prebuilt image, you do not need this file:
see [`../../FLASH.md`](../../FLASH.md).

---

## 0. Where the source lives

Single source of truth: **`firmware/pio/`** (a PlatformIO project).
`firmware/flow_sidecar/` is a *regenerated* flat copy for people who would rather use the
Arduino IDE. Every generated file carries a `GENERATED FILE - DO NOT EDIT` banner naming
its upstream path.

```
firmware/
  pio/                          <-- EDIT HERE
    platformio.ini
    include/  config.h  actions.h  glyphs.h  sidecar.h  settings.h  serial_proto.h
    src/      main.cpp  chain.cpp  mono.cpp  leds.cpp  layers.cpp  actions.cpp
              glyphs.cpp  settings.cpp  serial_proto.cpp
    tools/    sync_ino.ps1      <-- regenerates the sketch folder
    .gitignore                  <-- .pio/ is a build dir, never commit it
  flow_sidecar/                 <-- GENERATED (plus this FIRMWARE_NOTES.md)
    flow_sidecar.ino  (= src/main.cpp)
    config.h actions.h glyphs.h sidecar.h settings.h serial_proto.h
    chain.cpp mono.cpp leds.cpp layers.cpp actions.cpp glyphs.cpp
    settings.cpp serial_proto.cpp
    FIRMWARE_NOTES.md           <-- not generated
```

Regenerate after any change under `pio/`:

```
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\pio\tools\sync_ino.ps1
```

The flat layout is deliberate: `#include "config.h"` resolves both under PlatformIO (which
puts `include/` on the search path) and under the Arduino IDE (which compiles every
`.cpp`/`.h` sitting beside the `.ino`).

| Module | Contents |
|---|---|
| `main.cpp` / `flow_sidecar.ino` | setup/loop, DualKey GPIO scanning, layer switching |
| `chain.cpp` | Chain bus: enumeration, one-transaction-per-loop scheduler, node polling, input semantics, chained-node RGB LEDs |
| `mono.cpp` | Chain Mono panel: priority state machine, rendering, text-scroll sequence |
| `leds.cpp` | The DualKey's own two WS2812s + battery/VBUS sampling |
| `layers.cpp` | The layer table (actions as data) + colour/glyph/name maps |
| `actions.cpp` | USB HID dispatch, the deferred-release queue, the USB string descriptors |
| `glyphs.cpp` | 8×8 bitmaps + the two generated panel views |
| **`settings.cpp`** | **v2.1** — the mutable `RuntimeConfig`, its defaults, NVS persistence |
| **`serial_proto.cpp`** | **v2.1** — the host settings protocol *and* the `?` console (one owner for the CDC port) |
| `config.h` | Every tunable: pins, bus, timings, deadzones, layer-table *shape*. Since v2.1 most behavioural constants here are **defaults for `g_cfg`**, tagged as such inline |
| `settings.h` | `RuntimeConfig` + the NVS blob identity/version |
| `serial_proto.h` | Protocol string constants + the module's design contract |
| `sidecar.h` | The small cross-module API/state surface |

---

## 1. Build with PlatformIO (preferred)

```
pio run                      # compile
pio run -t upload            # flash (see the download-mode dance, §3)
pio device monitor           # 115200 baud; press '?' for a status dump
pio run -t clean             # nuke .pio/build
```

Always pass `-d <project dir>` if you are not standing in `firmware/pio/`.

### Why the community platform

M5Chain needs **arduino-esp32 core 3.x**. The official `platformio/espressif32` platform is
still on 2.0.x and will not build this project, so `platformio.ini` points at the
**pioarduino** fork:

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```

### Board

There is **no Chain DualKey board json in pioarduino** — the whole `boards/` listing was
checked; only the older `m5stack-*` profiles exist. So the generic ESP32-S3 devkit profile
is used and the differences are pinned explicitly:

```
board                            = esp32-s3-devkitc-1   ; ESP32-S3, 8 MB QD, no PSRAM
board_build.flash_size           = 8MB
board_upload.flash_size          = 8MB
board_build.partitions           = default_8MB.csv
board_build.arduino.memory_type  = qio_qspi             ; ESP32-S3FN8: quad flash, no PSRAM
```

Everything DualKey-specific (pins, LEDs, Chain UART) lives in `config.h`, not the board
definition, so the generic profile costs nothing.

### USB flags — these replace the Arduino IDE's Tools menu

```
build_unflags = -DARDUINO_USB_MODE=1     ; the board json hard-codes this
build_flags   = -DARDUINO_USB_MODE=0     ; USB-OTG / TinyUSB  -> HID is possible
                -DARDUINO_USB_CDC_ON_BOOT=1  ; `Serial` = TinyUSB CDC, same cable as HID
```

`ARDUINO_USB_MODE=1` selects the hardware USB-Serial-JTAG peripheral, which **cannot do
HID** (`USBHIDKeyboard.h` compiles to nothing in that mode). The `build_unflags` line is
not optional: without it the flag is defined twice on the command line, every translation
unit warns `"ARDUINO_USB_MODE" redefined`, and the winner depends on flag ordering.

**UNVERIFIED #1 from v1 is now moot under PlatformIO** — there is no USB Mode menu to get
wrong. It still applies if you build the generated sketch in the Arduino IDE (§2).

### Windows long-path workaround

`platformio.ini` sets `core_dir = C:/pio`.

pioarduino's `esp32-arduino-libs` package contains ~200-character paths (bundled
esp-matter / connectedhomeip headers). Unpacking those under the default
`C:\Users\<you>\.platformio\...` blows past the 260-character Win32 `MAX_PATH` and the
install aborts with `WindowsLongPathError` on any machine where
`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled` is 0 — which is
the Windows default.

Two ways out; the ini takes the second because it needs no admin rights and changes no
system setting:

* **(a)** set `LongPathsEnabled = 1` (admin + reboot), then delete the `[platformio]`
  block and PlatformIO goes back to its normal per-user `core_dir`;
* **(b)** move the core dir somewhere short — `core_dir = C:/pio`. ← in use

`C:\pio` only holds downloaded toolchains/frameworks/libraries; deleting it just forces a
re-download.

### Run `pio` from PowerShell or cmd, **not** from Git Bash / MSYS

pioarduino shells out to esp-idf's `idf_tools.py`, which hard-refuses when `MSYSTEM` is
set:

```
ERROR: MSys/Mingw is not supported.
...
'xtensa-esp32s3-elf-g++' is not recognized as an internal or external command
```

The toolchain then never lands on `PATH` and every compile fails. The same `pio run`
from PowerShell works first time. This bites anything that shells out through Git Bash,
not just an interactive session.

### Warning policy

```
build_src_flags = -Wall -Wextra
```

Applies to `src/` only, so the framework and the libraries keep their own settings
while our code is held to `-Wall -Wextra` clean. The only warnings in a full build come
from `M5Chain/src/ChainBuzzer/ChainBuzzer.hpp:155` (`'note_names' defined but not used`) —
a library header we do not use; ignore it. It appears **once per translation unit that
includes `M5Chain.h`**, so v2.1 shows six of them (v2.0 showed four); that count going up
when you add a module is expected and is not a regression.

### Libraries

```
lib_deps = m5stack/M5Chain@^1.0.10
           adafruit/Adafruit NeoPixel@^1.15.2
           bblanchon/ArduinoJson@^7.4      ; v2.1
```

`ArduinoJson` is pinned to the **v7** API deliberately: v7 removed `StaticJsonDocument`
and made the elastic `JsonDocument` the only document type. The owner/name were verified
against the registry API (`https://api.registry.platformio.org/v3/packages/bblanchon/
library/ArduinoJson`, latest 7.4.3) rather than the HTML page, which is a JS app.
`Preferences` and `USB` need no `lib_deps` entry — they ship with the arduino-esp32 core
and are picked up by LDF (the dependency graph shows `Preferences @ 3.3.11`,
`USB @ 3.3.11`).

---

## 2. Build with the Arduino IDE (the generated folder)

Still supported. There are now **eight** `.cpp` files beside the `.ino` (the IDE compiles
them automatically).

1. *File ▸ Preferences ▸ Additional boards manager URLs*: add the M5Stack index.
2. *Boards Manager* → **M5Stack** board package **≥ 3.2.4**.
3. *Tools ▸ Board ▸ M5Stack* → **`M5ChainDualKey`**.
4. *Manage Libraries*: `M5Chain` **≥ 1.0.10**, `Adafruit_NeoPixel` **≥ 1.15.2**, and
   (v2.1) `ArduinoJson` **7.x — not 6.x**, since v7 removed `StaticJsonDocument` and the
   code uses the v7 `JsonDocument` API. `USB` / `USBHIDKeyboard` / `USBHIDMouse` /
   `USBHIDConsumerControl` / `Preferences` ship with the board package.
5. If a *Tools ▸ USB Mode* entry exists, set it to **USB-OTG (TinyUSB)**; if *USB CDC On
   Boot* exists, **Enabled** gives you `Serial` alongside HID. (Under PlatformIO both are
   forced by `build_flags` — see §1.)

`M5Unified` is **not** used: its `Button_Class::wasHold()` is a threshold-crossing one-shot,
not a live "is held" flag — wrong semantics for the F8/F9 true-hold requirement. The two
key GPIOs are read and debounced directly.

---

## 3. Flashing — the download-mode dance

The DualKey has **no reset button**, and once a USB-HID sketch owns the port the usual
auto-reset-into-bootloader handshake does not exist.

**Required before *every* upload:**

1. Move the 3-position side switch to the **middle** position.
2. Unplug USB-C.
3. **Hold Key1** (the button farther from the lanyard hole).
4. Plug USB-C back in while still holding Key1.
5. Release Key1. The board is now in download mode; a new COM port appears.
6. `pio run -t upload` (set `upload_port` in `platformio.ini`, or let it autodetect).

**To reboot into the flashed sketch** (not download mode): switch middle, unplug, replug
**without** holding Key1.

**Factory restore**: M5Burner → device type "Chain DualKey" → "Chain DualKey User Demo" →
Download → Burn (device must be in download mode first).

**Wiring**: all five nodes go on the **right-hand** HY2.0-4P port (RX=G5 / TX=G6 —
`CHAIN_RX_PIN` / `CHAIN_TX_PIN` in `config.h`). The triangle arrow moulded into each Chain
Bridge / Chain Return connector must point **away from the DualKey**, outward along the
chain. Node order does not matter — the firmware enumerates and dispatches by device type.

---

## 4. Build record (0.1.0, the shipped image)

`FLOW_EXTRA_LAYERS 0` — this is what `bin/fethr-sidecar-0.1.0/` contains:

```
RAM:   [==        ]  18.1% (used 59288 bytes from 327680 bytes)
Flash: [=         ]  12.8% (used 427890 bytes from 3342336 bytes)
```

| Artifact | Bytes | Flash offset |
|---|---:|---|
| `firmware.factory.bin` (all of the below, merged) | 503,456 | `0x0` |
| `bootloader.bin` | 19,968 | `0x0` |
| `partitions.bin` | 3,072 | `0x8000` |
| `boot_app0.bin` (from the core's `tools/partitions/`) | 8,192 | `0xE000` |
| `firmware.bin` | 437,920 | `0x10000` |

The offsets are the ones PlatformIO printed while merging the factory image, and
`0x10000 + 437,920 = 503,456` — the merged image is contiguous from 0.

With `FLOW_EXTRA_LAYERS 1` (also verified to compile clean): RAM unchanged at
59,288 B, flash 428,062 B — **+172 B** for the three extra layer rows.

Both builds are zero-warning from `src/` under `-Wall -Wextra`. The only warnings in
a full build are six copies of `M5Chain/src/ChainBuzzer/ChainBuzzer.hpp:155:
'note_names' defined but not used` — one per translation unit that includes
`M5Chain.h`, in a library header this firmware does not use.

The build is **not** bit-for-bit reproducible (the ELF carries a build timestamp), so
a rebuild will not match `SHA256SUMS.txt`. Those hashes check a download, not a
toolchain.

Resolved toolchain:

| Component | Version |
|---|---|
| PlatformIO Core | 6.2.0 |
| platform `espressif32` (pioarduino) | 55.3.311 |
| `framework-arduinoespressif32` (arduino-esp32 core) | **3.3.11** |
| `framework-arduinoespressif32-libs` (ESP-IDF) | 5.5.5+sha.b774170ff46 |
| `toolchain-xtensa-esp-elf` | 14.2.0+20260121 |
| `tool-esptoolpy` | 5.3.0 |
| Board profile | `esp32-s3-devkitc-1` — *Espressif ESP32-S3-DevKitC-1-N8 (8 MB QD, No PSRAM)* |
| `M5Chain` | 1.0.10 |
| `Adafruit NeoPixel` | 1.15.5 |
| `ArduinoJson` | **7.4.3** |
| `Preferences` / `USB` (bundled with the core) | 3.3.11 |

---

## 4a. Host protocol (v2.1)

**The contract lives in `../../PROTOCOL.md`** — commands, reply shapes, event names and
the config object are defined there, and `serial_proto.cpp` implements exactly that. This
section only records the things a reader of the firmware needs that the protocol document
does not say.

### One port, three conversations

The CDC port carries FLOG debug lines, the `?` console, and newline-delimited JSON at the
same time. Disambiguation is on the **first byte after a line boundary**:

* `{` → buffer until newline, then parse as a command;
* anything else → hand straight to the one-character console, unbuffered.

That second rule is what keeps `?` working with no newline in a terminal, and it means a
JSON payload containing `?` or `s` can never fire a status dump. Every human line the
firmware prints starts with `[` or a letter, never `{`.

Inbound lines are capped at `FLOW_PROTO_MAX_LINE` (512 B) and **dropped, not truncated**,
with one `{"ev":"err","msg":"line too long"}` per dropped line — half a command must never
parse.

### The one message that exceeds 512 bytes

`get_config` serialises the whole config object and lands around **700 bytes**. The
protocol's 512-byte figure governs what the *device accepts*; the outbound buffer
(`FLOW_PROTO_OUT_BYTES`, 1 KB) is sized for this reply. If a future field ever pushed it
past 1 KB the line would be replaced by `{"ev":"err","msg":"reply too large"}` rather than
emitted malformed — the host never has to resynchronise on a broken line.

### Allocation policy

Replies and events are built with `snprintf` into **one static buffer** and written
straight to `Serial`: no heap, no `String`, nothing per-loop. `deserializeJson()` does
allocate, but only while a host command is in hand, and the document is created, read and
destroyed inside one call. A command handler must never emit an event while a reply is
half-built (both use that buffer); where a command produces both — `layer`, `identify` —
the reply is flushed *first* and the side effect performed afterwards.

### NVS blob

Namespace `fethr`, key `cfg`, written **only** by the `save` command (NVS writes are slow;
a chatty host would otherwise wear the flash).

```
offset size field
0      4    magic    = 0x46534331  'F','S','C','1'
4      2    version  = FLOW_CFG_VERSION      <-- currently 1
6      2    size     = sizeof(RuntimeConfig)
8      n    RuntimeConfig, raw
```

A stored blob is accepted only when the byte length, magic, version **and** recorded
`sizeof` all match the running build. Anything else is treated as "no configuration": the
compile-time defaults win and the stale blob is left alone until the next `save`. So
adding, removing or reordering a `RuntimeConfig` field, or bumping `FLOW_MAX_LAYERS` /
`FN_COUNT`, resets cleanly instead of being reinterpreted as garbage. **Bump
`FLOW_CFG_VERSION` when you change the layout anyway** — the `size` check catches most
cases but not a same-size field swap.

### Applying a change without breaking the scheduler

`set` validates, writes `g_cfg`, then calls `settingsApplyAll()`, which only sets flags:

* `ledsConfigChanged()` drops the WS2812 repaint cache;
* `monoConfigChanged()` queues a rotation and a brightness write and voids the cached view.

The queued Mono writes are drained by `monoService()` **one Chain transaction per loop**,
ahead of the view logic, exactly like every other panel write. Nothing in the settings path
touches the bus inline and nothing calls `delay()`.

### Mono idle = the layer letter (new default)

`mono_idle: "letter"` shows the current layer's letter whenever nothing else wants the
panel. The Chain Mono has **one global brightness for the whole panel** — there is no
per-pixel intensity — so "the letter, but dimmer than a notification" is not expressible.
The two candidates were a dotted every-other-pixel stipple of the letter, or the plain
letter. **The plain letter won**: the glyphs are 5×7 strokes one pixel wide, and stippling
them produces a dashed, hard-to-read shape rather than a dimmer one. What distinguishes
idle from the post-layer-change letter is simply that it persists.
`mono_idle: "blank"` restores the v2.0 behaviour.

### `mouse_y_sign` is not `MOUSE_Y_SIGN`

`MOUSE_Y_SIGN` (`-1`, in `config.h`) is the fixed stick-space → HID-cursor-space
conversion: HID Y grows *downward*, so stick-up must emit a negative dy. It is what makes
"up" mean "up" at all. The protocol's `mouse_y_sign` (default `+1`) is a separate **user**
inversion multiplied on top of it — which is why PROTOCOL.md's default is `+1` and not
`-1`. Both are applied at the one site, `navMouse()`.

### Host engine state, and where the checkmark went (0.1.0)

`{"cmd":"state","value":...}` is the host telling the device what its dictation
engine is doing. The device cannot work any of this out for itself: it knows a key is
held, but not whether the server answered.

Implementation is deliberately split by lifetime. `transcribing` and `cleaning` are
**continuous** — `mono.cpp` and `leds.cpp` read `hostState()` on every pass, so the
spinner and the amber breathe run for exactly as long as the state is current, and
stop the moment it changes. `pasted` and `error` are **momentary**: `hostStateSet()`
fires a timed `monoOverlay()` and a `ledFlashAll()` and nothing afterwards depends on
them. `idle` and `recording` clear the continuous effects and deliberately do **not**
cancel a running overlay, because a host that reports `pasted` and then `idle` a
moment later must still leave its checkmark on screen.

The spinner obeys the same rule as every other panel view: `monoDesired()` returns a
`(MV_SPIN, frame)` pair, the frame number is derived from `millis()`, and
`monoService()` only writes when the pair changes. One transaction per
`MONO_SPIN_FRAME_MS`, still at most one per `loop()`.

Priority puts a physical hold above anything the host says: while a dictation key is
down the mic glyph wins, whatever state was last pushed. What the user's finger is
doing now beats a report about a moment ago.

**The checkmark moved.** It used to appear when a dictation key was released, which
is the wrong moment — the transcript has not arrived yet. Now the release shows
nothing *if a host is attached*, and the ✓ arrives with `state: pasted`. With no host
the release-time ✓ is restored, because it is then the only feedback the device can
give. "Attached" is `hostPresent()`: a `hello` **or** a `state` command within
`HOST_PRESENT_MS` (60 s). `state` has to count — a host says `hello` once per
connection, and a 60-second window keyed on `hello` alone would expire mid-session
and put the wrong checkmark back.

### `battery` event vs the LED tint

The event's `low` is the plain `< BATT_LOW_MV` predicate PROTOCOL.md specifies. That is
deliberately **not** `batteryLow()`, which carries `BATT_HYST_MV` of hysteresis so the
orange idle tint does not flap. The host gets the raw predicate and can smooth it itself.

### USB identity

`USB.productName("fethr sidecar")` / `USB.manufacturerName("fethr")` are called in
`actionsBegin()` **before** `USB.begin()`. Verified in
`cores/esp32/USB.{h,cpp}` (arduino-esp32 3.3.11): both are `bool setter(const char *)`
that `return !_started`, i.e. they are silently ignored after `USB.begin()`. VID stays at
the Espressif default `0x303A` — no PID/VID registration needed, and the host matches on
VID + product string.

---

## 5. What v2 adds

Everything from v1 still behaves identically. New:

> Several subsections here describe the MEDIA/EDIT/MOUSE layers, which 0.1.0 does
> not build (`FLOW_EXTRA_LAYERS 0`). They are kept because they document code that
> is still in the tree and one flag away.

### MOUSE layer (4th layer, glyph **P**, magenta) — behind `FLOW_EXTRA_LAYERS`

| Control | Action |
|---|---|
| Nav stick | Relative cursor, quadratic acceleration curve: dead below `JOY_DEADZONE`, then `MOUSE_MIN_PX`→`MOUSE_MAX_PX` (1→20 px) per report, one report per `MOUSE_REPORT_MS` (10 ms) |
| Nav click | Left click |
| Key1 | Left button **hold** (`ACT_MOUSE_HOLD`) — press on down, release on up, so drag works |
| Key2 | Right click |
| Chain Key tap | Middle click |
| Chain Key double-tap | Double left click (`ACT_MOUSE_DOUBLE`) |
| Scroll stick | Wheel / pan, as on every layer |
| Angle knob | Wheel |

Quadratic (not linear) is what keeps the first third of stick travel usable for
pixel-accurate aiming while full deflection still crosses a 4K screen in about a second.

### Chain Key double-tap

Two taps within `CHAIN_KEY_DOUBLE_MS` (350 ms) fire a per-layer second action:
FLOW → Ctrl+Z (and, behind the flag, MEDIA → previous track, EDIT → Ctrl+Shift+Z,
MOUSE → double left click).

**Latency tradeoff (deliberate, documented in `config.h`):** on any layer whose
`chain_key_double` is not `ACT_NONE`, the *single* tap is delayed by the full 350 ms
window. Every layer currently defines one, so the Chain Key's single tap costs +350 ms
everywhere. Firing the single immediately and the double afterwards was rejected: "undo"
would arrive after the paste it was meant to undo. Set a layer's `chain_key_double` to
`{ACT_NONE, 0, MOD_NONE, FN_NONE}` to get the instant single tap back on that layer.
**This delay applies only to the Chain Key** — Key1/Key2 (the F8/F9 dictation holds) are
still driven straight off raw GPIO edges with no classifier in the path.

### RGB semantics on the DualKey keys

Each key's idle colour now encodes **its function on the current layer**, not the layer:

| Function | Colour |
|---|---|
| dictation raw (F8) | blue |
| dictation clean (F9) | violet |
| re-paste (F7) | cyan |
| media | amber |
| undo | green |
| redo | green-teal |
| mouse buttons | white |
| Enter | pale yellow |
| (unmapped) | the layer colour |

Priority: flash (both keys — a layer change, or one of the 0.1.0 host-state one-shots:
green once on `pasted`, red twice on `error`) > **hold = red breathe** (millis-driven
triangle wave, `LED_HOLD_PERIOD_MS`) > **host busy = amber breathe on both keys**
(0.1.0, same wave, while the host reports transcribing or cleaning) > **tap = 80 ms
full-brightness flash of that key** > idle at `LED_BASE_PCT`.

### Battery indicator (`FLOW_BATTERY_MONITOR`, default on)

`analogRead(G10)` once every 30 s, `V = raw/4095 × 3.3 × 1.51` (kept in integer
millivolts). Below `BATT_LOW_MV` (3500 mV) the *idle* colours are pulled `BATT_TINT_PCT`
of the way toward orange; `BATT_HYST_MV` stops it flapping. Only idle is tinted, so you can
still tell which key does what.

### Chained-node LEDs (`FLOW_NODE_LEDS`, default on)

| Node | Behaviour |
|---|---|
| Chain Key | Its function colour for the layer; full brightness for `NODE_LED_TAP_MS` on a tap; **turns the colour of the layer you are about to land on** once a layer-switch hold has been counted for `NODE_LED_COUNT_AFTER_MS` — i.e. "keep holding" |
| Joysticks (×2) | Layer colour, brightness proportional to deflection |
| Angle | Hue tracks knob position, green → red across the range; only ever changes on a detent change |

**Bus budget.** Node-LED writes sit at the *bottom* of the round-robin: they are only
issued when every input task is already up to date, and each node is additionally
rate-limited to `NODE_LED_MIN_INTERVAL_MS` (100 ms → ≤10 Hz). Worst case 4 × 10 = 40
writes/s ≈ 80 ms/s of extra bus time at 115200 with a ~2 ms round trip. Joystick brightness
is **quantised** to `NODE_LED_JOY_THRESHOLD` steps rather than compared with a tolerance,
so identical quantised values compare equal and a slowly drifting stick cannot trickle out
a write per poll. Input polling can therefore never be starved.

### Chain Mono panel

Priority, highest first — and the panel is written **only** when the (kind, arg) pair
describing it changes:

1. hold glyph (mic / mic+sparkle / pointer / dot, chosen from the held action's function id)
2. timed overlay (layer letter, checkmark, host cross)
3. **host spinner** (0.1.0) — while the host reports `transcribing` or `cleaning`
4. volume bar — 8 columns filling with detent position, visible for `MONO_VOLBAR_MS`
   (600 ms) following any knob movement on an `ANGLE_VOLUME` layer
5. nav arrow — only while the nav stick is actually deflected
6. scroll animation — two bars marching, `MONO_SCROLL_FRAME_MS` per frame, while the
   scroll stick (or the knob in `ANGLE_WHEEL` mode) is emitting
7. idle — blank, or the layer letter (`mono_idle`)

**Layer-name scroll** (`FLOW_MONO_SCROLL_LAYER_NAME`, default on): on a layer change the
full name is scrolled once using the node's own
`MONO_STRING_SCROLL_MODE`, then the panel returns to pixel mode and shows the layer letter.
Implemented as a 3-transaction scripted sequence (mode → string → mode back), issued one
transaction per loop like everything else, and it yields immediately if a dictation hold
starts mid-scroll. Set the flag to 0 for letter-only. With a single layer built, a
layer change never happens and this path only runs for `identify` and `mono`.

### Serial

Boot banner `[flow-sidecar] v0.1.0 boot - 1 layers, proto 1, '?' for status`, and a
non-blocking one-character console: **`?`** (or `s`) prints firmware version, current
layer, chain readiness, the enumerated device list with type codes, role assignment, angle
detent, battery and VBUS millivolts, hold state, whether the config came from NVS or the
defaults, and (0.1.0) whether a host is attached plus the last state it pushed.

---

## 5a. What v2.1 adds

Everything from v2.0 behaves identically **unless the host changes it**. New:

| Area | Change |
|---|---|
| **Runtime config** | Every colour, percentage, timing, sign, the Mono idle/brightness/rotation, double-tap enable, `layer_hold_ms`, `boot_layer` and `led_index_key1` now live in a mutable `g_cfg` (`settings.h`). The `config.h` #defines are its **defaults**. |
| **Persistence** | `Preferences`, namespace `fethr`, one versioned blob, written only by `save`. |
| **Host protocol** | Newline-delimited JSON on the CDC port — see `PROTOCOL.md` and §4a. |
| **Mono idle** | Defaults to the layer letter instead of blank. |
| **USB identity** | Product string `fethr sidecar`, manufacturer `fethr`, VID unchanged (`0x303A`). |
| **VBUS telemetry** | `analogRead(G2)` sampled alongside VBAT every 30 s; reported by `status` and `?`. Closes v2.0 limitation #10 halfway — charge status (G9) is still unused. |
| **`boot_layer`** | The device can come up on any layer, not only FLOW. |
| **`double_tap: false`** | Removes the Chain Key's +350 ms single-tap latency outright (nothing to wait for). The per-layer `ACT_NONE` opt-out from v2.0 still works. |
| **Events** | `boot`, `layer`, `hold`, `tap`, `chain`, `battery` are emitted unsolicited so the host app can show "recording" without polling. |

What v2.1 deliberately does **not** do: change any binding. Which key fires what is still
compile-time data in `layers.cpp`; the protocol edits appearance and feel, not the keymap.

---

## 5b. What 0.1.0 adds (the public release)

| Area | Change |
|---|---|
| **Scope** | `FLOW_EXTRA_LAYERS 0`: the FLOW layer only. `LAYER_COUNT == 1`, so the Chain Key's long hold is inert — no flash, no scroll, no `g_ck_consumed`, and the tap still fires on release. Set the flag to 1 to get the other three back. |
| **Version** | `FLOW_SIDECAR_VERSION "0.1.0"`, matching the app. The USB product string is unchanged (`fethr sidecar`) and so is the protocol revision (`proto: 1`) — nothing on the wire broke. |
| **`state` command** | The host pushes its engine state; the device renders it. Spinner + amber breathe while working, ✓ + green flash on `pasted`, ✗ + two red flashes on `error`. §4a. |
| **Checkmark semantics** | ✓ now means "the text landed", not "you let go". With no host attached the old release-time ✓ is restored. §4a. |
| **New glyphs** | `G_CROSS` (a static bitmap) and `glyphSpinnerFrame()` (generated, like the volume bar and the scroll animation). |

The `state` command is additive: a host that never sends one gets exactly the v2.1
behaviour, because `hostPresent()` stays false and the release-time checkmark stays.

---

## 6. VERIFIED vs UNVERIFIED external API surface

Everything under "verified" was read from the raw upstream source — for v2, from the
**installed** copies under `.pio/libdeps/chain_dualkey/` and
`C:\pio\packages\framework-arduinoespressif32\`, which outrank the research document.

### Verified — `arduino-esp32` **3.3.11**, `libraries/USB/src/`

| Name | Signature / value | Source file |
|---|---|---|
| `USBHIDKeyboard::begin/press/release/releaseAll` | `size_t press(uint8_t)`, `size_t release(uint8_t)`, `void releaseAll()` | `USBHIDKeyboard.h` |
| `KEY_F7` / `KEY_F8` / `KEY_F9` | `0xC8` / `0xC9` / `0xCA` | `USBHIDKeyboard.h` |
| `KEY_RETURN` | `0xB0` | `USBHIDKeyboard.h` |
| `KEY_UP_ARROW` / `DOWN` / `LEFT` / `RIGHT` | `0xDA` / `0xD9` / `0xD8` / `0xD7` | `USBHIDKeyboard.h` |
| `KEY_LEFT_CTRL/SHIFT/ALT/GUI` | `0x80` / `0x81` / `0x82` / `0x83` | `USBHIDKeyboard.h` |
| `USBHIDMouse` | `typedef USBHIDRelativeMouse USBHIDMouse` | `USBHIDMouse.h` |
| `USBHIDRelativeMouse::move` | `void move(int8_t x, int8_t y, int8_t wheel = 0, int8_t pan = 0)` — used for **both** cursor (v2) and wheel/pan | `USBHIDMouse.h` |
| `USBHIDMouseBase::press/release` | `void press(uint8_t b = MOUSE_LEFT)`, `void release(uint8_t b = MOUSE_LEFT)` — the basis of `ACT_MOUSE_HOLD` (v2) | `USBHIDMouse.h` |
| `MOUSE_LEFT/RIGHT/MIDDLE/ALL` | `0x01` / `0x02` / `0x04` / `0x1F` | `USBHIDMouse.h` |
| `USBHIDConsumerControl` | `void begin()`, `size_t press(uint16_t)`, `size_t release()` — `release()` takes **no argument**, so only one usage can be held at a time | `USBHIDConsumerControl.h` |
| `CONSUMER_CONTROL_VOLUME_INCREMENT` / `_DECREMENT` / `_MUTE` | `0x00E9` / `0x00EA` / `0x00E2` | `USBHIDConsumerControl.h` |
| `CONSUMER_CONTROL_PLAY_PAUSE` / `_SCAN_NEXT` / `_SCAN_PREVIOUS` | `0x00CD` / `0x00B5` / `0x00B6` | `USBHIDConsumerControl.h` |
| `USB.begin()` | called last, after every HID device's `begin()` | vendor `Chain_DualKey_Arduino_USB_HID.md` |
| `ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT` | compile-time defines, **not** runtime calls; `0` / `1` here | core `USB.cpp`, board `boards.txt` |
| **`ESPUSB::productName`** (v2.1) | `bool productName(const char *name)` — stores into a `String` member; the body is `if (!_started) {...} return !_started;`, so it is a **no-op after `USB.begin()`** | `cores/esp32/USB.h:80`, `USB.cpp` |
| **`ESPUSB::manufacturerName`** (v2.1) | `bool manufacturerName(const char *name)` — same `_started` gate | `cores/esp32/USB.h:83`, `USB.cpp` |
| **`ESPUSB::VID`** (v2.1) | `bool VID(uint16_t)` exists but is **not called** — the Espressif default `0x303A` is what the host matches on | `cores/esp32/USB.h:51` |

### Verified — `Preferences` (bundled, arduino-esp32 3.3.11)

| Name | Signature | Source file |
|---|---|---|
| `Preferences::begin` | `bool begin(const char *name, bool readOnly = false, const char *partition_label = NULL)` — returns **false** when the namespace does not exist in read-only mode, which is the normal first-boot path | `libraries/Preferences/src/Preferences.h` |
| `putBytes` / `getBytes` | `size_t putBytes(const char *key, const void *value, size_t len)` / `size_t getBytes(const char *key, void *buf, size_t maxLen)` — `getBytes` returns 0 for a missing key, so the length check alone rejects it | same |
| `clear` / `end` | `bool clear()`, `void end()` | same |

### Verified — `ArduinoJson` **7.4.3**

| Name | Note |
|---|---|
| `JsonDocument` | v7's elastic document; `StaticJsonDocument` **no longer exists** in v7. Used for deserialisation only. |
| `deserializeJson(doc, const char *)` | Duplicates strings into the document, so `as<const char *>()` results stay valid for the life of the document — which is the whole of one command. |
| `variant.is<T>()` | `is<long>()` is false for `true`/`false`/floats, so the range checks in `applySet()` cannot be fooled by a bool. |
| `variant.is<JsonArrayConst>()` / `JsonArrayConst::size()` | Used to validate `[r,g,b]` shape before reading. |

### Verified — `m5stack/M5Chain` **1.0.10** (installed copy)

| Name | Signature / value | Source file |
|---|---|---|
| `class Chain` | inherits `ChainCommon`, `ChainJoystick`, `ChainKey`, `ChainAngle`, `ChainMono`, … | `Chain/Chain.hpp` |
| `begin` | `void begin(HardwareSerial *serial, unsigned long baud = 115200, int8_t rxPin = -1, int8_t txPin = -1, …)` | `ChainCommon.hpp` |
| `getDeviceNum` | `chain_status_t getDeviceNum(uint16_t *deviceNum, unsigned long timeout = 100)` | `ChainCommon.hpp` |
| `getDeviceList` | `bool getDeviceList(device_list_t *list, unsigned long timeout = 100)` | `ChainCommon.hpp` |
| `device_info_t` / `device_list_t` | `{uint16_t id; chain_device_type_t device_type;}` / `{uint16_t count; device_info_t *devices;}` | `ChainCommon.hpp` |
| `getEnumPleaseNum` | `uint16_t getEnumPleaseNum(void)` — hot-plug request counter, used as a topology-change hint | `ChainCommon.hpp` |
| device type enum | `CHAIN_ANGLE_TYPE_CODE 0x0002`, `CHAIN_KEY_TYPE_CODE 0x0003`, `CHAIN_JOYSTICK_TYPE_CODE 0x0004`, `CHAIN_MONO_TYPE_CODE 0x000D` | `ChainCommon.hpp` |
| `chain_status_t` | `CHAIN_OK 0x00`, `CHAIN_PARAMETER_ERROR`, `CHAIN_RETURN_PACKET_ERROR`, `CHAIN_BUSY`, `CHAIN_TIMEOUT` | `ChainCommon.hpp` |
| `chain_save_flash_t` | `CHAIN_SAVE_FLASH_DISABLE` / `_ENABLE` | `ChainCommon.hpp` |
| `setRGBValue` | `chain_status_t setRGBValue(uint16_t id, uint8_t index, uint8_t num, uint8_t *rgb, uint8_t size, uint8_t *operationStatus, unsigned long timeout = 100)` — **v2 drives this continuously** (line 459 of the installed header) | `ChainCommon.hpp` |
| `setRGBLight` | brightness 0–100 — **not used**; nodes keep their default 40 | `ChainCommon.hpp` |
| `getJoystickMappedInt16Value` | `chain_status_t getJoystickMappedInt16Value(uint8_t id, int16_t *x, int16_t *y, unsigned long timeout = 100)` | `ChainJoystick.hpp` |
| `getJoystickButtonStatus` / `getKeyButtonStatus` | `(uint8_t id, uint8_t *buttonStatus, unsigned long timeout = 100)` — raw pressed/not-pressed | `ChainJoystick.hpp` / `ChainKey.hpp` |
| `getAngle12BitAdc` | `chain_status_t getAngle12BitAdc(uint8_t id, uint16_t *adcValue, unsigned long timeout = 100)` | `ChainAngle.hpp` |
| `mono_mode_t` | `MONO_PIXEL_MODE = 0x00`, `MONO_STRING_SCROLL_MODE = 0x01` | `ChainMono.hpp:17` |
| `mono_rotation_t` / `mono_brightness_level_t` | `MONO_ROTATION_0/_90/_180/_270`; `MONO_BRIGHTNESS_OFF`, `_LEVEL_1` … `_LEVEL_7` | `ChainMono.hpp` |
| `setMonoMode` / `setMonoClear` | `(uint8_t id, …, uint8_t *operationStatus, unsigned long timeout = 100)` | `ChainMono.hpp` |
| `setMonoRotation` / `setMonoBrightness` | `(uint8_t id, <enum>, uint8_t *operationStatus, chain_save_flash_t = CHAIN_SAVE_FLASH_DISABLE, unsigned long timeout = 100)` | `ChainMono.hpp` |
| `setMonoBufferRefresh` | `chain_status_t setMonoBufferRefresh(uint8_t id, uint8_t (&buffer)[8], uint8_t *operationStatus, unsigned long timeout = 100)` — the parameter is a *reference to a real `uint8_t[8]`*, so a `const` glyph must be `memcpy`'d into a local array first | `ChainMono.hpp` |
| `setMonoPixel` (batch) | `chain_status_t setMonoPixel(uint8_t id, MonoPixelInfo *pixels, uint8_t count, uint8_t *operationStatus, unsigned long timeout = 100)` — fallback path | `ChainMono.hpp` |
| `MonoPixelInfo` | `{uint8_t x; uint8_t y; bool state;}` | `ChainMono.hpp` |
| **`setMonoStringScroll`** (v2) | `chain_status_t setMonoStringScroll(uint8_t id, const char *string, mono_scroll_dir_t dir, mono_scroll_mode_t mode, uint16_t IntervalMs, uint8_t *operationStatus, unsigned long timeout = 100)` | `ChainMono.hpp:237` |
| **`mono_scroll_dir_t`** (v2) | `MONO_SCROLL_LEFT 0`, `_RIGHT 1`, `_UP 2`, `_DOWN 3` | `ChainMono.hpp:88` |
| **`mono_scroll_mode_t`** (v2) | `MONO_SCROLL_MODE_ONCE 0`, `_LOOP 1`, `_BOUNCE 2` | `ChainMono.hpp:79` |
| Mono buffer bit order | `Display_buffer[N]` = row N, **bit7 → bit0 = X 0 → 7**, 1 = on | `protocol/Chain_Mono_Protocol.md` |

### Verified — board pins (M5Stack's Chain DualKey documentation)

Key1 = G0, Key2 = G17, WS2812 data = G21, WS2812 power-enable = G40 (must be HIGH),
battery voltage ADC = G10 and **USB/VBUS voltage ADC = G2** (both
`V = analogRead()/4095 × 3.3 × 1.51`), charge-status ADC = G9 (still unused), side switch
sense = G7/G8 (**never drive as output**), right Chain port RX=G5 / TX=G6.
Sources: M5Stack's Chain DualKey product page and its Arduino Button / LED / Switch /
Power examples, <https://docs.m5stack.com/en/chain/Chain_DualKey>. The ×1.51 divider
figure comes from the Power example and has not been checked against a meter.

### UNVERIFIED — coded defensively, check on the bench

Everything left here is a **physical-hardware** unknown. The v1 software unknowns (#1 USB
mode, #7 whether `setMonoBufferRefresh` exists) are resolved.

| # | Item | How the firmware copes |
|---|---|---|
| 1 | ~~USB Mode / USB CDC On Boot board menu~~ **RESOLVED** under PlatformIO: `build_flags` are the setting; `USBHIDKeyboard` compiled, so TinyUSB mode is active. Only still open if you build in the Arduino IDE. | — |
| 2 | Left Chain port pin direction — ESPHome says TX=G48/RX=G47, the wiki PinMap says the opposite. **They contradict each other.** | Left port is unused. `CHAIN_LEFT_RX_PIN`/`_TX_PIN` follow ESPHome and are reference-only. |
| 3 | Which NeoPixel index (0 or 1) is physically under Key1 vs Key2 | **(v2.1)** No reflash needed: `{"cmd":"set","path":"led_index_key1","value":1}` then `save`. `LED_INDEX_KEY1` in `config.h` is now just its default. |
| 4 | Joystick axis polarity | **(v2.1)** `set nav_x_sign` / `nav_y_sign` / `scroll_x_sign` / `scroll_y_sign` to `-1`, then `save`. The `config.h` signs are the defaults. |
| 4b | **(v2)** Cursor Y direction on the MOUSE layer: HID Y grows *downward*, so stick-up must emit a negative dy | `MOUSE_Y_SIGN` (`-1`) is the fixed coordinate-space conversion and stays compile-time. If the cursor is inverted *after* that, `set mouse_y_sign -1` — the two multiply. |
| 5 | Whether the HY2.0-4P 5 V rail is gated by any GPIO | Assumed always on. If **nothing** enumerates on a known-good chain, suspect this first. |
| 6 | WS2812 colour order (`NEO_GRB` comes from the vendor LED example, not a datasheet) | If red/green look swapped, change `NEO_GRB` to `NEO_RGB` in `leds.cpp`. |
| 7 | ~~Whether an older `M5Chain` lacks `setMonoBufferRefresh`~~ **RESOLVED**: present in the pinned 1.0.10. | `FLOW_MONO_USE_BUFFER_REFRESH 0` still switches `monoWriteBuffer()` to the 64-pixel batch path. |
| 8 | Exact `int16_t` full scale from the joystick mapping (docs say ±4095; part tolerance varies) | `JOY_DEADZONE`, the scroll ramp and the cursor accel curve all clamp. |
| 9 | **(v2)** How long a string actually takes to cross the panel at `MONO_NAME_SCROLL_INTERVAL_MS` per pixel | **(v2.1)** The window is now derived from the text length — `MONO_TEXT_SCROLL_PX_PER_CHAR` (6) per character plus the 8-pixel panel width, capped at `MONO_TEXT_SCROLL_MAX_MS` — because the `mono` command may hand over 32 characters where a layer name is four. Still an estimate, not a poll of `getMonoStringScrollState` (which would cost a transaction per check). |
| 10 | **(v2)** VBAT divider accuracy — the ×1.51 factor is the vendor example's, not a calibrated value | `BATT_LOW_MV` / `BATT_HYST_MV` are `config.h` constants; `?` and the `status` reply both print the measured millivolts so you can calibrate against a meter. Set `FLOW_BATTERY_MONITOR 0` to disable entirely. |
| 10b | **(v2.1)** VBUS on G2 uses the same ×1.51 factor as VBAT per the vendor Power example — plausible for a 5 V rail read through a 3.3 V ADC, but **not measured** | `usb_mv` is reported, never acted on. A wrong scale factor misreports a number; it cannot affect behaviour. |
| 11 | **(v2)** Whether the chained nodes' own click-feedback pulses (the STM32 firmware additively pulses green/blue/red on single/double/long press) visibly fight the colours we write | The firmware does not try to fight it; `set node_leds false` (v2.1) or `FLOW_NODE_LEDS 0` turns our writes off. |
| 12 | **(v2.1)** Whether `mono_idle: "letter"` leaving a glyph lit indefinitely is noticeable on battery life, and whether the panel has any burn-in behaviour | Untested. `set mono_idle "blank"` restores v2.0's blank idle; `mono_brightness` also lowers it. |
| 13 | **(v2.1)** NVS wear | `save` is the only writer and is host-driven, so the firmware itself never writes. An app that saves on every slider drag would be the problem; nothing in the firmware rate-limits it. |

---

## 7. First power-on checklist

Plug in with the side switch in the **middle** position, *not* holding Key1.

**1. Serial** (`pio device monitor`). Expect within a second of boot:

```
[flow-sidecar] v0.1.0 boot - 1 layers, proto 1, '?' for status
{"ev":"boot","fw":"0.1.0"}
[chain] id=1 type=0x0003        <- Chain Key
[chain] id=2 type=0x0004        <- Chain Joystick
[chain] id=3 type=0x0004        <- Chain Joystick
[chain] id=4 type=0x0002        <- Chain Angle
[chain] id=5 type=0x000D        <- Chain Mono
[chain] roles nav=2 scroll=3 key=1 angle=4 mono=5
{"ev":"chain","nodes":[{"id":1,"type":"key","role":"key"}, ...]}
{"ev":"layer","index":0,"name":"FLOW"}
[flow-sidecar] layer 0 (FLOW)
```

Ids depend on your physical order — only the **types** and the **roles** line matter.
`[chain] no devices` means the bus is not talking: check connector arrows, the port
(G5/G6 vs G47/G48) and the 5 V rail. Press **`?`** for the full status dump.

**2. LEDs.** Key1 dim **blue** (dictation-raw), Key2 dim **violet** (dictation-clean) —
*not* both the layer colour, that is the v2 change. The Chain Key node should go **cyan**
(re-paste), the joysticks dim blue, the Angle node somewhere on green→red depending on
where the knob sits.

**3. Mono.** "FLOW" scrolls once, then a letter **F** — and the **F stays up**, because
`mono_idle` defaults to `letter`. `{"cmd":"set","path":"mono_idle","value":"blank"}`
gets the "letter for one second, then blank" behaviour back.

**3b. Protocol** (still in `pio device monitor` — it is a plain serial terminal, so you can
type commands by hand). Each of these must produce exactly one reply line:

```
{"cmd":"hello","id":1}          -> {"ev":"hello","id":1,"fw":"0.1.0","proto":1,
                                    "layers":["FLOW"],...}
{"cmd":"status"}                -> {"ev":"status","layer":0,"vbat_mv":...,"usb_mv":...}
{"cmd":"get_config"}            -> {"ev":"config",...}          (~700 bytes, one line)
{"cmd":"identify"}              -> {"ev":"ok"}   + both key LEDs flash white x3, Mono "ID"
{"cmd":"mono","text":"HELLO"}   -> {"ev":"ok"}   + "HELLO" scrolls once
{"cmd":"state","value":"transcribing"} -> {"ev":"ok"} + Mono spinner, keys breathe amber
{"cmd":"state","value":"pasted"}       -> {"ev":"ok"} + Mono checkmark 700 ms, green flash
{"cmd":"state","value":"error"}        -> {"ev":"ok"} + Mono cross 900 ms, two red flashes
{"cmd":"state","value":"idle"}         -> {"ev":"ok"} + back to normal
{"cmd":"state","value":"busy"}         -> {"ev":"err","msg":"unknown state"}
{"cmd":"layer","index":1}       -> {"ev":"err","msg":"bad layer index"}   (one layer built)
{"cmd":"set","path":"led_idle_pct","value":60}   -> {"ev":"ok","path":"led_idle_pct"}
{"cmd":"set","path":"led_idle_pct","value":600}  -> {"ev":"err","msg":"expected 0..100"}
{"cmd":"set","path":"nope","value":1}            -> {"ev":"err","msg":"unknown path"}
{"cmd":"nope"}                                   -> {"ev":"err","msg":"unknown cmd"}
```

Note that sending any `state` (or `hello`) starts a 60-second "a host is attached"
window, during which releasing a dictation key no longer shows its own checkmark —
which is exactly what test 4 below checks.

Then the persistence round trip, which is the one that actually proves NVS:
`set` a visible colour → `{"cmd":"save"}` → unplug/replug → the colour survives →
`{"cmd":"reset_config"}` → it goes back to blue. And a `{"ev":"battery",...}` line should
appear on its own roughly every 30 s throughout.

Pressing **`?`** must still print the human status dump while all of that works — that is
the first-byte-of-the-line rule doing its job.

**4. Mappings — open Notepad and work down the list.**

| Test | Expected |
|---|---|
| Hold Key1, **no app running** | Key LED 0 **breathes red**, Mono shows the **mic** glyph, F8 goes down. Release → back to dim blue, Mono shows a **checkmark** for 400 ms, F8 goes up. |
| Hold Key1, **app running** | Same, except the release shows **no checkmark**: the panel goes to the spinner while the server works, then ✓ for 700 ms with a green LED flash as the text lands. |
| Hold Key2 | Same, **mic+sparkle** glyph and F9. The cleanup pass makes the spinner run longer. |
| Kill the ASR server and hold Key1 | Mono **✗** for 900 ms, two red flashes. |
| Tap the Chain Key once, then wait | ~350 ms later: both DualKey LEDs flash, host re-pastes (F7). The delay is intentional — §5. |
| Tap the Chain Key **twice** quickly | Ctrl+Z instead of a second paste. |
| **Hold the Chain Key ≥ 1 s** | With one layer built: **nothing at all** — no node-LED colour change, no DualKey flash, no panel scroll. On release the normal tap (or double-tap) fires. |
| Nav stick | Caret moves; Mono shows an **arrow** in that direction while held; hold → one move, ~300 ms pause, then ~20 moves/s. |
| Nav stick click | Newline (Enter). |
| Scroll stick fore/aft | Page scrolls, faster further from centre; Mono runs the **two-bar scroll animation**. |
| Scroll stick left/right | Horizontal pan. |
| Scroll stick click | Middle click. |
| Turn the Angle knob | Volume steps; Mono shows the **volume bar** for 600 ms; the Angle node's LED hue shifts green→red. |

Rebuilt with `FLOW_EXTRA_LAYERS 1`, a ≥1 s Chain Key hold cycles FLOW → MEDIA → EDIT
→ MOUSE instead: the node LED turns the next layer's colour after ~200 ms, both
DualKey LEDs flash it ×2 at 1 s, and the panel scrolls the name then shows its
letter. Those three layers have never run on hardware — that is why they are behind
the flag.

**5. Hot-plug.** Unplug a node mid-chain and reconnect it. Within a couple of seconds the
log should show `[chain] hot-plug detected` (or the 5-consecutive-failure path) followed by
a fresh device list. Key1/Key2 must keep working throughout — that is the whole point of
the one-transaction-per-loop scheduler.

---

## 8. Known limitations

1. **USB only.** No BLE. The side switch's BLE/Wi-Fi positions are not read and no radio is
   started; G7/G8 are left untouched on purpose.
2. **One Chain bus.** All nodes on the G5/G6 port. `config.h` documents exactly what a
   second `Chain` instance on G47/G48 would need; deliberately not implemented.
3. **No Chain Key ⇒ no layer switching.** Layer cycling is bound solely to a ≥1 s hold of
   the Chain Key. Without that node the firmware runs fine but stays on layer 0. Moot in
   0.1.0, which has only layer 0 anyway.
4. **Chain Key single-tap latency.** +`CHAIN_KEY_DOUBLE_MS` (350 ms) on every layer that
   defines a double action. See §5 for why, and how to opt out per layer.
5. **The Angle knob is absolute, the volume is relative.** The first ADC reading after boot
   or re-enumeration is adopted silently, so the knob position never corresponds to an
   absolute volume — only movement matters. The v2 volume bar shows *knob position*, which
   is therefore not the host's actual volume.
6. **Consumer-control rate cap.** Detent crossings are queued (max 8) and emitted one per
   20 ms, because two identical consumer reports in the same millisecond can be coalesced
   by the host. A very fast sweep trails the knob rather than dropping steps.
7. **Wheel/pan are one step per report.** `move()` takes `int8_t`; the firmware varies the
   *rate* (20–200 ms between reports), which caps out around 50 lines/s. The v2 **cursor**
   does the opposite — fixed 10 ms rate, variable step — because a cursor needs range.
8. **Chain Key / Joystick nodes flash their own LED on click** in their default active
   reporting mode (additive green/blue/red pulse decaying over ~1 s). That composites on
   top of whatever colour we set. Not fought.
9. **Node RGB brightness left at the device default (40).** Setting it costs an extra
   transaction per node; persisting it costs a ~20 ms flash write that stalls the node's
   UART.
10. **Battery telemetry is voltage only.** v2.1 adds VBUS (G2) to `status`, but charge
    status (G9) is still unused, so "low" still cannot distinguish discharging from
    charging — you can now at least see whether USB power is present.
11. **Chain node count capped at 16** per bus (`MAX_CHAIN_DEVICES`), static storage, no heap
    allocation anywhere in the firmware.
12. **`.pio/` is a build directory.** It is git-ignored and must never be committed; the
    sidecar tree should only ever carry `pio/` sources plus the generated sketch folder.
13. **(v2.1) The protocol cannot remap keys.** It edits appearance and feel — colours,
    brightness, timings, signs, idle mode, boot layer. Which key fires which HID action is
    still compile-time data in `layers.cpp`, because an Action carries a keycode, a
    modifier mask *and* a function id used for colours and glyphs; making that editable is
    a protocol v2 job, not a v1 one.
14. **(v2.1) `boot_layer` needs a `save`.** Setting it changes nothing until the next boot,
    and nothing is persisted until `save` — by design, but it is the one path where "it
    did not do anything" is the correct behaviour.
15. **(v2.1) `get_config` is ~700 bytes, above the protocol's 512-byte line size.** That
    limit governs what the device *accepts*; see §4a.
16. **(v2.1) The `boot` event may be lost.** It is printed as soon as `Serial.begin()`
    returns, which can be before the host has opened the CDC port. A host that attaches
    later gets the same information from `hello`, which is why `hello` exists.
17. **(v2.1) `identify` and `mono` need the Mono node.** With no panel on the bus they
    still reply `ok` and still flash the key LEDs (for `identify`); the scroll is simply
    skipped rather than erroring, because "no panel" is a normal configuration.
18. **(0.1.0) Host state is pushed, never polled.** The device has no timeout of its
    own: a host that says `transcribing` and then crashes leaves the spinner running
    and the keys breathing amber until it reconnects — reconnecting re-runs `hello`,
    and the app pushes `idle` on its next state change. The device deliberately does
    not guess, because a legitimate cleanup pass can take several seconds and timing
    it out would be worse than showing it.
19. **(0.1.0) The 60-second host window is a heuristic.** Quitting the app does not
    tell the device anything, so for up to a minute afterwards a key release still
    shows no checkmark. It costs one missing glyph on one dictation.
20. **(0.1.0) One layer.** `FLOW_EXTRA_LAYERS 0`. See the header of this file.
