/*
 * settings.h - the mutable runtime configuration (v2.1).
 *
 * WHAT CHANGED IN v2.1
 *   Up to v2.0 every colour, percentage, timing and sign was a #define read
 *   straight out of config.h at the point of use. v2.1 keeps those #defines as
 *   the DEFAULTS and copies them once, at boot, into the single mutable
 *   `g_cfg` below. Every consumer (leds.cpp, mono.cpp, chain.cpp, layers.cpp)
 *   now reads g_cfg, so the host settings protocol in PROTOCOL.md can change
 *   any of them live.
 *
 * SCHEDULER RULES STILL APPLY
 *   Nothing in here talks to the Chain bus or the LEDs directly. A `set` marks
 *   the affected subsystem dirty (settingsApplyAll) and the normal
 *   one-transaction-per-loop services pick the change up. No delay(), no
 *   inline bus traffic.
 *
 * PERSISTENCE
 *   One versioned binary blob in NVS namespace "fethr", key "cfg", written by
 *   the `save` command only - never automatically, because NVS writes are slow
 *   and a chatty host could otherwise wear the flash out. The blob carries a
 *   magic + version + sizeof header, so ANY layout change here (new field,
 *   reordered field, FLOW_MAX_LAYERS bump, FN_COUNT bump) makes an old blob
 *   fail validation and the firmware falls back to defaults instead of
 *   reinterpreting stale bytes.
 */

#ifndef FLOW_SETTINGS_H
#define FLOW_SETTINGS_H

#include <Arduino.h>

#include "config.h"

/* ================================================================== */
/* NVS blob identity                                                   */
/* ================================================================== */

#define FLOW_CFG_NAMESPACE "fethr"
#define FLOW_CFG_KEY       "cfg"

/* 'F','S','C','1' - a sanity check against a foreign blob in the same key. */
#define FLOW_CFG_MAGIC 0x46534331UL

/* BUMP THIS whenever RuntimeConfig's layout changes. */
#define FLOW_CFG_VERSION 1

/* ================================================================== */
/* The configuration itself                                            */
/* ================================================================== */

/*
 * Field order mirrors the config object in PROTOCOL.md so the two can be read
 * side by side. Plain integer members only: the struct is memcpy'd in and out
 * of NVS as a single blob, so it must stay trivially copyable, and it must
 * contain no pointers.
 */
struct RuntimeConfig {
  /* --- colours --- */
  uint8_t layer_rgb[FLOW_MAX_LAYERS][3]; /* per-layer colour              */
  uint8_t fn_rgb[FN_COUNT][3];           /* per-function colour           */
  uint8_t hold_rgb[3];                   /* key colour while a hold runs  */

  /* --- DualKey LEDs --- */
  uint8_t  led_idle_pct;   /* 0..100, idle brightness of the fn colour */
  uint16_t led_flash_ms;   /* 0..2000, tap flash duration              */
  uint8_t  node_leds;      /* bool: drive the chained nodes' own LEDs  */
  uint8_t  led_index_key1; /* 0|1: which NeoPixel sits under Key1      */

  /* --- Mono panel --- */
  uint8_t  mono_brightness; /* 0..7                                     */
  uint16_t mono_rotation;   /* 0|90|180|270 degrees                     */
  uint8_t  mono_idle;       /* MONO_IDLE_BLANK | MONO_IDLE_LETTER       */

  /* --- Chain Key --- */
  uint8_t  double_tap;    /* bool: double-tap actions live            */
  uint16_t double_tap_ms; /* 50..2000, double-tap window              */
  uint16_t layer_hold_ms; /* 200..5000, hold-to-cycle-layer threshold */

  /* --- startup --- */
  uint8_t boot_layer; /* 0..LAYER_COUNT-1 */

  /* --- axis orientation, -1 or +1 --- */
  int8_t nav_x_sign;
  int8_t nav_y_sign;
  int8_t scroll_x_sign;
  int8_t scroll_y_sign;
  int8_t mouse_y_sign; /* USER inversion, applied on top of MOUSE_Y_SIGN */
};

/* The one instance. Zero-initialised before settingsBegin() runs; nothing
 * reads it before setup() has called that. */
extern RuntimeConfig g_cfg;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Fill `c` from the compile-time defaults in config.h / layers.cpp. */
void settingsDefaults(RuntimeConfig &c);

/* Defaults, then overlay a valid NVS blob if one exists. Call FIRST in
 * setup(), before ledsBegin()/chainBegin(), so those come up in the saved
 * colours rather than flashing the defaults for a frame. */
void settingsBegin(void);

/* Persist g_cfg. Returns false if NVS refused the write. */
bool settingsSave(void);

/* Factory defaults in RAM *and* NVS, applied live. */
bool settingsReset(void);

/* Tell every subsystem its cached view of g_cfg is stale. Cheap: sets flags,
 * issues no bus traffic. Call after any mutation of g_cfg. */
void settingsApplyAll(void);

/* True if the last settingsBegin() found and accepted a stored blob. */
bool settingsLoadedFromNvs(void);

#endif /* FLOW_SETTINGS_H */
