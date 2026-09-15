/*
 * leds.cpp - the DualKey's own two WS2812 key LEDs.
 *
 * The Chain RGB commands only address CHAINED nodes; these two are local and
 * bit-banged by Adafruit_NeoPixel. The strip is only re-shown when a computed
 * colour actually changes, so an idle sidecar does no bit-banging at all.
 *
 * Priority, highest first:
 *   1. flash                both keys: a layer change, or one of the host
 *                           state one-shots (green on `pasted`, red twice on
 *                           `error`) - all routed through ledFlashAll()
 *   2. hold breathe         that key, g_cfg.hold_rgb, millis()-driven
 *                           triangle wave
 *   3. host busy breathe    both keys, amber, same wave, while the host
 *                           reports transcribing or cleaning
 *   4. tap flash            that key, its function colour at full brightness
 *   5. idle                 that key's FUNCTION colour on the current layer,
 *                           at g_cfg.led_idle_pct, tinted toward orange if
 *                           the battery is low
 *
 * v2.1: every colour and percentage above comes from g_cfg, not from the
 * config.h #defines (which are now only its defaults), and the key->pixel
 * mapping is g_cfg.led_index_key1 so the host can fix a reversed pair without
 * a reflash. ledsConfigChanged() drops the repaint cache after any change.
 */

#include "sidecar.h"

#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel LED(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

/*
 * Which physical pixel each key owns. Two LEDs, so key2 is always the other
 * one; validated on the way in (led_index_key1 is constrained to 0|1).
 *
 * 0.2.0: g_cfg.swap_keys flips it as well. led_index_key1 describes the
 * UNSWAPPED board - the pixel under PIN_KEY1 - so when the two buttons trade
 * places the colour has to follow the finger, which is exactly what the old
 * compile-time KEYS_SWAPPED #if did to LED_INDEX_KEY1. Setting swap_keys and
 * then finding the LEDs reversed is still fixable with led_index_key1; the two
 * simply compose.
 */
static inline uint8_t ledForKey(uint8_t key_index)
{
  uint8_t first = (uint8_t)(g_cfg.led_index_key1 & 1u);
  if (g_cfg.swap_keys) first = (uint8_t)(1u - first);
  return (key_index == 0) ? first : (uint8_t)(1u - first);
}

/* ---- layer-change flash (both keys) ---- */
static uint8_t  g_flash_halves = 0; /* remaining on/off half-periods */
static uint32_t g_flash_start  = 0;
static uint8_t  g_flash_r = 0, g_flash_g = 0, g_flash_b = 0;

/* ---- per-key tap flash ---- */
static uint32_t g_key_flash_until[2] = {0, 0};

/* ---- repaint bookkeeping ---- */
static uint8_t g_led_last[NUM_LEDS][3];
static bool    g_led_primed = false;

/* ---- battery / USB ---- */
static uint32_t g_batt_at_ms = 0;
static uint16_t g_batt_mv    = 0;
static uint16_t g_usb_mv     = 0;
static bool     g_batt_low   = false;

static inline uint8_t scalePct(uint8_t v, uint8_t pct)
{
  return (uint8_t)(((uint16_t)v * pct) / 100);
}

/* ================================================================== */
/* Colour helpers                                                      */
/* ================================================================== */

/* Pull a colour BATT_TINT_PCT of the way toward orange. Applied to idle
 * colours only, so a low battery reads as "everything went orange-ish" rather
 * than hiding which key does what. */
void tintForBattery(uint8_t rgb[3])
{
#if FLOW_BATTERY_MONITOR
  if (!g_batt_low) return;
  const uint8_t t[3] = {BATT_TINT_R, BATT_TINT_G, BATT_TINT_B};
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t mixed = (uint16_t)(((uint16_t)rgb[i] * (100 - BATT_TINT_PCT)) / 100) +
                     (uint16_t)(((uint16_t)t[i] * BATT_TINT_PCT) / 100);
    rgb[i] = (uint8_t)(mixed > 255 ? 255 : mixed);
  }
#else
  (void)rgb;
#endif
}

bool     batteryLow(void) { return g_batt_low; }
uint16_t batteryMilliVolts(void) { return g_batt_mv; }
uint16_t usbMilliVolts(void) { return g_usb_mv; }

void ledsConfigChanged(void)
{
  /* Forget what is on the strip so ledsUpdate() re-derives and re-shows every
   * pixel from the new g_cfg, including a changed led_index_key1. */
  g_led_primed = false;
}

/* ================================================================== */
/* Battery / USB sampling                                              */
/* ================================================================== */

/* Vendor Power example: V = analogRead()/4095.0 * 3.3 * 1.51, kept in integer
 * millivolts as raw * 3300 * 151 / (4095 * 100). Worst-case numerator is
 * 4095 * 3300 * 151 = 2.04e9, which fits uint32. Both rails use the same
 * x1.51 divider (docs/products/Chain_DualKey_Arduino_Power.md). */
static uint16_t adcMilliVolts(uint8_t pin)
{
  uint32_t raw = (uint32_t)analogRead(pin);
  return (uint16_t)((raw * 3300UL * 151UL) / (4095UL * 100UL));
}

static void batteryTick(uint32_t now)
{
#if FLOW_BATTERY_MONITOR
  if (g_batt_at_ms != 0 && (int32_t)(now - g_batt_at_ms) < 0) return;
  g_batt_at_ms = now + BATT_SAMPLE_MS;

  g_batt_mv = adcMilliVolts(PIN_VBAT);
  g_usb_mv  = adcMilliVolts(PIN_VBUS); /* reported by `status`, not acted on */

  bool low = g_batt_low ? (g_batt_mv < (BATT_LOW_MV + BATT_HYST_MV)) : (g_batt_mv < BATT_LOW_MV);
  if (low != g_batt_low) {
    g_batt_low   = low;
    g_led_primed = false; /* force a repaint in the new tint */
    FLOG("[batt] %umV %s\r\n", (unsigned)g_batt_mv, low ? "LOW" : "ok");
  }
#else
  (void)now;
#endif
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void ledsBegin(void)
{
  /* The on-board WS2812s are dark until their power rail is enabled. */
  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, HIGH);
#if FLOW_BATTERY_MONITOR
  pinMode(PIN_VBAT, INPUT);
  pinMode(PIN_VBUS, INPUT);
#endif
  LED.begin();
  LED.clear();
  LED.show();
  g_led_primed = false;
}

void ledFlashAll(uint8_t r, uint8_t g, uint8_t b, uint8_t count, uint32_t now)
{
  g_flash_r      = r;
  g_flash_g      = g;
  g_flash_b      = b;
  g_flash_halves = (uint8_t)(count * 2); /* each blink = one on + one off */
  g_flash_start  = now;
}

void ledFlashKey(uint8_t key_index, uint32_t now)
{
  if (key_index > 1) return;
  g_key_flash_until[key_index] = now + g_cfg.led_flash_ms;
}

/* Breathe level while a hold is active: a triangle wave, no float, no delay.
 * Returns 0..255, which the caller scales g_cfg.hold_rgb by. */
static uint8_t holdBreathe(uint32_t now)
{
  uint32_t phase = now % (uint32_t)LED_HOLD_PERIOD_MS;
  uint32_t half  = (uint32_t)LED_HOLD_PERIOD_MS / 2;
  uint32_t up    = (phase < half) ? phase : ((uint32_t)LED_HOLD_PERIOD_MS - phase);
  uint32_t lvl   = (up * 255U) / (half ? half : 1U); /* 0..255 */

  uint32_t lo = (255U * LED_HOLD_MIN_PCT) / 100U;
  return (uint8_t)(lo + ((lvl * (255U - lo)) / 255U));
}

void ledsUpdate(uint32_t now)
{
  batteryTick(now);

  /* ---- layer-change flash state ---- */
  bool    flashing = false;
  uint8_t fr = 0, fg = 0, fb = 0;

  if (g_flash_halves > 0) {
    uint32_t phase = (now - g_flash_start) / LED_FLASH_HALF_MS;
    if (phase >= g_flash_halves) {
      g_flash_halves = 0;
    } else {
      flashing = true;
      if ((phase & 1) == 0) { /* even half-period = lit */
        fr = g_flash_r;
        fg = g_flash_g;
        fb = g_flash_b;
      }
    }
  }

  const LayerRuntime &L     = layerAt(g_layer);
  bool                dirty = !g_led_primed;

  uint8_t hs   = hostState();
  bool    busy = (hs == HOST_TRANSCRIBING || hs == HOST_CLEANING);

  for (uint8_t k = 0; k < 2; k++) {
    uint8_t idx = ledForKey(k);
    uint8_t rgb[3];

    if (flashing) {
      rgb[0] = fr;
      rgb[1] = fg;
      rgb[2] = fb;
    } else if (g_key[k].hold_active) {
      /* The configured hold colour, scaled by the breathe wave. */
      uint8_t lvl = holdBreathe(now);
      for (uint8_t i = 0; i < 3; i++) {
        rgb[i] = (uint8_t)(((uint16_t)g_cfg.hold_rgb[i] * lvl) / 255u);
      }
    } else if (busy) {
      /* Same wave as a hold, in amber: the user has let go and the host is
       * still working. Both keys, because neither is "the one" any more. */
      const uint8_t amber[3] = {LED_HOST_BUSY_R, LED_HOST_BUSY_G, LED_HOST_BUSY_B};
      uint8_t       lvl      = holdBreathe(now);
      for (uint8_t i = 0; i < 3; i++) {
        rgb[i] = (uint8_t)(((uint16_t)amber[i] * lvl) / 255u);
      }
    } else {
      const Action &a = (k == 0) ? L.key1 : L.key2;
      fnColor(a.fn, g_layer, rgb);
      if ((int32_t)(now - g_key_flash_until[k]) < 0) {
        /* tap flash: the function colour at full brightness */
      } else {
        rgb[0] = scalePct(rgb[0], g_cfg.led_idle_pct);
        rgb[1] = scalePct(rgb[1], g_cfg.led_idle_pct);
        rgb[2] = scalePct(rgb[2], g_cfg.led_idle_pct);
        tintForBattery(rgb);
      }
    }

    if (g_led_last[idx][0] != rgb[0] || g_led_last[idx][1] != rgb[1] ||
        g_led_last[idx][2] != rgb[2]) {
      g_led_last[idx][0] = rgb[0];
      g_led_last[idx][1] = rgb[1];
      g_led_last[idx][2] = rgb[2];
      dirty              = true;
    }
    LED.setPixelColor(idx, LED.Color(rgb[0], rgb[1], rgb[2]));
  }

  /* Only bit-bang the WS2812 chain when something actually changed. */
  if (dirty) {
    LED.show();
    g_led_primed = true;
  }
}
