# M5Stack Chain Series — Firmware Research for Macro-Keyboard Build

Researched 2026-09-13. Hardware in hand: 1x Chain DualKey (ESP32-S3FN8 head unit), 1x Chain Key,
2x Chain Joystick, 1x Chain Angle, 1x Chain Mono (8x8 LED, STM32G031 node).

Sources fetched and cited inline. Anything not directly confirmed by a fetched source is marked
**UNCONFIRMED**.

---

# 1. Stock firmware verdict

**Overall: the stock M5DualKey-UserDemo factory firmware CANNOT implement the desired mapping.**
Its web-config only exposes a fixed list of ~12 *paired* shortcut presets (both physical buttons
assigned together to a matched function, e.g. "Arrow Keys: Up/Down"), not independent arbitrary
keycodes (F7/F8/F9) with explicit press-on-down/release-on-up semantics. Custom Arduino firmware
is required for the core Key1/Key2/Chain-Key mapping. Some Joystick/Angle behaviors are partially
reachable stock; Mono glyph-on-hold is not reachable stock at all (no such trigger exists in the
web UI model).

| # | Desired mapping | Stock verdict | Evidence |
|---|---|---|---|
| 1 | DualKey Key1 **HOLD → F8** (true hold: press on down, release on up) | **CANNOT** | The factory web config's "HID Device Status" section only offers preset paired functions — Copy/Paste, Undo/Redo, Tab Switch, Window Switch, Zoom, Page, Volume, Media, Home/End, Arrow Up/Down, Arrow Left/Right — assigned per-button from that fixed list. There is no "assign arbitrary keycode (F7/F8/F9)" field, and no documented distinction between a momentary hold-down/hold-up HID report vs. a one-shot triggered shortcut. [docs.m5stack.com/en/guide/input_device/chain_dualkey](https://docs.m5stack.com/en/guide/input_device/chain_dualkey) |
| 2 | Key2 **HOLD → F9** | **CANNOT** | Same reasoning as #1 — same fixed preset list, same lack of arbitrary-keycode assignment. |
| 3 | Chain Key **TAP → F7** | **CANNOT** | The bottom-of-page "Chain Bus" section lets you open a connected node's "HID Function Config" and assign functions to its trigger events (Press/Release/etc.), but the assignable functions are drawn from the **same fixed preset list** as the local buttons — no arbitrary F-key assignment is documented. [docs.m5stack.com/en/guide/input_device/chain_dualkey](https://docs.m5stack.com/en/guide/input_device/chain_dualkey) |
| 4 | Joystick1 **X/Y → arrow keys** | **PARTIAL** | "Arrow Keys" (Up/Down and Left/Right, as two separate presets) is in the stock preset list, and the Chain Bus UI explicitly supports assigning "HID Function Config" per connected node's trigger/data events, so mapping joystick axis motion to arrow keys is plausible. However, the exact granularity (does one joystick get both axes mapped simultaneously to 4-way arrows, or only one axis pair per node?) is **UNCONFIRMED** — the guide doesn't show a joystick-specific config screenshot. |
| 4b | Joystick1 **button press → Enter** | **CANNOT** (not confirmed possible) | "Enter" is not one of the preset HID functions listed (Copy/Paste, Undo/Redo, Tab/Window Switch, Zoom, Page, Volume, Media, Home/End, Arrows). No arbitrary-key option exists to assign Enter to a joystick button press event. |
| 5 | Joystick2 **→ mouse move/scroll or media keys** | **PARTIAL** | Media Control (Prev/Next, Play/Pause/Stop) is in the preset list and could plausibly be bound to a node's trigger events. Mouse **move/scroll from analog X/Y** is **UNCONFIRMED / likely CANNOT** — nothing in the guide suggests the stock firmware exposes joystick analog value to relative mouse-movement/scroll-wheel HID reports; the preset list is all keyboard/consumer shortcuts, no mouse. |
| 6 | Angle knob **→ volume/scroll wheel** | **PARTIAL** | Volume Control (Up/Down) is a preset; whether a continuous ADC knob can be bound to repeated Volume Up/Down "tick" events (vs. only being usable with discrete-trigger nodes like Key/Joystick-button) is **UNCONFIRMED**. Scroll-wheel mapping is **not** in the preset list at all → CANNOT for scroll. |
| 7 | Mono **shows glyph while a key is held** | **CANNOT** | The stock config model only lets you set the Mono node's own indicator/RGB and, per its own product page, is a Chain series node — but the DualKey web UI's "HID Function Config" framework is about mapping *input* trigger events to *HID output* actions; there's no documented path for cross-node reactive rendering (Key1 hold → drive Mono's pixel/text). Nothing in the factory-firmware guide mentions Mono at all as a chain-bus target in the web UI (it predates Mono's release in the visible screenshots) — likely just not wired up in stock firmware. |

**Bottom line:** build custom Arduino firmware. The stock firmware is good only for quick smoke-testing individual nodes before you replace it.

---

# 2. Stock firmware how-to

Source: [docs.m5stack.com/en/guide/input_device/chain_dualkey](https://docs.m5stack.com/en/guide/input_device/chain_dualkey) (Chain DualKey Factory Firmware User Guide).

- **AP SSID**: `DualKey_XXXX` (XXXX = 4-char alphanumeric code unique per device).
- **AP password**: `12345678`.
- **Config URL**: `http://192.168.4.1` (visit after connecting to the AP).
- **Switch positions** (3-position slide switch, labelled in system prompt as part of hardware):
  - **Middle (OFF/USB)**: device is off when unpowered; when USB-connected it runs in **USB wired mode** (and *also* enables BLE + Wi-Fi simultaneously).
  - **Left or Right**: both positions behave identically in stock firmware — they enable **BLE + Wi-Fi** mode.
  - Battery charges whenever external power is connected, regardless of switch position.
- **Host connection**:
  - Wired: USB-C cable.
  - BLE: pair with `DualKey-XXXX` from the host's Bluetooth settings; confirm the pairing code. Only one BLE host can be paired at a time (unpair to switch).
  - USB wired + BLE can be connected **simultaneously** (one host each).
- **Wi-Fi config**: in the web UI's "Wi-Fi" panel you can join a real network (SSID/password/static IP) instead of AP mode; the device then serves the config page from that IP instead of `192.168.4.1`.
- **Wi-Fi reset**: hold both Key1+Key2 for 5s until Key1 LED turns off / Key2 LED turns on, release — LEDs flash red x3 then white x3, Wi-Fi settings are cleared and AP mode re-enables.
- **LED config**: web UI "Basic Information" panel sets each key's RGB color and shows live button/switch/battery state; setting a button's color to pure black (0,0,0) makes it rainbow-cycle instead of off.
- **Chain Bus config**: bottom of the page shows a topology diagram of everything wired to either Chain Bus port; clicking a device jumps to its card, where you can set its indicator LED and open "HID Function Config" to bind its trigger events to the same preset HID function list used for Key1/Key2. "Bus RGB" sets the indicator color for an entire bus side.
- **Firmware update path**: via **M5Burner** — pick device type **"Chain DualKey"**, then **"Chain DualKey User Demo"** → Download → Burn. Device must first be put in Download Mode (see §5) before connecting.
- Chained node connection direction matters: the triangle arrow on Chain Bridge/Chain Return connectors must point **outward** from the DualKey master.

---

# 3. Chain bus protocol

Source docs: [Chain Joystick](https://docs.m5stack.com/en/chain/Chain_Joystick), [Chain Angle](https://docs.m5stack.com/en/chain/Chain_Angle), [Chain Key](https://docs.m5stack.com/en/chain/Chain_Key), [Chain Mono](https://docs.m5stack.com/en/chain/Chain_Mono), plus the linked Communication Protocol PDFs for each (Joystick/Angle/Key/Mono — all fetched directly), and the general bus overview at [docs.m5stack.com/en/arduino/projects/chain/chain_bus](https://docs.m5stack.com/en/arduino/projects/chain/chain_bus).

## Physical layer
- **UART, 115200 bps, 8N1** on every node (STM32G031G8U6 core on all input/display nodes). Confirmed identically on Joystick, Angle, Key, Mono spec tables.
- Each node has **two HY2.0-4P connectors** (IN/OUT), daisy-chained. **Orientation matters**: the triangle arrow molded into the Chain Bridge/Chain Return connector must point *away* from the DualKey master — i.e., master's port → node's input side, node's output side → next node's input side.
- DualKey exposes **two independent Chain Bus UARTs**, one per side: on the ESP32-S3, **UART1 = G48 (TX)/G47 (RX)**, **UART2 = G5 (RX)/G6 (TX)** — confirmed via the ESPHome integration page's `uart:` block (`chain_uart_left: tx GPIO48 / rx GPIO47`, `chain_uart_right: tx GPIO6 / rx GPIO5`) and matches the pin numbers given for the hardware in hand. [docs.m5stack.com/en/homeassistant/devices/chain_dualkey](https://docs.m5stack.com/en/homeassistant/devices/chain_dualkey)
- To use both DualKey Chain ports at once in Arduino, instantiate **two separate `Chain` class objects**, each bound to its own `HardwareSerial`/pin pair. [docs.m5stack.com/en/arduino/projects/chain/chain_bus](https://docs.m5stack.com/en/arduino/projects/chain/chain_bus)

## Frame format (identical framing across Joystick/Angle/Key/Mono protocol docs)
```
All packets start with 0xAA 0x55 and end with 0x55 0xAA
Byte:      0            1             2        3    4     5     6     7     ...
Field:  Length_low  Length_high    Index_id   Cmd  Data1 Data2 Data3 Data4  ... CRC
```
- **Length** = byte count from `Index_id` through `CRC` inclusive (excludes the length field itself, excludes header/trailer).
- **Index_id** = the node's position/address in the chain (device index ID; broadcast/system frames use `0xFF`, e.g. heartbeat and enumerate).
- **CRC** = 8-bit, **simple additive checksum** (not a real CRC despite the name): sum all data bytes between the header block and the CRC field, truncated to uint8:
  ```c
  uint8_t calculateCRC(const uint8_t *buffer, uint16_t size) {
      uint8_t crc8 = 0;
      for (uint8_t i = 4; i < (size - 3); i++) { crc8 += buffer[i]; }
      return crc8;
  }
  ```
  (identical function given verbatim in the Joystick, Angle, and Key protocol PDFs.)

## Node addressing / enumeration
- **`0xFE` "enumerate"**: host sends `Send_num` (starts 0), each node in the chain increments and forwards; the host receives back `Receive_num` = total device count. This is how node **Index_id / position** is established — nodes are numbered by distance from the master, closest = ID 1, incrementing outward (confirmed both in the ESPHome docs' `chain_id` rule and mirrored in this protocol's enumerate command).
- **`0xFC` "Enumeration requests"**: sent by an end-of-chain device autonomously (on hot-plug or power-on) to tell the host to re-scan.
- **`0xFD` "Heartbeat"**: periodic keepalive; host can also use it to detect a live chain device.
- **`0xF8` Query UID**: 4-byte or 12-byte unique ID per node (`UID_Type` param).
- **`0xF9` / `0xFA`**: bootloader version / firmware version query.
- **`0xFB` Query device type**: returns 16-bit `Device_type` — confirmed type codes: **Angle = 0x0002, Key = 0x0003, Joystick = 0x0004, Mono = 0x000D**.

## Per-node commands

### Shared on all nodes (Joystick/Angle/Key/Mono): RGB LED
- `0x20` Set RGB values (Index_id, start-index, count N, then N×{R,G,B}) → `Operation_status` (0 fail / 1 ok).
- `0x21` Get RGB values.
- `0x22` Set RGB brightness (0–100, default 40) with `Save_to_flash` flag (flash write ~20ms, avoid doing it every loop).
- `0x23` Get RGB brightness.
  - Note: Joystick and Angle each have **1** RGB LED (Index=0, Num=1). Chain Key has **2** RGB LEDs but they're driven as one logical color (Index=0, Num=1 still).

### Chain Joystick (device type 0x0004) — [protocol PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1191/M5Stack-Chain-Joystick-Protocol-EN.pdf)
- `0x30` Get 16-bit ADC (raw): returns X/Y each as uint16, theoretical range 0–65535 (practical range narrower due to part tolerance).
- `0x31` Get 8-bit ADC (raw): X/Y each 0–255.
- `0x32`/`0x33` Get/Set the **mapping calibration table**: per-axis negative-min/negative-max/positive-min/positive-max ADC breakpoints (16 bytes) used to convert raw ADC into a centered/signed range.
- `0x34` **Get mapped 16-bit value**: X/Y each **signed int16, range −4095…4095** — this is what the Arduino API's `getJoystickMappedInt16Value()` wraps (confirmed: matches the tutorial's `int16_t x_value, y_value`).
- `0x35` Get mapped 8-bit value: X/Y each **signed int8, range −128…127**.
- `0xE0` **Button press event** (Z-axis click), autonomously pushed by the node in "active reporting" mode. `Status`: 0=Single Click, 1=Double Click, 2=Long Press. Triggering it also auto-flashes the RGB LED (green/blue/red pulse respectively, decaying over 1s) as visual feedback layered on top of your own LED color.
- `0xE1` Check button status (polled): 0 = not pressed, 1 = pressed — **this is the raw instantaneous down/up state**, usable to build true press/hold/release logic yourself instead of relying on the single/double/long "press type" classifier.
- `0xE2`/`0xE3` Set/get double-click interval `(Double+1)*100ms` (0–9 → 100–1000ms) and long-press interval `(Long+3)*1s` (0–7 → 3–10s).
- `0xE4`/`0xE5` Set/get button reporting mode: 0 = non-active (poll only), 1 = active (node pushes `0xE0` events unsolicited). Default = active.
- Physical: Hall-effect joystick, X/Y analog + Z digital click, 1× WS2812C LED. PinMap: STM32 PB0=button input, PA7=XOUT, PA6=YOUT, PA8=RGB, UART1 on PB6/PB7, UART2 on PA2/PA3 (node-internal pins, not exposed to you — you talk to it only over the Chain Bus).

### Chain Angle (device type 0x0002) — [protocol PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1197/M5Stack-Chain-Angle-Protocol-EN.pdf)
- `0x30` Get 12-bit ADC: **0–4095** (±0–10 count error at the extremes). This is what `getAngle12BitAdc()` wraps.
- `0x31` Get 8-bit ADC: 0–255 (±0–3 error).
- `0x32`/`0x33` Set/get rotation direction: `Clockwise_direct` 0 = clockwise-decreases, 1 = clockwise-increases (default).
- No button on Chain Angle — it is a bare 280°±10° potentiometer knob (per product page spec table), no discrete click/press events in the protocol (no `0xE0`-series commands present in its protocol doc, unlike Joystick/Key).
- To use it as **volume or a scroll wheel**, your custom firmware would poll `0x30` each loop, track delta vs. last reading, and emit `USBHIDConsumerControl` volume up/down ticks or `Mouse.move()`/wheel events per detent of movement (see §4).

### Chain Key (device type 0x0003) — [protocol PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1192/M5Stack-Chain-Key-Protocol-EN.pdf)
- Button semantics are **byte-for-byte identical** to the Joystick's button block: `0xE0` push event (Single/Double/Long + RGB flash), `0xE1` poll raw pressed/not-pressed, `0xE2`/`0xE3` double/long interval config, `0xE4`/`0xE5` reporting-mode get/set.
- 2× WS2812C LEDs, hot-swap mechanical blue switch. No joystick/angle-specific commands (no `0x30`+ block) — Key's own protocol doc only has the RGB block + the button block + the common system block (F8/F9/FA/FB/FC/FD/FE).

### Chain Mono (device type 0x000D) — [protocol PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1245/M5Stack-Chain-Mono-Protocol-EN.pdf)
- 8×8 monochrome LED matrix, no RGB block (unlike the input nodes) — instead has its own display command set:
  - `0x10`/`0x11` Set/get display mode: 0 = **Pixel control mode** (direct pixel set), 1 = **Scrolling character mode**.
  - `0x30` **Set pixels** (batch, 1–64 pixels per call): each pixel packed into 1 byte — bit6 = on/off, bits[5:3] = X (0–7), bits[2:0] = Y (0–7).
  - `0x31` **Full-screen buffer refresh**: 8 bytes, one per row (`Display_buffer0`=row0 … `Display_buffer7`=row7), each byte's bit7→bit0 = X coord 0→7 high-bit-first, 1=on.
  - `0x32` Get pixel value(s) by coordinate; `0x33` get the full 8-byte display buffer.
  - `0x34` **Display a single ASCII character** (32–127, 5×7 font) at an X/Y offset (nibble-packed: high nibble = x-offset 0–7, low nibble = y-offset 0–7) — only valid in pixel-control mode.
  - `0x40` **Display scrolling string** (only valid in scroll mode): mode nibble = direction (0 right/1 left/2 up/3 down) + loop-type (0 once/1 loop/2 bounce), plus `Scroll_interval` in ms/pixel (0–65535) and the ASCII string itself (32–127, 5×7 font, up to 32 chars per the product page).
  - `0x41` Get current scrolling-string config; `0x42`/`0x43` set/get scroll play-state (0=playing,1=paused,2=stopped/cleared).
  - `0xE0`/`0xE1` Set/get screen rotation: 0/90/180/270°.
  - `0xE2`/`0xE3` Set/get brightness **level 0–7** (not 0–100 like the RGB nodes), default 7.
  - `0xE4` Clear screen.
- For "show a glyph while a key is held": your ESP32-S3 firmware would issue `0x34` (or a `0x30` pixel batch) the instant it sees Key1/Key2 go down, then issue a clear (`0xE4`) or blank pixel batch the instant it sees the key go up — driven entirely by your own custom code, not by the node itself (Mono has no concept of "react to another node's button").

## Common system block on every node
`0xF8` UID query, `0xF9` bootloader version, `0xFA` firmware version, `0xFB` device type, `0xFC` enumeration-request push, `0xFD` heartbeat, `0xFE` enumerate/count — byte-identical command IDs and semantics across Joystick/Angle/Key/Mono protocol docs.

---

# 4. Arduino path

## Board/toolchain
- **Board package**: M5Stack ESP32 board manager, **version ≥ 3.2.4** — install via [Arduino Board Manager guide](https://docs.m5stack.com/en/arduino/arduino_board). [docs.m5stack.com/en/arduino/chain_dualkey/program](https://docs.m5stack.com/en/arduino/chain_dualkey/program)
- **Board selection**: `M5ChainDualKey` (exact board name in Arduino IDE's board dropdown, confirmed via the program-compile page screenshot caption).
- **Libraries**:
  - `M5Unified` **≥ 0.2.11** — for the on-board `Button_Class` (Key1/Key2 debounce+edge state) and general M5 init.
  - `M5Chain` **≥ 1.0.0** (general); **≥ 1.0.4** specifically to get the Chain Mono pixel/text API — per the Mono tutorial's stated build requirement (vs. 1.0.0 for Joystick/Angle/Key/bus tutorials).
  - `Adafruit_NeoPixel` **≥ 1.15.2** — for driving the DualKey's own 2 onboard WS2812 key LEDs directly (separate from the Chain Bus RGB commands, which only address *chained* node LEDs).
- **USB mode**: the DualKey uses the ESP32-S3's native USB-OTG/TinyUSB stack via the arduino-esp32 core's built-in `USB.h` + `USBHIDKeyboard.h` + `USBHIDMouse.h` (no separate "USB CDC vs USB OTG" board menu setting was mentioned in the fetched pages — **UNCONFIRMED** whether a manual "USB Mode" IDE menu selection is required; the sample code just does `USB.begin()`).
- **Download-mode entry** (needed to flash *new* Arduino code, since DualKey has no reset button):
  1. Move the 3-position switch to the **middle** position.
  2. **Hold Key1** (the button farther from the lanyard hole).
  3. Connect the USB-C cable while still holding Key1.
  4. Release Key1 — device is now in download mode; select the port in Arduino IDE.
  - If the device was already flashed with a USB-HID sketch, you must repeat this cycle (unplug → switch to middle → hold Key1 → replug → release Key1) before it will show up for a *new* upload, since a HID keyboard doesn't expose a normal serial/reset handshake.
  - To just reboot into your already-flashed sketch (not download mode): middle switch position, unplug, replug **without** holding Key1.

## Confirmed pins (Chain DualKey / ESP32-S3FN8)
| Function | Pin(s) | Source |
|---|---|---|
| Key1 (button, active-low) | **G0** | Button + USB HID example code (`#define pin_Key1 0`) |
| Key2 (button, active-low) | **G17** | same |
| Onboard key RGB LED data | **G21** | LED example (`#define LED_SIG_PIN 21`) + ESPHome (`pin: GPIO21`) |
| Onboard key RGB LED power-enable | **G40** | LED example (`#define LED_PWR_PIN 40`) + ESPHome (`switch platform: gpio, pin: GPIO40, restore_mode: ALWAYS_ON`) — **must be driven HIGH** before `NeoPixel.begin()`/`show()` will light anything |
| Side switch → BLE position | **G8** | Switch example (`#define SWITCH_BLE 8`) — matches ESPHome `SWITCH 2` on GPIO8 |
| Side switch → Wi-Fi/right position | **G7** | Switch example (`#define SWITCH_WIFI 7`) — matches ESPHome `SWITCH 1` on GPIO7 |
| Battery charge-status ADC | **G9** | Power example (`PIN_ADC_CHRG 9`) |
| Battery voltage ADC | **G10** | Power example (`PIN_ADC_BATT 10`) — formula `analogRead()/4095.0*3.3*1.51` |
| USB VBUS voltage ADC | **G2** | Power example (`PIN_ADC_VBUS 2`) |
| Chain Bus UART1 (one side) | **TX=G48, RX=G47** | ESPHome `chain_uart_left` |
| Chain Bus UART2 (other side) | **RX=G5, TX=G6** | ESPHome `chain_uart_right`; also confirmed as the pins used in every M5Chain Arduino tutorial's `#define RXD_PIN GPIO_NUM_5 / TXD_PIN GPIO_NUM_6` (with the code comment noting "47/48 for the other side") |

Note: `SWITCH_1(G8)`/`SWITCH_2(G7)` must **never** be driven as `OUTPUT HIGH` in your sketch — the switch docs page warns this breaks the device's ability to power off cleanly.

## M5Unified `Button_Class` API (confirmed from the fetched Button tutorial)
```cpp
m5::Button_Class Key1;
Key1.setRawState(millis(), !digitalRead(pin));   // feed raw debounced state each loop
Key1.wasPressed()        // edge: went down this loop
Key1.wasReleased()       // edge: went up this loop
Key1.wasSingleClicked()
Key1.wasDoubleClicked()
Key1.wasHold()           // edge: crossed the hold-time threshold (one-shot, NOT a continuous "is held" state)
Key1.wasReleaseFor(5000) // released after having been held >= 5000ms
```
**Important for your "true hold" requirement**: `wasHold()` is a *one-shot threshold-crossing event* (fires once after the button has been down for some duration), not "currently physically down." For **F8/F9 to behave as a true hold** — HID key-down the instant Key1 is physically pressed, HID key-up the instant it's physically released, with no artificial long-press delay — the simplest reliable approach is to **bypass the debounced click/hold classifier entirely** and drive the keyboard straight off the raw pin state each loop, e.g.:
```cpp
bool key1_down = !digitalRead(pin_Key1);   // active-low
if (key1_down && !key1_was_down) Keyboard.press(KEY_F8);
if (!key1_down && key1_was_down) Keyboard.release(KEY_F8);
key1_was_down = key1_down;
```
This avoids `wasHold()`'s implicit delay and matches "press on down / release on up" exactly. The full `Button_Class` API reference (isPressed()/isHolding() state accessors etc.) lives at [docs.m5stack.com/en/arduino/m5unified/button_class](https://docs.m5stack.com/en/arduino/m5unified/button_class) — **not fetched in this session**, so treat any additional method names beyond the ones shown above as **UNCONFIRMED**.

## USB HID keyboard/mouse (confirmed via fetched hid.md page)
```cpp
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
Keyboard.begin(); Mouse.begin(); USB.begin();
Keyboard.press(KEY_LEFT_CTRL); Keyboard.press('c'); ... Keyboard.releaseAll();
Mouse.move(dx, dy);
Mouse.click(MOUSE_RIGHT);
```
Library: arduino-esp32's built-in `USB` component. Refs given: [USB lib source](https://github.com/espressif/arduino-esp32/tree/master/libraries/USB), [USBHIDKeyboard.h](https://github.com/espressif/arduino-esp32/blob/master/libraries/USB/src/USBHIDKeyboard.h), [USBHIDMouse.h](https://github.com/espressif/arduino-esp32/blob/master/libraries/USB/src/USBHIDMouse.h), [Espressif USB docs](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/usb.html).

- **`USBHIDConsumerControl`** (for volume/media keys and possibly scroll) is **UNCONFIRMED in this session** — it was not shown in the fetched hid.md sample and I did not open the library's file listing to verify its presence/API. It is part of the same `arduino-esp32/libraries/USB` tree by reputation, but confirm directly at the GitHub link above before relying on it for the Angle-knob volume mapping.
- Mouse **scroll wheel**: `USBHIDMouse` in the fetched sample only demonstrates `move()` and `click()`. A `move(x, y, wheel)` or dedicated scroll call was **not shown** — check `USBHIDMouse.h` directly (UNCONFIRMED).

## BLE HID keyboard (confirmed via fetched ble.md page — alternative to USB HID)
The BLE tutorial builds a **raw BLE HID keyboard from scratch** using ESP32 Arduino's `BLEDevice`/`BLEHIDDevice`/`BLECharacteristic` classes (not a high-level "BleKeyboard" wrapper) — you define your own HID report map (their example: 1-byte modifier + 6-byte keycode array, standard boot-keyboard layout), advertise as `BLEHIDDevice`, and manually build/send `KeyReport` structs via `inputReportCharacteristic->notify()`. Device name in their example: `"Chain DualKey Keyboard"`. Keycodes follow the USB HID Usage Tables (`0x04`='a', etc. — [HID Usage Tables 1.6 PDF](https://www.usb.org/sites/default/files/hut1_6.pdf)); modifiers (Ctrl/Shift/Alt/GUI) are a separate bitfield, not normal keycodes. References given: [arduino-esp32 BLE lib](https://github.com/espressif/arduino-esp32/tree/master/libraries/BLE), [ESP32 Arduino BLE docs](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ble.html), [ESP-IDF BT/BLE HID demo](https://github.com/espressif/esp-idf/blob/v5.5.1/examples/bluetooth/esp_hid_device), Bluetooth [HID Service 1.0](https://www.bluetooth.com/specifications/specs/human-interface-device-service-1-0/) / [HID over GATT 1.0](https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile-1-0/) specs. Note in the doc: **unpair/re-pair on the host whenever you change any BLE identity fields** (name/manufacturer/pnp/hidInfo/reportMap), or the host's BT stack can crash on reconnect.

## M5Chain library API (confirmed function names, from fetched tutorials — class is `Chain`, instance commonly named `M5Chain`)
```cpp
#include "M5Chain.h"
Chain M5Chain;
M5Chain.begin(&Serial2, 115200, rxPin, txPin);      // per-bus init; instantiate a 2nd Chain object for the DualKey's other UART
bool M5Chain.isDeviceConnected();
chain_status_t M5Chain.getDeviceNum(uint16_t *count);
bool M5Chain.getDeviceList(device_list_t *list);     // list->devices[i].id, .device_type
M5Chain.setRGBLight(id, brightness0to100, &opr_status);
M5Chain.setRGBValue(id, startIndex, count, uint8_t rgbArray[], arraySize, &opr_status);

// Joystick
M5Chain.setJoystickButtonTriggerInterval(id, BUTTON_DOUBLE_CLICK_TIME_500MS, BUTTON_LONG_PRESS_TIME_5S, &opr_status);
M5Chain.getJoystickMappedInt16Value(id, &x_value, &y_value);   // signed, ~-4095..4095
M5Chain.getJoystickButtonStatus(id, &button_status);            // raw pressed/not-pressed
M5Chain.getJoystickButtonPressStatus(id, &button_press_type);   // CHAIN_BUTTON_PRESS_SINGLE/_DOUBLE/_LONG (queued events)

// Angle
M5Chain.getAngle12BitAdc(id, &angle_12bit);   // 0..4095

// Key
M5Chain.setKeyButtonTriggerInterval(id, BUTTON_DOUBLE_CLICK_TIME_500MS, BUTTON_LONG_PRESS_TIME_5S, &opr_status);
M5Chain.getKeyButtonStatus(id, &button_status);
M5Chain.getKeyButtonPressStatus(id, &button_press_type);        // same enum as Joystick

// Mono
M5Chain.setMonoMode(id, MONO_PIXEL_MODE, &opr_status);          // or MONO_SCROLL_MODE (name inferred from protocol, verify enum)
M5Chain.setMonoRotation(id, MONO_ROTATION_0, &opr_status);
M5Chain.setMonoBrightness(id, MONO_BRIGHTNESS_LEVEL_7, &opr_status);
M5Chain.setMonoClear(id, &opr_status);
M5Chain.setMonoPixel(id, x, y, true/false, &opr_status);        // single pixel
M5Chain.setMonoPixel(id, MonoPixelInfo pixels[], count, &opr_status);  // batch; struct {uint8_t x, y; bool on;}

// Device type codes / enum (from chain_bus.md example)
CHAIN_UNKNOWN_TYPE_CODE, CHAIN_ENCODER_TYPE_CODE, CHAIN_ANGLE_TYPE_CODE, CHAIN_KEY_TYPE_CODE,
CHAIN_JOYSTICK_TYPE_CODE, CHAIN_TOF_TYPE_CODE   // (commented-out in the sample but named: CHAIN_UART/_SWITCH/_PEDAL/_PIR/_MIC/_BUZZER_TYPE_CODE — presumably added in a newer lib version)

// Status codes
typedef enum {
    CHAIN_OK = 0x00, CHAIN_PARAMETER_ERROR = 0x01, CHAIN_RETURN_PACKET_ERROR = 0x02,
    CHAIN_BUSY = 0x04, CHAIN_TIMEOUT = 0x05
} chain_status_t;
```
Source tutorials: [chain_bus](https://docs.m5stack.com/en/arduino/projects/chain/chain_bus), [chain_joystick](https://docs.m5stack.com/en/arduino/projects/chain/chain_joystick), [chain_angle](https://docs.m5stack.com/en/arduino/projects/chain/chain_angle), [chain_key](https://docs.m5stack.com/en/arduino/projects/chain/chain_key), [chain_mono](https://docs.m5stack.com/en/arduino/projects/chain/chain_mono). Library repo: [github.com/m5stack/M5Chain](https://github.com/m5stack/M5Chain); examples referenced by the docs pages: `Joystick_Example`, `Key_Example` under `M5Chain/examples/` (**directory listing not independently fetched this session** — trust the per-product tutorial page snippets above over guessing at example filenames beyond what's linked).

`getJoystickButtonStatus`/`getKeyButtonStatus` map onto protocol command `0xE1` (raw instantaneous pressed/not-pressed) — this is your hook for **true press/hold/release** logic on remote Chain-Key/Joystick-button nodes, exactly analogous to reading Key1/Key2's raw GPIO state directly on the DualKey itself. Don't rely solely on `getJoystickButtonPressStatus`'s Single/Double/Long classification if you want hold-to-repeat or hold-to-display-glyph behavior — poll the raw status instead.

## Factory firmware source (M5DualKey-UserDemo)
- **Framework: ESP-IDF**, not Arduino — confirmed by the repo's `CMakeLists.txt`, `sdkconfig*`, `partitions.csv`, and stated **"esp-idf version: 5.5.1"** in its README. [github.com/m5stack/M5DualKey-UserDemo](https://github.com/m5stack/M5DualKey-UserDemo)
- Required library dependency listed: **M5Chain** (same library used from Arduino).
- Repo structure: `components/`, `main/` — typical ESP-IDF component layout. I did not open `main/` source files in this session, so the actual HID/BLE implementation details inside are **UNCONFIRMED** beyond what the web-config guide documents about *behavior*. If you want to fork the stock firmware rather than write from scratch, you'd need ESP-IDF 5.5.1 (not Arduino IDE) and would inherit its `main/` source under `M5Template-C-CPP` scaffolding (repo "Generated from m5stack/M5Template-C-CPP").
- Given the mismatch between the desired mapping and the stock preset-list architecture, **writing fresh Arduino firmware using M5Unified + M5Chain + USBHIDKeyboard/Mouse (§4 above) is the more practical path** than trying to extend the ESP-IDF factory firmware's HID Function Config system.

---

# 5. Gotchas

- **Download mode is finicky and firmware-state-dependent**: once a sketch claims the USB port as a HID keyboard, the normal auto-reset-into-bootloader trick doesn't work — you must physically hold Key1 while plugging in, every time you want to flash new code.
- **No reset button** — power-cycle via unplug/replug (switch in middle position) is the only way to restart into your already-flashed sketch.
- **G7/G8 (side switch pins) must stay inputs** — driving them HIGH as outputs breaks clean power-off (explicit warning on the Switch API page).
- **G40 (LED power-enable) must be set HIGH before the onboard NeoPixels will do anything** — easy to forget since it's separate from the NeoPixel data pin (G21).
- **Middle switch position = OFF when unpowered, USB+BLE+WiFi-all-at-once when USB-powered** — there is no switch position that is "USB-only, BLE/WiFi off"; if you need to minimize radio activity you'll have to disable BLE/Wi-Fi in your own firmware rather than relying on the switch.
- **Node discovery/addressing order**: nodes are numbered by physical distance from the master (closest = ID 1, incrementing outward), independently per bus side (left bus and right bus each have their own 1..N numbering) — confirmed both in the ESPHome `chain_id` rule and the Chain Bus protocol's enumerate command. Get this wrong and you'll be sending Joystick commands to what you think is the Angle.
- **Using both DualKey Chain-Bus ports simultaneously requires two separate `Chain`/`M5Chain` class instances** in your sketch, each bound to its own UART/pin pair — a single instance only manages one side.
- **BLE HID identity changes (name/PnP/report map) can wedge the host's Bluetooth stack** if you don't unpair first, per an explicit warning on the BLE tutorial page — annoying during iterative custom-firmware development, so unpair before every re-flash that touches BLE identity fields.
- **Flash-writing RGB brightness / rotation / Save_to_flash=1 settings takes ~20ms and disables the node's serial interrupt during that write** — don't do this in a tight loop (e.g., don't persist brightness to flash every frame while animating).
- **`wasHold()` in M5Unified's Button_Class is a threshold-crossing one-shot event, not a live "is held" boolean** — for the true press/hold/release semantics this build needs, drive Keyboard.press()/release() off raw `digitalRead()` transitions yourself rather than the button class's derived events (see §4).
- Chain Angle has **no button/click protocol block at all** (confirmed: its protocol PDF has no `0xE0`-series commands, unlike Joystick and Key) — there is no "knob push-click" feature to fall back on if you were hoping for one; it's rotation-only.

---

# 6. Open questions (could not confirm — do not guess when building)

1. Does the stock web UI's "HID Function Config" for a chained node let you bind **independent single actions per trigger type** (e.g., Joystick-button Single-click → one function, Double-click → a different function), or only one function per node overall? Not shown in the fetched guide.
2. Exact granularity of stock Joystick→Arrow-Keys mapping: does it use the analog X/Y continuously (repeat-fires while off-center) or only fire once per detent/edge? Not documented.
3. Does `USBHIDConsumerControl` exist and work in the arduino-esp32 core used by this board package, and what's its API (needed for Volume/Media-key output from the Angle knob and Joystick2)? Only inferred from the general USB library repo link, not opened/verified this session.
4. Does `USBHIDMouse` expose a scroll-wheel parameter/method (for "Angle → scroll wheel" or "Joystick2 → scroll")? Only `move()` and `click()` were shown in the fetched sample.
5. Exact `M5Chain` enum member names for Mono's `setMonoMode()` scroll-mode constant (I've seen `MONO_PIXEL_MODE` confirmed in the pixel-mode tutorial; the scroll-mode constant name is inferred, not directly observed in a fetched code sample — check `M5Chain.h` directly before compiling).
6. Contents of `M5Chain/examples/Joystick_Example` and `Key_Example` (linked by the product pages but the GitHub directory listing itself wasn't fetched this session) — these may contain more complete field-usage patterns than the short docs-page snippets captured here.
7. Whether the ESP32-S3 Arduino core requires an explicit **"USB Mode" board-manager submenu selection** (e.g., "USB-OTG (TinyUSB)" vs "Hardware CDC") for the DualKey board profile, the way many generic ESP32-S3 boards do — not mentioned on the fetched Quick-Start/HID pages, possibly because `M5ChainDualKey` board preset already fixes this; unconfirmed either way.
8. M5DualKey-UserDemo's actual `main/` ESP-IDF source was not opened — the "how it does HID / how it talks to chain nodes" implementation-level detail (vs. documented user-facing behavior) is unconfirmed.
