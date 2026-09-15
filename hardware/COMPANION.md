# The companion display

An optional second screen for the sidecar: an **M5Stack AtomS3R** on an **Atomic
ToChain Base**, cabled to the DualKey's spare Chain port. On a 128×128 LCD it shows
**what every key on the current layer does**, in that layer's colour, and animates the
four things you are actually waiting on — a dictation in progress, a layer change, a
tap, the knob. Its button cycles layers.

It is entirely optional. Nothing about the sidecar changes if you never plug one in;
the firmware sends a greeting into the spare port every 1.5 seconds and gets on with
its job when nothing answers.

> **None of this has been run on hardware.** The firmware compiles clean on both
> sides and the protocol is symmetric with the one the app already uses, but no
> AtomS3R has been attached to a DualKey yet. See "What is unverified" at the end.

> **Both images must be current.** `bin/fethr-sidecar-0.1.0/` and
> `bin/fethr-companion-atoms3r-0.1.0/` were rebuilt together and belong together. An
> older DualKey image sends no key legend, and the companion would sit on "no legend
> yet" forever; an older companion image ignores the legend it is sent. Neither
> version string moved — the project moves those when a release is cut, not when a
> feature lands — so check the file dates, not the banner.

## What you need

| Part | Note |
|---|---|
| [AtomS3R](https://docs.m5stack.com/en/core/AtomS3R) | ESP32-S3-PICO-1-N8R8, 0.85″ 128×128 IPS, one button under the screen |
| [Atomic ToChain Base](https://docs.m5stack.com/en/accessory/Atomic_ToChain_Base) | Clips under the Atom and brings its bottom IO out as a HY2.0-4P socket |
| One HY2.0-4P cable | A Chain Bridge/Grove cable is the right part — straight-through, 4-pin, 2.0 mm |

## Wiring

Plug the cable between the **ToChain Base** and **whichever DualKey Chain port the
rest of the chain is not using**. That is the whole instruction.

Two things are being worked out for you:

* **Which port.** The DualKey has two HY2.0-4P ports and its firmware auto-probes
  both at boot to find the Chain bus. Whichever pair it settles on — `{G47,G48}` or
  `{G5,G6}` — the companion link takes the other. So it does not matter which port
  you put the chain on and which you put the Atom on, only that they are different.
* **Which way round.** The ToChain Base maps the Atom's bottom IO to the socket as
  `GND / 5V / IO1=G5 / IO2=G6`, and the Atom end is wired fixed: **RX = G5, TX = G6**.
  Because both ends are microcontrollers on a straight-through cable, there is no
  "host side" convention that says whose TX meets whose RX. So the **DualKey probes
  both orders**, alternating on every unanswered greeting, and locks onto whichever
  one gets a reply. Either orientation works; it just costs up to three seconds at
  startup.

The Atom is powered over its own USB-C, not from the cable. The 5 V pin on the
HY2.0 connector is not used by this link.

## What the screen shows

The resting screen is a **key legend for the layer you are on**:

```
  ┌────────────────────┐
  │ 5n      FETHR  4.01V│   layer name in the layer's colour;
  ├────────────────────┤   nodes and battery in the corners
  │ K1 hold: dictate   │
  │ K2 hold: clean     │   one row per key
  │ K3: repaste        │
  │                    │
  │ stick arrows / vol │   the stick and the knob
  │ ▄▄▄▄▄▄▄▄░░░░░░░░░░ │   knob position, while it is turning
  └────────────────────┘
```

Switch to MEDIA and the same three rows read `K1: play/pause`, `K2: next track`,
`K3: mute`. **K3 is the Chain Key**, the one on the chained node, not a third button on
the DualKey.

The important part is where those strings come from: **the DualKey builds them from its
own layer table** and sends them with every greeting and every layer change. The
companion holds no copy of the keymap at all — it renders four strings it was handed. So
the legend cannot go stale: change a binding in `layers.cpp` and the screen changes with
it, because there is nothing else for it to show.

**Top band** — the active layer's name, in that layer's own colour, with the Chain node
count on the left and the DualKey's battery voltage on the right (updated every 30 s;
blank until the first reading arrives). The colour is the live one, so if you have
recoloured a layer from the app's Sidecar page, the companion follows.

### The four animations

| When | What the screen does |
|---|---|
| **You hold a dictation key** | The legend gets out of the way: a drawn microphone fills the screen, pulsing in size and brightness once every 700 ms, with **talk!** underneath in red. |
| **You let go** | If the app has ever spoken to the device, a spinner and **working…** until the transcript lands — then a green check and **landed** for 700 ms, or a red cross and **try again** for 900 ms. With no app attached (the device used as a plain USB keyboard) the check appears on release instead, because that is the only feedback there is. Same rule the Chain Mono panel follows. |
| **You tap a key** | That key's legend row inverts into the layer colour for 150 ms, with a small drawn icon on the right for the bindings that have an obvious one — play/pause, next, previous, mute, undo, redo, Enter. A stick click flashes the footer, which is the line that mentions the stick. |
| **You change layer** | A block of the new layer's colour sweeps across the screen and off the other side in about 250 ms, revealing the new legend. |
| **You turn the knob** | A thin bar along the bottom edge shows where the knob is, for 800 ms after it stops. |

A physical hold outranks whatever the app last said — the same priority the Chain Mono
panel uses, so the two screens never disagree.

**Before the link comes up** — a dim "waiting for sidecar…". You will see this for a
second or two after either device is plugged in, and it is also what you get if the
cable is in the wrong port or the DualKey is not powered.

Everything above is driven off `millis()`; nothing sleeps or blocks. The screen is only
redrawn when the frame it would draw actually differs from the one on the panel, and
every animation is quantised to ~30 fps before that comparison, so an animation costs
one redraw per frame and a resting companion costs none.

## The button

The programmable button is the whole face of the Atom — press the screen.

| Gesture | Action |
|---|---|
| **Click** | Next layer on the DualKey |
| **Hold ≥ 0.8 s** | Identify: the DualKey flashes both key LEDs white three times and scrolls "ID" on its Mono panel |

A hold never also counts as a click. Debouncing is M5Unified's.

A layer change from the button is the same event as one from the Chain Key or from the
app, so every screen and the app all learn about it together.

**If the sidecar builds only one layer** there is nothing to cycle to and a click does
nothing; hold still identifies. The image in `bin/fethr-sidecar-0.1.0/` builds all four
(`FLOW_EXTRA_LAYERS 1`), so on that build a click walks FETHR → MEDIA → EDIT → MOUSE.

## Flashing the Atom

Prebuilt images are in [`bin/fethr-companion-atoms3r-0.1.0/`](bin/fethr-companion-atoms3r-0.1.0/),
and the procedure is the same as the DualKey's ([FLASH.md](FLASH.md)) apart from how you
get into download mode. Write `firmware.factory.bin` at offset `0x0`.

**Download mode on the AtomS3R** is easier than the DualKey's, because the Atom has a
real reset button — it is the small side button, not the screen:

> Press and hold the reset button for about **2 seconds**, until the internal **green
> LED lights up**, then release. The green LED turns off, and the board is in download
> mode.
>
> — [M5Stack AtomS3R documentation](https://docs.m5stack.com/en/core/AtomS3R), *Download Mode*

Then flash exactly as in FLASH.md §3 — the browser flasher at
<https://espressif.github.io/esptool-js/>, or:

```
uv tool run esptool --chip esp32s3 --port COM8 --baud 921600 write_flash 0x0 firmware.factory.bin
```

Building it yourself:

```
pio run -d hardware/firmware/companion-atoms3r
pio run -d hardware/firmware/companion-atoms3r -t upload
pio device monitor
```

Run `pio` from PowerShell or cmd on Windows, not Git Bash — the same esp-idf
restriction as the DualKey project.

## Checking it works

Plug the Atom into USB and open a serial terminal at 115200. Press **`?`**:

```
--- fethr companion 0.1.0 ---
board   : AtomS3R
link    : linked on rx=G5 tx=G6
last rx : 812ms ago
sidecar : fw 0.1.0
layer   : 0/4 FETHR  rgb=0,90,255
legend 0: K1 hold: dictate
legend 1: K2 hold: clean
legend 2: K3: repaste
legend 3: stick arrows / vol
state   : idle
host    : seen
chain   : 5 node(s)
battery : 4012mV
```

The four `legend` lines are the quickest check that the link is carrying the new
fields: if they are blank while `link` says `linked`, the DualKey is running an image
from before the legend existed.

From the DualKey's own serial port, `?` now ends with a matching line:

```
compan.: linked, rx=G47 tx=G48, last rx 1204ms ago
```

and the app's `status` reply carries `"companion":{"linked":true}`.

If it says `probing` on both ends for more than a few seconds, the two devices are on
the same port (they must be on different ones), the cable is not seated, or the
DualKey is not powered.

## The link, in one paragraph

Newline-delimited JSON at 115200, one object per line — the same format the app uses
over USB, documented in [PROTOCOL.md](PROTOCOL.md) (§ *Companion link*). The companion
sends `{"cmd":"hello"}` every three seconds until the sidecar answers, then
`{"cmd":"ping"}` every three seconds as a keepalive; ten seconds of silence in either
direction drops the link and restarts the greetings. The sidecar pushes `layer`,
`state`, `hold`, `tap`, `knob`, `chain` and `battery` events as they happen — `hello`
and `layer` carry the four legend strings — and accepts `layer_next`, `layer_prev`,
`layer` and `identify`. Neither side ever blocks: each parses at most one line per pass
of its main loop, and knob detents are coalesced to ten a second so a fast sweep cannot
crowd out a tap.

## What is unverified

Everything below is untested on assembled hardware.

* **The whole thing.** No AtomS3R has been connected to a DualKey. Both firmwares
  compile clean; nothing has been observed working.
* **The spare port's pin pair.** The DualKey's own bus probe settled on `G47/G48`
  when a chain was attached on the port next to the lanyard hole — so the companion
  should land on `G5/G6` — but that was one observation with one cable. The link
  derives its pins from whatever the bus probe decided, so it follows automatically
  if that turns out differently.
* **Whether either port's 5 V rail matters.** The Atom is USB-powered and the link
  only uses two signal pins and ground, so it should not; untested.
* **The panel driver.** M5Stack changed the AtomS3R's screen IC from GC9107 to
  ST7735 in May 2026. M5GFX autodetects the panel at run time and both are supported,
  but only one of the two has been reasoned about here and neither has been seen.
* **Screen legibility, and the legend in particular.** Font sizes, the drawn mic, the
  tap icons, the layout and the 60 % brightness were all chosen on a 128×128 grid on
  paper, not on a 0.85″ panel in a lit room. The legend rows are the sharpest version
  of this question: they are drawn in the 16-pixel font when all three fit the panel's
  width and drop to the 8-pixel console font together when any of them does not, so a
  layer whose longest row is one character over falls off a legibility cliff. If the
  rows come out too small in practice the fix is to shorten the labels in
  `layerLegend()` on the DualKey, not to change the font here — the strings are built
  in one place precisely so this is a one-file change.
* **The animation timings.** 700 ms for the mic pulse, 250 ms for the layer wipe,
  150 ms for a tap flash and 800 ms for the knob bar are guesses about what reads as
  deliberate rather than twitchy on a small screen. All four are `#define`s at the top
  of `companion_proto.h`.
* **Whether the ~30 fps cap is the right ceiling.** Each animation frame is a full
  128×128×2 sprite blit over SPI. The arithmetic says that is comfortable; it has not
  been measured, and the mic pulse is the frame that does the most drawing.
* **Simultaneous reconfiguration.** If the DualKey's Chain bus loses every node it
  re-probes all four pin candidates, which includes the pair the companion is using.
  The companion watches for that and moves to the other port, but the hand-off has
  not been exercised.
