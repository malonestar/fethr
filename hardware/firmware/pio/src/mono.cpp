/*
 * mono.cpp - the Chain Mono 8x8 indicator.
 *
 * The panel is a strictly prioritised, single-writer state machine. What should
 * be on screen is reduced to a tiny (kind, arg) pair; the panel is only written
 * when that pair changes, and never more than once per loop() iteration.
 *
 * Priority, highest first (this is the order the spec asks for):
 *   1. hold glyph          a DualKey hold action is active
 *   2. timed overlay       layer letter / checkmark / host cross
 *   3. host spinner        the host reports transcribing or cleaning
 *   4. volume bar          within MONO_VOLBAR_MS of a knob move on an
 *                          ANGLE_VOLUME layer
 *   5. nav arrow           while the nav stick is deflected
 *   6. scroll animation    while the scroll stick is emitting
 *   7. idle                g_cfg.mono_idle: blank, or the layer's letter
 *
 * A physical hold outranks anything the host asks for, deliberately: what the
 * user is doing with their finger right now beats a report about what happened
 * a moment ago.
 *
 * The text scroll is a separate short scripted sequence (mode -> string ->
 * wait -> mode back) that runs one Chain transaction per call, like everything
 * else, and yields to a hold glyph if one starts mid-scroll. v2.1 drives it
 * from a copied buffer so the `mono` and `identify` commands can scroll
 * arbitrary text, not just a layer name from the static table.
 *
 * IDLE = LAYER LETTER (v2.1 default)
 *   The Chain Mono has ONE global brightness level for the whole panel - there
 *   is no per-pixel intensity - so "the letter, dimmer than a notification"
 *   is not expressible. The two options were a dotted every-other-pixel
 *   stipple of the letter, or the plain letter. The plain letter won: the
 *   glyphs are 5x7 strokes one pixel wide, and stippling them leaves a
 *   dashed, hard-to-read shape rather than a dimmer one. So the idle letter
 *   is drawn exactly like the post-layer-change letter; what distinguishes
 *   idle is simply that it persists. `mono_idle: "blank"` restores v2.0.
 */

#include "sidecar.h"

#include <string.h>

/* ---- what the panel is asked to show ---- */
enum MonoKind : uint8_t {
  MV_BLANK = 0,
  MV_GLYPH,  /* arg = GlyphId                         */
  MV_VOLBAR, /* arg = 0..8 filled columns             */
  MV_ARROW,  /* arg = StickDir                        */
  MV_SCROLL, /* arg = frame 0..3 | 0x80 if travelling up */
  MV_SPIN    /* arg = frame 0..3                      */
};

struct MonoView {
  uint8_t kind;
  uint8_t arg;
};

static inline bool sameView(const MonoView &a, const MonoView &b)
{
  return a.kind == b.kind && a.arg == b.arg;
}

/* 0xFF/0xFF is not a reachable view, so the first service() always paints. */
static MonoView g_shown    = {0xFF, 0xFF};
static uint32_t g_retry_at = 0;

/* ---- timed overlay ---- */
static uint8_t  g_overlay_glyph = G_BLANK;
static uint32_t g_overlay_until = 0;

/* ---- hints from the input handlers ---- */
static uint8_t  g_nav_dir      = DIR_NONE;
static bool     g_scroll_up    = false;
static uint32_t g_scroll_t0    = 0;
static uint32_t g_scroll_until = 0;
static uint8_t  g_vol_level    = 0;
static uint32_t g_vol_until    = 0;

/* ---- text scroll sequence ---- */
enum NameScrollState : uint8_t {
  NS_IDLE = 0,
  NS_SET_MODE,
  NS_SET_STRING,
  NS_WAIT,
  NS_RESTORE
};

static uint8_t  g_ns_state = NS_IDLE;
static uint32_t g_ns_until = 0;
static uint32_t g_ns_show_ms = MONO_NAME_SCROLL_SHOW_MS;
static bool     g_ns_announce_layer = false;

/* The text is COPIED, not pointed at: `mono` scrolls a host-supplied string
 * that lives in the protocol's line buffer and is gone by the next loop(). */
static char g_ns_text[FLOW_PROTO_MONO_TEXT + 1] = {0};

/* ---- deferred config apply (v2.1) ----
 * Rotation and brightness are node commands, so a `set` cannot push them
 * inline without breaking the one-transaction-per-loop rule. They are queued
 * here and drained by monoService(), one per call, ahead of the view logic. */
static bool g_cfg_rotation_due   = false;
static bool g_cfg_brightness_due = false;

/* ================================================================== */
/* The only place that talks to the Mono panel's drawing API           */
/* ================================================================== */

/*
 * Default path: setMonoBufferRefresh() - one transaction for all 64 pixels.
 * Verified in M5Chain/src/ChainMono/ChainMono.hpp:
 *   chain_status_t setMonoBufferRefresh(uint8_t id, uint8_t (&buffer)[8],
 *                                       uint8_t *operationStatus,
 *                                       unsigned long timeout = 100);
 * The buffer parameter is a reference to a real uint8_t[8], so a const glyph
 * has to be copied into a local array first.
 *
 * Fallback path (FLOW_MONO_USE_BUFFER_REFRESH 0): a 64-entry batch
 * setMonoPixel(), which is also a verified overload.
 */
static chain_status_t monoWriteBuffer(uint8_t id, const uint8_t *glyph, uint8_t *op)
{
#if FLOW_MONO_USE_BUFFER_REFRESH
  uint8_t buf[8];
  memcpy(buf, glyph, sizeof(buf));
  return M5Chain.setMonoBufferRefresh(id, buf, op, CHAIN_CALL_TIMEOUT_MS);
#else
  MonoPixelInfo px[64];
  uint8_t       n = 0;
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      px[n].x     = x;
      px[n].y     = y;
      px[n].state = (glyph[y] & (uint8_t)(0x80 >> x)) != 0;
      n++;
    }
  }
  /* A 64-pixel packet is ~70 bytes each way - give it the longer budget. */
  return M5Chain.setMonoPixel(id, px, n, op, CHAIN_ENUM_TIMEOUT_MS);
#endif
}

/* ================================================================== */
/* Init                                                                */
/* ================================================================== */

/* g_cfg stores plain numbers (see config.h); the node wants its own enums. */
static mono_rotation_t monoRotationEnum(uint16_t degrees)
{
  switch (degrees) {
    case 90:  return MONO_ROTATION_90;
    case 180: return MONO_ROTATION_180;
    case 270: return MONO_ROTATION_270;
    default:  return MONO_ROTATION_0;
  }
}

static mono_brightness_level_t monoBrightnessEnum(uint8_t level)
{
  if (level > 7) level = 7;
  return (mono_brightness_level_t)level;
}

/* Mode/rotation/brightness only have to be set once per (re)connection. This
 * short burst runs inside chain enumeration, never in the steady-state loop. */
void monoInit(void)
{
  uint8_t id = chainMonoId();
  if (id == 0) return;

  g_ns_state = NS_IDLE;
  g_retry_at = 0;

  /* Enumeration just pushed the current values; nothing is outstanding. */
  g_cfg_rotation_due   = false;
  g_cfg_brightness_due = false;

  uint8_t op = 0;
  if (M5Chain.setMonoMode(id, MONO_PIXEL_MODE, &op, CHAIN_ENUM_TIMEOUT_MS) != CHAIN_OK) {
    FLOG("[mono] mode set failed\r\n");
    chainMonoUnavailable(); /* treat as absent rather than retrying forever */
    return;
  }
  /* Rotation and brightness are left unsaved (CHAIN_SAVE_FLASH_DISABLE, the
   * default): a flash write takes ~20 ms and stalls the node's UART. The
   * sidecar's own NVS copy is what survives a power cycle. */
  M5Chain.setMonoRotation(id, monoRotationEnum(g_cfg.mono_rotation), &op,
                          CHAIN_SAVE_FLASH_DISABLE, CHAIN_ENUM_TIMEOUT_MS);
  M5Chain.setMonoBrightness(id, monoBrightnessEnum(g_cfg.mono_brightness), &op,
                            CHAIN_SAVE_FLASH_DISABLE, CHAIN_ENUM_TIMEOUT_MS);
  M5Chain.setMonoClear(id, &op, CHAIN_ENUM_TIMEOUT_MS);

  g_shown.kind = MV_BLANK;
  g_shown.arg  = 0;
}

void monoConfigChanged(void)
{
  g_cfg_rotation_due   = true;
  g_cfg_brightness_due = true;
  /* mono_idle may have flipped, so whatever is on the panel is no longer
   * necessarily what the view logic would choose. */
  g_shown.kind = 0xFF;
  g_shown.arg  = 0xFF;
}

/* ================================================================== */
/* Hints                                                               */
/* ================================================================== */

void monoOverlay(uint8_t glyph_id, uint32_t ms, uint32_t now)
{
  g_overlay_glyph = glyph_id;
  g_overlay_until = now + ms;
}

void monoHintNav(uint8_t dir)
{
  g_nav_dir = dir;
}

void monoHintScroll(bool up, uint32_t now)
{
  /* Keep the animation phase continuous across a run of emits; only restart it
   * when the indicator had actually expired. */
  if ((int32_t)(now - g_scroll_until) >= 0 || up != g_scroll_up) {
    g_scroll_t0 = now;
    g_scroll_up = up;
  }
  g_scroll_until = now + MONO_SCROLL_HOLD_MS;
}

void monoHintVolume(uint8_t level_0_8, uint32_t now)
{
  g_vol_level = (level_0_8 > 8) ? 8 : level_0_8;
  g_vol_until = now + MONO_VOLBAR_MS;
}

/*
 * Start a one-shot scroll of `text`.
 *
 * The show window is computed from the length rather than fixed, because the
 * `mono` command may hand over up to FLOW_PROTO_MONO_TEXT characters while a
 * layer name is four or five: MONO_TEXT_SCROLL_PX_PER_CHAR of travel per
 * character plus the 8-pixel panel width, times the per-pixel interval. A
 * layer name lands close to the original MONO_NAME_SCROLL_SHOW_MS.
 *
 * No bus traffic here - this only arms the state machine that monoService()
 * steps one transaction at a time.
 */
void monoScrollText(const char *text, bool announce_layer)
{
  if (text == NULL) return;
  if (chainMonoId() == 0) return; /* no panel: nothing to do, and no error */

  strncpy(g_ns_text, text, sizeof(g_ns_text) - 1);
  g_ns_text[sizeof(g_ns_text) - 1] = '\0';

  size_t   len = strlen(g_ns_text);
  uint32_t px  = (uint32_t)len * MONO_TEXT_SCROLL_PX_PER_CHAR + 8u;
  uint32_t ms  = px * (uint32_t)MONO_NAME_SCROLL_INTERVAL_MS;
  if (ms > (uint32_t)MONO_TEXT_SCROLL_MAX_MS) ms = MONO_TEXT_SCROLL_MAX_MS;

  g_ns_show_ms        = ms;
  g_ns_announce_layer = announce_layer;
  g_ns_state          = NS_SET_MODE;
}

void monoOnLayerChanged(uint32_t now)
{
  g_nav_dir      = DIR_NONE;
  g_scroll_until = now;
  g_vol_until    = now;

#if FLOW_MONO_SCROLL_LAYER_NAME
  if (chainMonoId() != 0) {
    /* The layer letter is raised when the panel comes back to pixel mode, so
     * the two do not fight over the screen. */
    monoScrollText(layerAt(g_layer).name, /*announce_layer=*/true);
    return;
  }
#endif
  monoOverlay(layerAt(g_layer).glyph, MONO_LAYER_MS, now);
}

/* ================================================================== */
/* View selection + rendering                                          */
/* ================================================================== */

static MonoView monoDesired(uint32_t now)
{
  MonoView v;

  for (uint8_t k = 0; k < 2; k++) {
    if (g_key[k].hold_active) {
      v.kind = MV_GLYPH;
      v.arg  = fnGlyph(g_key[k].hold_action.fn);
      return v;
    }
  }

  if ((int32_t)(now - g_overlay_until) < 0) {
    v.kind = MV_GLYPH;
    v.arg  = g_overlay_glyph;
    return v;
  }

  /* The host is working. Only the frame number is stored, so the panel is
   * written once per MONO_SPIN_FRAME_MS like any other view change - the
   * animation costs no extra bus traffic beyond its own frames. */
  uint8_t hs = hostState();
  if (hs == HOST_TRANSCRIBING || hs == HOST_CLEANING) {
    v.kind = MV_SPIN;
    v.arg  = (uint8_t)(((now - hostStateSince()) / MONO_SPIN_FRAME_MS) & 0x03);
    return v;
  }

  if ((int32_t)(now - g_vol_until) < 0) {
    v.kind = MV_VOLBAR;
    v.arg  = g_vol_level;
    return v;
  }

  if (g_nav_dir != DIR_NONE) {
    v.kind = MV_ARROW;
    v.arg  = g_nav_dir;
    return v;
  }

  if ((int32_t)(now - g_scroll_until) < 0) {
    uint8_t frame = (uint8_t)(((now - g_scroll_t0) / MONO_SCROLL_FRAME_MS) & 0x03);
    v.kind        = MV_SCROLL;
    v.arg         = (uint8_t)(frame | (g_scroll_up ? 0x80 : 0x00));
    return v;
  }

  /* Idle. See the header comment for why the letter is full brightness. */
  if (g_cfg.mono_idle == MONO_IDLE_LETTER) {
    v.kind = MV_GLYPH;
    v.arg  = layerAt(g_layer).glyph;
    return v;
  }

  v.kind = MV_BLANK;
  v.arg  = 0;
  return v;
}

static uint8_t arrowGlyphFor(uint8_t dir)
{
  switch (dir) {
    case DIR_UP:    return G_ARROW_UP;
    case DIR_DOWN:  return G_ARROW_DOWN;
    case DIR_LEFT:  return G_ARROW_LEFT;
    case DIR_RIGHT: return G_ARROW_RIGHT;
    default:        return G_BLANK;
  }
}

static void monoRender(const MonoView &v, uint8_t out[8])
{
  switch (v.kind) {
    case MV_GLYPH:  memcpy(out, glyphById(v.arg), 8); break;
    case MV_ARROW:  memcpy(out, glyphById(arrowGlyphFor(v.arg)), 8); break;
    case MV_VOLBAR: glyphVolumeBar(v.arg, out); break;
    case MV_SCROLL: glyphScrollFrame((uint8_t)(v.arg & 0x03), (v.arg & 0x80) != 0, out); break;
    case MV_SPIN:   glyphSpinnerFrame(v.arg, out); break;
    default:        memcpy(out, glyphById(G_BLANK), 8); break;
  }
}

/* ================================================================== */
/* Service                                                             */
/* ================================================================== */

/*
 * The layer-name scroll.
 *
 * VERIFIED against M5Chain 1.0.10 ChainMono.hpp:
 *   mono_mode_t        MONO_STRING_SCROLL_MODE = 0x01
 *   mono_scroll_dir_t  MONO_SCROLL_LEFT / _RIGHT / _UP / _DOWN
 *   mono_scroll_mode_t MONO_SCROLL_MODE_ONCE / _LOOP / _BOUNCE
 *   chain_status_t setMonoStringScroll(uint8_t id, const char *string,
 *                                      mono_scroll_dir_t dir,
 *                                      mono_scroll_mode_t mode,
 *                                      uint16_t IntervalMs,
 *                                      uint8_t *operationStatus,
 *                                      unsigned long timeout = 100)
 * UNVERIFIED on hardware: how long a string actually takes to traverse the
 * panel at MONO_NAME_SCROLL_INTERVAL_MS per pixel, hence the length-derived
 * g_ns_show_ms window rather than polling the scroll state (polling would
 * cost a transaction per check for no real benefit).
 *
 * Returns true when a Chain transaction was consumed.
 */
static bool nameScrollStep(uint32_t now, uint8_t id)
{
  uint8_t        op = 0;
  chain_status_t st;

  switch (g_ns_state) {
    case NS_SET_MODE:
      st = M5Chain.setMonoMode(id, MONO_STRING_SCROLL_MODE, &op, CHAIN_CALL_TIMEOUT_MS);
      chainResult(st);
      g_ns_state = (st == CHAIN_OK) ? NS_SET_STRING : NS_RESTORE;
      return true;

    case NS_SET_STRING:
      st = M5Chain.setMonoStringScroll(id, g_ns_text, MONO_SCROLL_LEFT, MONO_SCROLL_MODE_ONCE,
                                       (uint16_t)MONO_NAME_SCROLL_INTERVAL_MS, &op,
                                       CHAIN_CALL_TIMEOUT_MS);
      chainResult(st);
      g_ns_until = now + ((st == CHAIN_OK) ? g_ns_show_ms : 0u);
      g_ns_state = NS_WAIT;
      return true;

    case NS_WAIT:
      /* A dictation hold outranks the animation - cut it short. */
      if (g_key[0].hold_active || g_key[1].hold_active) g_ns_until = now;
      if ((int32_t)(now - g_ns_until) < 0) return false; /* no bus used */
      g_ns_state = NS_RESTORE;
      return false;

    case NS_RESTORE:
      st = M5Chain.setMonoMode(id, MONO_PIXEL_MODE, &op, CHAIN_CALL_TIMEOUT_MS);
      chainResult(st);
      g_ns_state = NS_IDLE;
      /* Whatever the panel now holds is not what g_shown claims. */
      g_shown.kind = 0xFF;
      g_shown.arg  = 0xFF;
      /* A layer change ends on its letter; a host notification just returns
       * to whatever the idle rule says. */
      if (g_ns_announce_layer) monoOverlay(layerAt(g_layer).glyph, MONO_LAYER_MS, now);
      return true;

    default:
      return false;
  }
}

/* Drains at most ONE queued rotation/brightness write. Returns true if a bus
 * transaction was consumed. */
static bool configApplyStep(uint8_t id)
{
  uint8_t op = 0;

  if (g_cfg_rotation_due) {
    g_cfg_rotation_due = false;
    chainResult(M5Chain.setMonoRotation(id, monoRotationEnum(g_cfg.mono_rotation), &op,
                                        CHAIN_SAVE_FLASH_DISABLE, CHAIN_CALL_TIMEOUT_MS));
    /* Rotation repaints the panel itself, so the cached view is void. */
    g_shown.kind = 0xFF;
    g_shown.arg  = 0xFF;
    return true;
  }
  if (g_cfg_brightness_due) {
    g_cfg_brightness_due = false;
    chainResult(M5Chain.setMonoBrightness(id, monoBrightnessEnum(g_cfg.mono_brightness), &op,
                                          CHAIN_SAVE_FLASH_DISABLE, CHAIN_CALL_TIMEOUT_MS));
    return true;
  }
  return false;
}

bool monoService(uint32_t now)
{
  uint8_t id = chainMonoId();
  if (id == 0) return false;
  if ((int32_t)(now - g_retry_at) < 0) return false;

  /* A pending rotation/brightness change goes first: it is at most two
   * transactions, it only happens when the host touched the config, and
   * painting a frame at the old brightness first would just be a flicker. */
  if (configApplyStep(id)) return true;

  if (g_ns_state != NS_IDLE) {
    if (nameScrollStep(now, id)) return true;
    if (g_ns_state != NS_IDLE) return false; /* still waiting out the scroll */
  }

  MonoView want = monoDesired(now);
  if (sameView(want, g_shown)) return false;

  uint8_t buf[8];
  monoRender(want, buf);

  uint8_t        op = 0;
  chain_status_t st = monoWriteBuffer(id, buf, &op);
  chainResult(st);
  if (st == CHAIN_OK && op == 1) {
    g_shown = want;
  } else {
    /* Back off so a sick panel cannot monopolise the bus. */
    g_retry_at = now + 200;
  }
  return true;
}
