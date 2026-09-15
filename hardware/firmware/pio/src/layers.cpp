/*
 * layers.cpp - the layer table (actions as data) and the function -> appearance
 * maps that let the LEDs and the Mono panel describe a binding without knowing
 * anything about keycodes.
 *
 * Fields are positional - keep the comments aligned when editing.
 * Action literal layout: { type, code, mods, fn }
 *
 * The COLOURS in this file (both DEFAULT_LAYERS[].r/g/b and FN_RGB) are
 * DEFAULTS. settings.cpp copies them into g_cfg at boot and fnColor() reads
 * g_cfg, so the host can repaint anything at run time without a reflash.
 * Since 0.2.0 the BINDINGS work the same way - see below.
 *
 * 0.2.0: THE TABLE BELOW IS NO LONGER WHAT THE FIRMWARE READS.
 * It is `DEFAULT_LAYERS`, the seed. settings.cpp copies it into
 * g_cfg.layers[] at boot, `set_action` / `set_layer_meta` edit that copy, and
 * the NVS blob persists it. Every reader goes through layerAt() / layerCount()
 * so there is exactly one runtime source of truth; the compile-time table only
 * decides what a factory-default device comes up with.
 *
 * SCOPE: all four defaults are always compiled. FLOW_EXTRA_LAYERS (config.h)
 * says how many of them a factory-default device seeds - 4 for the shipped
 * image, 1 for the original FLOW-only build.
 */

#include "settings.h"

#include "keynames.h"

#include <stdio.h>
#include <string.h>

const LayerConfig DEFAULT_LAYERS[] = {
    /* =============== Layer 0: FLOW (default) =============== */
    {"FETHR",
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
};

const uint8_t DEFAULT_LAYER_COUNT =
    (uint8_t)(sizeof(DEFAULT_LAYERS) / sizeof(DEFAULT_LAYERS[0]));

/* RuntimeConfig::layers and ::layer_rgb are both sized by FLOW_MAX_LAYERS and
 * are part of the NVS blob layout, so growing the table above without bumping
 * that constant and FLOW_CFG_VERSION would silently drop a layer. */
static_assert(sizeof(DEFAULT_LAYERS) / sizeof(DEFAULT_LAYERS[0]) <= FLOW_MAX_LAYERS,
              "DEFAULT_LAYER_COUNT exceeds FLOW_MAX_LAYERS - bump it and FLOW_CFG_VERSION");

/* FLOW_EXTRA_LAYERS changed meaning in 0.2.0: it is a COUNT now, not a
 * boolean. The old `0` would have seeded an empty table, so fail the build
 * rather than let it through. */
static_assert(FLOW_EXTRA_LAYERS >= 1 && FLOW_EXTRA_LAYERS <= FLOW_MAX_LAYERS,
              "FLOW_EXTRA_LAYERS is now a layer COUNT (1..FLOW_MAX_LAYERS), not a flag");

/* Every layer name must survive the copy into LayerRuntime::name, which is the
 * protocol's 8-character limit. Checked at compile time so a long default can
 * never be silently truncated on the way into NVS. */
static_assert(sizeof("FETHR") <= FLOW_LAYER_NAME_MAX + 1 &&
                  sizeof("MEDIA") <= FLOW_LAYER_NAME_MAX + 1 &&
                  sizeof("EDIT") <= FLOW_LAYER_NAME_MAX + 1 &&
                  sizeof("MOUSE") <= FLOW_LAYER_NAME_MAX + 1,
              "a default layer name is longer than FLOW_LAYER_NAME_MAX");

/* ================================================================== */
/* The runtime table                                                   */
/* ================================================================== */

/*
 * layerAt() is THE accessor: every `LAYERS[x]` in the firmware became one of
 * these. Clamping here rather than at ~40 call sites is what lets a caller
 * pass g_layer without a bounds check, and it is also why g_cfg being
 * zero-initialised before settingsBegin() is harmless - index 0 is always
 * inside the array, it is just blank.
 */
const LayerRuntime &layerAt(uint8_t index)
{
  if (index >= g_cfg.layer_count) index = 0;
  return g_cfg.layers[index];
}

uint8_t layerCount(void) { return g_cfg.layer_count; }

void layerDefaultInto(uint8_t index, LayerRuntime &out)
{
  memset(&out, 0, sizeof(out));
  if (index >= DEFAULT_LAYER_COUNT) return;

  const LayerConfig &L = DEFAULT_LAYERS[index];
  snprintf(out.name, sizeof(out.name), "%s", L.name);
  out.glyph            = L.glyph;
  out.key1             = L.key1;
  out.key2             = L.key2;
  out.chain_key        = L.chain_key;
  out.chain_key_double = L.chain_key_double;
  out.nav_click        = L.nav_click;
  out.scroll_click     = L.scroll_click;
  out.nav_mode         = L.nav_mode;
  out.scroll_mode      = L.scroll_mode;
  out.angle_mode       = L.angle_mode;
}

void layerDefaultRgb(uint8_t index, uint8_t out_rgb[3])
{
  if (index >= DEFAULT_LAYER_COUNT) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
    return;
  }
  out_rgb[0] = DEFAULT_LAYERS[index].r;
  out_rgb[1] = DEFAULT_LAYERS[index].g;
  out_rgb[2] = DEFAULT_LAYERS[index].b;
}

/* ================================================================== */
/* Layer -> colour                                                     */
/* ================================================================== */

/* Moved here from leds.cpp in v2.1: it is now a pure lookup in the runtime
 * colour table, which this file owns, and the Mono/node-LED code needs it
 * whether or not the local WS2812s are compiled in. */
void layerRgb(uint8_t layer, uint8_t out_rgb[3])
{
  if (layer >= g_cfg.layer_count) layer = 0;
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
/* Function -> proto-2 class name                                      */
/* ================================================================== */

/*
 * The lower-case spelling `get_layers` / `set_action` use. Index order MUST
 * match enum FnId. FN_NONE is "custom" here rather than absent: an editable
 * action needs a way to say "no opinion about the colour", and the protocol's
 * word for that is `custom`.
 */
static const char *const FN_CLASS[FN_COUNT] = {
    "custom",     /* FN_NONE       */
    "dict_raw",   /* FN_DICT_RAW   */
    "dict_clean", /* FN_DICT_CLEAN */
    "repaste",    /* FN_REPASTE    */
    "media",      /* FN_MEDIA      */
    "undo",       /* FN_UNDO       */
    "redo",       /* FN_REDO       */
    "mouse_l",    /* FN_MOUSE_L    */
    "mouse_r",    /* FN_MOUSE_R    */
    "mouse_m",    /* FN_MOUSE_M    */
    "enter",      /* FN_ENTER      */
};

const char *fnClassName(uint8_t fn)
{
  if (fn >= (uint8_t)FN_COUNT) return FN_CLASS[FN_NONE];
  return FN_CLASS[fn];
}

/* Case-insensitive, which also means the upper-case v1 spelling ("DICT_RAW")
 * is accepted here - the two vocabularies differ only in case. */
static bool classEq(const char *a, const char *b)
{
  if (a == NULL || b == NULL) return false;
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
    if (ca != cb) return false;
  }
  return *a == *b;
}

bool fnClassByName(const char *name, uint8_t *out)
{
  if (name == NULL) return false;
  for (uint8_t f = 0; f < (uint8_t)FN_COUNT; f++) {
    if (classEq(FN_CLASS[f], name)) {
      if (out != NULL) *out = f;
      return true;
    }
  }
  return false;
}

/* ================================================================== */
/* Per-layer mode names (wire format)                                  */
/* ================================================================== */

const char *navModeName(uint8_t mode)
{
  switch (mode) {
    case NAV_ARROWS: return "arrows";
    case NAV_MOUSE:  return "mouse";
    default:         return "off";
  }
}

const char *scrollModeName(uint8_t mode)
{
  switch (mode) {
    case SCROLL_WHEEL_PAN:    return "wheel_pan";
    case SCROLL_WHEEL_ARROWS: return "wheel_arrows";
    default:                  return "off";
  }
}

const char *angleModeName(uint8_t mode)
{
  switch (mode) {
    case ANGLE_VOLUME: return "volume";
    case ANGLE_WHEEL:  return "wheel";
    default:           return "off";
  }
}

bool navModeByName(const char *name, uint8_t *out)
{
  uint8_t v;
  if (classEq(name, "off")) v = NAV_OFF;
  else if (classEq(name, "arrows")) v = NAV_ARROWS;
  else if (classEq(name, "mouse")) v = NAV_MOUSE;
  else return false;
  if (out != NULL) *out = v;
  return true;
}

bool scrollModeByName(const char *name, uint8_t *out)
{
  uint8_t v;
  if (classEq(name, "off")) v = SCROLL_OFF;
  else if (classEq(name, "wheel_pan")) v = SCROLL_WHEEL_PAN;
  else if (classEq(name, "wheel_arrows")) v = SCROLL_WHEEL_ARROWS;
  else return false;
  if (out != NULL) *out = v;
  return true;
}

bool angleModeByName(const char *name, uint8_t *out)
{
  uint8_t v;
  if (classEq(name, "off")) v = ANGLE_OFF;
  else if (classEq(name, "volume")) v = ANGLE_VOLUME;
  else if (classEq(name, "wheel")) v = ANGLE_WHEEL;
  else return false;
  if (out != NULL) *out = v;
  return true;
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

  /*
   * fn = FN_NONE on a BOUND action is the proto-2 `custom` class, and
   * PROTOCOL.md says its legend shows the key name - which is the only thing
   * left that describes it. FN_LABEL's "-" is still right for ACT_NONE, which
   * returned above.
   *
   * The name has to be built, so it lands in a file-static scratch buffer.
   * Safe because every caller - legendKeyRow() and companionEventTap() -
   * formats the result into its own storage before the next call; nothing
   * holds an actionLabel() pointer across one.
   */
  if (a.fn == FN_NONE) {
    static char scratch[FLOW_KEYNAME_MAX + 1];
    if (actionToKeyName(a, scratch, sizeof(scratch)) && scratch[0] != '\0') return scratch;
    return "-";
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
static void legendFooter(const LayerRuntime &L, char *out, size_t n)
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
  if (layer >= layerCount()) return;

  const LayerRuntime &L = layerAt(layer);
  switch (slot) {
    case 0: legendKeyRow(L.key1, "K1", out, out_len); break;
    case 1: legendKeyRow(L.key2, "K2", out, out_len); break;
    case 2: legendKeyRow(L.chain_key, "K3", out, out_len); break;
    case 3: legendFooter(L, out, out_len); break;
    default: break;
  }
}
