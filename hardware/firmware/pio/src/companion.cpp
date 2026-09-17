/*
 * companion.cpp - the companion display link.
 *
 * See companion.h for what the companion is and why the link probes at all,
 * and COMPANION.md for the wiring and the user-facing behaviour.
 *
 * Three rules, the same three serial_proto.cpp works to:
 *
 *   1. NEVER BLOCK. The reader drains whatever the UART has buffered and stops
 *      at the first COMPLETE line; a half-arrived command stays half-arrived.
 *      With nothing plugged in, companionTick() costs one available() check
 *      plus a beacon every COMPANION_PROBE_MS.
 *
 *   2. NO HEAP IN THE STEADY STATE. Every outbound line is assembled with
 *      snprintf into the single static g_out. Only deserializeJson() allocates,
 *      and only while a companion command is being parsed.
 *
 *   3. ONE LINE PARSED PER LOOP. companionRead() returns as soon as it has
 *      handled one, so a chatty companion cannot starve key scanning.
 *
 * A command handler must not be mid-way through building a line when it calls
 * something that emits one. In practice only `hello` does both, and it flushes
 * its reply before the follow-up snapshot events.
 */

#include "sidecar.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if FLOW_COMPANION
#include <ArduinoJson.h>
#include <driver/gpio.h> /* gpio_reset_pin() - see openOrder() */
#endif

#if FLOW_COMPANION

/* ================================================================== */
/* Link state                                                          */
/* ================================================================== */

static bool     g_open    = false; /* the UART has been begin()'d at all    */
static bool     g_linked  = false; /* a companion has answered recently     */
static bool     g_probed  = false; /* at least one beacon has gone out      */
static uint8_t  g_order   = 0;     /* 0 = (rx=pin_a, tx=pin_b), 1 = swapped */
static int8_t   g_pin_a   = -1;    /* the port pair the chain did NOT take  */
static int8_t   g_pin_b   = -1;
static int8_t   g_rx      = -1;    /* what the UART is actually open on     */
static int8_t   g_tx      = -1;

static uint32_t g_last_rx     = 0;
static uint32_t g_next_beacon = 0;
static uint32_t g_next_ping   = 0;
#ifndef COMPANION_PING_MS
#define COMPANION_PING_MS 3000
#endif
static uint32_t g_next_batt   = 0;

/* Knob forwarding (0.3.0). The knob can cross a detent every few milliseconds
 * during a fast sweep, which would be both pointless and rude on a link that
 * also carries taps. Movements are coalesced: the newest detent is held here
 * and companionTick() emits it once the gap has elapsed, so the rate is capped
 * without the final resting position ever being dropped. */
static bool     g_knob_pending  = false;
static uint8_t  g_knob_detent   = 0;
static uint32_t g_knob_next_tx  = 0;

/* ================================================================== */
/* Outbound line assembly                                              */
/* ================================================================== */

static char     g_out[COMPANION_OUT_BYTES];
static uint16_t g_out_len   = 0;
static bool     g_out_trunc = false;

static void outReset(void)
{
  g_out_len   = 0;
  g_out[0]    = '\0';
  g_out_trunc = false;
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

static void evBegin(const char *ev)
{
  outReset();
  outPrintf("{\"ev\":\"%s\"", ev);
}

/* A truncated line would be malformed JSON, which is worse for the companion
 * than no line at all - it would have to resynchronise. Drop it instead; the
 * next event carries the same information. */
static void evEnd(void)
{
  if (!g_out_trunc && g_out_len + 1 < (uint16_t)sizeof(g_out)) {
    g_out[g_out_len++] = '}';
    g_out[g_out_len]   = '\0';
    COMPANION_UART.write((const uint8_t *)g_out, g_out_len);
    COMPANION_UART.print("\n");
  }
  outReset();
}

/* ================================================================== */
/* Messages out                                                        */
/* ================================================================== */

/*
 * The key legend for `layer`, as `"legend":["..","..","..",".."]`.
 *
 * Every string comes from layerLegend(), i.e. from the layer table itself, and
 * it rides along with `hello` and every `layer` event. That is the whole
 * anti-drift argument for the feature: the companion renders what it is given
 * and holds no table of its own, so a binding cannot be changed in layers.cpp
 * and left stale on the screen.
 *
 * None of these strings can contain a character JSON would need escaped - they
 * are built from ASCII literals in layers.cpp - so they are emitted raw. If a
 * label ever grows a quote or a backslash, escape it here.
 */
static void outLegend(uint8_t layer)
{
  char row[COMPANION_LEGEND_MAX + 1];

  outPrintf(",\"legend\":[");
  for (uint8_t s = 0; s < COMPANION_LEGEND_SLOTS; s++) {
    layerLegend(layer, s, row, sizeof(row));
    outPrintf("%s\"%s\"", (s ? "," : ""), row);
  }
  outPrintf("]");
}

/*
 * The beacon, and the reply to `hello`. Carries everything the companion needs
 * to draw its first frame without asking anything else: the firmware version,
 * the layer names (so it can label a layer it has never seen), the current
 * layer, that layer's RUNTIME colour and its key legend.
 */
static void sendHello(void)
{
  uint8_t rgb[3];
  layerRgb(g_layer, rgb);

  evBegin(COMPANION_EV_HELLO);
  outPrintf(",\"fw\":\"%s\",\"layers\":[", FLOW_SIDECAR_VERSION);
  for (uint8_t i = 0; i < layerCount(); i++) {
    outPrintf("%s\"%s\"", (i ? "," : ""), layerAt(i).name);
  }
  outPrintf("],\"layer\":%u,\"rgb\":[%u,%u,%u]", (unsigned)g_layer, (unsigned)rgb[0],
            (unsigned)rgb[1], (unsigned)rgb[2]);
  outLegend(g_layer);
  evEnd();
}

static void sendBattery(void)
{
  evBegin(COMPANION_EV_BATTERY);
  outPrintf(",\"vbat_mv\":%u", (unsigned)batteryMilliVolts());
  evEnd();
}

void companionEventLayer(uint8_t layer)
{
  if (!g_linked || layer >= layerCount()) return;

  uint8_t rgb[3];
  layerRgb(layer, rgb);

  evBegin(COMPANION_EV_LAYER);
  outPrintf(",\"index\":%u,\"name\":\"%s\",\"rgb\":[%u,%u,%u]", (unsigned)layer,
            layerAt(layer).name, (unsigned)rgb[0], (unsigned)rgb[1], (unsigned)rgb[2]);
  outLegend(layer);
  evEnd();
}

/*
 * A tap fired. Sent from the same places protoEventTap() is, so the host and
 * the companion cannot be told different stories about which control moved.
 *
 * `label` is deliberately actionLabel()'s prose and not the FnId name: the
 * companion draws a small icon for the bindings that have an obvious one, and
 * every control on the MEDIA layer shares FN_MEDIA, so the FnId cannot tell
 * "play/pause" from "mute".
 */
void companionEventTap(const char *key_name, const char *label)
{
  if (!g_linked || key_name == NULL) return;

  evBegin(COMPANION_EV_TAP);
  outPrintf(",\"key\":\"%s\"", key_name);
  if (label != NULL) outPrintf(",\"fn\":\"%s\"", label);
  evEnd();
}

/*
 * A knob detent changed. Only the newest value is kept; companionTick() does
 * the sending, once per COMPANION_KNOB_MIN_GAP_MS. Coalescing rather than
 * dropping matters because the interesting value is where the knob STOPPED,
 * which is by definition the last one.
 */
void companionEventKnob(uint8_t detent)
{
  if (!g_linked) return;
  g_knob_detent  = detent;
  g_knob_pending = true;
}

static void sendKnob(void)
{
  evBegin(COMPANION_EV_KNOB);
  outPrintf(",\"detent\":%u,\"of\":%u", (unsigned)g_knob_detent, (unsigned)ANGLE_DETENTS);
  evEnd();
}

void companionEventState(uint8_t host_state)
{
  if (!g_linked) return;
  const char *name = hostStateName(host_state);
  if (name == NULL) return;

  evBegin(COMPANION_EV_STATE);
  outPrintf(",\"value\":\"%s\"", name);
  evEnd();
}

void companionEventHold(uint8_t key_1based, bool active)
{
  if (!g_linked) return;
  evBegin(COMPANION_EV_HOLD);
  outPrintf(",\"key\":%u,\"active\":%s", (unsigned)key_1based, active ? "true" : "false");
  evEnd();
}

void companionEventChain(void)
{
  if (!g_linked) return;
  evBegin(COMPANION_EV_CHAIN);
  outPrintf(",\"count\":%u", (unsigned)chainNodeCount());
  evEnd();
}

/* ================================================================== */
/* Port selection                                                      */
/* ================================================================== */

/*
 * The chain's auto-probe has already run by the time this is called, so the
 * complement is simply "whichever pair it is not on". Both port pairs are named
 * in config.h; only their identity matters here, not their order.
 */
static void selectPort(void)
{
  int8_t crx = -1, ctx = -1;
  chainBusPins(&crx, &ctx);

  bool chain_on_port2 = (crx == CHAIN_RX_PIN || crx == CHAIN_TX_PIN || ctx == CHAIN_RX_PIN ||
                         ctx == CHAIN_TX_PIN);
  if (chain_on_port2) {
    g_pin_a = (int8_t)CHAIN_LEFT_RX_PIN;
    g_pin_b = (int8_t)CHAIN_LEFT_TX_PIN;
  } else {
    g_pin_a = (int8_t)CHAIN_RX_PIN;
    g_pin_b = (int8_t)CHAIN_TX_PIN;
  }
}

static void openOrder(uint8_t order)
{
  g_order = (uint8_t)(order & 1u);
  g_rx    = g_order ? g_pin_b : g_pin_a;
  g_tx    = g_order ? g_pin_a : g_pin_b;

  if (g_open) COMPANION_UART.end();
  /* HardwareSerial::end() does not reliably return the old TX pin to a plain
   * input; the GPIO matrix can leave it driven by the UART's TX signal. When
   * that pin then becomes RX, the receiver only ever sees its own idle level
   * and the companion's bytes never arrive - on both orders, for good. Seen
   * on hardware 2026-09-15 (link only ever worked when the very first order
   * was right). Reset both pins to a clean state before every begin(). */
  gpio_reset_pin((gpio_num_t)g_pin_a);
  gpio_reset_pin((gpio_num_t)g_pin_b);
  pinMode(g_pin_a, INPUT_PULLUP);
  pinMode(g_pin_b, INPUT_PULLUP);
  COMPANION_UART.begin(COMPANION_BAUD, SERIAL_8N1, g_rx, g_tx);
  g_open = true;

  FLOG("[companion] uart rx=G%d tx=G%d\r\n", (int)g_rx, (int)g_tx);
}

/* ================================================================== */
/* Reader                                                              */
/* ================================================================== */

static char     g_line[COMPANION_MAX_LINE + 1];
static uint16_t g_line_len  = 0;
static bool     g_line_drop = false;
static bool     g_in_json   = false;

static void handleLine(const char *line, uint32_t now)
{
  JsonDocument doc;
  if (deserializeJson(doc, line)) return; /* silence is the right answer here */

  JsonVariantConst root = doc.as<JsonVariantConst>();
  JsonVariantConst cv   = root["cmd"];
  const char      *cmd  = cv.is<const char *>() ? cv.as<const char *>() : NULL;
  if (cmd == NULL) return;

  /* Any well-formed command counts as proof of life, including one we go on to
   * reject - the companion is clearly there and talking. */
  g_last_rx = now;
  if (!g_linked) {
    g_linked = true;
    FLOG("[companion] linked (rx=G%d tx=G%d)\r\n", (int)g_rx, (int)g_tx);
  }

  if (strcmp(cmd, COMPANION_CMD_HELLO) == 0) {
    /* Reply first, then push the rest of the current state, so a companion that
     * was plugged in mid-session draws a correct frame immediately instead of
     * waiting for the next change. */
    sendHello();
    companionEventState(hostState());
    /* Hold is edge-driven, so a companion that appears mid-dictation would
     * otherwise not learn about the key that is already down until it came back
     * up - and would then be told "released" for a hold it never saw start. */
    for (uint8_t k = 0; k < 2; k++) {
      if (g_key[k].hold_active) companionEventHold((uint8_t)(k + 1), true);
    }
    companionEventChain();
    sendBattery();
    g_next_batt = now + COMPANION_BATT_EVENT_MS;
    return;
  }

  if (strcmp(cmd, COMPANION_CMD_PING) == 0) {
    return; /* keepalive only; g_last_rx above is the whole effect */
  }

  if (strcmp(cmd, COMPANION_CMD_LAYER_NEXT) == 0) {
    if (layerCount() > 1) cycleLayer(now);
    return;
  }

  if (strcmp(cmd, COMPANION_CMD_LAYER_PREV) == 0) {
    uint8_t n = layerCount();
    if (n > 1) setLayer((uint8_t)((g_layer + n - 1) % n), now);
    return;
  }

  if (strcmp(cmd, COMPANION_CMD_LAYER) == 0) {
    JsonVariantConst iv = root["index"];
    if (!iv.is<long>()) return;
    long n = iv.as<long>();
    if (n < 0 || n >= (long)layerCount()) return;
    setLayer((uint8_t)n, now);
    return;
  }

  if (strcmp(cmd, COMPANION_CMD_IDENTIFY) == 0) {
    ledFlashAll(255, 255, 255, 3, now);
    monoScrollText("ID", /*announce_layer=*/false);
    return;
  }
}

/* Returns as soon as ONE complete line has been handled. */
static void readLines(uint32_t now)
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
        return; /* one line per loop() */
      }
      continue;
    }

    /* The first byte of a line decides whether it is for us at all. Anything
     * not starting with '{' is the companion's own human output and is
     * swallowed. */
    if (!g_in_json && g_line_len == 0) {
      if (c != '{') continue;
      g_in_json = true;
    }

    if (g_line_drop) continue;
    if (g_line_len >= COMPANION_MAX_LINE) {
      /* Drop, do not truncate: half a command must never parse. */
      g_line_drop = true;
      g_line_len  = 0;
      continue;
    }
    g_line[g_line_len++] = c;
  }
}

/* ================================================================== */
/* Entry points                                                        */
/* ================================================================== */

bool companionLinked(void) { return g_linked; }

void companionBegin(uint32_t now)
{
  outReset();
  selectPort();
  openOrder(0);

  g_linked       = false;
  g_probed       = false;
  g_last_rx      = now;
  g_next_beacon  = now;
  g_next_batt    = now + COMPANION_BATT_EVENT_MS;
  g_knob_pending = false;
  g_knob_next_tx = now;
}

void companionTick(uint32_t now)
{
  if (!g_open) return;

  /* The chain re-probes its own pin pairs whenever it loses the bus, and its
   * candidate list includes the pair we are sitting on. If it has moved onto
   * us, move. Two compares per loop, and the alternative is two UARTs driving
   * one wire. */
  {
    int8_t crx = -1, ctx = -1;
    chainBusPins(&crx, &ctx);
    if (crx == g_pin_a || crx == g_pin_b || ctx == g_pin_a || ctx == g_pin_b) {
      FLOG("[companion] chain moved onto our pins - reselecting\r\n");
      selectPort();
      openOrder(0);
      g_linked      = false;
      g_probed      = false;
      g_next_beacon = now;
      return;
    }
  }

  readLines(now);

  if (g_linked && (uint32_t)(now - g_last_rx) > (uint32_t)COMPANION_LINK_TIMEOUT_MS) {
    FLOG("[companion] link lost\r\n");
    g_linked = false;
    /* Keep the pin order that was working: the companion was probably just
     * unplugged, and it will answer on the same order when it comes back. */
    g_probed      = false;
    g_next_beacon = now;
  }

  if (!g_linked) {
    /* A queued detent must not survive the link going down: by the time a
     * companion comes back it is stale, and it would arrive before the `hello`
     * that tells the screen which layer it belongs to. */
    g_knob_pending = false;

    if ((int32_t)(now - g_next_beacon) >= 0) {
      /* Alternate the order on every UNANSWERED attempt. The first beacon after
       * begin() or after a link loss goes out on the current order. */
      if (g_probed) openOrder((uint8_t)(g_order + 1));
      g_next_beacon = now + COMPANION_PROBE_MS;
      g_probed      = true;
      sendHello();
    }
    return;
  }

  if (g_knob_pending && (int32_t)(now - g_knob_next_tx) >= 0) {
    g_knob_next_tx = now + COMPANION_KNOB_MIN_GAP_MS;
    g_knob_pending = false;
    sendKnob();
  }

  if ((int32_t)(now - g_next_batt) >= 0) {
    g_next_batt = now + COMPANION_BATT_EVENT_MS;
    sendBattery();
  }

  /* Heartbeat. The companion drops the link after COMPANION_LINK_TIMEOUT_MS
   * (10 s on the Atom) without ANY inbound line, and at idle nothing above
   * fires for up to 30 s - seen on hardware 2026-09-15 as the Atom flipping to
   * "waiting for sidecar" every half minute. Any line resets its timer, so a
   * bare event every 3 s keeps it linked. */
  if ((int32_t)(now - g_next_ping) >= 0) {
    g_next_ping = now + COMPANION_PING_MS;
    evBegin("ping");
    evEnd();
  }
}

void companionPrintStatus(void)
{
  uint32_t age = millis() - g_last_rx;
  Serial.printf("compan.: %s, rx=G%d tx=G%d, last rx %lums ago\r\n",
                g_linked ? "linked" : "probing", (int)g_rx, (int)g_tx,
                (unsigned long)age);
}

#else /* !FLOW_COMPANION */

/* Empty stubs: the call sites stay unconditional and the linker drops the
 * module's state entirely. */

bool companionLinked(void) { return false; }
void companionBegin(uint32_t now) { (void)now; }
void companionTick(uint32_t now) { (void)now; }
void companionPrintStatus(void) {}
void companionEventLayer(uint8_t layer) { (void)layer; }
void companionEventState(uint8_t host_state) { (void)host_state; }
void companionEventHold(uint8_t key_1based, bool active)
{
  (void)key_1based;
  (void)active;
}
void companionEventChain(void) {}
void companionEventTap(const char *key_name, const char *label)
{
  (void)key_name;
  (void)label;
}
void companionEventKnob(uint8_t detent) { (void)detent; }

#endif /* FLOW_COMPANION */
