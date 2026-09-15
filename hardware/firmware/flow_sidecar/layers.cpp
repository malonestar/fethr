/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/src/layers.cpp
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * layers.cpp - the layer table (actions as data) and the function -> appearance
 * maps that let the LEDs and the Mono panel describe a binding without knowing
 * anything about keycodes.
 *
 * Fields are positional - keep the comments aligned when editing.
 * Action literal layout: { type, code, mods, fn }
 *
 * The COLOURS in this file (both LAYERS[].r/g/b and FN_RGB) are DEFAULTS.
 * settings.cpp copies them into g_cfg at boot and fnColor() reads g_cfg, so
 * the host can repaint anything at run time without a reflash. The bindings
 * themselves - which key does what - remain compile-time data.
 *
 * SCOPE: FLOW is the only layer built by default. MEDIA/EDIT/MOUSE are
 * complete but have not been exercised on hardware, so they sit behind
 * FLOW_EXTRA_LAYERS (config.h). Everything that walks the table reads
 * LAYER_COUNT, so turning the flag on is the whole change.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

const LayerConfig LAYERS[] = {
    /* =============== Layer 0: FLOW (default) =============== */
    {"FLOW",
     0, 90, 255, /* blue */
     G_F,
     {ACT_KEY_HOLD, KEY_F8, MOD_NONE, FN_DICT_RAW},   /* Key1 hold -> raw dictation     */
     {ACT_KEY_HOLD, KEY_F9, MOD_NONE, FN_DICT_CLEAN}, /* Key2 hold -> cleaned dictation */
     {ACT_KEY_TAP, KEY_F7, MOD_NONE, FN_REPASTE},     /* ChainKey tap -> re-paste       */
     {ACT_CHORD_TAP, 'z', MOD_CTRL, FN_UNDO},         /* ChainKey double -> undo paste  */
     NAV_ARROWS,
     {ACT_KEY_TAP, KEY_RETURN, MOD_NONE, FN_ENTER},
     SCROLL_WHEEL_PAN,
     {ACT_MOUSE_BTN, MOUSE_MIDDLE, MOD_NONE, FN_MOUSE_M},
     ANGLE_VOLUME},

#if FLOW_EXTRA_LAYERS
    /* =============== Layer 1: MEDIA =============== */
    {"MEDIA",
     255, 110, 0, /* amber */
     G_M,
     {ACT_CONSUMER_TAP, CONSUMER_CONTROL_PLAY_PAUSE, MOD_NONE, FN_MEDIA},
     {ACT_CONSUMER_TAP, CONSUMER_CONTROL_SCAN_NEXT, MOD_NONE, FN_MEDIA},
     {ACT_CONSUMER_TAP, CONSUMER_CONTROL_MUTE, MOD_NONE, FN_MEDIA},
     {ACT_CONSUMER_TAP, CONSUMER_CONTROL_SCAN_PREVIOUS, MOD_NONE, FN_MEDIA}, /* double */
     NAV_ARROWS,
     {ACT_KEY_TAP, KEY_RETURN, MOD_NONE, FN_ENTER},
     SCROLL_WHEEL_PAN,
     {ACT_MOUSE_BTN, MOUSE_MIDDLE, MOD_NONE, FN_MOUSE_M},
     ANGLE_VOLUME},

    /* =============== Layer 2: EDIT =============== */
    {"EDIT",
     0, 255, 110, /* green */
     G_E,
     {ACT_CHORD_TAP, 'z', MOD_CTRL, FN_UNDO}, /* Key1 tap -> Ctrl+Z */
     {ACT_CHORD_TAP, 'y', MOD_CTRL, FN_REDO}, /* Key2 tap -> Ctrl+Y */
     {ACT_KEY_TAP, KEY_RETURN, MOD_NONE, FN_ENTER},
     {ACT_CHORD_TAP, 'z', MOD_CTRL | MOD_SHIFT, FN_REDO}, /* double -> Ctrl+Shift+Z */
     NAV_ARROWS,
     {ACT_KEY_TAP, KEY_RETURN, MOD_NONE, FN_ENTER},
     SCROLL_WHEEL_PAN,
     {ACT_MOUSE_BTN, MOUSE_MIDDLE, MOD_NONE, FN_MOUSE_M},
     ANGLE_WHEEL},

    /* =============== Layer 3: MOUSE (v2) =============== */
    /* Nav stick drives the cursor with an acceleration curve; the scroll stick
     * keeps its wheel/pan role; Key1 is a true button hold so drag works. */
    {"MOUSE",
     255, 0, 160, /* magenta - deliberately unlike the other three */
     G_P,
     {ACT_MOUSE_HOLD, MOUSE_LEFT, MOD_NONE, FN_MOUSE_L},  /* Key1: hold = drag  */
     {ACT_MOUSE_BTN, MOUSE_RIGHT, MOD_NONE, FN_MOUSE_R},  /* Key2: right click  */
     {ACT_MOUSE_BTN, MOUSE_MIDDLE, MOD_NONE, FN_MOUSE_M}, /* ChainKey: middle   */
     {ACT_MOUSE_DOUBLE, MOUSE_LEFT, MOD_NONE, FN_MOUSE_L},/* double -> dbl-click*/
     NAV_MOUSE,
     {ACT_MOUSE_BTN, MOUSE_LEFT, MOD_NONE, FN_MOUSE_L},
     SCROLL_WHEEL_PAN,
     {ACT_MOUSE_BTN, MOUSE_MIDDLE, MOD_NONE, FN_MOUSE_M},
     ANGLE_WHEEL},
#endif /* FLOW_EXTRA_LAYERS */
};

const uint8_t LAYER_COUNT = (uint8_t)(sizeof(LAYERS) / sizeof(LAYERS[0]));

/* RuntimeConfig::layer_rgb is sized by FLOW_MAX_LAYERS and is part of the NVS
 * blob layout, so growing the table above without bumping both that constant
 * and FLOW_CFG_VERSION would silently drop a layer's colour. */
static_assert(sizeof(LAYERS) / sizeof(LAYERS[0]) <= FLOW_MAX_LAYERS,
              "LAYER_COUNT exceeds FLOW_MAX_LAYERS - bump it and FLOW_CFG_VERSION");

/* ================================================================== */
/* Layer -> colour                                                     */
/* ================================================================== */

/* Moved here from leds.cpp in v2.1: it is now a pure lookup in the runtime
 * colour table, which this file owns, and the Mono/node-LED code needs it
 * whether or not the local WS2812s are compiled in. */
void layerRgb(uint8_t layer, uint8_t out_rgb[3])
{
  if (layer >= LAYER_COUNT) layer = 0;
  out_rgb[0] = g_cfg.layer_rgb[layer][0];
  out_rgb[1] = g_cfg.layer_rgb[layer][1];
  out_rgb[2] = g_cfg.layer_rgb[layer][2];
}

/* ================================================================== */
/* Function -> colour                                                  */
/* ================================================================== */

/* Index order MUST match enum FnId. FN_NONE is handled separately (it means
 * "no opinion", i.e. fall back to the layer colour). */
static const uint8_t FN_RGB[FN_COUNT][3] = {
    {0, 0, 0},       /* FN_NONE       - unused, see fnColor() */
    {0, 90, 255},    /* FN_DICT_RAW   - blue        */
    {170, 0, 255},   /* FN_DICT_CLEAN - violet      */
    {0, 200, 200},   /* FN_REPASTE    - cyan        */
    {255, 110, 0},   /* FN_MEDIA      - amber       */
    {0, 255, 90},    /* FN_UNDO       - green       */
    {0, 255, 180},   /* FN_REDO       - green-teal  */
    {255, 255, 255}, /* FN_MOUSE_L    - white       */
    {255, 255, 255}, /* FN_MOUSE_R    - white       */
    {255, 255, 255}, /* FN_MOUSE_M    - white       */
    {255, 255, 120}, /* FN_ENTER      - pale yellow */
};

/* The compile-time table, for settings.cpp to seed g_cfg.fn_rgb from. */
void fnDefaultRgb(uint8_t fn, uint8_t out_rgb[3])
{
  if (fn >= (uint8_t)FN_COUNT) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
    return;
  }
  out_rgb[0] = FN_RGB[fn][0];
  out_rgb[1] = FN_RGB[fn][1];
  out_rgb[2] = FN_RGB[fn][2];
}

/* The RUNTIME lookup - everything that paints an LED comes through here. */
void fnColor(uint8_t fn, uint8_t layer, uint8_t out_rgb[3])
{
  if (fn == FN_NONE || fn >= (uint8_t)FN_COUNT) {
    layerRgb(layer, out_rgb);
    return;
  }
  out_rgb[0] = g_cfg.fn_rgb[fn][0];
  out_rgb[1] = g_cfg.fn_rgb[fn][1];
  out_rgb[2] = g_cfg.fn_rgb[fn][2];
}

/* ================================================================== */
/* Function -> protocol name                                           */
/* ================================================================== */

/* Index order MUST match enum FnId. These strings are WIRE FORMAT: they are
 * the keys of the `fn_rgb` object and the `fn` field of a `hold` event, so a
 * rename breaks the host app. FN_NONE deliberately has no name - it means
 * "no opinion", which is not something the host can set a colour for. */
static const char *const FN_NAME[FN_COUNT] = {
    NULL,          /* FN_NONE       */
    "DICT_RAW",    /* FN_DICT_RAW   */
    "DICT_CLEAN",  /* FN_DICT_CLEAN */
    "REPASTE",     /* FN_REPASTE    */
    "MEDIA",       /* FN_MEDIA      */
    "UNDO",        /* FN_UNDO       */
    "REDO",        /* FN_REDO       */
    "MOUSE_L",     /* FN_MOUSE_L    */
    "MOUSE_R",     /* FN_MOUSE_R    */
    "MOUSE_M",     /* FN_MOUSE_M    */
    "ENTER",       /* FN_ENTER      */
};

const char *fnName(uint8_t fn)
{
  if (fn >= (uint8_t)FN_COUNT) return NULL;
  return FN_NAME[fn];
}

uint8_t fnByName(const char *name)
{
  if (name == NULL) return FN_NONE;
  for (uint8_t f = 1; f < (uint8_t)FN_COUNT; f++) {
    if (FN_NAME[f] != NULL && strcmp(FN_NAME[f], name) == 0) return f;
  }
  return FN_NONE;
}

/* ================================================================== */
/* Function -> hold glyph                                              */
/* ================================================================== */

uint8_t fnGlyph(uint8_t fn)
{
  switch (fn) {
    case FN_DICT_RAW:   return G_MIC;
    case FN_DICT_CLEAN: return G_MIC_CLEAN;
    case FN_MOUSE_L:
    case FN_MOUSE_R:
    case FN_MOUSE_M:    return G_POINTER;
    default:            return G_HOLD;
  }
}

/* ================================================================== */
/* Function -> human label, and the companion's key legend (0.3.0)     */
/* ================================================================== */

/*
 * The companion display's idle screen is a legend for the current layer, and
 * these strings are the whole of it: they are built here, from the same table
 * the keys actually fire from, and shipped over the link. The screen therefore
 * cannot drift from the bindings - there is no second copy to forget.
 *
 * They are NOT wire-format identifiers. `fn_rgb` keys and the `fn` field of a
 * host `hold` event are still FN_NAME above; these are prose for a human
 * reading a 128x128 panel, and renaming one breaks nothing but the label.
 */

/* Index order MUST match enum FnId. FN_NONE's entry is the placeholder an
 * unmapped control shows, so unlike FN_NAME it is not NULL. */
static const char *const FN_LABEL[FN_COUNT] = {
    "-",            /* FN_NONE       */
    "dictate",      /* FN_DICT_RAW   */
    "clean",        /* FN_DICT_CLEAN */
    "repaste",      /* FN_REPASTE    */
    "media",        /* FN_MEDIA      */
    "undo",         /* FN_UNDO       */
    "redo",         /* FN_REDO       */
    "left click",   /* FN_MOUSE_L    */
    "right click",  /* FN_MOUSE_R    */
    "middle click", /* FN_MOUSE_M    */
    "enter",        /* FN_ENTER      */
};

/*
 * Keyed on the ACTION, not on its FnId, because one FnId can cover several
 * different bindings: every control on the MEDIA layer carries FN_MEDIA, and
 * "play/pause" and "mute" are not the same legend. The FnId is only the
 * fallback, which is exactly right for the keyboard actions - those are the
 * ones whose meaning the FnId already names.
 */
const char *actionLabel(const Action &a)
{
  switch (a.type) {
    case ACT_NONE:
      return "-";

    case ACT_CONSUMER_TAP:
      switch (a.code) {
        case CONSUMER_CONTROL_PLAY_PAUSE:       return "play/pause";
        case CONSUMER_CONTROL_SCAN_NEXT:        return "next track";
        case CONSUMER_CONTROL_SCAN_PREVIOUS:    return "prev track";
        case CONSUMER_CONTROL_MUTE:             return "mute";
        case CONSUMER_CONTROL_VOLUME_INCREMENT: return "vol up";
        case CONSUMER_CONTROL_VOLUME_DECREMENT: return "vol down";
        default:                                break;
      }
      break;

    case ACT_MOUSE_HOLD:
      return "drag";

    case ACT_MOUSE_DOUBLE:
      return "double click";

    case ACT_MOUSE_BTN:
      switch (a.code) {
        case MOUSE_LEFT:   return "left click";
        case MOUSE_RIGHT:  return "right click";
        case MOUSE_MIDDLE: return "middle click";
        default:           break;
      }
      break;

    default:
      break;
  }

  if (a.fn < (uint8_t)FN_COUNT) return FN_LABEL[a.fn];
  return "-";
}

/* "K1 hold: dictate" / "K3: repaste". The gesture is only spelled out for a
 * hold, because a tap is the default and saying so costs four characters the
 * 16-character budget does not have. */
static void legendKeyRow(const Action &a, const char *key, char *out, size_t n)
{
  if (a.type == ACT_NONE) {
    snprintf(out, n, "%s: -", key);
    return;
  }
  if (actionIsHold(a)) {
    snprintf(out, n, "%s hold: %s", key, actionLabel(a));
  } else {
    snprintf(out, n, "%s: %s", key, actionLabel(a));
  }
}

static const char *navRoleLabel(uint8_t mode)
{
  switch (mode) {
    case NAV_ARROWS: return "arrows";
    case NAV_MOUSE:  return "cursor";
    default:         return NULL;
  }
}

static const char *angleRoleLabel(uint8_t mode)
{
  switch (mode) {
    case ANGLE_VOLUME: return "vol";
    case ANGLE_WHEEL:  return "wheel";
    default:           return NULL;
  }
}

/* Slot 3: what the nav stick and the angle knob do, as "stick role / knob
 * role". Longest form is "stick cursor / wheel" - 20 characters, which is both
 * COMPANION_LEGEND_MAX and what the companion's 6-pixel font fits across the
 * panel. A control that does nothing on this layer drops out of the line
 * entirely rather than printing a dash nobody can decode. */
static void legendFooter(const LayerConfig &L, char *out, size_t n)
{
  const char *nav   = navRoleLabel(L.nav_mode);
  const char *angle = angleRoleLabel(L.angle_mode);

  if (nav != NULL && angle != NULL) {
    snprintf(out, n, "stick %s / %s", nav, angle);
  } else if (nav != NULL) {
    snprintf(out, n, "stick %s", nav);
  } else if (angle != NULL) {
    snprintf(out, n, "knob %s", angle);
  } else {
    out[0] = '\0';
  }
}

void layerLegend(uint8_t layer, uint8_t slot, char *out, size_t out_len)
{
  if (out == NULL || out_len == 0) return;
  out[0] = '\0';
  if (layer >= LAYER_COUNT) return;

  const LayerConfig &L = LAYERS[layer];
  switch (slot) {
    case 0: legendKeyRow(L.key1, "K1", out, out_len); break;
    case 1: legendKeyRow(L.key2, "K2", out, out_len); break;
    case 2: legendKeyRow(L.chain_key, "K3", out, out_len); break;
    case 3: legendFooter(L, out, out_len); break;
    default: break;
  }
}
