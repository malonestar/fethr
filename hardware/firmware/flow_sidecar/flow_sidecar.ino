/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/src/main.cpp
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * main.cpp - flow_sidecar v2
 *
 * M5Stack Chain DualKey (ESP32-S3) macro-keyboard sidecar for the local Flow
 * dictation client.
 *
 * Host contract (the Windows/macOS Flow client already implements these):
 *   hold F8 = raw dictation      (key down on press, key up on release)
 *   hold F9 = cleaned dictation  (same)
 *   tap  F7 = re-paste the last transcript
 *
 * Hardware: Chain DualKey head unit + a single Chain bus carrying
 *   1x Chain Key, 2x Chain Joystick, 1x Chain Angle, 1x Chain Mono.
 * Wiring order is NOT assumed: nodes are enumerated and dispatched by
 * device_type, with the lower-id joystick taking the "nav" role.
 *
 * Design rules enforced across the whole firmware:
 *   - loop() never calls delay(); everything is millis()-driven.
 *   - F8/F9 are driven off raw GPIO edges, never off a click classifier, so
 *     the hold has no artificial onset delay.
 *   - At most ONE Chain transaction is issued per loop() iteration, so a
 *     missing or slow node cannot stall local key scanning.
 *   - The Mono panel and the LEDs are only written when their state changes.
 *
 * v2.1 adds a host settings protocol on the same CDC port (PROTOCOL.md,
 * serial_proto.cpp) backed by a persisted runtime config (settings.cpp).
 * Nothing about the scheduler changed: protoTick() is one more non-blocking
 * step in loop(), and every setting it can change is read at the point of use.
 *
 * See FIRMWARE_NOTES.md for the build/flash procedure and the
 * VERIFIED/UNVERIFIED API table.
 */

#include <Arduino.h>

#include <string.h>

#include "sidecar.h"

/* ================================================================== */
/* Shared state                                                        */
/* ================================================================== */

uint8_t  g_layer = 0;
LocalKey g_key[2];

/* ---- host engine state ---- */
static uint8_t  g_host_state    = HOST_IDLE;
static uint32_t g_host_state_at = 0;
static uint32_t g_host_seen_at  = 0;
static bool     g_host_seen     = false;

/* ================================================================== */
/* Forward declarations                                                */
/* ================================================================== */

static void scanLocalKeys(uint32_t now);
static void bootBanner(void);

/* ================================================================== */
/* Setup / loop                                                        */
/* ================================================================== */

void setup()
{
  /* FIRST: the persisted configuration. Everything below reads g_cfg, so
   * loading it here is what stops the device flashing the compiled-in
   * defaults for a frame before the saved colours land. */
  settingsBegin();

  /* Local keys are active-low. */
  g_key[0].pin = PIN_KEY1;
  g_key[1].pin = PIN_KEY2;
  for (uint8_t i = 0; i < 2; i++) {
    pinMode(g_key[i].pin, INPUT_PULLUP);
    g_key[i].raw              = false;
    g_key[i].stable           = false;
    g_key[i].changed_ms       = 0;
    g_key[i].hold_active      = false;
    g_key[i].hold_action.type = ACT_NONE;
    g_key[i].hold_action.code = 0;
    g_key[i].hold_action.mods = MOD_NONE;
    g_key[i].hold_action.fn   = FN_NONE;
  }

  /* g_cfg.boot_layer decides where we come up. Set before anything paints. */
  g_layer = (g_cfg.boot_layer < LAYER_COUNT) ? g_cfg.boot_layer : 0;

  ledsBegin();

  Serial.begin(115200);
  bootBanner();
  protoBegin(); /* emits the `boot` event */

  /* Claim the USB device before touching the bus, so the host starts
   * enumerating while the chain is being probed. */
  actionsBegin();

  uint32_t now = millis();
  chainBegin(now); /* emits the `chain` event once enumeration succeeds */

  /* AFTER chainBegin(): the companion takes whichever HY2.0-4P port the bus
   * auto-probe did not settle on. Opens a UART and arms the beacon; nothing
   * blocks and nothing waits for an answer. */
  companionBegin(now);

  /* Announce the starting layer on the panel and the key LEDs: scroll the
   * layer name once, then settle on its letter (same as a layer change). */
  monoScrollText(LAYERS[g_layer].name, /*announce_layer=*/true);
  ledsUpdate(now);
  protoEventLayer(g_layer);
  FLOG("[flow-sidecar] layer %u (%s)\r\n", (unsigned)g_layer, LAYERS[g_layer].name);
}

void loop()
{
  uint32_t now = millis();

  actionsTick(now);     /* retire deferred HID releases           */
  scanLocalKeys(now);   /* DualKey GPIOs - every single pass      */
  chainInputTick(now);  /* software timers: knob queue, tap window*/
  chainService(now);    /* at most one Chain transaction          */
  ledsUpdate(now);      /* repaint only when a colour changed     */
  protoTick(now);       /* non-blocking console + JSON protocol   */
  companionTick(now);   /* companion link: one line in, timers    */
}

/* Whole seconds since boot. millis() wraps after ~49.7 days; so does this,
 * and the host is expected to treat a decrease as a reboot. */
uint32_t uptimeSeconds(uint32_t now)
{
  return now / 1000UL;
}

/* ================================================================== */
/* Host engine state                                                   */
/* ================================================================== */

/* Index order MUST match enum HostState. These strings are WIRE FORMAT - they
 * are the accepted values of the `state` command. */
static const char *const HOST_STATE_NAME[HOST_STATE_COUNT] = {
    "idle", "recording", "transcribing", "cleaning", "pasted", "error",
};

uint8_t  hostState(void) { return g_host_state; }
uint32_t hostStateSince(void) { return g_host_state_at; }

const char *hostStateName(uint8_t state)
{
  if (state >= (uint8_t)HOST_STATE_COUNT) return NULL;
  return HOST_STATE_NAME[state];
}

bool hostStateByName(const char *name, uint8_t *out)
{
  if (name == NULL) return false;
  for (uint8_t s = 0; s < (uint8_t)HOST_STATE_COUNT; s++) {
    if (strcmp(HOST_STATE_NAME[s], name) == 0) {
      if (out != NULL) *out = s;
      return true;
    }
  }
  return false;
}

void hostSeen(uint32_t now)
{
  g_host_seen    = true;
  g_host_seen_at = now;
}

bool hostPresent(uint32_t now)
{
  if (!g_host_seen) return false;
  return (uint32_t)(now - g_host_seen_at) < (uint32_t)HOST_PRESENT_MS;
}

/*
 * PASTED and ERROR are re-armed even when they repeat: two pastes in a row
 * should produce two checkmarks. The continuous states are not, because
 * re-arming them would restart the spinner's phase on every repeat.
 */
void hostStateSet(uint8_t state, uint32_t now)
{
  if (state >= (uint8_t)HOST_STATE_COUNT) return;
  if (state == g_host_state && state != HOST_PASTED && state != HOST_ERROR) return;

  g_host_state    = state;
  g_host_state_at = now;

  switch (state) {
    case HOST_PASTED:
      monoOverlay(G_CHECK, MONO_HOST_OK_MS, now);
      ledFlashAll(LED_HOST_OK_R, LED_HOST_OK_G, LED_HOST_OK_B, 1, now);
      break;
    case HOST_ERROR:
      monoOverlay(G_CROSS, MONO_HOST_ERR_MS, now);
      ledFlashAll(LED_HOST_ERR_R, LED_HOST_ERR_G, LED_HOST_ERR_B, 2, now);
      break;
    default:
      /* IDLE/RECORDING/TRANSCRIBING/CLEANING have no one-shot of their own;
       * the panel and the LEDs read hostState() each pass. */
      break;
  }

  /* The early return above means this fires on a CHANGE, which is exactly the
   * contract the companion link advertises. */
  companionEventState(state);

  FLOG("[host] state %s\r\n", HOST_STATE_NAME[state]);
}

/* ================================================================== */
/* Local key scanning                                                  */
/* ================================================================== */

static void scanLocalKeys(uint32_t now)
{
  for (uint8_t i = 0; i < 2; i++) {
    LocalKey &k = g_key[i];

    bool level = (digitalRead(k.pin) == LOW); /* active-low */
    if (level != k.raw) {
      k.raw        = level;
      k.changed_ms = now;
      continue; /* wait out the bounce window */
    }
    if (level == k.stable) continue;
    if ((now - k.changed_ms) < DEBOUNCE_MS) continue;

    k.stable = level;

    if (level) {
      /* Press edge. Capture the action now so a layer change mid-hold cannot
       * strand a key down on the host. */
      const Action &a = (i == 0) ? LAYERS[g_layer].key1 : LAYERS[g_layer].key2;
      if (actionIsHold(a)) {
        k.hold_action = a;
        k.hold_active = true;
        actionHoldPress(a);
        protoEventHold((uint8_t)(i + 1), true, a.fn);
        companionEventHold((uint8_t)(i + 1), true);
        FLOG("[key%u] hold down type=%u code=0x%02X\r\n", (unsigned)(i + 1), (unsigned)a.type,
             (unsigned)a.code);
      } else {
        actionFire(a, now);
        ledFlashKey(i, now);
        protoEventTap((i == 0) ? "1" : "2");
        companionEventTap((i == 0) ? "1" : "2", actionLabel(a));
        FLOG("[key%u] tap type=%u code=0x%02X\r\n", (unsigned)(i + 1), (unsigned)a.type,
             (unsigned)a.code);
      }
    } else {
      /* Release edge. */
      if (k.hold_active) {
        actionHoldRelease(k.hold_action);
        k.hold_active = false;
        /* With a host attached the checkmark means "the text landed", which
         * only the host knows - it arrives as `state: pasted`. Releasing the
         * key is not that moment. With no host (plain-keyboard use) the
         * release IS all the feedback there is, so show it. */
        if (!hostPresent(now)) monoOverlay(G_CHECK, MONO_CHECK_MS, now);
        protoEventHold((uint8_t)(i + 1), false, k.hold_action.fn);
        companionEventHold((uint8_t)(i + 1), false);
        FLOG("[key%u] hold up\r\n", (unsigned)(i + 1));
      }
    }
  }
}

void releaseAllHolds(void)
{
  for (uint8_t i = 0; i < 2; i++) {
    if (g_key[i].hold_active) {
      actionHoldRelease(g_key[i].hold_action);
      g_key[i].hold_active = false;
    }
  }
}

/* ================================================================== */
/* Layer switching                                                     */
/* ================================================================== */

/* The one place a layer actually changes. cycleLayer() and the host's `layer`
 * command both land here so neither can forget a step. */
static void enterLayer(uint8_t layer, uint32_t now)
{
  /* End any hold cleanly, then drop every outstanding tap/consumer/mouse
   * report so the new layer starts from a blank HID state. */
  releaseAllHolds();
  actionsReleaseAll();

  g_layer = layer;

  uint8_t rgb[3];
  layerRgb(g_layer, rgb); /* the RUNTIME colour, not LAYERS[].r/g/b */
  ledFlashAll(rgb[0], rgb[1], rgb[2], 2, now);

  chainOnLayerChanged(now); /* resets stick/knob state, repaints node LEDs,
                             * and hands the panel its layer announcement */
  /* A layer change from ANY source lands here, so both listeners are told
   * exactly once whether it came from the Chain Key, the host or the
   * companion's own button. */
  protoEventLayer(g_layer);
  companionEventLayer(g_layer);

  FLOG("[layer] -> %u (%s)\r\n", (unsigned)g_layer, LAYERS[g_layer].name);
}

void cycleLayer(uint32_t now)
{
  enterLayer((uint8_t)((g_layer + 1) % LAYER_COUNT), now);
}

bool setLayer(uint8_t layer, uint32_t now)
{
  if (layer >= LAYER_COUNT || layer == g_layer) return false;
  enterLayer(layer, now);
  return true;
}

/* ================================================================== */
/* Serial                                                              */
/* ================================================================== */

/* Human banner. Deliberately does NOT start with '{' - that first byte is
 * what the host protocol uses to tell machine lines from human ones. */
static void bootBanner(void)
{
#if FLOW_DEBUG || FLOW_SERIAL_CONSOLE
  Serial.printf("\r\n[flow-sidecar] v" FLOW_SIDECAR_VERSION
                " boot - %u layers, proto %u, '?' for status\r\n",
                (unsigned)LAYER_COUNT, (unsigned)FLOW_SIDECAR_PROTO);
#endif
}

/* The reader moved to serial_proto.cpp in v2.1: one place now owns the CDC
 * port, because the '?' console and the JSON protocol share it and have to
 * agree on where a line starts. */
