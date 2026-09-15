/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/src/settings.cpp
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * settings.cpp - defaults, NVS persistence and the "apply" fan-out for the
 * runtime configuration described in settings.h.
 *
 * NVS LAYOUT (namespace "fethr", key "cfg", one blob):
 *
 *   offset  size  field
 *   0       4     magic    = FLOW_CFG_MAGIC  ('F','S','C','1')
 *   4       2     version  = FLOW_CFG_VERSION
 *   6       2     size     = sizeof(RuntimeConfig)
 *   8       n     the RuntimeConfig itself, raw
 *
 * A stored blob is accepted only when the byte length, the magic, the version
 * and the recorded sizeof all match this build. Any mismatch is treated as
 * "no configuration": the defaults win and the stale blob is simply left in
 * place until the next `save` overwrites it. That is the whole point of the
 * header - a field added to RuntimeConfig can never be read back as garbage.
 */

#include "sidecar.h"

#include <Preferences.h>
#include <string.h>

RuntimeConfig g_cfg;

static bool g_from_nvs = false;

/* Packed is NOT used: the blob is only ever written and read by this same
 * build (the header's `size` check enforces that), and packing a struct on
 * xtensa costs unaligned-access shims on every field read in the hot loop. */
struct ConfigBlob {
  uint32_t      magic;
  uint16_t      version;
  uint16_t      size;
  RuntimeConfig cfg;
};

/* ================================================================== */
/* Defaults                                                            */
/* ================================================================== */

void settingsDefaults(RuntimeConfig &c)
{
  memset(&c, 0, sizeof(c));

  /* How many layers a factory-default device has. FLOW_EXTRA_LAYERS is the
   * requested count; the two clamps make a bad value a small table rather
   * than an out-of-bounds seed loop. */
  uint8_t want = (uint8_t)FLOW_EXTRA_LAYERS;
  if (want < 1) want = 1;
  if (want > DEFAULT_LAYER_COUNT) want = DEFAULT_LAYER_COUNT;
  c.layer_count = want;

  /* The keymap and the layer colours both come from the compile-time table, so
   * there is exactly one place that says "FLOW is blue" and one that says what
   * Key1 does on it. Slots past layer_count stay zeroed. */
  for (uint8_t i = 0; i < c.layer_count; i++) {
    layerDefaultInto(i, c.layers[i]);
    layerDefaultRgb(i, c.layer_rgb[i]);
  }

  /* Function colours come from the FN_RGB table in layers.cpp. */
  for (uint8_t f = 0; f < (uint8_t)FN_COUNT; f++) fnDefaultRgb(f, c.fn_rgb[f]);

  c.hold_rgb[0] = LED_HOLD_R;
  c.hold_rgb[1] = LED_HOLD_G;
  c.hold_rgb[2] = LED_HOLD_B;

  c.led_idle_pct   = LED_BASE_PCT;
  c.led_flash_ms   = LED_TAP_FLASH_MS;
  c.node_leds      = FLOW_NODE_LEDS ? 1 : 0;
  c.led_index_key1 = LED_INDEX_KEY1;

  c.mono_brightness = MONO_BRIGHTNESS;
  c.mono_rotation   = MONO_ROTATION;
  c.mono_idle       = MONO_IDLE_DEFAULT;

  c.double_tap    = FLOW_DOUBLE_TAP ? 1 : 0;
  c.double_tap_ms = CHAIN_KEY_DOUBLE_MS;
  c.layer_hold_ms = LAYER_HOLD_MS;

  c.boot_layer = 0;

  c.nav_x_sign    = NAV_X_SIGN;
  c.nav_y_sign    = NAV_Y_SIGN;
  c.scroll_x_sign = SCROLL_X_SIGN;
  c.scroll_y_sign = SCROLL_Y_SIGN;
  c.mouse_y_sign  = +1; /* on top of MOUSE_Y_SIGN - see config.h */

  c.nav_swap_xy    = 0;
  c.scroll_swap_xy = 0;
  c.swap_keys      = 0; /* Key 1 = the button farther from the lanyard hole */
}

/* ================================================================== */
/* Load / save                                                         */
/* ================================================================== */

bool settingsLoadedFromNvs(void) { return g_from_nvs; }

void settingsBegin(void)
{
  settingsDefaults(g_cfg);
  g_from_nvs = false;

  Preferences prefs;
  if (!prefs.begin(FLOW_CFG_NAMESPACE, /*readOnly=*/true)) {
    /* Namespace does not exist yet - first boot, or after a reset. Not an
     * error; the defaults already loaded above are the answer. */
    FLOG("[cfg] no stored config, using defaults\r\n");
    return;
  }

  ConfigBlob blob;
  memset(&blob, 0, sizeof(blob)); /* so a short read cannot be inspected raw */
  size_t n = prefs.getBytes(FLOW_CFG_KEY, &blob, sizeof(blob));
  prefs.end();

  if (n != sizeof(blob) || blob.magic != FLOW_CFG_MAGIC ||
      blob.version != FLOW_CFG_VERSION || blob.size != sizeof(RuntimeConfig)) {
    FLOG("[cfg] stored blob rejected (%u bytes, magic %08X, v%u) - defaults\r\n", (unsigned)n,
         (unsigned)blob.magic, (unsigned)blob.version);
    return;
  }

  g_cfg      = blob.cfg;
  g_from_nvs = true;
  FLOG("[cfg] loaded v%u blob from NVS\r\n", (unsigned)FLOW_CFG_VERSION);
}

bool settingsSave(void)
{
  Preferences prefs;
  if (!prefs.begin(FLOW_CFG_NAMESPACE, /*readOnly=*/false)) {
    FLOG("[cfg] NVS open for write failed\r\n");
    return false;
  }

  ConfigBlob blob;
  blob.magic   = FLOW_CFG_MAGIC;
  blob.version = FLOW_CFG_VERSION;
  blob.size    = (uint16_t)sizeof(RuntimeConfig);
  blob.cfg     = g_cfg;

  size_t written = prefs.putBytes(FLOW_CFG_KEY, &blob, sizeof(blob));
  prefs.end();

  if (written != sizeof(blob)) {
    FLOG("[cfg] NVS write short (%u/%u)\r\n", (unsigned)written, (unsigned)sizeof(blob));
    return false;
  }
  g_from_nvs = true;
  FLOG("[cfg] saved %u bytes\r\n", (unsigned)written);
  return true;
}

bool settingsReset(void)
{
  settingsDefaults(g_cfg);
  settingsApplyAll();

  Preferences prefs;
  if (!prefs.begin(FLOW_CFG_NAMESPACE, /*readOnly=*/false)) {
    /* Nothing stored to clear - RAM is already back to defaults, so the
     * command did what it promised. */
    g_from_nvs = false;
    return true;
  }
  bool ok = prefs.clear();
  prefs.end();
  g_from_nvs = false;
  FLOG("[cfg] reset to defaults (nvs clear %s)\r\n", ok ? "ok" : "FAILED");
  return ok;
}

/* ================================================================== */
/* Apply                                                               */
/* ================================================================== */

/*
 * The first two callees only set a flag. The LEDs repaint on the next
 * ledsUpdate(); the Mono panel re-sends rotation/brightness from
 * monoService(), one Chain transaction per loop like every other panel write.
 *
 * keysApplySwap() is the exception: it re-assigns two GPIOs and re-seeds the
 * debounce state, which is the whole point of swap_keys being live. It is
 * idempotent - it returns immediately when the assignment already matches - so
 * calling it after EVERY `set` cannot disturb a key that is being held while
 * some unrelated colour is changed.
 */
void settingsApplyAll(void)
{
  keysApplySwap();
  ledsConfigChanged();
  monoConfigChanged();
}
