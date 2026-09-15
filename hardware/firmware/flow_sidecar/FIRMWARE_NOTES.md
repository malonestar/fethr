# fethr Sidecar Firmware 0.2.0 — Build, Flash, Verify

Firmware for the M5Stack **Chain DualKey** (ESP32-S3FN8) acting as a USB-HID
macro-keyboard sidecar for the fethr dictation app.

> **This file is hand-maintained.** Everything *else* in `firmware/flow_sidecar/` with a
> `.h` / `.cpp` / `.ino` extension is **generated** — see §0.

> **Version numbering — read this before the section numbers confuse you.**
> 0.1.0 was the first public release and matched the app's version.
> `FLOW_SIDECAR_VERSION` is now **`0.2.0`** (§5c), cut for the runtime layout work.
>
> Sections below still refer to "v2" and "v2.1" where they describe when a feature
> arrived during development — those were internal numbers for what shipped as 0.1.0,
> and they are kept because the reasoning is attached to them. Confusingly, **the
> labels "0.2.0" in §9 and "0.3.0" in §9a are *those* internal numbers too** — they
> mark the companion link and the key legend, both of which shipped inside the 0.1.0
> image. The firmware version `0.2.0` is a different thing from the "0.2.0" tag on
> §9. Where a section means the release, it says "the 0.2.0 release".

> **Scope.** The tree — and `bin/fethr-sidecar-0.2.0/` — sets
> **`FLOW_EXTRA_LAYERS 4`**: a factory-default device seeds all four layers (FLOW,
> MEDIA, EDIT, MOUSE).
>
> **That flag changed meaning in the 0.2.0 release.** It used to be a boolean
> (`0` = FLOW only, `1` = build all four); it is now a *count* (`1` or `4`) because
> the layer table is runtime data and the flag only decides how many defaults get
> seeded. `layers.cpp` `static_assert`s the range, so the old `0` fails the build
> rather than producing an empty table — but a stale instruction saying "set it to 1
> to get all four layers" now does the opposite of what it says.
>
> With one layer the Chain Key's long hold is inert: no LED flash, no panel scroll, no
> layer change, and the tap and double-tap still fire on release. Everything that walks
> the layer table reads `layerCount()`, which is now a runtime value, so the count can
> also differ because the device was seeded on an older build.

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
              companion.h  keynames.h
    src/      main.cpp  chain.cpp  mono.cpp  leds.cpp  layers.cpp  actions.cpp
              glyphs.cpp  settings.cpp  serial_proto.cpp  companion.cpp
              keynames.cpp
    tools/    sync_ino.ps1      <-- regenerates the sketch folder
              stage_bin.ps1     <-- copies a built image into hardware/bin/<ver>/
    .gitignore                  <-- .pio/ is a build dir, never commit it
  companion-atoms3r/            <-- the companion display, a SEPARATE project
    platformio.ini                  (AtomS3R + M5Unified; §9)
    include/  companion_proto.h
    src/      main.cpp
  flow_sidecar/                 <-- GENERATED (plus this FIRMWARE_NOTES.md)
    flow_sidecar.ino  (= src/main.cpp)
    config.h actions.h glyphs.h sidecar.h settings.h serial_proto.h companion.h
    chain.cpp mono.cpp leds.cpp layers.cpp actions.cpp glyphs.cpp
    settings.cpp serial_proto.cpp companion.cpp
    FIRMWARE_NOTES.md           <-- not generated
```

`sync_ino.ps1` globs `include/*.h` and `src/*.cpp`, so a new module is picked up
with no edit to the script. `companion-atoms3r/` is **not** synced into the sketch
folder: it is a different board with a different library set and would not compile
beside the DualKey sources.

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
| `layers.cpp` | The **default** layer table (actions as data), the runtime accessors `layerAt()`/`layerCount()`, and the colour/glyph/name/class maps |
| **`keynames.cpp`** | **0.2.0** — the proto-2 key-name vocabulary: name ⇄ HID code in three separate number spaces, action-type and modifier names |
| `actions.cpp` | USB HID dispatch, the deferred-release queue, the USB string descriptors |
| `glyphs.cpp` | 8×8 bitmaps + the two generated panel views |
| **`settings.cpp`** | **v2.1** — the mutable `RuntimeConfig`, its defaults, NVS persistence |
| **`serial_proto.cpp`** | **v2.1** — the host settings protocol *and* the `?` console (one owner for the CDC port) |
| **`companion.cpp`** | **0.2.0** — the optional companion-display UART link: port selection, TX/RX probe, events out, commands in (§9) |
| `config.h` | Every tunable: pins, bus, timings, deadzones, layer-table *shape*. Since v2.1 most behavioural constants here are **defaults for `g_cfg`**, tagged as such inline |
| `settings.h` | `RuntimeConfig` (which since 0.2.0 carries the layer table) + the NVS blob identity/version |
| `serial_proto.h` | Protocol string constants + the module's design contract |
| `keynames.h` | The key-name vocabulary's contract + the VERIFIED constant list it was built from |
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
includes `M5Chain.h`**, so v2.1 showed six and the companion link made it seven; that
count going up when you add a module is expected and is not a regression.

0.2.0 added `keynames.cpp` and the count stayed at **seven**, because that module
includes `config.h` and the USB headers but deliberately **not** `sidecar.h` — it needs
no Chain API, and not pulling `M5Chain.h` in keeps both the warning count and the
compile time where they were.

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

## 4. Build record

### 4.0 Current tree = the shipped image (`FLOW_EXTRA_LAYERS 4`, `FLOW_COMPANION 1`)

The tree and `bin/fethr-sidecar-0.2.0/` are the same build; there is no separate
"shipped" configuration to record.

```
RAM:   [==        ]  19.0% (used 62184 bytes from 327680 bytes)
Flash: [=         ]  13.2% (used 439666 bytes from 3342336 bytes)
```

| Artifact | Bytes | Flash offset |
|---|---:|---|
| `firmware.factory.bin` (all of the below, merged) | 515,808 | `0x0` |
| `bootloader.bin` | 19,968 | `0x0` |
| `partitions.bin` | 3,072 | `0x8000` |
| `boot_app0.bin` (from the core's `tools/partitions/`) | 8,192 | `0xE000` |
| `firmware.bin` | 450,272 | `0x10000` |

The offsets are the ones PlatformIO printed while merging the factory image, and
`0x10000 + 450,272 = 515,808` — the merged image is contiguous from 0.
`tools/stage_bin.ps1 -Version 0.2.0` is what copies these into `hardware/bin/` and
writes `SHA256SUMS.txt`.

Cost of the runtime-layout work against the 0.1.0 image (59,912 B / 433,786 B):
**+2,272 B RAM, +5,880 B flash**. Where the RAM went, which is worth knowing because
it is not where you would guess:

| | Bytes |
|---|---:|
| `FLOW_PROTO_OUT_BYTES` 1024 → 3072 (the `get_layers` reply) | +2,048 |
| `RuntimeConfig::layers[4]` — 4 × 50 B | ~200 |
| `layer_count` + the three new bools, the `actionLabel()` key scratch, padding | ~24 |

So the layer table itself is cheap; the buffer is the cost. `get_layers` serialises
six actions per layer with a name, a mods array and an fn class each, which lands near
2.2 KB worst case — see §4a's note on the one message that exceeds 512 bytes, which is
now two.

Still zero warnings from `src/` under `-Wall -Wextra`, and still **seven** copies of
`M5Chain/src/ChainBuzzer/ChainBuzzer.hpp:155: 'note_names' defined but not used` — the
new `keynames.cpp` does not include `M5Chain.h`, so it did not add an eighth.

The companion firmware's own build record is in §9.

### 4.1 Earlier build records, for comparison

| Configuration | RAM | Flash |
|---|---:|---:|
| `FLOW_EXTRA_LAYERS 0`, no companion (the original 0.1.0 image) | 59,288 | 427,890 |
| `FLOW_EXTRA_LAYERS 1`, no companion | 59,288 | 428,062 |
| `FLOW_EXTRA_LAYERS 1` + companion link (§9) | 59,776 | 432,494 |
| `FLOW_EXTRA_LAYERS 1` + companion link + key legend (§9a) — the 0.1.0 image | 59,912 | 433,786 |
| **`FLOW_EXTRA_LAYERS 4` + runtime layout (the 0.2.0 release)** | **62,184** | **439,666** |

(The first four rows use the flag's OLD boolean meaning; `1` there is "all four
layers". From the 0.2.0 release the flag is a count — see the Scope note at the top.)

So the three extra layers cost **+172 B** flash and no RAM, the companion link
**+488 B RAM / +4,432 B flash**, the legend **+136 B / +1,292 B**, and the runtime
layout **+2,272 B / +5,880 B**.

All of them are zero-warning from `src/` under `-Wall -Wextra`. (The first two predate
`companion.cpp`, hence six `note_names` warnings rather than seven — see §4.0.)

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

### The two messages that exceed 512 bytes

`get_config` serialises the whole config object and lands around **700 bytes**, and
(0.2.0) `get_layers` serialises the whole keymap and lands near **2.2 KB** worst case —
four layers × six actions, each carrying a key name, a `mods` array and an `fn` class.
The protocol's 512-byte figure governs what the *device accepts*; the outbound buffer
(`FLOW_PROTO_OUT_BYTES`, now 3 KB) is sized for the larger of these. If a future field
ever pushed one past it the line is replaced by `{"ev":"err","msg":"reply too large"}`
rather than emitted malformed — the host never has to resynchronise on a broken line.

That is also the argument for `mods` always being present as `[]`: the shape is worth
more to a host than the ~70 bytes omitting the empty ones would save, and the buffer
was sized for the full form anyway.

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
4      2    version  = FLOW_CFG_VERSION      <-- currently 2
6      2    size     = sizeof(RuntimeConfig)
8      n    RuntimeConfig, raw
```

A stored blob is accepted only when the byte length, magic, version **and** recorded
`sizeof` all match the running build. Anything else is treated as "no configuration": the
compile-time defaults win and the stale blob is left alone until the next `save`. So
adding, removing or reordering a `RuntimeConfig` field, or bumping `FLOW_MAX_LAYERS` /
`FN_COUNT` / `FLOW_LAYER_NAME_MAX`, resets cleanly instead of being reinterpreted as
garbage. **Bump `FLOW_CFG_VERSION` when you change the layout anyway** — the `size`
check catches most cases but not a same-size field swap.

**Version 2 (the 0.2.0 release)** put the whole layer table in the blob:
`LayerRuntime layers[FLOW_MAX_LAYERS]` plus `layer_count`, and the three new booleans
`swap_keys` / `nav_swap_xy` / `scroll_swap_xy`. A v1 blob is therefore rejected and a
device upgraded from 0.1.0 comes up on factory defaults exactly once. That is the
intended outcome rather than a migration gap: a v1 blob has no keymap to carry forward,
and the app holds the user's colour preferences anyway.

`LayerRuntime` is why `LayerConfig`'s `const char *name` could not simply move into
`RuntimeConfig` — the blob is `memcpy`'d, so it must stay trivially copyable and hold
no pointers. The runtime struct carries `char name[FLOW_LAYER_NAME_MAX + 1]` instead,
and the colour is deliberately *not* duplicated: it stays in `layer_rgb[]`, which
`set layer_rgb.N` already edited and `set_layer_meta`'s `rgb` now writes too.

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

> Several subsections here describe the MEDIA/EDIT/MOUSE layers, which the original
> 0.1.0 image did not build. They are **seeded by default** now
> (`FLOW_EXTRA_LAYERS 4`), and what they describe is the *default* binding of each
> control — since the 0.2.0 release any of it can be rebound at run time.

### MOUSE layer (4th layer, glyph **P**, magenta) — a default, not a build option

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

Boot banner `[flow-sidecar] v0.2.0 boot - 4 layers, proto 2, '?' for status`, and a
non-blocking one-character console: **`?`** (or `s`) prints firmware version, current
layer, chain readiness, the enumerated device list with type codes, role assignment, angle
detent, battery and VBUS millivolts, hold state, whether the config came from NVS or the
defaults, (0.1.0) whether a host is attached plus the last state it pushed, and (0.2.0)
a `keys` line with the two GPIOs actually being scanned and the three orientation flags —
which is the quickest way to tell a `swap_keys` that did not take from one that did.

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
| **Version** | `FLOW_SIDECAR_VERSION "0.1.0"`, matching the app. The USB product string is unchanged (`fethr sidecar`) and so was the protocol revision (`proto: 1`) — nothing on the wire broke. (The 0.2.0 release moved both the version and `proto` to 2; see §5c.) |
| **`state` command** | The host pushes its engine state; the device renders it. Spinner + amber breathe while working, ✓ + green flash on `pasted`, ✗ + two red flashes on `error`. §4a. |
| **Checkmark semantics** | ✓ now means "the text landed", not "you let go". With no host attached the old release-time ✓ is restored. §4a. |
| **New glyphs** | `G_CROSS` (a static bitmap) and `glyphSpinnerFrame()` (generated, like the volume bar and the scroll animation). |

The `state` command is additive: a host that never sends one gets exactly the v2.1
behaviour, because `hostPresent()` stays false and the release-time checkmark stays.

---

## 5c. What the 0.2.0 release adds — the runtime layout

v2.1's summary ended "what v2.1 deliberately does **not** do: change any binding.
Which key fires what is still compile-time data in `layers.cpp`." **That is the line
this release crosses.**

| Area | Change |
|---|---|
| **Runtime layer table** | `RuntimeConfig::layers[]` + `layer_count`. `layers.cpp`'s table is now `DEFAULT_LAYERS` — the seed, not the source of truth. Every reader goes through `layerAt()` / `layerCount()`. |
| **`get_layers`** | The whole table: per layer the name, colour, the three stick/knob modes and all six action slots. |
| **`set_action`** | Rebind one slot on one layer, live. |
| **`set_layer_meta`** | Rename / recolour a layer and change `nav_mode` / `scroll_mode` / `angle_mode`. Any subset of fields. |
| **Key names** | `keynames.cpp` — the protocol's vocabulary ⇄ HID codes, in three separate number spaces scoped by action type. |
| **`swap_keys`** | The old `KEYS_SWAPPED` compile flag as a live setting. The flag is **gone**. |
| **`nav_swap_xy` / `scroll_swap_xy`** | Swap a stick's axes before the sign multiply. |
| **`layout_changed` event** | Emitted after any layout edit, so a second client re-reads. |
| **`proto: 2`**, `FLOW_CFG_VERSION 2` | See §4a. |

### Why one accessor and not a global `LAYERS[]`

`layerAt()` clamps an out-of-range index to layer 0 rather than asserting. That is
what lets ~40 call sites pass `g_layer` with no bounds check of their own, and it is
also why `g_cfg` being zero-initialised before `settingsBegin()` runs is harmless —
index 0 is always inside the array, it is merely blank. `layerCount()` returning 0 at
that moment is likewise safe: `cycleLayer()` guards the modulo.

### Why a rebind cannot strand a key

`scanLocalKeys()` copies the `Action` into `g_key[].hold_action` on the **press** edge
and releases from that copy, and `chain.cpp` captures the Chain Key's pending single
tap the same way. That was originally there so a *layer change* mid-hold could not
leave F8 down on the host; it turns out to be exactly the property `set_action` needs,
so the gesture in flight finishes with the binding it started with and no extra
machinery was required. It is worth not "tidying" that copy away.

### Why `swap_keys` is more than a variable

`keysApplySwap()` (`main.cpp`) re-assigns the two GPIOs, and is called from
`settingsApplyAll()` after **every** `set`. Two properties make that safe:

* it returns immediately when the assignment already matches, so an unrelated colour
  change during a dictation hold does nothing to the keys;
* when it does swap it calls `releaseAllHolds()` first, then seeds `raw`/`stable` from
  the pins' *current* levels rather than from `false` — otherwise swapping while a
  button happened to be down would synthesise a press edge on the other one.

The LED follows the finger: `ledForKey()` flips `led_index_key1` when `swap_keys` is
set, which is precisely what the old `#if KEYS_SWAPPED` block did to `LED_INDEX_KEY1`
at compile time. The two settings compose, so a board with both wrong is still fixable.

### Why `mods` is not its own action type on the wire

The firmware has `ACT_CHORD_TAP`; the protocol does not. `key_tap` with a non-empty
`mods` **is** the chord, and `keyNameToAction()` upgrades the type on the way in while
`actionTypeName()` reports `ACT_CHORD_TAP` as `key_tap` on the way out. Round-tripping
`get_layers` → `set_action` is therefore lossless without the host ever learning a
seventh type name.

### Two spellings of `fn`

`fn_rgb`'s keys and the `hold` event's `fn` stay upper-case (`DICT_RAW`); the proto-2
class in `get_layers`/`set_action` is lower-case (`dict_raw`) and adds `custom` for
FN_NONE. Both are the same `FnId`; `fnName()`/`fnByName()` serve the first,
`fnClassName()`/`fnClassByName()` the second, and the latter is case-insensitive so a
host that sends the upper-case spelling is understood. Renaming either would break a
shipped host, which is why both exist rather than one being migrated.

`custom` on a bound action makes `actionLabel()` return the **key name**, so the
companion's legend shows `K1: f13` rather than `K1: -`. That is the one place
`actionLabel()` returns a built string rather than a literal; it lands in a file-static
scratch buffer, which is safe only because every caller formats the result into its own
storage before the next call. Do not start caching an `actionLabel()` pointer.

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
| **(0.2.0) `KEY_RETURN` / `KEY_ESC` / `KEY_BACKSPACE` / `KEY_TAB` / `KEY_SPACE`** | `0xB0` / `0xB1` / `0xB2` / `0xB3` / **`0x20`** — `space` is the plain ASCII byte, which is why `keynames.cpp` looks the named table up *before* the ASCII path | `USBHIDKeyboard.h` |
| **(0.2.0) `KEY_INSERT` / `KEY_HOME` / `KEY_PAGE_UP` / `KEY_DELETE` / `KEY_END` / `KEY_PAGE_DOWN`** | `0xD1` / `0xD2` / `0xD3` / `0xD4` / `0xD5` / `0xD6`. Note the header spells them `PAGE_UP`/`PAGE_DOWN` while the protocol vocabulary is `pageup`/`pagedown` | `USBHIDKeyboard.h` |
| **(0.2.0) `KEY_CAPS_LOCK` / `KEY_PRINT_SCREEN`** | `0xC1` / `0xCE` | `USBHIDKeyboard.h` |
| **(0.2.0) F-key block** | `KEY_F1..KEY_F12` = `0xC2..0xCD` but `KEY_F13..KEY_F24` = **`0xF0..0xFB`** — two runs, not one, so neither `f<n>` direction can be a single subtraction | `USBHIDKeyboard.h` |
| **(0.2.0) `CONSUMER_CONTROL_STOP` / `_BRIGHTNESS_INCREMENT` / `_BRIGHTNESS_DECREMENT`** | `0x00B7` / `0x006F` / `0x0070` | `USBHIDConsumerControl.h` |
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

### Verified — `M5Unified` **0.2.22** / `M5GFX` (the companion firmware, §9)

Read from the installed copies under
`firmware/companion-atoms3r/.pio/libdeps/companion_atoms3r/`.

| Name | Signature / value | Source file |
|---|---|---|
| `Button_Class::wasClicked` | `bool wasClicked(void) const` — true for one pass after a brief press **and release** | `M5Unified/src/utility/Button_Class.hpp` |
| `Button_Class::wasHold` | `bool wasHold(void) const` — a one-shot fired when the press crosses the threshold, so a hold never also reports a click | same |
| `Button_Class::setHoldThresh` | `void setHoldThresh(std::uint32_t msec)` — default `_msecHold` is **500**; the companion sets 800 | same |
| `M5Unified::getBoard` | `board_t getBoard(void)`. There is **no** `getBoardName()` — the companion maps the enum to a string itself | `M5Unified.hpp` |
| `board_t` AtomS3R family | `board_M5AtomS3R = 18`, `board_M5AtomS3RExt = 143`, `board_M5AtomS3RCam = 144` | `M5GFX/src/lgfx/boards.hpp` |
| AtomS3R display autodetect | Probed at run time on **G21 MOSI / G15 SCK / G42 DC / G14 CS / G48 RST**, 3-wire SPI. **Both panel ICs are handled**: `Panel_ST7735S` (ids `0x7683`/`0x897C`) and `Panel_GC9107` (id `0x079100`), 128×128 either way, with a slow re-probe for GC9107 batches that only answer at 100 kHz | `M5GFX/src/M5GFX.cpp` (the `EFUSE_PKG_VERSION_ESP32S3PICO` branch) |
| Display pins vs the link | None of the LCD pins above is G5 or G6, so the companion's UART cannot collide with its own screen | same |
| `M5Canvas` | `M5Canvas c(&M5.Display); c.setColorDepth(16); c.createSprite(w,h); c.pushSprite(x,y)` — the whole frame is composed off-screen and blitted once | `M5GFX/src/M5GFX.h` → `LGFX_Sprite` |
| `board_build.arduino.memory_type` | Selects which prebuilt static libs are linked, **not** the bootloader: the Atom build pulls `esp32s3/qio_opi/libesp_psram.a`, the DualKey `esp32s3/qio_qspi/`, and both get the same `bootloader_qio_80m` from `esp32s3/bin/`. Confirmed in each project's `firmware.map` | `framework-arduinoespressif32-libs` |

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
| 4 | Joystick axis polarity | **(v2.1)** `set nav_x_sign` / `nav_y_sign` / `scroll_x_sign` / `scroll_y_sign` to `-1`, then `save`. The `config.h` signs are the defaults. **(0.2.0)** `set nav_swap_xy` / `scroll_swap_xy` to `true` covers a stick mounted at 90°, which the signs alone could not reach. |
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
| 14 | **(0.2.0) The companion link in its entirety.** No AtomS3R has been attached to a DualKey. Both firmwares compile clean; nothing has been observed working | The link is inert when nothing answers: a beacon every 1.5 s into a port that may not be connected, and no effect on anything else. `FLOW_COMPANION 0` removes it. |
| 14b | **(0.2.0)** Which pin pair is actually free. The bus probe settled on `G47/G48` in the one hardware observation on record, which should leave `G5/G6` for the companion — but that was one cable on one board | The companion asks `chainBusPins()` at boot instead of assuming, so it follows whatever the probe decided. |
| 14c | **(0.2.0)** Whether a second `HardwareSerial` on the DualKey's spare port works at all — e.g. whether that port's pins are shared with anything, or its 5 V rail gated | Untested. The DualKey's own `?` dump prints the link state and the pins in use, which is the first thing to look at. |
| 14d | **(0.2.0)** The hand-off if the Chain bus re-probes onto the companion's pins. `chainService()` rotates through all four candidates when the bus goes quiet, and two of them are the pair the companion holds | `companionTick()` compares the chain's current pins against its own every pass and moves to the other port if they collide. Coded, never exercised. |
| 14e | **(0.2.0)** Screen legibility — font sizes, the drawn glyphs and 60 % brightness were chosen on a 128×128 grid on paper, not on a 0.85″ panel | All of it is in `uiDraw()` and its helpers in the companion's `main.cpp`; nothing persists, so a change is a reflash and nothing else. |
| 14f | **(0.2.0)** Which AtomS3R panel IC is on the unit in hand. M5Stack changed it from GC9107 to ST7735 in May 2026 | M5GFX autodetects and supports both (§6, verified). Nothing in this firmware names a panel. |
| 14g | **(0.3.0)** Whether the 16-pixel font actually fits a 16-character legend row on the real panel. If it does not, all three rows drop to the 8-pixel console font **together** — so one character over budget on one layer changes the look of every row on it | Measured with `textWidth()` at draw time, not assumed, so it cannot overflow the panel; it can only be smaller than intended. The fix if it reads badly is to shorten the labels in `layerLegend()` (one file, one place), not to change the font. |
| 14h | **(0.3.0)** Whether ~30 fps of full 128×128×2 sprite blits over SPI is comfortable. The mic pulse is the busiest frame | Arithmetic says yes; unmeasured. `UI_FRAME_MS` in `companion_proto.h` is the single knob, and raising it degrades smoothness rather than correctness — every animation derives its phase from the bucket. |
| 14i | **(0.3.0)** Whether the animation *durations* (700 ms mic pulse, 250 ms layer wipe, 150 ms tap flash, 800 ms knob bar) read as deliberate rather than twitchy | All four are `#define`s at the top of `companion_proto.h`. Nothing else depends on their values; `UI_REC_FRAMES` and `UI_WIPE_FRAMES` are derived so a duration cannot desynchronise from its own animation. |
| 14j | **(0.3.0)** Whether a tap's 150 ms row flash is even perceptible next to the existing LED flash and Mono overlay, or whether three simultaneous feedbacks are one too many | Cosmetic either way. Removing it is deleting the `tap_row` branch in `drawLegend()`; the `tap` event itself is also what the icons need. |
| 15 | **(0.2.0 release) The whole runtime layout, on hardware.** `get_layers` / `set_action` / `set_layer_meta` compile and the shapes match PROTOCOL.md, but nothing has been exercised against a real device — no reply has been seen on a wire | Additive: a host that never sends the three new commands gets exactly 0.1.0's behaviour. The first thing to check is that `get_layers` arrives as ONE line and is not cut short by `{"ev":"err","msg":"reply too large"}`, which is what a buffer that turned out too small looks like. |
| 15a | **(0.2.0 release)** Whether `swap_keys` really is glitch-free when flipped **while a key is held**. The release-then-reseed path is coded and reasoned about; it has not been done with a finger on the button | Worst case is a dropped keystroke, not a stuck one: `releaseAllHolds()` runs before the pins move. If it misbehaves, do the swap from the app's settings page rather than mid-dictation. |
| 15b | **(0.2.0 release)** Whether the ASCII key path produces the right character on a **non-US** host layout. `a`..`z`, digits and the eleven punctuation marks are handed to the core's `_asciimap`, which is `KeyboardLayout_en_US` unless `Keyboard.begin()` is given another | Only affects keys the *user* binds; nothing in the defaults uses punctuation except `z`/`y`, which are the same on most Latin layouts. The fix, if it matters, is a layout setting passed to `Keyboard.begin()` in `actionsBegin()`. |
| 15c | **(0.2.0 release)** Whether an 8-character layer name is legible on the Mono panel's name scroll and in the companion's layer label. The limit is the protocol's, chosen for the blob, not measured on glass | `MONO_TEXT_SCROLL_PX_PER_CHAR` already derives the scroll window from the length, so a long name scrolls longer rather than being cut. The companion label is `textWidth()`-measured at draw time. |
| 15d | **(0.2.0 release)** A renamed layer keeps its seeded Mono glyph (`FLOW` → `F` even after it is renamed `MACROS`) | Deliberate, not an oversight: there is no letter-glyph generator, only the hand-drawn `G_F`/`G_M`/`G_E`/`G_P`. Adding one is a `glyphs.cpp` change plus a `glyph` field on `set_layer_meta`; PROTOCOL.md records the omission. |

---

## 7. First power-on checklist

Plug in with the side switch in the **middle** position, *not* holding Key1.

**1. Serial** (`pio device monitor`). Expect within a second of boot:

```
[flow-sidecar] v0.2.0 boot - 4 layers, proto 2, '?' for status
{"ev":"boot","fw":"0.2.0"}
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
{"cmd":"hello","id":1}          -> {"ev":"hello","id":1,"fw":"0.2.0","proto":2,
                                    "layers":["FLOW","MEDIA","EDIT","MOUSE"],...}
{"cmd":"status"}                -> {"ev":"status","layer":0,"vbat_mv":...,"usb_mv":...}
{"cmd":"get_config"}            -> {"ev":"config",...}          (~700 bytes, one line)
{"cmd":"identify"}              -> {"ev":"ok"}   + both key LEDs flash white x3, Mono "ID"
{"cmd":"mono","text":"HELLO"}   -> {"ev":"ok"}   + "HELLO" scrolls once
{"cmd":"state","value":"transcribing"} -> {"ev":"ok"} + Mono spinner, keys breathe amber
{"cmd":"state","value":"pasted"}       -> {"ev":"ok"} + Mono checkmark 700 ms, green flash
{"cmd":"state","value":"error"}        -> {"ev":"ok"} + Mono cross 900 ms, two red flashes
{"cmd":"state","value":"idle"}         -> {"ev":"ok"} + back to normal
{"cmd":"state","value":"busy"}         -> {"ev":"err","msg":"unknown state"}
{"cmd":"layer","index":1}       -> {"ev":"ok"} + a `layer` event  (MEDIA; `err` on a
                                    device seeded with one layer, where only 0 exists)
{"cmd":"set","path":"led_idle_pct","value":60}   -> {"ev":"ok","path":"led_idle_pct"}
{"cmd":"set","path":"led_idle_pct","value":600}  -> {"ev":"err","msg":"expected 0..100"}
{"cmd":"set","path":"nope","value":1}            -> {"ev":"err","msg":"unknown path"}
{"cmd":"nope"}                                   -> {"ev":"err","msg":"unknown cmd"}
```

**3c. The runtime layout** (0.2.0). Same terminal. `get_layers` is the big one — it
must arrive as **one** line of roughly 2 KB, not as
`{"ev":"err","msg":"reply too large"}`:

```
{"cmd":"get_layers","id":9}
  -> {"ev":"layers","id":9,"layers":[
       {"index":0,"name":"FLOW","rgb":[0,90,255],"nav_mode":"arrows",
        "scroll_mode":"wheel_pan","angle_mode":"volume","actions":{
          "key1":{"type":"key_hold","key":"f8","mods":[],"fn":"dict_raw"},
          "key2":{"type":"key_hold","key":"f9","mods":[],"fn":"dict_clean"},
          "chain_key":{"type":"key_tap","key":"f7","mods":[],"fn":"repaste"},
          "chain_key_double":{"type":"key_tap","key":"z","mods":["ctrl"],"fn":"undo"},
          "nav_click":{"type":"key_tap","key":"enter","mods":[],"fn":"enter"},
          "scroll_click":{"type":"mouse_btn","key":"middle","mods":[],"fn":"mouse_m"}}},
       ... three more ...]}

{"cmd":"set_action","layer":0,"slot":"key2",
 "action":{"type":"key_tap","key":"c","mods":["ctrl"],"fn":"custom"}}
  -> {"ev":"ok"}  + {"ev":"layout_changed"}   (Key2's LED leaves violet; the
                                               companion legend reads "K2: c")
{"cmd":"set_action","layer":0,"slot":"key2","action":{"type":"key_tap","key":"nope"}}
  -> {"ev":"err","msg":"unknown key"}          (and nothing changed)
{"cmd":"set_action","layer":0,"slot":"nope","action":{"type":"none"}}
  -> {"ev":"err","msg":"unknown slot"}
{"cmd":"set_layer_meta","layer":1,"name":"MACROS","rgb":[255,0,0]}
  -> {"ev":"ok"}  + {"ev":"layout_changed"}    (hello now reports "MACROS"; the Mono
                                                glyph stays M - see UNVERIFIED 15d)
{"cmd":"set_layer_meta","layer":1,"name":"WAY_TOO_LONG"}
  -> {"ev":"err","msg":"name must be 1..8 chars"}
{"cmd":"set","path":"swap_keys","value":true}
  -> {"ev":"ok","path":"swap_keys"}            (the two buttons AND their LEDs trade
                                                places immediately, no reboot)
{"cmd":"set","path":"nav_swap_xy","value":true}
  -> {"ev":"ok","path":"nav_swap_xy"}          (nav stick left/right now moves up/down)
```

Then `{"cmd":"save"}`, unplug/replug, and `get_layers` again: the rebinding must
survive. That is the NVS round trip for the layer table specifically, and it is the
half of persistence that version 2 of the blob exists for.

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
| **Hold the Chain Key ≥ 1 s** | Cycles FLOW → MEDIA → EDIT → MOUSE: the node LED turns the next layer's colour after ~200 ms, both DualKey LEDs flash it ×2 at 1 s, and the panel scrolls the name then shows its letter. On a device seeded with one layer (`FLOW_EXTRA_LAYERS 1`) the hold does **nothing at all** and the normal tap (or double-tap) fires on release. |
| Nav stick | Caret moves; Mono shows an **arrow** in that direction while held; hold → one move, ~300 ms pause, then ~20 moves/s. |
| Nav stick click | Newline (Enter). |
| Scroll stick fore/aft | Page scrolls, faster further from centre; Mono runs the **two-bar scroll animation**. |
| Scroll stick left/right | Horizontal pan. |
| Scroll stick click | Middle click. |
| Turn the Angle knob | Volume steps; Mono shows the **volume bar** for 600 ms; the Angle node's LED hue shifts green→red. |

MEDIA/EDIT/MOUSE have had far less time on hardware than FLOW. They are seeded by
default now rather than hidden behind a flag, and since the 0.2.0 release anything
about them — name, colour, every binding, what the stick and knob do — can be changed
from the app without a reflash.

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
20. ~~**(0.1.0) One layer.**~~ **No longer true of the shipped image**: all four are
    seeded (`FLOW_EXTRA_LAYERS 4`). MEDIA/EDIT/MOUSE remain the least-exercised part of
    this firmware.
20a. **(0.2.0 release) The layer COUNT is still fixed.** `set_action` and
    `set_layer_meta` can change everything about a layer, but not how many there are:
    `FLOW_MAX_LAYERS` sizes the NVS blob and `FLOW_EXTRA_LAYERS` decides the seed. Adding
    a fifth layer is a blob-version bump, not a host command, and the wire format has no
    add/remove verb by design.
21. **(0.2.0) The companion link has no reply channel.** Commands from the companion
    get no `ok`/`err`; a rejected one is silently dropped. It is a display, not a
    settings client, and its only real feedback is the `layer` event a successful
    change produces anyway.
22. **(0.2.0) The companion is not a Chain node** and never will be. It cannot appear
    in `nodes`, it holds no bus id, and `chainNodeCount()` does not count it. The
    `status` reply gives it its own `companion` object for exactly this reason.
23. **(0.2.0) One companion.** The link is a point-to-point UART on one port pair;
    there is no addressing and no second screen.
24. **(0.3.0) The legend describes three keys and two analogue controls, not the
    Chain Key's double-tap.** Four slots is what fits a 128×128 panel; the double-tap
    action (`chain_key_double`) has no row, and neither does the scroll stick's
    direction behaviour. The `tap` event still reports `"chain2"` when a double fires,
    so it flashes the Chain Key's row.
25. **(0.3.0) Tap icons are matched on the label text**, not on an identifier — the
    identifier cannot distinguish the MEDIA bindings (§9a). Renaming a label in
    `actionLabel()` silently loses its icon; the row still flashes. Deliberate: the
    alternative is a second enumeration to keep in step with the first.
26. **(0.3.0) The knob bar shows knob POSITION, not host volume.** Same caveat as
    limitation 5 for the Mono panel's volume bar, and for the same reason: the first
    ADC reading after boot is adopted silently, so only movement is meaningful.
27. **(0.3.0) The legend is the current layer's only.** There is no per-layer cache on
    the companion, because a `layer` event always carries the new legend; a cache would
    buy nothing but a way to be out of date.

---

## 9. The companion display (0.2.0)

An optional AtomS3R on the DualKey's spare Chain port. **The user-facing document is
[`../../COMPANION.md`](../../COMPANION.md) and the wire contract is in
[`../../PROTOCOL.md`](../../PROTOCOL.md) § *Companion link*** — this section is only
what a reader of the firmware needs that those do not say.

### Two probes stacked

The companion is not a Chain node; it is a second controller on a plain UART. Two
things are unknown at boot and both are resolved at run time rather than configured:

1. **Which port.** `chainBegin()`'s auto-probe decides which pin pair carries the
   Chain bus. `companionBegin()` runs **after** it, calls the new `chainBusPins()`,
   and takes the complement. Ordering matters: called before `chainBegin()` it would
   read a pin pair the chain had not committed to yet.
2. **Which way round.** Both ends are controllers on a straight-through cable, so
   there is no host/device convention for TX and RX. The **DualKey** alternates its
   two candidate orders on every unanswered beacon (`COMPANION_PROBE_MS`, 1.5 s); the
   Atom's side is fixed at RX=G5/TX=G6 through the ToChain Base. Either end's hello
   resolves it, so in practice it locks within about three seconds of both being
   powered.

`CHAIN_UART` is `Serial2` and `COMPANION_UART` is `Serial1` — different peripherals,
so the two links never contend for hardware, only potentially for pins.

### The collision case

`chainService()` rotates through **all four** pin candidates when the bus goes quiet,
and two of them are the pair the companion is holding. `companionTick()` therefore
re-reads `chainBusPins()` every pass and, if the chain has landed on its pins, moves to
the other port and restarts probing. Two integer compares per loop; the alternative is
two UARTs driving one wire.

### Scheduler rules

Identical to `serial_proto.cpp`, and for the same reasons: never blocks, no `delay()`,
**at most one inbound line parsed per `loop()`** (`readLines()` returns the moment it
has handled one), and every outbound line built with `snprintf` into one static buffer.
Only `deserializeJson()` allocates, and only while a companion command is in hand.

Events are emitted from the single place each fact changes, so neither listener can
drift from the other:

| Event | Emitted from |
|---|---|
| `layer` | `enterLayer()` — the one place a layer changes, whatever asked for it |
| `state` | `hostStateSet()`, **after** its unchanged-state early return, so it fires on a change |
| `hold` | `scanLocalKeys()`, beside the existing `protoEventHold()` |
| `chain` | `chainEnumerate()`, beside `protoEventChain()` |
| `battery` | `companionTick()`'s own 30 s timer |
| `tap`, `knob` | 0.3.0 — see §9a for both, and for why `tap` carries a label rather than an `FnId` |

All of them are no-ops while the link is down, so no call site has to check.

### `FLOW_COMPANION 0`

Compiles the module out: the `#else` arm of `companion.cpp` defines every entry point
as an empty stub, so no call site changes and no state exists. Same pattern
`serial_proto.cpp` uses for `FLOW_SERIAL_PROTO`. The `status` reply keeps its
`companion` field and reports `linked: false` — a host should not have to tell "no
companion" from "no field".

### Building the Atom side

A **separate** PlatformIO project, `firmware/companion-atoms3r/`. It is not synced into
the Arduino sketch folder: different board, different libraries, and it would not
compile beside the DualKey sources.

```
pio run -d hardware/firmware/companion-atoms3r
pio run -d hardware/firmware/companion-atoms3r -t upload
```

Three choices in its `platformio.ini` are worth knowing:

* **`board = esp32-s3-devkitc-1`, not `m5stack-atoms3`.** pioarduino does ship
  `boards/m5stack-atoms3.json`, but that is the AtomS3 — ESP32-S3FN8, **no PSRAM** —
  and it also defines `-DARDUINO_M5Stack_ATOMS3`, a claim about the hardware that is
  false here. M5Stack's own PlatformIO snippet for the AtomS3R uses the generic devkit
  profile with the memory type corrected, which is also what the DualKey project does.
  M5Unified identifies the board at run time regardless (§6).
* **`board_build.arduino.memory_type = qio_opi`** — quad flash + **octal** PSRAM, the
  physical wiring of the ESP32-S3-PICO-1-N8R8. Paired with `-DBOARD_HAS_PSRAM`. This
  selects which prebuilt libs are linked, verified in the map file (§6). M5Stack's
  snippet also passes `-mfix-esp32-psram-cache-issue`; that is an ESP32-classic silicon
  workaround and is deliberately not carried over.
* **`ARDUINO_USB_MODE=1` is left as the board json sets it** — the opposite of the
  DualKey, which has to `build_unflags` it. The companion sends no HID, so there is
  nothing TinyUSB buys, and the hardware USB-Serial-JTAG peripheral survives a crashed
  application better because nothing in the sketch drives it.

`core_dir = C:/pio` is shared with the DualKey project on purpose: one toolchain
download, one set of resolved versions across both firmwares.

### Companion build record

With the key legend and the animations (§9a):

```
RAM:   [=         ]   8.8% (used 28916 bytes from 327680 bytes)
Flash: [==        ]  17.5% (used 584791 bytes from 3342336 bytes)
```

against `28,652 B / 576,423 B` for the first companion build — **+264 B RAM,
+8,368 B flash**, most of it the GFX font and the `sinf` path the mic pulse pulls in.

Zero warnings, `src/` under `-Wall -Wextra`, and — unlike the DualKey build — zero
warnings from the libraries either. The 32 KB display sprite is allocated at run time
and is not in the RAM figure. Images: `bin/fethr-companion-atoms3r-0.1.0/`
(`firmware.factory.bin` at `0x0`, emitted by pioarduino itself; no esptool merge step
is needed).

`COMPANION_FW_VERSION` stays `"0.1.0"` on purpose — same rule as
`FLOW_SIDECAR_VERSION`: the number moves when an image is cut for a release, not when a
feature lands, and `bin/` has only ever carried one companion release. The two images
in `bin/` were rebuilt together and must be flashed together; an older DualKey sends no
`legend` and the screen would sit on "no legend yet".

| Component | Version |
|---|---|
| platform `espressif32` (pioarduino) | 55.3.311 |
| arduino-esp32 core | 3.3.11 |
| `M5Unified` (pulls `M5GFX`) | 0.2.22 |
| `ArduinoJson` | 7.4.3 |

---

## 9a. The key legend and the companion's animations (0.3.0)

**The user-facing document is [`../../COMPANION.md`](../../COMPANION.md) and the wire
contract is [`../../PROTOCOL.md`](../../PROTOCOL.md) § *Companion link*.** This section
is what a reader of the firmware needs that those do not say.

### One source of truth, on the DualKey side

The companion's resting screen is a per-layer key legend, and every string on it is
built in **`layerLegend()` in `layers.cpp`** — beside `fnColor()` and `fnGlyph()`, from
the same rows the keys actually fire from — and shipped as a `legend` array on `hello`
and on every `layer` event.

Since the 0.2.0 release those rows are the **runtime** table, reached through
`layerAt()`, and `set_action`/`set_layer_meta` re-emit the `layer` event. So the
anti-drift argument below is no longer only about a careless edit to a source file: a
key rebound from the app updates the companion's screen in the same breath, with no
code path that could forget to.

The companion holds **no** table of bindings. That is the entire point: a screen that
kept its own copy would be one edit away from confidently lying about what a key does,
which is worse than having no screen. Change `layers.cpp` and the legend changes,
because there is nothing else for the panel to draw.

`actionLabel()` is keyed on the **action**, not on its `FnId`. It has to be: every
control on the MEDIA layer carries `FN_MEDIA`, so the id cannot tell "play/pause" from
"mute". Consumer usages and mouse buttons are matched on `code`; everything else falls
back to a per-`FnId` label, which is exactly right for the keyboard actions because
those are the ones the `FnId` already names.

Three character budgets are in play and they are not the same number:

* `COMPANION_LEGEND_MAX` is **20** — what the companion's 6-pixel console font fits
  across 128 px with a margin, and what `"stick cursor / wheel"` needs.
* the three key rows are *built* to fit **16**, so the companion can use the 16-pixel
  font when they are short. `"K1 hold: dictate"` and `"K3: middle click"` are the two
  that sit exactly on it. The gesture is spelled out only for a hold, because a tap is
  the default and saying so costs four characters the budget does not have.
* `COMPANION_OUT_BYTES` went **256 → 384**. A four-layer `hello` with the legend lands
  near 175 bytes, so this is headroom rather than a fit. A truncated line is dropped
  rather than sent malformed (`evEnd()`), so overflowing would cost the beacon
  entirely — worth the 128 bytes.

### `tap` and `knob`

Both are emitted from the same places the host's own events are, so the two listeners
cannot be told different stories:

| Event | Emitted from |
|---|---|
| `tap` | `scanLocalKeys()` (Key1/Key2), `chainKeyFire()` (chain, chain2), `pollNavStick()` / `pollScrollStick()` (the stick clicks) — each beside its `protoEventTap()` |
| `knob` | `angleUpdate()`, on both the active and the `ANGLE_OFF` path |

`tap` carries `actionLabel()`'s prose in `fn`, **not** the `FN_*` identifier, for the
same reason `actionLabel()` exists — see PROTOCOL.md.

`knob` is rate-limited to 10/s by **coalescing, not dropping**: `companionEventKnob()`
only records the newest detent and `companionTick()` sends it when
`COMPANION_KNOB_MIN_GAP_MS` has elapsed. The interesting value is where the knob
*stopped*, which is by definition the last one, so dropping would be exactly wrong. A
pending detent is discarded when the link drops — it would otherwise arrive ahead of
the `hello` that says which layer it belongs to.

The `ANGLE_OFF` path emits too. A knob that turns and shows nothing reads as a broken
screen rather than as an unbound control; it also means the bar behaves the same on
every layer, which is one less thing for a user to learn.

### Redraw discipline on the Atom, with animations in it

The v0.1.0 rule was "draw only when the `Frame` struct changes", and animation could
have broken it — a sine evaluated against `millis()` changes on every pass. It does not,
because **every animation is quantised into `UI_FRAME_MS` (33 ms) buckets inside
`uiDesired()`**, before the value reaches the struct. The bucket index is the struct's
`arg`, so the comparison still decides redraws, ~30 fps falls out as a ceiling with no
timer anywhere, and a resting companion still costs zero blits.

The consequence worth remembering when editing: **`uiDraw()` must stay a pure function
of `Frame`.** It never calls `millis()`. Reading the clock inside a draw would make the
equality test unsound and the redraw rate unbounded, which is the bug this structure
exists to prevent.

The recording pulse is anchored to `millis()` itself rather than to the key press. A
press anchor would restart the sine on every `hold` event — including the one the
*other* key emits — so the glyph would visibly jump mid-dictation.

### Where the checkmark went, from the other end of the link

The DualKey suppresses its own release-time ✓ when a host is attached (§4a). The
companion needs the same rule and cannot see `hostPresent()`, so it derives it: a
`state` event has only ever been forwarded because something pushed one, so **one
`state` event is proof that an app is attached**. With that, a key release starts a
wait — spinner and "working…" — that ends on `pasted`, `error` or an explicit `idle`.
Without it, the release gets its checkmark immediately, which is the plain-USB-keyboard
case.

The wait has **no timeout**, deliberately, and for the same reason limitation #18
gives: a cleanup pass can legitimately take several seconds, and guessing wrong is
worse than showing the work. It ends on a report or on the link dropping, never on a
clock.

### Fonts

The three legend rows use **one** font, chosen by the widest of them: the 16-pixel
`Font2` if all three fit the panel, otherwise the 8-pixel `Font0` for all three. Picking
per row would be easy and would look like a bug — three lines of the same kind of
information should not be three sizes. The footer is always `Font0`; it is longer, and
it is reference rather than the headline.

Nothing on the wire is non-ASCII. The bundled GFX fonts have no glyphs beyond ASCII, so
the labels use `/` and `:` where a typographic middle dot would have read better.
