/*
 * main.cpp - fethr companion display (M5Stack AtomS3R + Atomic ToChain Base).
 *
 * A second screen for the fethr sidecar. It is NOT a Chain node: it hangs off
 * whichever HY2.0-4P port the Chain bus did not claim and talks newline-
 * delimited JSON over a plain UART. The wire format and the probing story are
 * in companion_proto.h and ../../COMPANION.md.
 *
 * WHAT IS ON THE SCREEN (0.2.0)
 *   The resting screen is a KEY LEGEND for the current layer: the layer name in
 *   its own colour across the top, then one row per key, then a footer for the
 *   stick and the knob. None of those strings are built here. The sidecar
 *   derives them from its own layer table and ships them with every `hello` and
 *   every `layer` event, and this firmware renders what it was handed - so the
 *   screen cannot disagree with the keys, because there is no second copy of
 *   the bindings to go stale.
 *
 *   On top of that the screen animates the four things the user is actually
 *   waiting on: a full-screen pulsing mic while a dictation key is down, a
 *   colour wipe on a layer change, a 150 ms flash of the row a tap belongs to,
 *   and a position bar while the knob is turning.
 *
 * SHAPE OF THE FIRMWARE
 *   loop() is four non-blocking steps and never calls delay():
 *     M5.update()   - button debounce/classification, M5Unified's job
 *     linkTick()    - at most ONE inbound line parsed, plus the hello/ping timer
 *     uiTick()      - recompute the desired frame; redraw only if it changed
 *     consoleTick() - the '?' status dump on the USB CDC port
 *
 * DISPLAY PRIORITY - the first three entries are deliberately the same order as
 * the Chain Mono panel in the DualKey firmware, so the two screens never
 * disagree about what the engine is doing:
 *     1. no link          "waiting for sidecar"
 *     2. physical hold    the recording takeover (what the user's finger is
 *                         doing now outranks a report about a moment ago)
 *     3. layer wipe       a layer change is worth interrupting for
 *     4. timed overlay    check on `pasted`, cross on `error`
 *     5. host working     spinner while `transcribing` / `cleaning`, and while
 *                         waiting for the result of a hold that just ended
 *     6. idle             the legend
 *
 * DRAWING
 *   Everything is composed into one off-screen M5Canvas and pushed in a single
 *   blit, so the panel never shows a half-drawn frame. The frame is described
 *   by a small plain-old-data struct; if this pass's struct equals the one on
 *   screen, nothing is drawn at all.
 *
 *   Every animation is millis()-driven and is QUANTISED INTO UI_FRAME_MS
 *   BUCKETS before it reaches that struct. That is what caps the redraw rate at
 *   ~30 fps without a timer anywhere: the struct only compares unequal when the
 *   bucket changes. uiDraw() is a pure function of the struct - it never reads
 *   millis() - which is what makes the comparison a sound redraw test.
 */

#include <M5Unified.h>

#include <ArduinoJson.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "companion_proto.h"

/* ================================================================== */
/* What the sidecar has told us                                        */
/* ================================================================== */

#define MAX_LAYERS    8
#define MAX_NAME_LEN  12

static char    g_layer_name[MAX_LAYERS][MAX_NAME_LEN + 1];
static uint8_t g_layer_count = 0;
static uint8_t g_layer       = 0;
static uint8_t g_rgb[3]      = {0, 90, 255}; /* the FLOW blue, until told otherwise */

/* The legend for the CURRENT layer only. There is no per-layer cache on
 * purpose: a `layer` event always carries the new legend, so caching would buy
 * nothing but a way to be wrong. */
static char g_legend[LEGEND_SLOTS][LEGEND_MAX + 1];

static uint8_t  g_state      = HOST_IDLE;
static uint32_t g_state_at   = 0;
static bool     g_hold       = false;
static uint8_t  g_nodes      = 0;
static uint16_t g_vbat_mv    = 0;
static char     g_sidecar_fw[12] = {0};

/*
 * Has a host app EVER driven this session? The sidecar only forwards `state`
 * when something pushed one, so a single `state` event is proof that an app is
 * attached - and that is what decides what a key release means. With an app,
 * releasing starts a wait for the transcript; without one, releasing is the end
 * of the story and gets its checkmark immediately. Exactly the rule the Chain
 * Mono panel uses, arrived at from the other side of the link.
 */
static bool     g_host_ever = false;
static bool     g_await     = false; /* released, waiting for pasted/error */
static uint32_t g_await_at  = 0;

/* Animation anchors. 0 is a legitimate millis() value for about one
 * millisecond after boot, so each of these carries its own armed flag rather
 * than treating 0 as "never". */
static bool     g_wipe      = false;
static uint32_t g_wipe_at   = 0;

static bool     g_tap       = false;
static uint32_t g_tap_at    = 0;
static uint8_t  g_tap_row   = 0;
static uint8_t  g_tap_icon  = 0;

static bool     g_knob      = false;
static uint32_t g_knob_at   = 0;
static uint8_t  g_knob_pct  = 0;

/* Index order MUST match enum HostState. WIRE FORMAT. */
static const char *const HOST_STATE_NAME[HOST_STATE_COUNT] = {
    "idle", "recording", "transcribing", "cleaning", "pasted", "error",
};

static bool hostStateByName(const char *name, uint8_t *out)
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

/* ================================================================== */
/* Tap icons                                                           */
/* ================================================================== */

/*
 * A `tap` event carries the sidecar's own human label for the action that
 * fired ("play/pause", "mute", "undo", ...), not an FnId - every binding on the
 * MEDIA layer shares one FnId, so the id could not tell those apart. Matching
 * on the label is therefore the only thing that CAN work, and it degrades
 * correctly: a label with no icon simply flashes its row.
 */
enum IconId : uint8_t {
  ICON_NONE = 0,
  ICON_PLAY,
  ICON_NEXT,
  ICON_PREV,
  ICON_MUTE,
  ICON_UNDO,
  ICON_REDO,
  ICON_ENTER
};

static uint8_t iconForLabel(const char *label)
{
  if (label == NULL) return ICON_NONE;
  if (strstr(label, "play") != NULL) return ICON_PLAY;
  if (strstr(label, "next") != NULL) return ICON_NEXT;
  if (strstr(label, "prev") != NULL) return ICON_PREV;
  if (strstr(label, "mute") != NULL) return ICON_MUTE;
  if (strstr(label, "undo") != NULL) return ICON_UNDO;
  if (strstr(label, "redo") != NULL) return ICON_REDO;
  if (strstr(label, "enter") != NULL) return ICON_ENTER;
  return ICON_NONE;
}

/* Which legend row a tap belongs to. Row 3 is the footer, which is where the
 * two stick clicks land - they have no row of their own and flashing the wrong
 * key's row would be worse than flashing the line that does mention the stick. */
static uint8_t rowForTapKey(const char *key)
{
  if (key == NULL) return 0xFF;
  if (strcmp(key, "1") == 0) return 0;
  if (strcmp(key, "2") == 0) return 1;
  if (strcmp(key, "chain") == 0 || strcmp(key, "chain2") == 0) return 2;
  if (strcmp(key, "nav") == 0 || strcmp(key, "scroll") == 0) return 3;
  return 0xFF;
}

/* ================================================================== */
/* Link                                                                */
/* ================================================================== */

static bool     g_linked  = false;
static uint32_t g_last_rx = 0;
static uint32_t g_next_tx = 0;

/* ---- outbound: snprintf into one static buffer, no heap ---- */

static char g_out[COMPANION_OUT_BYTES];

static void sendLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sendLine(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(g_out, sizeof(g_out), fmt, ap);
  va_end(ap);

  /* A truncated line would be malformed JSON; drop it rather than make the
   * sidecar resynchronise. Nothing this firmware sends can reach the limit. */
  if (n < 0 || (size_t)n >= sizeof(g_out)) return;

  COMPANION_UART.write((const uint8_t *)g_out, (size_t)n);
  COMPANION_UART.print("\n");
}

static void sendHello(void)
{
  sendLine("{\"cmd\":\"%s\",\"who\":\"%s\",\"fw\":\"%s\"}", COMPANION_CMD_HELLO, COMPANION_WHO,
           COMPANION_FW_VERSION);
}

static void sendPing(void) { sendLine("{\"cmd\":\"%s\"}", COMPANION_CMD_PING); }

static void sendSimple(const char *cmd) { sendLine("{\"cmd\":\"%s\"}", cmd); }

/* ---- inbound ---- */

static char     g_line[COMPANION_MAX_LINE + 1];
static uint16_t g_line_len  = 0;
static bool     g_line_drop = false;
static bool     g_in_json   = false;

static void copyName(char *dst, const char *src)
{
  strncpy(dst, (src != NULL) ? src : "?", MAX_NAME_LEN);
  dst[MAX_NAME_LEN] = '\0';
}

static void readRgb(JsonVariantConst v)
{
  if (!v.is<JsonArrayConst>()) return;
  JsonArrayConst a = v.as<JsonArrayConst>();
  if (a.size() != 3) return;

  uint8_t rgb[3];
  for (uint8_t i = 0; i < 3; i++) {
    JsonVariantConst e = a[i];
    if (!e.is<long>()) return;
    long n = e.as<long>();
    if (n < 0 || n > 255) return;
    rgb[i] = (uint8_t)n;
  }
  memcpy(g_rgb, rgb, sizeof(g_rgb));
}

/*
 * The legend is replaced wholesale or not at all. A partial update - some rows
 * from the new layer, some left over from the old - would be a screen that is
 * confidently wrong, which is the one failure mode this whole design exists to
 * avoid. A missing or malformed field therefore leaves the previous legend up.
 */
static void readLegend(JsonVariantConst v)
{
  if (!v.is<JsonArrayConst>()) return;
  JsonArrayConst a = v.as<JsonArrayConst>();

  char next[LEGEND_SLOTS][LEGEND_MAX + 1];
  uint8_t n = 0;
  for (JsonVariantConst e : a) {
    if (n >= LEGEND_SLOTS) break;
    if (!e.is<const char *>()) return;
    strncpy(next[n], e.as<const char *>(), LEGEND_MAX);
    next[n][LEGEND_MAX] = '\0';
    n++;
  }
  if (n != LEGEND_SLOTS) return;

  memcpy(g_legend, next, sizeof(g_legend));
}

static void handleLine(const char *line, uint32_t now)
{
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  JsonVariantConst root = doc.as<JsonVariantConst>();
  JsonVariantConst ev_v = root["ev"];
  const char      *ev   = ev_v.is<const char *>() ? ev_v.as<const char *>() : NULL;
  if (ev == NULL) return;

  g_last_rx = now;
  g_linked  = true;

  if (strcmp(ev, COMPANION_EV_HELLO) == 0) {
    /* A hello is a resync. Hold is edge-driven, so a `true` we saw before the
     * link dropped could otherwise pin the screen on the mic forever; the
     * sidecar re-sends hold for any key that is still down immediately after
     * this. The same argument applies to a wait for a result that will never
     * now arrive. */
    g_hold  = false;
    g_await = false;

    JsonVariantConst fw = root["fw"];
    if (fw.is<const char *>()) {
      strncpy(g_sidecar_fw, fw.as<const char *>(), sizeof(g_sidecar_fw) - 1);
      g_sidecar_fw[sizeof(g_sidecar_fw) - 1] = '\0';
    }

    /* The layer NAMES only ever arrive here, so a companion that is plugged in
     * mid-session still labels a layer it has never seen a change for. */
    JsonVariantConst layers = root["layers"];
    if (layers.is<JsonArrayConst>()) {
      JsonArrayConst a = layers.as<JsonArrayConst>();
      uint8_t        n = 0;
      for (JsonVariantConst e : a) {
        if (n >= MAX_LAYERS) break;
        if (!e.is<const char *>()) continue;
        copyName(g_layer_name[n], e.as<const char *>());
        n++;
      }
      g_layer_count = n;
    }

    JsonVariantConst idx = root["layer"];
    if (idx.is<long>()) {
      long n = idx.as<long>();
      if (n >= 0 && n < MAX_LAYERS) g_layer = (uint8_t)n;
    }
    readRgb(root["rgb"]);
    readLegend(root["legend"]);
    return;
  }

  if (strcmp(ev, COMPANION_EV_LAYER) == 0) {
    JsonVariantConst idx = root["index"];
    if (idx.is<long>()) {
      long n = idx.as<long>();
      if (n >= 0 && n < MAX_LAYERS) {
        g_layer = (uint8_t)n;
        if (g_layer_count <= g_layer) g_layer_count = (uint8_t)(g_layer + 1);
        JsonVariantConst nm = root["name"];
        if (nm.is<const char *>()) copyName(g_layer_name[g_layer], nm.as<const char *>());
      }
    }
    readRgb(root["rgb"]);
    readLegend(root["legend"]);

    /* The sidecar only emits `layer` from the one place a layer actually
     * changes, so this needs no change detection of its own. */
    g_wipe    = true;
    g_wipe_at = now;
    g_tap     = false; /* a row flash belongs to the legend it was drawn over */
    return;
  }

  if (strcmp(ev, COMPANION_EV_STATE) == 0) {
    JsonVariantConst vv = root["value"];
    uint8_t          s  = 0;
    if (vv.is<const char *>() && hostStateByName(vv.as<const char *>(), &s)) {
      g_host_ever = true;

      /* The wait ends on a REPORT, never on a timeout: `pasted` and `error`
       * are the two results, and an explicit `idle` is the host saying it is
       * not working on anything, which is just as good an answer. Anything
       * else (transcribing/cleaning) is the wait continuing by another name. */
      if (s == HOST_PASTED || s == HOST_ERROR || s == HOST_IDLE) g_await = false;

      /* `pasted` and `error` are re-armed even when they repeat: two pastes in
       * a row should produce two checkmarks. Matches the DualKey. */
      if (s != g_state || s == HOST_PASTED || s == HOST_ERROR) {
        g_state    = s;
        g_state_at = now;
      }
    }
    return;
  }

  if (strcmp(ev, COMPANION_EV_HOLD) == 0) {
    JsonVariantConst av = root["active"];
    if (av.is<bool>()) {
      bool active = av.as<bool>();
      if (active) {
        g_await = false; /* a new hold supersedes the previous one's result */
      } else if (g_hold) {
        if (g_host_ever) {
          /* An app is driving: the text has NOT landed yet, so show the work
           * rather than a checkmark, and wait for the app to say which. */
          g_await    = true;
          g_await_at = now;
        } else {
          /* No app has ever spoken. The release is then the only feedback that
           * exists, so give it the checkmark - the plain-USB-keyboard case,
           * and the same concession the Mono panel makes. */
          g_state    = HOST_PASTED;
          g_state_at = now;
        }
      }
      g_hold = active;
    }
    return;
  }

  if (strcmp(ev, COMPANION_EV_TAP) == 0) {
    JsonVariantConst kv  = root["key"];
    uint8_t          row = kv.is<const char *>() ? rowForTapKey(kv.as<const char *>()) : 0xFF;
    if (row == 0xFF) return;

    JsonVariantConst fv = root["fn"];
    g_tap_icon =
        fv.is<const char *>() ? iconForLabel(fv.as<const char *>()) : (uint8_t)ICON_NONE;
    g_tap_row  = row;
    g_tap      = true;
    g_tap_at   = now;
    return;
  }

  if (strcmp(ev, COMPANION_EV_KNOB) == 0) {
    JsonVariantConst dv = root["detent"];
    JsonVariantConst ov = root["of"];
    if (!dv.is<long>() || !ov.is<long>()) return;
    long d  = dv.as<long>();
    long of = ov.as<long>();
    if (of < 2 || d < 0 || d >= of) return;

    /* Stored as a percentage, not as a detent, so the bar does not have to
     * carry the divisor around and a sidecar with a different ANGLE_DETENTS
     * needs no change here. */
    g_knob_pct = (uint8_t)((d * 100) / (of - 1));
    g_knob     = true;
    g_knob_at  = now;
    return;
  }

  if (strcmp(ev, COMPANION_EV_CHAIN) == 0) {
    JsonVariantConst cv = root["count"];
    if (cv.is<long>()) {
      long n = cv.as<long>();
      if (n >= 0 && n < 256) g_nodes = (uint8_t)n;
    }
    return;
  }

  if (strcmp(ev, COMPANION_EV_BATTERY) == 0) {
    JsonVariantConst bv = root["vbat_mv"];
    if (bv.is<long>()) {
      long n = bv.as<long>();
      if (n >= 0 && n < 65536) g_vbat_mv = (uint16_t)n;
    }
    return;
  }
}

/* Returns as soon as ONE complete line has been handled, so a chatty sidecar
 * cannot stall the button or the screen. */
static void linkRead(uint32_t now)
{
  while (COMPANION_UART.available() > 0) {
    int ci = COMPANION_UART.read();
    if (ci < 0) break;
    char c = (char)ci;

    if (c == '\n' || c == '\r') {
      bool complete = (!g_line_drop && g_in_json && g_line_len > 0);
      if (complete) g_line[g_line_len] = '\0';

      g_line_len  = 0;
      g_line_drop = false;
      g_in_json   = false;

      if (complete) {
        handleLine(g_line, now);
        return;
      }
      continue;
    }

    /* The sidecar's FLOG debug output shares this wire in neither direction,
     * but the rule costs nothing and makes the link robust against one. */
    if (!g_in_json && g_line_len == 0) {
      if (c != '{') continue;
      g_in_json = true;
    }

    if (g_line_drop) continue;
    if (g_line_len >= COMPANION_MAX_LINE) {
      g_line_drop = true;
      g_line_len  = 0;
      continue;
    }
    g_line[g_line_len++] = c;
  }
}

static void linkBegin(uint32_t now)
{
  COMPANION_UART.begin(COMPANION_BAUD, SERIAL_8N1, COMPANION_RX_PIN, COMPANION_TX_PIN);
  g_linked  = false;
  g_last_rx = now;
  g_next_tx = now; /* first hello goes out immediately */
}

static void linkTick(uint32_t now)
{
  linkRead(now);

  if (g_linked && (uint32_t)(now - g_last_rx) > (uint32_t)COMPANION_LINK_TIMEOUT_MS) {
    g_linked = false;
    /* Keep the last known layer/colour/legend - they are still the best guess
     * for the next frame - but drop everything that is only true "right now". */
    g_hold  = false;
    g_await = false;
    g_state = HOST_IDLE;
    g_wipe  = false;
    g_tap   = false;
    g_knob  = false;
  }

  if ((int32_t)(now - g_next_tx) >= 0) {
    g_next_tx = now + COMPANION_HELLO_MS;
    if (g_linked) {
      sendPing();
    } else {
      sendHello();
    }
  }
}

/* ================================================================== */
/* UI                                                                  */
/* ================================================================== */

enum ViewKind : uint8_t {
  V_WAIT = 0, /* no link                                  */
  V_LEGEND,   /* the resting screen: per-layer key legend  */
  V_REC,      /* full-screen recording takeover            */
  V_WIPE,     /* layer-change colour wipe                  */
  V_SPIN,     /* working; arg = frame 0..3                 */
  V_OK,       /* green check + "landed"                    */
  V_ERR       /* red cross + "try again"                   */
};

/*
 * Everything that can appear on screen, flattened. Two of these compare equal
 * exactly when the panel would be identical, which is the whole redraw rule -
 * and it only holds because uiDraw() reads nothing but this struct.
 */
struct Frame {
  uint8_t  kind;
  uint8_t  arg;         /* animation bucket, meaning depends on kind */
  uint8_t  layer;
  uint8_t  rgb[3];
  uint8_t  nodes;
  uint16_t vbat_mv;
  uint8_t  name_hash;   /* cheap "did the label change" check  */
  uint8_t  legend_hash; /* ditto for all four legend strings   */
  uint8_t  tap_row;     /* 0..3, 0xFF = no row flashing        */
  uint8_t  tap_icon;
  uint8_t  knob_pct;    /* 0..100, 0xFF = bar hidden           */
};

static bool frameEqual(const Frame &a, const Frame &b)
{
  return a.kind == b.kind && a.arg == b.arg && a.layer == b.layer && a.rgb[0] == b.rgb[0] &&
         a.rgb[1] == b.rgb[1] && a.rgb[2] == b.rgb[2] && a.nodes == b.nodes &&
         a.vbat_mv == b.vbat_mv && a.name_hash == b.name_hash &&
         a.legend_hash == b.legend_hash && a.tap_row == b.tap_row &&
         a.tap_icon == b.tap_icon && a.knob_pct == b.knob_pct;
}

static M5Canvas g_canvas(&M5.Display);

/* 0xFF is not a reachable kind, so the first uiTick() always paints. */
static Frame g_shown = {0xFF, 0, 0, {0, 0, 0}, 0, 0, 0, 0, 0xFF, 0, 0xFF};

static const char *layerName(void)
{
  if (g_layer < g_layer_count && g_layer_name[g_layer][0] != '\0') return g_layer_name[g_layer];
  return "?";
}

static uint8_t nameHash(const char *s)
{
  uint8_t h = 0;
  for (; *s != '\0'; s++) h = (uint8_t)(h * 31u + (uint8_t)*s);
  return h;
}

static uint8_t legendHash(void)
{
  uint8_t h = 0;
  for (uint8_t i = 0; i < LEGEND_SLOTS; i++) {
    h = (uint8_t)(h * 31u + nameHash(g_legend[i]));
  }
  return h;
}

/* One animation bucket, so every kind quantises the same way. */
static uint8_t animFrame(uint32_t elapsed, uint8_t modulo)
{
  uint32_t f = elapsed / (uint32_t)UI_FRAME_MS;
  return (modulo > 0) ? (uint8_t)(f % modulo) : (uint8_t)f;
}

/* How many UI_FRAME_MS buckets the recording pulse and the wipe each span.
 * Both are derived rather than written down twice, so changing a duration in
 * companion_proto.h cannot desynchronise the animation from its own length. */
#define UI_REC_FRAMES  ((uint8_t)(UI_REC_PERIOD_MS / UI_FRAME_MS))
#define UI_WIPE_FRAMES ((uint8_t)(UI_WIPE_MS / UI_FRAME_MS))

static Frame uiDesired(uint32_t now)
{
  Frame f;
  f.kind        = V_LEGEND;
  f.arg         = 0;
  f.layer       = g_layer;
  f.rgb[0]      = g_rgb[0];
  f.rgb[1]      = g_rgb[1];
  f.rgb[2]      = g_rgb[2];
  f.nodes       = g_nodes;
  f.vbat_mv     = g_vbat_mv;
  f.name_hash   = nameHash(layerName());
  f.legend_hash = legendHash();
  f.tap_row     = 0xFF;
  f.tap_icon    = ICON_NONE;
  f.knob_pct    = 0xFF;

  if (!g_linked) {
    f.kind = V_WAIT;
    return f;
  }

  /* A physical hold outranks anything the host reported: what the finger is
   * doing now beats a report about a moment ago. */
  if (g_hold || g_state == HOST_RECORDING) {
    f.kind = V_REC;
    /* The pulse is free-running off millis() rather than anchored to the press.
     * Anchoring would restart the sine on every hold event - including the one
     * the OTHER key emits - so the glyph would visibly jump mid-dictation. */
    f.arg = animFrame(now, UI_REC_FRAMES);
    return f;
  }

  if (g_wipe && (uint32_t)(now - g_wipe_at) < (uint32_t)UI_WIPE_MS) {
    f.kind = V_WIPE;
    f.arg  = animFrame(now - g_wipe_at, 0);
    return f;
  }

  if (g_state == HOST_PASTED && (uint32_t)(now - g_state_at) < (uint32_t)UI_OK_MS) {
    f.kind = V_OK;
    return f;
  }
  if (g_state == HOST_ERROR && (uint32_t)(now - g_state_at) < (uint32_t)UI_ERR_MS) {
    f.kind = V_ERR;
    return f;
  }

  if (g_state == HOST_TRANSCRIBING || g_state == HOST_CLEANING || g_await) {
    uint32_t anchor = g_await ? g_await_at : g_state_at;
    f.kind          = V_SPIN;
    f.arg           = (uint8_t)(((now - anchor) / UI_SPIN_FRAME_MS) & 0x03);
    return f;
  }

  /* The two decorations only apply to the resting screen: there is no legend
   * row to flash on any of the takeover views, and a knob bar under a mic
   * glyph would be noise. */
  if (g_tap && (uint32_t)(now - g_tap_at) < (uint32_t)UI_TAP_MS) {
    f.tap_row  = g_tap_row;
    f.tap_icon = g_tap_icon;
  }
  if (g_knob && (uint32_t)(now - g_knob_at) < (uint32_t)UI_KNOB_MS) {
    f.knob_pct = g_knob_pct;
  }
  return f; /* V_LEGEND */
}

/* ---- drawing helpers. All coordinates are in the sprite, 128x128. ---- */

#define UI_W        128
#define UI_H        128
#define UI_BAND_H   28  /* layer name band                 */
#define UI_ROW0_Y   44  /* centre of the first legend row  */
#define UI_ROW_DY   21
#define UI_ROW_H    18
#define UI_FOOT_Y   110 /* centre of the footer line       */
#define UI_KNOB_Y   122 /* top of the knob bar             */
#define UI_KNOB_H   4
#define UI_CENTER_X 64
#define UI_CENTER_Y 68  /* middle of the state area        */

static uint16_t layerColor(const Frame &f)
{
  return g_canvas.color565(f.rgb[0], f.rgb[1], f.rgb[2]);
}

/* A dimmed copy of a colour, for separators and the resting dot. */
static uint16_t dim(const Frame &f, uint8_t pct)
{
  return g_canvas.color565((uint8_t)((f.rgb[0] * pct) / 100), (uint8_t)((f.rgb[1] * pct) / 100),
                           (uint8_t)((f.rgb[2] * pct) / 100));
}

/* ---- the top band ---- */

/*
 * Node count and battery live in the band's two corners rather than in a footer
 * of their own: the footer is the legend's fourth line now, and these two
 * numbers are reference information that should not cost a row the user reads
 * every day.
 */
static void drawBandCorners(const Frame &f)
{
  char buf[12];
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextColor(g_canvas.color565(90, 90, 100));

  if (f.nodes > 0) {
    g_canvas.setTextDatum(top_left);
    snprintf(buf, sizeof(buf), "%un", (unsigned)f.nodes);
    g_canvas.drawString(buf, 3, 3);
  }

  /* Before the first `battery` event there is no reading to show, and 0 mV
   * would be a lie rather than a blank. */
  if (f.vbat_mv > 0) {
    g_canvas.setTextDatum(top_right);
    snprintf(buf, sizeof(buf), "%u.%02uV", (unsigned)(f.vbat_mv / 1000),
             (unsigned)((f.vbat_mv % 1000) / 10));
    g_canvas.drawString(buf, UI_W - 3, 3);
  }
}

static void drawBand(const Frame &f)
{
  g_canvas.setFont(&fonts::FreeSansBold12pt7b);
  g_canvas.setTextColor(layerColor(f));
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(layerName(), UI_CENTER_X, UI_BAND_H / 2 + 2);
  g_canvas.drawFastHLine(10, UI_BAND_H + 2, UI_W - 20, dim(f, 35));
  drawBandCorners(f);
}

/* ---- the legend ---- */

/*
 * One font for all three rows, chosen by the WIDEST of them. Per-row selection
 * would be easy and would look like a bug: three lines of the same kind of
 * information should not be three different sizes. The sidecar builds the key
 * rows to fit 16 characters, which Font2 usually manages; anything longer drops
 * the whole block to the 6-pixel console font rather than running off the edge.
 */
static void setRowFont(void)
{
  g_canvas.setFont(&fonts::Font2);
  for (uint8_t i = 0; i < 3; i++) {
    if (g_canvas.textWidth(g_legend[i]) > (UI_W - 10)) {
      g_canvas.setFont(&fonts::Font0);
      return;
    }
  }
}

static void drawIcon(uint8_t icon, int32_t cx, int32_t cy, uint16_t c)
{
  switch (icon) {
    case ICON_PLAY: /* triangle + two bars: the play/pause key, both halves */
      g_canvas.fillTriangle(cx - 7, cy - 5, cx - 7, cy + 5, cx - 1, cy, c);
      g_canvas.fillRect(cx + 2, cy - 5, 2, 10, c);
      g_canvas.fillRect(cx + 6, cy - 5, 2, 10, c);
      break;

    case ICON_NEXT:
      g_canvas.fillTriangle(cx - 7, cy - 5, cx - 7, cy + 5, cx + 1, cy, c);
      g_canvas.fillRect(cx + 3, cy - 5, 2, 10, c);
      break;

    case ICON_PREV:
      g_canvas.fillTriangle(cx + 7, cy - 5, cx + 7, cy + 5, cx - 1, cy, c);
      g_canvas.fillRect(cx - 5, cy - 5, 2, 10, c);
      break;

    case ICON_MUTE: /* speaker cone with a cross through it */
      g_canvas.fillRect(cx - 7, cy - 2, 3, 4, c);
      g_canvas.fillTriangle(cx - 4, cy - 6, cx - 4, cy + 6, cx, cy, c);
      g_canvas.drawLine(cx + 2, cy - 4, cx + 8, cy + 4, c);
      g_canvas.drawLine(cx + 8, cy - 4, cx + 2, cy + 4, c);
      break;

    case ICON_UNDO: /* left arrow with a tail curling under it */
      g_canvas.fillTriangle(cx - 7, cy - 1, cx - 1, cy - 6, cx - 1, cy + 4, c);
      g_canvas.drawLine(cx - 1, cy - 1, cx + 6, cy - 1, c);
      g_canvas.drawLine(cx + 6, cy - 1, cx + 6, cy + 5, c);
      g_canvas.drawLine(cx + 6, cy + 5, cx - 2, cy + 5, c);
      break;

    case ICON_REDO: /* the mirror image, so the pair reads as a pair */
      g_canvas.fillTriangle(cx + 7, cy - 1, cx + 1, cy - 6, cx + 1, cy + 4, c);
      g_canvas.drawLine(cx + 1, cy - 1, cx - 6, cy - 1, c);
      g_canvas.drawLine(cx - 6, cy - 1, cx - 6, cy + 5, c);
      g_canvas.drawLine(cx - 6, cy + 5, cx + 2, cy + 5, c);
      break;

    case ICON_ENTER: /* the return arrow: riser on the right, head on the left */
      g_canvas.drawLine(cx + 7, cy - 6, cx + 7, cy + 2, c);
      g_canvas.drawLine(cx + 7, cy + 2, cx - 3, cy + 2, c);
      g_canvas.fillTriangle(cx - 7, cy + 2, cx - 2, cy - 2, cx - 2, cy + 6, c);
      break;

    default:
      break;
  }
}

static void drawLegend(const Frame &f)
{
  setRowFont();
  g_canvas.setTextDatum(middle_left);

  for (uint8_t i = 0; i < 3; i++) {
    int32_t y   = UI_ROW0_Y + (int32_t)i * UI_ROW_DY;
    bool    lit = (f.tap_row == i);

    if (lit) {
      /* Invert the row rather than tint it: at this size a colour change is
       * easy to miss and a solid block is not. */
      g_canvas.fillRoundRect(2, y - UI_ROW_H / 2, UI_W - 4, UI_ROW_H, 3, layerColor(f));
      g_canvas.setTextColor(TFT_BLACK);
    } else {
      g_canvas.setTextColor(g_canvas.color565(200, 200, 210));
    }
    g_canvas.drawString(g_legend[i], 7, y);

    if (lit && f.tap_icon != ICON_NONE) drawIcon(f.tap_icon, UI_W - 14, y, TFT_BLACK);
  }

  /* Footer: what the stick and the knob do. Always the console font - it is a
   * longer string than the rows and it is reference, not the headline. */
  g_canvas.setFont(&fonts::Font0);
  g_canvas.setTextDatum(middle_center);
  if (f.tap_row == 3) {
    g_canvas.fillRoundRect(2, UI_FOOT_Y - 6, UI_W - 4, 13, 3, layerColor(f));
    g_canvas.setTextColor(TFT_BLACK);
  } else {
    g_canvas.setTextColor(g_canvas.color565(110, 110, 125));
  }
  g_canvas.drawString(g_legend[3], UI_CENTER_X, UI_FOOT_Y);

  /* Knob position, as a thin bar along the bottom edge. */
  if (f.knob_pct <= 100) {
    int32_t w = (int32_t)((UI_W - 12) * (int32_t)f.knob_pct) / 100;
    g_canvas.fillRect(6, UI_KNOB_Y, UI_W - 12, UI_KNOB_H, dim(f, 18));
    if (w > 0) g_canvas.fillRect(6, UI_KNOB_Y, w, UI_KNOB_H, layerColor(f));
  }
}

/* ---- the takeover views ---- */

/*
 * Recording. A drawn mic - capsule, cradle, stem, base - pulsing on one sine
 * per UI_REC_PERIOD_MS in both size and brightness, with a halo ring breathing
 * against it, and "talk!" underneath. Full screen: while a dictation key is
 * down, nothing else on this panel is worth a pixel.
 */
static void drawRecording(const Frame &f)
{
  float phase = (float)f.arg / (float)UI_REC_FRAMES;
  float s     = sinf(phase * 2.0f * (float)PI); /* -1 .. +1 */
  float k     = (s + 1.0f) * 0.5f;              /*  0 .. 1  */

  uint8_t  lvl = (uint8_t)(150.0f + 105.0f * k);
  uint16_t red = g_canvas.color565(lvl, (uint8_t)(24 + 16 * k), (uint8_t)(24 + 16 * k));
  uint16_t hal = g_canvas.color565((uint8_t)(50 + 60 * k), 12, 12);

  int32_t cx = UI_CENTER_X;
  int32_t r  = 10 + (int32_t)(3.0f * k); /* capsule half-width */
  int32_t h  = 26 + (int32_t)(6.0f * k); /* capsule height     */
  int32_t top = 30;

  g_canvas.drawCircle(cx, top + h / 2, r + 14 + (int32_t)(5.0f * k), hal);
  g_canvas.fillRoundRect(cx - r, top, r * 2, h, r, red);

  int32_t cradle = top + h + 5;
  g_canvas.drawLine(cx - r - 5, cradle - 9, cx - r - 5, cradle, red);
  g_canvas.drawLine(cx + r + 5, cradle - 9, cx + r + 5, cradle, red);
  g_canvas.drawLine(cx - r - 5, cradle, cx + r + 5, cradle, red);
  g_canvas.drawLine(cx, cradle, cx, cradle + 9, red);
  g_canvas.drawLine(cx - 9, cradle + 9, cx + 9, cradle + 9, red);

  g_canvas.setFont(&fonts::FreeSansBold12pt7b);
  g_canvas.setTextDatum(middle_center);
  g_canvas.setTextColor(g_canvas.color565(lvl, 40, 40));
  g_canvas.drawString("talk!", cx, 112);
}

/*
 * The layer wipe: a block of the NEW layer's colour sweeps in from the left,
 * covers the screen, and sweeps out to the right, revealing the new legend
 * already drawn underneath. Two halves of one pass rather than a fade, because
 * a 128 px panel has no room for a subtle transition to read as deliberate.
 */
static void drawWipe(const Frame &f)
{
  drawBand(f);
  drawLegend(f);

  uint32_t span = (uint32_t)UI_WIPE_FRAMES;
  if (span == 0) return;
  float p = (float)f.arg / (float)span; /* 0 .. 1 */
  if (p > 1.0f) p = 1.0f;

  int32_t x0, w;
  if (p < 0.5f) {
    x0 = 0;
    w  = (int32_t)(2.0f * p * (float)UI_W);
  } else {
    x0 = (int32_t)((2.0f * p - 1.0f) * (float)UI_W);
    w  = UI_W - x0;
  }
  if (w > 0) g_canvas.fillRect(x0, 0, w, UI_H, layerColor(f));
}

static void drawSpinner(const Frame &f)
{
  /* Four dots on a circle; the active one is bright and larger. The frame
   * number is derived from millis() by uiDesired(), so the animation costs one
   * redraw per UI_SPIN_FRAME_MS and nothing in between. */
  static const int8_t DX[4] = {0, 20, 0, -20};
  static const int8_t DY[4] = {-20, 0, 20, 0};

  for (uint8_t i = 0; i < 4; i++) {
    bool     on = (i == f.arg);
    uint16_t c  = on ? layerColor(f) : dim(f, 25);
    g_canvas.fillCircle(UI_CENTER_X + DX[i], UI_CENTER_Y + DY[i], on ? 7 : 4, c);
  }
}

static void drawCheck(void)
{
  uint16_t c = g_canvas.color565(40, 220, 90);
  /* Three parallel strokes so the tick reads as a stroke, not a hairline. */
  for (int8_t o = -2; o <= 2; o++) {
    g_canvas.drawLine(UI_CENTER_X - 20, UI_CENTER_Y + o, UI_CENTER_X - 6, UI_CENTER_Y + 14 + o, c);
    g_canvas.drawLine(UI_CENTER_X - 6, UI_CENTER_Y + 14 + o, UI_CENTER_X + 20, UI_CENTER_Y - 14 + o,
                      c);
  }
}

static void drawCross(void)
{
  uint16_t c = g_canvas.color565(230, 40, 40);
  for (int8_t o = -2; o <= 2; o++) {
    g_canvas.drawLine(UI_CENTER_X - 16 + o, UI_CENTER_Y - 16, UI_CENTER_X + 16 + o,
                      UI_CENTER_Y + 16, c);
    g_canvas.drawLine(UI_CENTER_X + 16 + o, UI_CENTER_Y - 16, UI_CENTER_X - 16 + o,
                      UI_CENTER_Y + 16, c);
  }
}

static void drawCaption(const char *text, uint16_t c)
{
  g_canvas.setFont(&fonts::Font2);
  g_canvas.setTextDatum(middle_center);
  g_canvas.setTextColor(c);
  g_canvas.drawString(text, UI_CENTER_X, UI_FOOT_Y);
}

static void drawWaiting(void)
{
  g_canvas.setFont(&fonts::Font2);
  g_canvas.setTextDatum(middle_center);
  g_canvas.setTextColor(g_canvas.color565(70, 70, 80));
  g_canvas.drawString("waiting for", UI_CENTER_X, 56);
  g_canvas.drawString("sidecar...", UI_CENTER_X, 74);

  g_canvas.setTextColor(g_canvas.color565(45, 45, 55));
  g_canvas.drawString("fethr", UI_CENTER_X, 18);
}

static void uiDraw(const Frame &f)
{
  g_canvas.fillSprite(TFT_BLACK);

  switch (f.kind) {
    case V_WAIT:
      drawWaiting();
      g_canvas.pushSprite(0, 0);
      return;

    case V_REC:
      /* Full-screen takeover: no band, no legend. */
      drawRecording(f);
      g_canvas.pushSprite(0, 0);
      return;

    case V_WIPE:
      drawWipe(f);
      g_canvas.pushSprite(0, 0);
      return;

    default:
      break;
  }

  drawBand(f);

  switch (f.kind) {
    case V_SPIN:
      drawSpinner(f);
      drawCaption("working...", g_canvas.color565(150, 150, 165));
      break;
    case V_OK:
      drawCheck();
      drawCaption("landed", g_canvas.color565(40, 220, 90));
      break;
    case V_ERR:
      drawCross();
      drawCaption("try again", g_canvas.color565(230, 60, 60));
      break;
    default:
      drawLegend(f);
      break;
  }

  g_canvas.pushSprite(0, 0);
}

static void uiTick(uint32_t now)
{
  Frame want = uiDesired(now);
  if (frameEqual(want, g_shown)) return;
  uiDraw(want);
  g_shown = want;
}

/* ================================================================== */
/* USB console                                                         */
/* ================================================================== */

/*
 * M5Unified identifies the board at run time - it probes the LCD's SPI bus for
 * a panel id and the internal I2C bus for sensors - and exposes the answer as a
 * board_t, not a string (there is no getBoardName()). These are the three
 * members of the AtomS3R family it can return; anything else means autodetect
 * did not find the display it expected, which is the first thing to check if
 * the screen stays dark.
 *
 * Verified in M5GFX/src/lgfx/boards.hpp: board_M5AtomS3R = 18,
 * board_M5AtomS3RExt = 143, board_M5AtomS3RCam = 144.
 */
static const char *boardName(void)
{
  switch (M5.getBoard()) {
    case m5::board_t::board_M5AtomS3R:    return "AtomS3R";
    case m5::board_t::board_M5AtomS3RExt: return "AtomS3R + Ext base";
    case m5::board_t::board_M5AtomS3RCam: return "AtomS3R + Cam base";
    default:                              return "UNRECOGNISED";
  }
}

/*
 * One character, like the DualKey's. '?' prints the link state. Non-blocking:
 * with nothing attached this costs one available() check per loop.
 */
static void consoleTick(uint32_t now)
{
  while (Serial.available() > 0) {
    int ci = Serial.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (c != '?' && c != 's' && c != 'S') continue;

    Serial.printf("\r\n--- fethr companion " COMPANION_FW_VERSION " ---\r\n");
    Serial.printf("board   : %s (id %d)\r\n", boardName(), (int)M5.getBoard());
    Serial.printf("link    : %s on rx=G%d tx=G%d\r\n", g_linked ? "linked" : "probing",
                  (int)COMPANION_RX_PIN, (int)COMPANION_TX_PIN);
    Serial.printf("last rx : %lums ago\r\n", (unsigned long)(now - g_last_rx));
    Serial.printf("sidecar : fw %s\r\n", (g_sidecar_fw[0] != '\0') ? g_sidecar_fw : "?");
    Serial.printf("layer   : %u/%u %s  rgb=%u,%u,%u\r\n", (unsigned)g_layer,
                  (unsigned)g_layer_count, layerName(), (unsigned)g_rgb[0], (unsigned)g_rgb[1],
                  (unsigned)g_rgb[2]);
    for (uint8_t i = 0; i < LEGEND_SLOTS; i++) {
      Serial.printf("legend %u: %s\r\n", (unsigned)i, g_legend[i]);
    }
    Serial.printf("state   : %s%s%s\r\n",
                  (g_state < HOST_STATE_COUNT) ? HOST_STATE_NAME[g_state] : "?",
                  g_hold ? " (key held)" : "", g_await ? " (awaiting result)" : "");
    Serial.printf("host    : %s\r\n", g_host_ever ? "seen" : "never spoke");
    Serial.printf("chain   : %u node(s)\r\n", (unsigned)g_nodes);
    Serial.printf("battery : %umV\r\n", (unsigned)g_vbat_mv);
  }
}

/* ================================================================== */
/* Setup / loop                                                        */
/* ================================================================== */

void setup()
{
  auto cfg = M5.config();
  /* Nothing here needs the IMU, and leaving it asleep keeps the internal I2C
   * bus quiet. The display and the button are all this firmware uses. */
  cfg.internal_imu = false;
  cfg.clear_display = true;
  M5.begin(cfg);

  /* UI_ROTATION: 0..3 = 90-degree steps clockwise. 3 = a quarter turn
   * counter-clockwise from the panel's native orientation, which is how the
   * screen reads when the Atom sits on the ToChain base beside the DualKey
   * (set on hardware 2026-09-15). */
  M5.Display.setRotation(UI_ROTATION);
  M5.Display.setBrightness(UI_BRIGHTNESS);
  M5.Display.fillScreen(TFT_BLACK);

  /* 16-bit, so the layer colours arrive on the panel as sent. 128x128x2 =
   * 32 KB, which fits in DRAM on a board that also has 8 MB of PSRAM. */
  g_canvas.setColorDepth(16);
  g_canvas.createSprite(UI_W, UI_H);

  /* Placeholders until the first `hello`: the screen is honest about knowing
   * nothing rather than showing three blank rows that look like a dead link. */
  for (uint8_t i = 0; i < LEGEND_SLOTS; i++) g_legend[i][0] = '\0';
  strncpy(g_legend[0], "no legend yet", LEGEND_MAX);

  /* wasHold() is a one-shot fired when the press crosses this threshold, so a
   * hold never also reports a click. */
  M5.BtnA.setHoldThresh(800);

  Serial.begin(115200);
  Serial.printf("\r\n[fethr-companion] v" COMPANION_FW_VERSION " boot on %s, '?' for status\r\n",
                boardName());

  uint32_t now = millis();
  linkBegin(now);
  uiTick(now); /* paint the waiting screen immediately */
}

void loop()
{
  uint32_t now = millis();

  M5.update();

  /* Click = next layer. Hold = identify, which flashes the DualKey's key LEDs
   * white and scrolls "ID" on the Mono panel - the cheapest way to tell two
   * sidecars apart, and a live proof that the link works in this direction. */
  if (M5.BtnA.wasClicked()) sendSimple(COMPANION_CMD_LAYER_NEXT);
  if (M5.BtnA.wasHold()) sendSimple(COMPANION_CMD_IDENTIFY);

  linkTick(now);
  uiTick(now);
  consoleTick(now);
}
