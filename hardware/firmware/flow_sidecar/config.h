/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/include/config.h
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * config.h - every tunable for the Flow sidecar lives here.
 *
 * Pins, bus selection, timings, deadzones, the layer-table SHAPE and the
 * colour/glyph maps. No .cpp in this project contains a magic number of its
 * own; the layer DATA itself lives in layers.cpp.
 *
 * v2.1 change: most of the *behavioural* constants below are now DEFAULTS
 * rather than the values the firmware reads at run time. settings.h copies
 * them into a mutable `RuntimeConfig` (`g_cfg`) at boot, the host settings
 * protocol (PROTOCOL.md) edits that, and `Preferences` persists it. Each
 * affected #define is tagged "(default for g_cfg.<field>)" below; anything
 * untagged is still a hard compile-time constant.
 */

#ifndef FLOW_CONFIG_H
#define FLOW_CONFIG_H

#include <Arduino.h>

#define FLOW_SIDECAR_VERSION "0.1.0"

/* Host settings protocol revision (PROTOCOL.md). Bump only on a breaking
 * change to the wire format; the host checks it in the `hello` reply. */
#define FLOW_SIDECAR_PROTO 1

/* 0 = build the FLOW (dictation) layer only, which is what 0.1.0 ships;
 * the other layers are complete but unproven on hardware; set to 1 to build
 * them. With one layer the Chain Key's long hold has nothing to cycle to and
 * is inert - its tap and double-tap still work. */
#define FLOW_EXTRA_LAYERS 0

/* ================================================================== */
/* Debug / serial console                                              */
/* ================================================================== */

/* 1 = emit Serial diagnostics, 0 = silent.
 * Under PlatformIO, -DARDUINO_USB_CDC_ON_BOOT=1 in platformio.ini makes
 * `Serial` the TinyUSB CDC port, which coexists with the HID interfaces on the
 * same cable. Every print is still behind this switch so the firmware is
 * unaffected if that flag is ever turned off. */
#define FLOW_DEBUG 1

/* 1 = accept one-character commands on Serial ('?' prints a status dump).
 * Costs one non-blocking Serial.available() check per loop. */
#define FLOW_SERIAL_CONSOLE 1

/* 1 = run the newline-delimited JSON host settings protocol (PROTOCOL.md) on
 * the same CDC port. Lines starting with '{' are commands; everything else
 * falls through to the one-character console above, so both coexist. */
#define FLOW_SERIAL_PROTO 1

/* Longest accepted inbound line, per PROTOCOL.md. Longer lines are dropped
 * (not truncated) so a half-command can never be parsed as a whole one. */
#define FLOW_PROTO_MAX_LINE 512

/* Outbound scratch buffer. Bigger than FLOW_PROTO_MAX_LINE on purpose: the
 * `get_config` reply serialises the whole config object and lands around
 * 700 bytes. Static, so no reply or event ever touches the heap. */
#define FLOW_PROTO_OUT_BYTES 1024

/* Longest text the `mono` command may scroll (PROTOCOL.md says <= 32). */
#define FLOW_PROTO_MONO_TEXT 32

/* Unsolicited `battery` event cadence. */
#define FLOW_PROTO_BATT_EVENT_MS 30000

/* USB string descriptors. The host finds the device by VID 0x303A (Espressif,
 * left at the core default) plus this product string. */
#define FLOW_USB_PRODUCT      "fethr sidecar"
#define FLOW_USB_MANUFACTURER "fethr"

#if FLOW_DEBUG
#define FLOG(...)               \
  do {                          \
    Serial.printf(__VA_ARGS__); \
  } while (0)
#else
#define FLOG(...) \
  do {            \
  } while (0)
#endif

/* ================================================================== */
/* HID timings                                                         */
/* ================================================================== */

/* How long a tap holds the key/button down before the deferred release. */
#define FLOW_TAP_MS 12

/* Gap between the two clicks of an ACT_MOUSE_DOUBLE. Must exceed FLOW_TAP_MS
 * so the first button-up is on the wire before the second button-down, and
 * must stay under the host's double-click threshold (Windows default 500 ms). */
#define FLOW_DBLCLICK_GAP_MS 60

/* Number of simultaneously outstanding deferred releases. */
#define FLOW_PEND_SLOTS 4

#include "actions.h"
#include "glyphs.h"

/* ================================================================== */
/* Chain DualKey (ESP32-S3FN8) board pins                              */
/* ================================================================== */

#define PIN_KEY1      0  /* Key1 - the button farther from the lanyard hole */
#define PIN_KEY2      17 /* Key2                                            */
#define PIN_LED_DATA  21 /* WS2812 data for the two on-board key LEDs       */
#define PIN_LED_POWER 40 /* WS2812 power enable - MUST be driven HIGH       */
#define NUM_LEDS      2
#define PIN_VBAT      10 /* battery voltage ADC (vendor Power example)      */
#define PIN_VBUS      2  /* USB bus voltage ADC, same x1.51 divider         */

/* Which NeoPixel index sits under which key. Swap if they look reversed.
 * (default for g_cfg.led_index_key1; key2 is always the other one) */
#define LED_INDEX_KEY1 0
#define LED_INDEX_KEY2 1

/* G7 (SWITCH_1) and G8 (SWITCH_2) are the 3-position side-switch sense lines.
 * They are deliberately NOT touched anywhere in this firmware: driving them as
 * OUTPUT HIGH breaks the board's ability to power off cleanly. */

/* ================================================================== */
/* Chain bus                                                           */
/* ================================================================== */

/* Compile-time bus selection. The DualKey has two HY2.0-4P Chain ports:
 *
 *   "right" port (HY2.0-4P_2): RX = G5,  TX = G6    <-- default, all nodes here
 *   "left"  port (HY2.0-4P_1): RX = G47, TX = G48
 *
 * Note: the ESPHome integration page and the DualKey wiki PinMap disagree about
 * the left port (48/47 vs 47/48). The values below follow the ESPHome block;
 * if the left port ever gets used and enumerates nothing, try swapping them.
 *
 * To add the second bus later: declare a second `Chain` object, call begin() on
 * it with Serial1 + the left pins, give it its own role IDs + failure counter,
 * and extend the scheduler's round-robin with its tasks. Each Chain instance
 * manages exactly one UART. Not implemented here on purpose: single-bus only. */
#define CHAIN_UART        Serial2
#define CHAIN_RX_PIN      5
#define CHAIN_TX_PIN      6
#define CHAIN_LEFT_RX_PIN 47 /* reference only - second bus not implemented */
#define CHAIN_LEFT_TX_PIN 48 /* reference only - second bus not implemented */
#define CHAIN_BAUD        115200

/* Per-transaction timeout. The library default is 100 ms, which would stall the
 * loop badly when a node is unplugged; a Chain round trip at 115200 is ~2 ms. */
#define CHAIN_CALL_TIMEOUT_MS 20
/* Longer budget for the enumeration burst, which is only run on (re)connect. */
#define CHAIN_ENUM_TIMEOUT_MS 200

/* Consecutive non-CHAIN_OK results that trigger a re-enumeration. */
#define CHAIN_FAIL_LIMIT 5
/* Minimum gap between re-enumeration attempts, so an empty bus cannot spin. */
#define CHAIN_REENUM_COOLDOWN_MS 2000

/* Upper bound on nodes tracked per bus (5 in this build). */
#define MAX_CHAIN_DEVICES 16

/* Polling intervals per node. One Chain transaction is issued per loop()
 * iteration, round-robin, so these are lower bounds, not guarantees. */
#define CHAIN_POLL_KEY_MS   0  /* chain key: every scheduler visit */
#define CHAIN_POLL_JOY_MS   10
#define CHAIN_POLL_ANGLE_MS 10

/* ================================================================== */
/* Chained-node RGB LEDs (v2)                                          */
/* ================================================================== */

/* 1 = compile the node-LED driver in at all. Whether it actually writes is
 * additionally gated at run time by g_cfg.node_leds (default = this value).
 *
 * 1 = drive each chained node's own LED with per-layer semantics.
 * BUS BUDGET: 5 nodes at 115200 with a ~2 ms read/write round trip. Input
 * polling already wants ~500 transactions/s (nav + scroll = 2 each per 10 ms,
 * angle 1 per 10 ms, chain key every spare visit). Node-LED writes are issued
 * ONLY when every input task is up to date - they sit at the bottom of the
 * round-robin - and each node is additionally rate-limited to
 * NODE_LED_MIN_INTERVAL_MS, i.e. at most 4 * 10 = 40 writes/s = ~80 ms/s of
 * extra bus time worst case. Input polling can therefore never be starved. */
#define FLOW_NODE_LEDS 1

#define NODE_LED_MIN_INTERVAL_MS 100 /* <= 10 Hz per node                    */
#define NODE_LED_JOY_THRESHOLD   16  /* min 0..255 change worth a bus write  */
#define NODE_LED_JOY_IDLE_PCT    12  /* joystick LED brightness at centre, % */
#define NODE_LED_TAP_MS          150 /* chain-key node flash on tap          */
#define NODE_LED_COUNT_AFTER_MS  200 /* chain-key node turns the layer colour */
                                     /* this long into a layer-switch hold,   */
                                     /* i.e. "keep holding". Deliberately not */
                                     /* 0: a plain tap should not repaint it. */

/* ================================================================== */
/* Local key handling                                                  */
/* ================================================================== */

#define DEBOUNCE_MS 5

/* Chain Key held for at least this long cycles the layer instead of firing its
 * tap action. The tap action fires on RELEASE, only if held for less.
 * (default for g_cfg.layer_hold_ms) */
#define LAYER_HOLD_MS 1000

/* 1 = the Chain Key's double-tap actions are live (default for
 * g_cfg.double_tap). Turning it OFF also removes the single-tap latency
 * described below, because there is then nothing to wait for. */
#define FLOW_DOUBLE_TAP 1

/*
 * Chain Key double-tap window (v2).
 *
 * LATENCY TRADEOFF - read before changing:
 * A second tap can only be recognised by waiting for it, so on any layer whose
 * `chain_key_double` is NOT ACT_NONE the single-tap action is delayed by this
 * much. Every layer in layers.cpp currently defines a double action, so the
 * Chain Key's single tap costs +350 ms everywhere. The alternative -
 * fire the single immediately and emit the double as an extra action - would
 * mean "undo" arriving after the paste it was meant to undo, which is worse.
 * Set a layer's chain_key_double to {ACT_NONE, 0, MOD_NONE, FN_NONE} to get the
 * instant single tap back on that layer.
 *
 * Note this delay applies ONLY to the Chain Key. The DualKey's own Key1/Key2
 * (the F8/F9 dictation holds) are still driven straight off raw GPIO edges with
 * no classifier in the path at all.
 *
 * (default for g_cfg.double_tap_ms)
 */
#define CHAIN_KEY_DOUBLE_MS 350

/* ================================================================== */
/* Joysticks                                                           */
/* ================================================================== */

/* getJoystickMappedInt16Value() returns roughly -4095..4095 per axis. */
#define JOY_FULLSCALE 4095
#define JOY_DEADZONE  600

/* Axis orientation. Flip a sign to (-1) if a stick feels inverted.
 * (defaults for g_cfg.nav_x_sign / nav_y_sign / scroll_x_sign / scroll_y_sign) */
#define NAV_X_SIGN    (+1)
#define NAV_Y_SIGN    (+1)
#define SCROLL_X_SIGN (+1)
#define SCROLL_Y_SIGN (+1)

/* Nav stick arrow auto-repeat. */
#define NAV_REPEAT_DELAY_MS 300 /* first repeat after the initial press */
#define NAV_REPEAT_RATE_MS  50  /* subsequent repeats                   */

/* Scroll stick: one wheel/pan step per emit, emitted at a deflection-dependent
 * rate (just past the deadzone = slow, full deflection = fast). */
#define SCROLL_INTERVAL_MIN_MS 20  /* at full deflection        */
#define SCROLL_INTERVAL_MAX_MS 200 /* just past the deadzone    */
#define SCROLL_STEP            1   /* wheel clicks per emit     */
#define PAN_STEP               1   /* horizontal pan per emit   */

/* ================================================================== */
/* Mouse cursor (v2, NAV_MOUSE)                                        */
/* ================================================================== */

#define MOUSE_REPORT_MS 10 /* one relative report per 10 ms while deflected  */
#define MOUSE_MAX_PX    20 /* px per report at full deflection               */
#define MOUSE_MIN_PX    1  /* px per report just past the deadzone           */

/* Cursor sign, applied ON TOP of NAV_X_SIGN / NAV_Y_SIGN (the poll site
 * normalises the raw axes first, this stage converts to HID cursor space).
 * HID Y grows DOWNWARD, so a stick pushed "up" - positive Y once normalised -
 * must produce a NEGATIVE dy; hence (-1) here while the arrow-key mapping,
 * which needs no such flip, reads the same normalised value as (+1).
 *
 * These two are COMPILE-TIME COORDINATE-SPACE CONVERSION, not a preference:
 * MOUSE_Y_SIGN is what makes "stick up" mean "cursor up" at all. The host
 * protocol's `mouse_y_sign` (default +1) is a separate USER inversion applied
 * on top of this one, which is why its default is +1 and not -1. */
#define MOUSE_X_SIGN (+1)
#define MOUSE_Y_SIGN (-1)

/* ================================================================== */
/* Angle knob                                                          */
/* ================================================================== */

/* getAngle12BitAdc() returns 0..4095 over the pot's ~280 degrees. */
#define ANGLE_DETENTS    24
#define ANGLE_STEP       (4096 / ANGLE_DETENTS) /* 170 ADC counts per detent */
#define ANGLE_HYSTERESIS 30 /* ADC counts of overshoot needed to leave a detent */

/* Detent crossings are queued and drained one per gap, because two identical
 * consumer-control reports emitted back to back can be coalesced by the host. */
#define ANGLE_MAX_QUEUE   8
#define ANGLE_EMIT_GAP_MS 20

/* Wheel direction when the knob drives scrolling. */
#define ANGLE_WHEEL_SIGN (+1)

/* ================================================================== */
/* Chain Mono indicator                                                */
/* ================================================================== */

/* Plain numbers rather than the M5Chain enums, because these are now also the
 * host protocol's units and config.h is included by translation units that do
 * not pull in M5Chain.h. mono.cpp does the cast to mono_brightness_level_t /
 * mono_rotation_t. (defaults for g_cfg.mono_brightness / g_cfg.mono_rotation) */
#define MONO_BRIGHTNESS 5 /* 0..7  -> MONO_BRIGHTNESS_OFF .. _LEVEL_7      */
#define MONO_ROTATION   0 /* degrees, 0|90|180|270 -> MONO_ROTATION_0..270 */

/* What the panel shows when nothing else wants it (default for
 * g_cfg.mono_idle): 0 = blank (the v2.0 behaviour), 1 = the current layer's
 * letter. See mono.cpp for why the letter is drawn at full brightness. */
#define MONO_IDLE_BLANK   0
#define MONO_IDLE_LETTER  1
#define MONO_IDLE_DEFAULT MONO_IDLE_LETTER

#define MONO_CHECK_MS 400  /* checkmark shown after a hold ends       */
#define MONO_LAYER_MS 1000 /* layer letter shown after a layer change */

/* ---- host engine state (the `state` command, PROTOCOL.md) ---- */

/* Spinner cadence while the host reports `transcribing` / `cleaning`. The
 * panel is still only written when the frame number changes, i.e. at most one
 * transaction every MONO_SPIN_FRAME_MS. */
#define MONO_SPIN_FRAME_MS 120

#define MONO_HOST_OK_MS  700 /* checkmark on `pasted` */
#define MONO_HOST_ERR_MS 900 /* cross on `error`      */

/* How long a host counts as attached after its last `hello` or `state`.
 * Inside this window the panel's own "hold released" checkmark is suppressed,
 * because the host will say `pasted` when the text has actually landed; past
 * it the device is being used as a plain keyboard and shows its own. */
#define HOST_PRESENT_MS 60000

/* How long the volume bar stays up after the knob stops moving (ANGLE_VOLUME
 * layers only). Reading of the spec: the bar is visible for this long FOLLOWING
 * any knob movement, i.e. it is armed on movement and expires 600 ms later. */
#define MONO_VOLBAR_MS 600

/* Scroll-stick activity indicator: animation step, and how long after the last
 * wheel emit the animation keeps running. */
#define MONO_SCROLL_FRAME_MS 150
#define MONO_SCROLL_HOLD_MS  200

/* 1 = on a layer change, scroll the full layer NAME across the panel using the
 * Mono node's own MONO_STRING_SCROLL_MODE, then switch back to pixel mode and
 * show the layer letter. 0 = letter only.
 * Costs 3 extra Chain transactions per layer change (mode, string, mode back),
 * all issued one-per-loop like everything else. */
#define FLOW_MONO_SCROLL_LAYER_NAME 1

#define MONO_NAME_SCROLL_INTERVAL_MS 60  /* ms per pixel of travel            */
#define MONO_NAME_SCROLL_SHOW_MS     900 /* how long to let it run before the */
                                         /* panel returns to pixel mode       */

/* The `mono` protocol command scrolls up to FLOW_PROTO_MONO_TEXT characters,
 * far more than a 4-5 character layer name, so its window is computed instead
 * of fixed: 6 px of travel per character plus the 8 px panel width, times
 * MONO_NAME_SCROLL_INTERVAL_MS, clamped to MONO_TEXT_SCROLL_MAX_MS. */
#define MONO_TEXT_SCROLL_PX_PER_CHAR 6
#define MONO_TEXT_SCROLL_MAX_MS      15000

/* 1 = use the Chain Mono full-buffer refresh command (0x31, one transaction).
 * 0 = fall back to a 64-pixel batch write (0x30). Both are real M5Chain APIs;
 * the buffer refresh is cheaper and is the default. */
#define FLOW_MONO_USE_BUFFER_REFRESH 1

/* ================================================================== */
/* On-board LEDs                                                       */
/* ================================================================== */

#define LED_BASE_PCT      18  /* idle brightness of the function colour, %    */
                              /* (default for g_cfg.led_idle_pct)            */
#define LED_FLASH_HALF_MS 120 /* on/off half-period of a layer-change flash   */
#define LED_TAP_FLASH_MS  80  /* bright flash of one key when it taps         */
                              /* (default for g_cfg.led_flash_ms)            */
#define LED_HOLD_PERIOD_MS 900 /* breathe period while a hold is active       */
#define LED_HOLD_MIN_PCT  25  /* dimmest point of the breathe, %              */

/* Colour a key breathes while its hold action is active.
 * (default for g_cfg.hold_rgb) */
#define LED_HOLD_R 255
#define LED_HOLD_G 0
#define LED_HOLD_B 0

/* Host engine state feedback on the two key LEDs (the `state` command).
 * Busy = both keys breathe amber on the same wave as a hold; the two flashes
 * are one-shots routed through ledFlashAll(). Compile-time constants: they
 * describe the protocol's semantics rather than a per-device preference. */
#define LED_HOST_BUSY_R 255
#define LED_HOST_BUSY_G 110
#define LED_HOST_BUSY_B 0

#define LED_HOST_OK_R 0 /* one short flash on `pasted` */
#define LED_HOST_OK_G 255
#define LED_HOST_OK_B 60

#define LED_HOST_ERR_R 255 /* two flashes on `error` */
#define LED_HOST_ERR_G 0
#define LED_HOST_ERR_B 0

/* ================================================================== */
/* Battery (v2)                                                        */
/* ================================================================== */

/* 1 = sample the VBAT ADC on G10 and tint the idle key colour toward orange
 * once the pack drops below BATT_LOW_MV.
 * Formula from the vendor Power example: V = analogRead()/4095.0*3.3*1.51 */
#define FLOW_BATTERY_MONITOR 1

#define BATT_SAMPLE_MS   30000 /* one ADC read per 30 s                       */
#define BATT_LOW_MV      3500  /* below this = low                            */
#define BATT_HYST_MV     80    /* must climb this far back to clear "low"     */
#define BATT_TINT_PCT    55    /* how far to pull the idle colour to orange, %*/
#define BATT_TINT_R      255
#define BATT_TINT_G      80
#define BATT_TINT_B      0

/* ================================================================== */
/* Layer table shape                                                   */
/* ================================================================== */

/* Upper bound on LAYER_COUNT. Fixes the size of RuntimeConfig::layer_rgb, so
 * it is part of the NVS blob layout - bumping it must bump FLOW_CFG_VERSION.
 * layers.cpp static_asserts that the real table fits. */
#define FLOW_MAX_LAYERS 4

/* What the nav stick does on a layer. */
enum NavMode : uint8_t {
  NAV_OFF = 0,
  NAV_ARROWS, /* 4-way arrow keys with auto-repeat       */
  NAV_MOUSE   /* v2: relative cursor with an accel curve */
};

/* What the scroll stick does on a layer. */
enum ScrollMode : uint8_t {
  SCROLL_OFF = 0,
  SCROLL_WHEEL_PAN,   /* Y -> wheel, X -> horizontal pan             */
  SCROLL_WHEEL_ARROWS /* Y -> wheel, X -> Left/Right arrow autorepeat */
};

/* What the angle knob does on a layer. */
enum AngleMode : uint8_t {
  ANGLE_OFF = 0,
  ANGLE_VOLUME, /* consumer volume inc/dec per detent */
  ANGLE_WHEEL   /* mouse wheel ticks per detent       */
};

struct LayerConfig {
  const char *name;
  uint8_t     r, g, b;   /* layer colour                            */
  uint8_t     glyph;     /* GlyphId shown on layer change           */
  Action      key1;      /* DualKey Key1                            */
  Action      key2;      /* DualKey Key2                            */
  Action      chain_key; /* Chain Key single tap                    */
  Action      chain_key_double; /* Chain Key double tap (v2)        */
  uint8_t     nav_mode;     /* NavMode                              */
  Action      nav_click;    /* nav stick click                      */
  uint8_t     scroll_mode;  /* ScrollMode                           */
  Action      scroll_click; /* scroll stick click                   */
  uint8_t     angle_mode;   /* AngleMode                            */
};

extern const LayerConfig LAYERS[];
extern const uint8_t     LAYER_COUNT;

/* Layer colour lookup (layers.cpp). Reads g_cfg.layer_rgb; an out-of-range
 * layer yields layer 0 rather than reading past the table. */
void layerRgb(uint8_t layer, uint8_t out_rgb[3]);

/* Function -> appearance maps (layers.cpp). fnColor() reads the RUNTIME table
 * in g_cfg; fnDefaultRgb() hands out the compile-time one, which is what
 * settings.cpp seeds g_cfg from. */
void    fnColor(uint8_t fn, uint8_t layer, uint8_t out_rgb[3]);
uint8_t fnGlyph(uint8_t fn);
void    fnDefaultRgb(uint8_t fn, uint8_t out_rgb[3]);

/* Stable protocol name for an FnId ("DICT_RAW", ...). NULL for FN_NONE and
 * anything out of range, so a caller can simply omit the field. */
const char *fnName(uint8_t fn);

/* Reverse lookup for `set fn_rgb.<NAME>`. Returns FN_NONE when unknown. */
uint8_t fnByName(const char *name);

#endif /* FLOW_CONFIG_H */
