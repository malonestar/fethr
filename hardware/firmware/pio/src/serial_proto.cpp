/*
 * serial_proto.cpp - the host settings protocol described in PROTOCOL.md.
 *
 * See serial_proto.h for the shape of the thing and the allocation policy.
 * Three rules govern everything below:
 *
 *   1. NEVER BLOCK. The reader drains whatever Serial has buffered and
 *      returns; a half-arrived command simply stays half-arrived. With no
 *      host attached, protoTick() costs one Serial.available() check.
 *
 *   2. NO HEAP IN THE STEADY STATE. Replies and events are assembled with
 *      snprintf into the single static g_out buffer. Only deserializeJson()
 *      allocates, and only while a host command is being parsed; the document
 *      is created, read and destroyed inside one call.
 *
 *   3. ONE LINE PER COMMAND. Exactly one reply object per command, carrying
 *      the request's "id" if it had one. Events are separate lines and never
 *      carry an id.
 *
 * A command handler must not emit an event while a reply is half-built - both
 * use g_out. Where a command produces both (e.g. `layer`), the reply is
 * flushed first and the side effect performed afterwards.
 *
 * Everything except the '?' console and the two public entry points is
 * compiled out when FLOW_SERIAL_PROTO is 0, which leaves exactly the v2.0
 * serial behaviour.
 */

#include "sidecar.h"

#include "keynames.h"
#include "serial_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if FLOW_SERIAL_PROTO
#include <ArduinoJson.h>
#endif

/* ================================================================== */
/* Human console (always compiled)                                     */
/* ================================================================== */

/*
 * One-character console, unchanged from v2.0. It only ever sees bytes that
 * began a line and were not '{', so a JSON payload containing '?' or 's'
 * cannot trigger a status dump. A terminal user still just presses '?', with
 * or without a newline.
 */
static void consoleChar(char c)
{
#if FLOW_SERIAL_CONSOLE
  if (c == '?' || c == 's' || c == 'S') chainPrintStatus();
#else
  (void)c;
#endif
}

#if FLOW_SERIAL_PROTO

/* ================================================================== */
/* Outbound line assembly                                              */
/* ================================================================== */

static char     g_out[FLOW_PROTO_OUT_BYTES];
static uint16_t g_out_len   = 0;
static bool     g_out_trunc = false;

static void outReset(void)
{
  g_out_len   = 0;
  g_out[0]    = '\0';
  g_out_trunc = false;
}

static void outChar(char c)
{
  if ((size_t)(g_out_len + 1) >= sizeof(g_out)) {
    g_out_trunc = true;
    return;
  }
  g_out[g_out_len++] = c;
  g_out[g_out_len]   = '\0';
}

static void outPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void outPrintf(const char *fmt, ...)
{
  size_t room = sizeof(g_out) - (size_t)g_out_len;
  if (room <= 1) {
    g_out_trunc = true;
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(g_out + g_out_len, room, fmt, ap);
  va_end(ap);

  if (n < 0 || (size_t)n >= room) {
    g_out_trunc      = true;
    g_out[g_out_len] = '\0';
    return;
  }
  g_out_len = (uint16_t)(g_out_len + n);
}

/* Emits a JSON string literal, quotes included. Escaping is the minimum the
 * format requires plus \uXXXX for the remaining control characters, so a host
 * string echoed back in `path` or `msg` can never break the line. */
static void outJsonStr(const char *s)
{
  outChar('"');
  for (; s != NULL && *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '"':  outChar('\\'); outChar('"'); break;
      case '\\': outChar('\\'); outChar('\\'); break;
      case '\n': outChar('\\'); outChar('n'); break;
      case '\r': outChar('\\'); outChar('r'); break;
      case '\t': outChar('\\'); outChar('t'); break;
      default:
        if (c < 0x20) {
          outPrintf("\\u%04X", (unsigned)c);
        } else {
          outChar((char)c);
        }
        break;
    }
  }
  outChar('"');
}

/* A truncated line would be malformed JSON, which is worse than no line at
 * all - the host would have to resynchronise. Emit a fixed error instead. */
static void outFlush(void)
{
  if (g_out_trunc) {
    Serial.print("{\"ev\":\"err\",\"msg\":\"reply too large\"}\r\n");
  } else {
    Serial.write((const uint8_t *)g_out, g_out_len);
    Serial.print("\r\n");
  }
  outReset();
}

/* ================================================================== */
/* Request id + framing                                                */
/* ================================================================== */

/* Captured per command, cleared before each one. Events ignore it. */
static bool g_id_present = false;
static bool g_id_is_str  = false;
static long g_id_num     = 0;
static char g_id_str[33] = {0};

static void idClear(void)
{
  g_id_present = false;
  g_id_is_str  = false;
  g_id_num     = 0;
  g_id_str[0]  = '\0';
}

/* Reply framing - includes the id. */
static void replyBegin(const char *ev)
{
  outReset();
  outPrintf("{\"ev\":\"%s\"", ev);
  if (g_id_present) {
    outPrintf(",\"id\":");
    if (g_id_is_str) {
      outJsonStr(g_id_str);
    } else {
      outPrintf("%ld", g_id_num);
    }
  }
}

/* Event framing - never includes an id, even mid-command. */
static void eventBegin(const char *ev)
{
  outReset();
  outPrintf("{\"ev\":\"%s\"", ev);
}

static void frameEnd(void)
{
  outChar('}');
  outFlush();
}

static void replyOk(void)
{
  replyBegin(PROTO_EV_OK);
  frameEnd();
}

static void replyErr(const char *msg)
{
  replyBegin(PROTO_EV_ERR);
  outPrintf(",\"msg\":");
  outJsonStr(msg);
  frameEnd();
}

static const char *boolName(bool v) { return v ? "true" : "false"; }

static const char *monoIdleName(uint8_t v)
{
  return (v == MONO_IDLE_LETTER) ? PROTO_MONO_IDLE_LETTER : PROTO_MONO_IDLE_BLANK;
}

/* ================================================================== */
/* Shared fragments                                                    */
/* ================================================================== */

/* `nodes` array, used by hello, status and the chain event. */
static void outNodes(void)
{
  outPrintf(",\"nodes\":[");

  uint8_t count   = chainNodeCount();
  uint8_t emitted = 0;
  for (uint8_t i = 0; i < count; i++) {
    uint16_t    id        = 0;
    const char *type_name = NULL;
    const char *role      = NULL;
    if (!chainNodeAt(i, &id, NULL, &type_name, &role)) continue;

    if (emitted++) outChar(',');
    outPrintf("{\"id\":%u,\"type\":\"%s\"", (unsigned)id, type_name);
    /* Two joysticks share a device type, so the role is the only thing that
     * says which one drives the cursor. Omitted when the node has none. */
    if (role != NULL) outPrintf(",\"role\":\"%s\"", role);
    outChar('}');
  }
  outChar(']');
}

/* The whole config object, exactly the shape PROTOCOL.md documents. This is
 * the one message that exceeds the protocol's 512-byte line size (it lands
 * near 700 bytes); that limit governs what the DEVICE accepts, and g_out is
 * sized for this reply. */
static void outConfig(void)
{
  outPrintf(",\"layer_rgb\":[");
  for (uint8_t i = 0; i < layerCount(); i++) {
    if (i) outChar(',');
    outPrintf("[%u,%u,%u]", (unsigned)g_cfg.layer_rgb[i][0], (unsigned)g_cfg.layer_rgb[i][1],
              (unsigned)g_cfg.layer_rgb[i][2]);
  }

  outPrintf("],\"fn_rgb\":{");
  bool first = true;
  for (uint8_t f = 0; f < (uint8_t)FN_COUNT; f++) {
    const char *name = fnName(f);
    if (name == NULL) continue; /* FN_NONE means "no opinion", not a colour */
    if (!first) outChar(',');
    first = false;
    outPrintf("\"%s\":[%u,%u,%u]", name, (unsigned)g_cfg.fn_rgb[f][0],
              (unsigned)g_cfg.fn_rgb[f][1], (unsigned)g_cfg.fn_rgb[f][2]);
  }
  outChar('}');

  outPrintf(",\"hold_rgb\":[%u,%u,%u]", (unsigned)g_cfg.hold_rgb[0],
            (unsigned)g_cfg.hold_rgb[1], (unsigned)g_cfg.hold_rgb[2]);
  outPrintf(",\"led_idle_pct\":%u,\"led_flash_ms\":%u,\"node_leds\":%s",
            (unsigned)g_cfg.led_idle_pct, (unsigned)g_cfg.led_flash_ms,
            boolName(g_cfg.node_leds != 0));
  outPrintf(",\"mono_brightness\":%u,\"mono_rotation\":%u,\"mono_idle\":\"%s\"",
            (unsigned)g_cfg.mono_brightness, (unsigned)g_cfg.mono_rotation,
            monoIdleName(g_cfg.mono_idle));
  outPrintf(",\"double_tap\":%s,\"double_tap_ms\":%u,\"layer_hold_ms\":%u",
            boolName(g_cfg.double_tap != 0), (unsigned)g_cfg.double_tap_ms,
            (unsigned)g_cfg.layer_hold_ms);
  outPrintf(",\"boot_layer\":%u", (unsigned)g_cfg.boot_layer);
  outPrintf(",\"nav_y_sign\":%d,\"nav_x_sign\":%d,\"scroll_y_sign\":%d,\"scroll_x_sign\":%d",
            (int)g_cfg.nav_y_sign, (int)g_cfg.nav_x_sign, (int)g_cfg.scroll_y_sign,
            (int)g_cfg.scroll_x_sign);
  outPrintf(",\"mouse_y_sign\":%d", (int)g_cfg.mouse_y_sign);
  outPrintf(",\"led_index_key1\":%u", (unsigned)g_cfg.led_index_key1);
  /* proto 2. Mounting orientation: the two axis swaps compose with the four
   * signs above to cover every 90-degree stick mounting, and swap_keys is the
   * old KEYS_SWAPPED build flag as a setting. */
  outPrintf(",\"swap_keys\":%s,\"nav_swap_xy\":%s,\"scroll_swap_xy\":%s",
            boolName(g_cfg.swap_keys != 0), boolName(g_cfg.nav_swap_xy != 0),
            boolName(g_cfg.scroll_swap_xy != 0));
}

/* ================================================================== */
/* The layer table (proto 2)                                           */
/* ================================================================== */

/* Slot order is wire format: it is the order `get_layers` emits and the set
 * `set_action` accepts. The pointer-to-member indirection keeps the two from
 * drifting apart - one table names the slots AND locates them. */
struct ActionSlot {
  const char *name;
  Action LayerRuntime::*member;
};

static const ActionSlot ACTION_SLOTS[] = {
    {PROTO_SLOT_KEY1, &LayerRuntime::key1},
    {PROTO_SLOT_KEY2, &LayerRuntime::key2},
    {PROTO_SLOT_CHAIN_KEY, &LayerRuntime::chain_key},
    {PROTO_SLOT_CHAIN_KEY_DOUBLE, &LayerRuntime::chain_key_double},
    {PROTO_SLOT_NAV_CLICK, &LayerRuntime::nav_click},
    {PROTO_SLOT_SCROLL_CLICK, &LayerRuntime::scroll_click},
};
static const uint8_t ACTION_SLOT_COUNT =
    (uint8_t)(sizeof(ACTION_SLOTS) / sizeof(ACTION_SLOTS[0]));

static bool slotByName(const char *name, uint8_t *out)
{
  if (name == NULL) return false;
  for (uint8_t i = 0; i < ACTION_SLOT_COUNT; i++) {
    if (strcmp(ACTION_SLOTS[i].name, name) == 0) {
      if (out != NULL) *out = i;
      return true;
    }
  }
  return false;
}

/* One action object, exactly the `A` shape PROTOCOL.md documents. `mods` is
 * always present, even when empty, so a host can read `a.mods.length` without
 * a guard; `key` is "" for an unbound slot (and for any code the vocabulary
 * has no name for) rather than absent, for the same reason. */
static void outAction(const Action &a)
{
  char key[FLOW_KEYNAME_MAX + 1];
  actionToKeyName(a, key, sizeof(key));

  outPrintf("{\"type\":\"%s\",\"key\":", actionTypeName(a.type));
  outJsonStr(key);

  outPrintf(",\"mods\":[");
  bool first = true;
  for (uint8_t i = 0; i < FLOW_MOD_COUNT; i++) {
    uint8_t bit = 0;
    if (!modByName(modNameAt(i), &bit)) continue;
    if ((a.mods & bit) == 0) continue;
    if (!first) outChar(',');
    first = false;
    outPrintf("\"%s\"", modNameAt(i));
  }
  outPrintf("],\"fn\":\"%s\"}", fnClassName(a.fn));
}

static void outLayerObject(uint8_t index)
{
  const LayerRuntime &L = layerAt(index);

  uint8_t rgb[3];
  layerRgb(index, rgb);

  outPrintf("{\"index\":%u,\"name\":", (unsigned)index);
  outJsonStr(L.name);
  outPrintf(",\"rgb\":[%u,%u,%u]", (unsigned)rgb[0], (unsigned)rgb[1], (unsigned)rgb[2]);
  outPrintf(",\"nav_mode\":\"%s\",\"scroll_mode\":\"%s\",\"angle_mode\":\"%s\"",
            navModeName(L.nav_mode), scrollModeName(L.scroll_mode),
            angleModeName(L.angle_mode));

  outPrintf(",\"actions\":{");
  for (uint8_t s = 0; s < ACTION_SLOT_COUNT; s++) {
    if (s) outChar(',');
    outPrintf("\"%s\":", ACTION_SLOTS[s].name);
    outAction(L.*(ACTION_SLOTS[s].member));
  }
  outPrintf("}}");
}

/* ================================================================== */
/* Value extraction with range checks                                  */
/* ================================================================== */

static bool jsonRgb(JsonVariantConst v, uint8_t out[3])
{
  if (!v.is<JsonArrayConst>()) return false;
  JsonArrayConst a = v.as<JsonArrayConst>();
  if (a.size() != 3) return false;

  for (uint8_t i = 0; i < 3; i++) {
    JsonVariantConst e = a[i];
    if (!e.is<long>()) return false;
    long n = e.as<long>();
    if (n < 0 || n > 255) return false;
    out[i] = (uint8_t)n;
  }
  return true;
}

static bool jsonInt(JsonVariantConst v, long lo, long hi, long *out)
{
  if (!v.is<long>()) return false;
  long n = v.as<long>();
  if (n < lo || n > hi) return false;
  *out = n;
  return true;
}

static bool jsonBool(JsonVariantConst v, bool *out)
{
  if (!v.is<bool>()) return false;
  *out = v.as<bool>();
  return true;
}

static bool jsonSign(JsonVariantConst v, int8_t *out)
{
  long n = 0;
  if (!jsonInt(v, -1, 1, &n) || n == 0) return false;
  *out = (int8_t)n;
  return true;
}

/* ================================================================== */
/* set                                                                 */
/* ================================================================== */

/*
 * Applies one `set`. Returns false with *err pointing at a literal describing
 * why; the caller turns that into {"ev":"err","msg":...}. Every branch either
 * writes a fully validated value into g_cfg or leaves g_cfg untouched - there
 * is no partially applied path.
 */
static bool applySet(const char *path, JsonVariantConst v, const char **err)
{
  uint8_t rgb[3];
  long    n    = 0;
  bool    flag = false;

  *err = "bad value";

  /* ---- indexed: layer_rgb.N ---- */
  if (strncmp(path, "layer_rgb.", 10) == 0) {
    const char *idx = path + 10;
    if (idx[0] < '0' || idx[0] > '9' || idx[1] != '\0') {
      *err = "bad layer index";
      return false;
    }
    uint8_t i = (uint8_t)(idx[0] - '0');
    if (i >= layerCount()) {
      *err = "bad layer index";
      return false;
    }
    if (!jsonRgb(v, rgb)) {
      *err = "expected [r,g,b] 0..255";
      return false;
    }
    memcpy(g_cfg.layer_rgb[i], rgb, sizeof(rgb));
    return true;
  }

  /* ---- dotted: fn_rgb.NAME ---- */
  if (strncmp(path, "fn_rgb.", 7) == 0) {
    uint8_t fn = fnByName(path + 7);
    if (fn == FN_NONE) {
      *err = "unknown fn";
      return false;
    }
    if (!jsonRgb(v, rgb)) {
      *err = "expected [r,g,b] 0..255";
      return false;
    }
    memcpy(g_cfg.fn_rgb[fn], rgb, sizeof(rgb));
    return true;
  }

  if (strcmp(path, "hold_rgb") == 0) {
    if (!jsonRgb(v, rgb)) {
      *err = "expected [r,g,b] 0..255";
      return false;
    }
    memcpy(g_cfg.hold_rgb, rgb, sizeof(rgb));
    return true;
  }

  /* ---- scalars ---- */
  if (strcmp(path, "led_idle_pct") == 0) {
    if (!jsonInt(v, 0, 100, &n)) {
      *err = "expected 0..100";
      return false;
    }
    g_cfg.led_idle_pct = (uint8_t)n;
    return true;
  }

  if (strcmp(path, "led_flash_ms") == 0) {
    if (!jsonInt(v, 0, 2000, &n)) {
      *err = "expected 0..2000";
      return false;
    }
    g_cfg.led_flash_ms = (uint16_t)n;
    return true;
  }

  if (strcmp(path, "node_leds") == 0) {
    if (!jsonBool(v, &flag)) {
      *err = "expected bool";
      return false;
    }
    g_cfg.node_leds = flag ? 1 : 0;
    return true;
  }

  if (strcmp(path, "mono_brightness") == 0) {
    if (!jsonInt(v, 0, 7, &n)) {
      *err = "expected 0..7";
      return false;
    }
    g_cfg.mono_brightness = (uint8_t)n;
    return true;
  }

  if (strcmp(path, "mono_rotation") == 0) {
    if (!jsonInt(v, 0, 270, &n) || (n != 0 && n != 90 && n != 180 && n != 270)) {
      *err = "expected 0|90|180|270";
      return false;
    }
    g_cfg.mono_rotation = (uint16_t)n;
    return true;
  }

  if (strcmp(path, "mono_idle") == 0) {
    const char *s = v.is<const char *>() ? v.as<const char *>() : NULL;
    if (s != NULL && strcmp(s, PROTO_MONO_IDLE_BLANK) == 0) {
      g_cfg.mono_idle = MONO_IDLE_BLANK;
      return true;
    }
    if (s != NULL && strcmp(s, PROTO_MONO_IDLE_LETTER) == 0) {
      g_cfg.mono_idle = MONO_IDLE_LETTER;
      return true;
    }
    *err = "expected \"blank\"|\"letter\"";
    return false;
  }

  if (strcmp(path, "double_tap") == 0) {
    if (!jsonBool(v, &flag)) {
      *err = "expected bool";
      return false;
    }
    g_cfg.double_tap = flag ? 1 : 0;
    return true;
  }

  if (strcmp(path, "double_tap_ms") == 0) {
    if (!jsonInt(v, 50, 2000, &n)) {
      *err = "expected 50..2000";
      return false;
    }
    g_cfg.double_tap_ms = (uint16_t)n;
    return true;
  }

  if (strcmp(path, "layer_hold_ms") == 0) {
    if (!jsonInt(v, 200, 5000, &n)) {
      *err = "expected 200..5000";
      return false;
    }
    g_cfg.layer_hold_ms = (uint16_t)n;
    return true;
  }

  /* Takes effect at the next boot, by definition - the host is expected to
   * follow it with `save`. */
  if (strcmp(path, "boot_layer") == 0) {
    if (!jsonInt(v, 0, (long)layerCount() - 1, &n)) {
      *err = "bad layer index";
      return false;
    }
    g_cfg.boot_layer = (uint8_t)n;
    return true;
  }

  /* ---- axis signs ---- */
  {
    int8_t *sign = NULL;
    if (strcmp(path, "nav_x_sign") == 0) sign = &g_cfg.nav_x_sign;
    else if (strcmp(path, "nav_y_sign") == 0) sign = &g_cfg.nav_y_sign;
    else if (strcmp(path, "scroll_x_sign") == 0) sign = &g_cfg.scroll_x_sign;
    else if (strcmp(path, "scroll_y_sign") == 0) sign = &g_cfg.scroll_y_sign;
    else if (strcmp(path, "mouse_y_sign") == 0) sign = &g_cfg.mouse_y_sign;

    if (sign != NULL) {
      if (!jsonSign(v, sign)) {
        *err = "expected -1|1";
        return false;
      }
      return true;
    }
  }

  if (strcmp(path, "led_index_key1") == 0) {
    if (!jsonInt(v, 0, 1, &n)) {
      *err = "expected 0|1";
      return false;
    }
    g_cfg.led_index_key1 = (uint8_t)n;
    return true;
  }

  /* ---- mounting orientation (proto 2) ---- */

  /* All three are live. `swap_keys` is picked up by keysApplySwap() from
   * settingsApplyAll() - which the caller runs right after this returns - and
   * the two axis swaps are read in the poll path, so they take effect on the
   * next stick sample. Nothing here needs a reboot, which is the point. */
  {
    uint8_t *flag_field = NULL;
    if (strcmp(path, "swap_keys") == 0) flag_field = &g_cfg.swap_keys;
    else if (strcmp(path, "nav_swap_xy") == 0) flag_field = &g_cfg.nav_swap_xy;
    else if (strcmp(path, "scroll_swap_xy") == 0) flag_field = &g_cfg.scroll_swap_xy;

    if (flag_field != NULL) {
      if (!jsonBool(v, &flag)) {
        *err = "expected bool";
        return false;
      }
      *flag_field = flag ? 1 : 0;
      return true;
    }
  }

  *err = "unknown path";
  return false;
}

/* ================================================================== */
/* Commands                                                            */
/* ================================================================== */

/* millis() for the current protoTick(), so handlers do not each call it. */
static uint32_t g_now = 0;

static void captureId(JsonVariantConst v)
{
  idClear();
  if (v.isNull()) return;

  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    if (s == NULL) return;
    strncpy(g_id_str, s, sizeof(g_id_str) - 1);
    g_id_str[sizeof(g_id_str) - 1] = '\0';
    g_id_is_str                    = true;
    g_id_present                   = true;
    return;
  }
  if (v.is<long>()) {
    g_id_num     = v.as<long>();
    g_id_is_str  = false;
    g_id_present = true;
  }
  /* Anything else (object, array, bool) is simply not echoed. */
}

static void cmdHello(void)
{
  hostSeen(g_now);
  replyBegin(PROTO_EV_HELLO);
  outPrintf(",\"fw\":\"%s\",\"proto\":%u,\"layers\":[", FLOW_SIDECAR_VERSION,
            (unsigned)FLOW_SIDECAR_PROTO);
  for (uint8_t i = 0; i < layerCount(); i++) {
    if (i) outChar(',');
    outJsonStr(layerAt(i).name);
  }
  outPrintf("],\"layer\":%u", (unsigned)g_layer);
  outNodes();
  frameEnd();
}

static void cmdStatus(void)
{
  replyBegin(PROTO_EV_STATUS);
  outPrintf(",\"layer\":%u,\"vbat_mv\":%u,\"usb_mv\":%u", (unsigned)g_layer,
            (unsigned)batteryMilliVolts(), (unsigned)usbMilliVolts());
  outPrintf(",\"holds\":[%s,%s]", boolName(g_key[0].hold_active),
            boolName(g_key[1].hold_active));
  outNodes();
  /* The companion is not a Chain node, so it cannot appear in `nodes`; it gets
   * its own field. Present even when the link is compiled out, reporting
   * false - a host should not have to tell "no companion" from "no field". */
  outPrintf(",\"companion\":{\"linked\":%s}", boolName(companionLinked()));
  outPrintf(",\"uptime_s\":%lu", (unsigned long)uptimeSeconds(g_now));
  frameEnd();
}

static void cmdGetConfig(void)
{
  replyBegin(PROTO_EV_CONFIG);
  outConfig();
  frameEnd();
}

static void cmdSet(JsonVariantConst doc)
{
  JsonVariantConst pv   = doc["path"];
  const char      *path = pv.is<const char *>() ? pv.as<const char *>() : NULL;
  if (path == NULL) {
    replyErr("missing path");
    return;
  }

  const char *err = NULL;
  if (!applySet(path, doc["value"], &err)) {
    replyErr(err);
    return;
  }

  settingsApplyAll();

  replyBegin(PROTO_EV_OK);
  outPrintf(",\"path\":");
  outJsonStr(path);
  frameEnd();
}

/* ================================================================== */
/* The layout commands (proto 2)                                       */
/* ================================================================== */

/*
 * Everything a layout change has to do once it has been applied, in the one
 * order that is safe:
 *
 *   1. the REPLY is already flushed by the caller - reply and event share
 *      g_out, so a handler must never start an event mid-reply;
 *   2. settingsApplyAll() drops the LED cache and re-queues the panel writes,
 *      so a recoloured or rebound key repaints without a bus stall;
 *   3. `layout_changed` tells any OTHER host client to re-read get_layers;
 *   4. the companion gets a fresh `layer` event, which carries the rebuilt
 *      legend - that is what stops a renamed layer or a rebound key leaving a
 *      stale line on the screen, since the companion holds no table.
 */
static void layoutChanged(void)
{
  settingsApplyAll();
  protoEventLayoutChanged();
  companionEventLayer(g_layer);
}

static void cmdGetLayers(void)
{
  replyBegin(PROTO_EV_LAYERS);
  outPrintf(",\"layers\":[");
  for (uint8_t i = 0; i < layerCount(); i++) {
    if (i) outChar(',');
    outLayerObject(i);
  }
  outPrintf("]");
  frameEnd();
}

/*
 * `set_action`: rebind one slot on one layer, live.
 *
 * A hold that is already in flight is NOT disturbed: scanLocalKeys() copies
 * the Action into g_key[].hold_action at the press edge and releases from that
 * copy, and chain.cpp captures the Chain Key's single tap the same way. So the
 * gesture in progress finishes with the binding it started with, and the new
 * one takes effect from the next press - which is the only behaviour that
 * cannot strand a key down on the host.
 */
static void cmdSetAction(JsonVariantConst doc)
{
  long n = 0;
  if (!jsonInt(doc["layer"], 0, (long)layerCount() - 1, &n)) {
    replyErr("bad layer index");
    return;
  }
  uint8_t layer = (uint8_t)n;

  JsonVariantConst sv = doc["slot"];
  uint8_t          slot = 0;
  if (!slotByName(sv.is<const char *>() ? sv.as<const char *>() : NULL, &slot)) {
    replyErr("unknown slot");
    return;
  }

  JsonVariantConst av = doc["action"];
  if (!av.is<JsonObjectConst>()) {
    replyErr("missing action");
    return;
  }

  JsonVariantConst tv = av["type"];
  uint8_t          type = ACT_NONE;
  if (!actionTypeByName(tv.is<const char *>() ? tv.as<const char *>() : NULL, &type)) {
    replyErr("unknown type");
    return;
  }

  /* `fn` is optional: an action that does not say gets "custom", i.e. no
   * opinion about the colour, which is the sane default for a host that only
   * cares which key fires. */
  uint8_t          fn = FN_NONE;
  JsonVariantConst fv = av["fn"];
  if (!fv.isNull()) {
    if (!fv.is<const char *>() || !fnClassByName(fv.as<const char *>(), &fn)) {
      replyErr("unknown fn");
      return;
    }
  }

  uint8_t          mods = MOD_NONE;
  JsonVariantConst mv   = av["mods"];
  if (!mv.isNull()) {
    if (!mv.is<JsonArrayConst>()) {
      replyErr("expected mods array");
      return;
    }
    JsonArrayConst arr = mv.as<JsonArrayConst>();
    for (JsonVariantConst e : arr) {
      uint8_t bit = 0;
      if (!e.is<const char *>() || !modByName(e.as<const char *>(), &bit)) {
        replyErr("unknown mod");
        return;
      }
      mods = (uint8_t)(mods | bit);
    }
  }

  JsonVariantConst kv = av["key"];
  const char      *key = kv.is<const char *>() ? kv.as<const char *>() : NULL;

  /* Build and validate into a local FIRST: g_cfg is only written once every
   * field has resolved, so a rejected command leaves the layout untouched. */
  Action      built;
  const char *err = NULL;
  if (!keyNameToAction(type, key, mods, fn, &built, &err)) {
    replyErr((err != NULL) ? err : "bad action");
    return;
  }

  g_cfg.layers[layer].*(ACTION_SLOTS[slot].member) = built;

  replyOk();
  layoutChanged();
}

/*
 * `set_layer_meta`: rename/recolour a layer and change what its stick and knob
 * do. Every field is optional and any subset may be sent.
 *
 * Two passes on purpose - validate everything into locals, then commit - so a
 * command with a good name and a bad nav_mode changes nothing at all rather
 * than half of it.
 */
static void cmdSetLayerMeta(JsonVariantConst doc)
{
  long n = 0;
  if (!jsonInt(doc["layer"], 0, (long)layerCount() - 1, &n)) {
    replyErr("bad layer index");
    return;
  }
  uint8_t layer = (uint8_t)n;

  bool        have_name = false;
  char        name[FLOW_LAYER_NAME_MAX + 1] = {0};
  bool        have_rgb = false;
  uint8_t     rgb[3]   = {0, 0, 0};
  bool        have_nav = false, have_scroll = false, have_angle = false;
  uint8_t     nav_mode = 0, scroll_mode = 0, angle_mode = 0;

  JsonVariantConst nv = doc["name"];
  if (!nv.isNull()) {
    const char *s = nv.is<const char *>() ? nv.as<const char *>() : NULL;
    if (s == NULL || s[0] == '\0' || strlen(s) > (size_t)FLOW_LAYER_NAME_MAX) {
      replyErr("name must be 1..8 chars");
      return;
    }
    snprintf(name, sizeof(name), "%s", s);
    have_name = true;
  }

  JsonVariantConst cv = doc["rgb"];
  if (!cv.isNull()) {
    if (!jsonRgb(cv, rgb)) {
      replyErr("expected [r,g,b] 0..255");
      return;
    }
    have_rgb = true;
  }

  JsonVariantConst nmv = doc["nav_mode"];
  if (!nmv.isNull()) {
    if (!nmv.is<const char *>() || !navModeByName(nmv.as<const char *>(), &nav_mode)) {
      replyErr("expected arrows|mouse|off");
      return;
    }
    have_nav = true;
  }

  JsonVariantConst smv = doc["scroll_mode"];
  if (!smv.isNull()) {
    if (!smv.is<const char *>() || !scrollModeByName(smv.as<const char *>(), &scroll_mode)) {
      replyErr("expected wheel_pan|wheel_arrows|off");
      return;
    }
    have_scroll = true;
  }

  JsonVariantConst amv = doc["angle_mode"];
  if (!amv.isNull()) {
    if (!amv.is<const char *>() || !angleModeByName(amv.as<const char *>(), &angle_mode)) {
      replyErr("expected volume|wheel|off");
      return;
    }
    have_angle = true;
  }

  LayerRuntime &L = g_cfg.layers[layer];
  if (have_name) memcpy(L.name, name, sizeof(L.name));
  if (have_rgb) memcpy(g_cfg.layer_rgb[layer], rgb, sizeof(rgb));
  if (have_nav) L.nav_mode = nav_mode;
  if (have_scroll) L.scroll_mode = scroll_mode;
  if (have_angle) L.angle_mode = angle_mode;

  replyOk();
  layoutChanged();
}

static void cmdLayer(JsonVariantConst doc)
{
  long n = 0;
  if (!jsonInt(doc["index"], 0, (long)layerCount() - 1, &n)) {
    replyErr("bad layer index");
    return;
  }
  /* Reply first: setLayer() emits the layer event, and both use g_out. */
  replyOk();
  setLayer((uint8_t)n, g_now);
}

static void cmdIdentify(void)
{
  replyOk();
  ledFlashAll(255, 255, 255, 3, g_now);
  monoScrollText("ID", /*announce_layer=*/false);
}

static void cmdMono(JsonVariantConst doc)
{
  JsonVariantConst tv   = doc["text"];
  const char      *text = tv.is<const char *>() ? tv.as<const char *>() : NULL;
  if (text == NULL) {
    replyErr("missing text");
    return;
  }
  if (strlen(text) > (size_t)FLOW_PROTO_MONO_TEXT) {
    replyErr("text too long");
    return;
  }

  monoScrollText(text, /*announce_layer=*/false);
  replyOk();
}

/*
 * The host telling the device what IT is doing (PROTOCOL.md `state`).
 *
 * Also refreshes the "a host is attached" window, like `hello` - which is what
 * keeps the device from falling back to its own checkmark during a long
 * session, since a host normally says `hello` exactly once.
 */
static void cmdState(JsonVariantConst doc)
{
  JsonVariantConst vv    = doc["value"];
  const char      *value = vv.is<const char *>() ? vv.as<const char *>() : NULL;
  if (value == NULL) {
    replyErr("missing value");
    return;
  }

  uint8_t state = 0;
  if (!hostStateByName(value, &state)) {
    replyErr("unknown state");
    return;
  }

  /* Reply first: hostStateSet() paints, and both use g_out via FLOG. */
  replyOk();
  hostSeen(g_now);
  hostStateSet(state, g_now);
}

static void handleJsonLine(const char *line)
{
  JsonDocument         doc;
  DeserializationError derr = deserializeJson(doc, line);

  idClear();
  if (derr) {
    replyErr("bad json");
    return;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  captureId(root["id"]);

  JsonVariantConst cv  = root["cmd"];
  const char      *cmd = cv.is<const char *>() ? cv.as<const char *>() : NULL;
  if (cmd == NULL) {
    replyErr("missing cmd");
    return;
  }

  if (strcmp(cmd, "hello") == 0) {
    cmdHello();
  } else if (strcmp(cmd, "status") == 0) {
    cmdStatus();
  } else if (strcmp(cmd, "get_config") == 0) {
    cmdGetConfig();
  } else if (strcmp(cmd, "set") == 0) {
    cmdSet(root);
  } else if (strcmp(cmd, "save") == 0) {
    if (settingsSave()) {
      replyOk();
    } else {
      replyErr("nvs write failed");
    }
  } else if (strcmp(cmd, "reset_config") == 0) {
    if (settingsReset()) {
      replyOk();
    } else {
      replyErr("nvs clear failed");
    }
  } else if (strcmp(cmd, "get_layers") == 0) {
    cmdGetLayers();
  } else if (strcmp(cmd, "set_action") == 0) {
    cmdSetAction(root);
  } else if (strcmp(cmd, "set_layer_meta") == 0) {
    cmdSetLayerMeta(root);
  } else if (strcmp(cmd, "layer") == 0) {
    cmdLayer(root);
  } else if (strcmp(cmd, "identify") == 0) {
    cmdIdentify();
  } else if (strcmp(cmd, "mono") == 0) {
    cmdMono(root);
  } else if (strcmp(cmd, "state") == 0) {
    cmdState(root);
  } else {
    replyErr("unknown cmd");
  }
}

/* ================================================================== */
/* Reader state                                                        */
/* ================================================================== */

static char     g_line[FLOW_PROTO_MAX_LINE + 1];
static uint16_t g_line_len  = 0;
static bool     g_line_drop = false; /* overlong: swallow through the newline */
static bool     g_in_json   = false; /* this line started with '{'            */

static uint32_t g_batt_ev_at    = 0;
static bool     g_batt_ev_armed = false;

/* PROTOCOL.md defines `low` as a plain "< 3500 mV". That is deliberately NOT
 * batteryLow(), which carries BATT_HYST_MV of hysteresis so the LED tint does
 * not flap; the host gets the raw predicate and can smooth it as it likes. */
static void eventBattery(void)
{
  uint16_t mv = batteryMilliVolts();
  eventBegin(PROTO_EV_BATTERY);
  outPrintf(",\"vbat_mv\":%u,\"low\":%s", (unsigned)mv, boolName(mv < BATT_LOW_MV));
  frameEnd();
}

#endif /* FLOW_SERIAL_PROTO */

/* ================================================================== */
/* Events (public)                                                     */
/* ================================================================== */

void protoEventLayer(uint8_t layer)
{
#if FLOW_SERIAL_PROTO
  if (layer >= layerCount()) return;
  eventBegin(PROTO_EV_LAYER);
  outPrintf(",\"index\":%u,\"name\":", (unsigned)layer);
  outJsonStr(layerAt(layer).name);
  frameEnd();
#else
  (void)layer;
#endif
}

/* No payload: the editing client already knows what it sent, and any other
 * client needs the whole table rather than a diff it would have to merge. */
void protoEventLayoutChanged(void)
{
#if FLOW_SERIAL_PROTO
  eventBegin(PROTO_EV_LAYOUT_CHANGED);
  frameEnd();
#endif
}

void protoEventHold(uint8_t key_1based, bool active, uint8_t fn)
{
#if FLOW_SERIAL_PROTO
  eventBegin(PROTO_EV_HOLD);
  outPrintf(",\"key\":%u,\"active\":%s", (unsigned)key_1based, boolName(active));
  /* An unmapped hold has no function name; the field is omitted rather than
   * sent as null, so the host can simply test for presence. */
  const char *name = fnName(fn);
  if (name != NULL) outPrintf(",\"fn\":\"%s\"", name);
  frameEnd();
#else
  (void)key_1based;
  (void)active;
  (void)fn;
#endif
}

void protoEventTap(const char *key_name)
{
#if FLOW_SERIAL_PROTO
  if (key_name == NULL) return;
  eventBegin(PROTO_EV_TAP);
  outPrintf(",\"key\":");
  outJsonStr(key_name);
  frameEnd();
#else
  (void)key_name;
#endif
}

void protoEventChain(void)
{
#if FLOW_SERIAL_PROTO
  eventBegin(PROTO_EV_CHAIN);
  outNodes();
  frameEnd();
#endif
}

/* ================================================================== */
/* Entry points                                                        */
/* ================================================================== */

void protoBegin(void)
{
#if FLOW_SERIAL_PROTO
  outReset();
  g_line_len      = 0;
  g_line_drop     = false;
  g_in_json       = false;
  g_batt_ev_armed = false;

  /* The `boot` event. USB CDC may well not be open yet - nothing is waiting
   * for an ACK and nothing blocks, so this is a best-effort announcement. A
   * host that attaches later gets the same information from `hello`. */
  eventBegin(PROTO_EV_BOOT);
  outPrintf(",\"fw\":\"%s\"", FLOW_SIDECAR_VERSION);
  frameEnd();
#endif
}

void protoTick(uint32_t now)
{
#if FLOW_SERIAL_PROTO
  g_now = now;
#else
  (void)now;
#endif

  while (Serial.available() > 0) {
    int ci = Serial.read();
    if (ci < 0) break;
    char c = (char)ci;

#if FLOW_SERIAL_PROTO
    if (c == '\n' || c == '\r') {
      if (g_line_drop) {
        replyErr("line too long");
      } else if (g_in_json && g_line_len > 0) {
        g_line[g_line_len] = '\0';
        handleJsonLine(g_line);
      }
      g_line_len  = 0;
      g_line_drop = false;
      g_in_json   = false;
      continue;
    }

    /* The FIRST byte of a line decides which conversation it belongs to. */
    if (!g_in_json && g_line_len == 0) {
      if (c != '{') {
        consoleChar(c);
        continue;
      }
      g_in_json = true;
    }

    if (g_line_drop) continue;
    if (g_line_len >= FLOW_PROTO_MAX_LINE) {
      /* Drop, do not truncate: half a command must never parse. */
      g_line_drop = true;
      g_line_len  = 0;
      continue;
    }
    g_line[g_line_len++] = c;
#else
    consoleChar(c);
#endif
  }

#if FLOW_SERIAL_PROTO
  /* ---- periodic battery event ---- */
  if (!g_batt_ev_armed) {
    g_batt_ev_armed = true;
    g_batt_ev_at    = now; /* first report as soon as the loop is running */
  }
  if ((int32_t)(now - g_batt_ev_at) >= 0) {
    g_batt_ev_at = now + FLOW_PROTO_BATT_EVENT_MS;
    eventBattery();
  }
#endif
}
