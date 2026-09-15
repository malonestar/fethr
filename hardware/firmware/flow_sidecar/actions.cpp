/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/src/actions.cpp
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * actions.cpp - USB HID dispatch for the Flow sidecar.
 *
 * Every release() is deferred through the pending queue below, so nothing in
 * here ever calls delay(). actionsTick() retires due releases once per loop().
 */

#include "config.h"

/* ------------------------------------------------------------------ */
/* HID devices                                                          */
/* ------------------------------------------------------------------ */

static USBHIDKeyboard        Keyboard;
static USBHIDMouse           Mouse; /* typedef of USBHIDRelativeMouse */
static USBHIDConsumerControl Consumer;

/* ------------------------------------------------------------------ */
/* Deferred-release queue                                               */
/* ------------------------------------------------------------------ */

enum PendKind : uint8_t {
  PEND_FREE = 0,
  PEND_KEY,
  PEND_CONSUMER,
  PEND_MOUSE
};

struct PendingRelease {
  uint8_t  kind;
  uint16_t code;
  uint8_t  mods;
  uint32_t due_ms;
};

static PendingRelease g_pend[FLOW_PEND_SLOTS];

/* Second half of an ACT_MOUSE_DOUBLE. A single scheduled click is enough - a
 * double-click is click, gap, click - and keeping it out of the release queue
 * means pendFire() never has to re-enter pendAdd(). */
static bool     g_dbl_armed = false;
static uint16_t g_dbl_btn   = 0;
static uint32_t g_dbl_at    = 0;

static void hidPressMods(uint8_t mods)
{
  if (mods & MOD_CTRL) Keyboard.press(KEY_LEFT_CTRL);
  if (mods & MOD_SHIFT) Keyboard.press(KEY_LEFT_SHIFT);
  if (mods & MOD_ALT) Keyboard.press(KEY_LEFT_ALT);
  if (mods & MOD_GUI) Keyboard.press(KEY_LEFT_GUI);
}

static void hidReleaseMods(uint8_t mods)
{
  if (mods & MOD_GUI) Keyboard.release(KEY_LEFT_GUI);
  if (mods & MOD_ALT) Keyboard.release(KEY_LEFT_ALT);
  if (mods & MOD_SHIFT) Keyboard.release(KEY_LEFT_SHIFT);
  if (mods & MOD_CTRL) Keyboard.release(KEY_LEFT_CTRL);
}

static void pendFire(PendingRelease &p)
{
  switch (p.kind) {
    case PEND_KEY:
      Keyboard.release((uint8_t)p.code);
      hidReleaseMods(p.mods);
      break;
    case PEND_CONSUMER:
      Consumer.release();
      break;
    case PEND_MOUSE:
      Mouse.release((uint8_t)p.code);
      break;
    default:
      break;
  }
  p.kind = PEND_FREE;
}

/* Release every outstanding item of a given kind right now. Used before
 * re-pressing something that is still down, so the host sees a real edge. */
static void pendFlushKind(uint8_t kind)
{
  for (uint8_t i = 0; i < FLOW_PEND_SLOTS; i++) {
    if (g_pend[i].kind == kind) pendFire(g_pend[i]);
  }
}

static void pendFlushKey(uint16_t code)
{
  for (uint8_t i = 0; i < FLOW_PEND_SLOTS; i++) {
    if (g_pend[i].kind == PEND_KEY && g_pend[i].code == code) pendFire(g_pend[i]);
  }
}

static void pendAdd(uint8_t kind, uint16_t code, uint8_t mods, uint32_t due_ms)
{
  for (uint8_t i = 0; i < FLOW_PEND_SLOTS; i++) {
    if (g_pend[i].kind == PEND_FREE) {
      g_pend[i].kind   = kind;
      g_pend[i].code   = code;
      g_pend[i].mods   = mods;
      g_pend[i].due_ms = due_ms;
      return;
    }
  }
  /* Queue full (should not happen with human-speed input): retire the oldest
   * slot immediately and reuse it rather than dropping the release. */
  pendFire(g_pend[0]);
  g_pend[0].kind   = kind;
  g_pend[0].code   = code;
  g_pend[0].mods   = mods;
  g_pend[0].due_ms = due_ms;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void actionsBegin(void)
{
  for (uint8_t i = 0; i < FLOW_PEND_SLOTS; i++) g_pend[i].kind = PEND_FREE;
  g_dbl_armed = false;

  Keyboard.begin();
  Mouse.begin();
  Consumer.begin();

  /*
   * USB string descriptors (v2.1). The host app finds the sidecar by VID
   * 0x303A (Espressif, left at the core default so no PID/VID registration is
   * needed) plus this product string - see PROTOCOL.md.
   *
   * Verified against arduino-esp32 3.3.11, cores/esp32/USB.h:
   *   bool productName(const char *name);
   *   bool manufacturerName(const char *name);
   * Both store into a String member that is read when the descriptors are
   * built, so they MUST be called before USB.begin(); afterwards they are
   * ignored (ESPUSB::_started gates them).
   */
  USB.productName(FLOW_USB_PRODUCT);
  USB.manufacturerName(FLOW_USB_MANUFACTURER);

  USB.begin(); /* must come after every HID device has been begun */
}

void tapKey(uint16_t code, uint8_t mods, uint32_t now)
{
  pendFlushKey(code);
  hidPressMods(mods);
  Keyboard.press((uint8_t)code);
  pendAdd(PEND_KEY, code, mods, now + FLOW_TAP_MS);
}

void tapConsumer(uint16_t usage, uint32_t now)
{
  /* USBHIDConsumerControl::release() takes no argument, so only one usage can
   * be outstanding at a time - retire any previous one first. */
  pendFlushKind(PEND_CONSUMER);
  Consumer.press(usage);
  pendAdd(PEND_CONSUMER, usage, MOD_NONE, now + FLOW_TAP_MS);
}

void tapMouseButton(uint16_t button, uint32_t now)
{
  pendFlushKind(PEND_MOUSE);
  Mouse.press((uint8_t)button);
  pendAdd(PEND_MOUSE, button, MOD_NONE, now + FLOW_TAP_MS);
}

/* Relative wheel / horizontal-pan report with no cursor movement. */
void mouseScroll(int8_t wheel, int8_t pan)
{
  Mouse.move(0, 0, wheel, pan);
}

/* Relative cursor report with no wheel movement. */
void mouseMove(int8_t dx, int8_t dy)
{
  Mouse.move(dx, dy, 0, 0);
}

void actionsTick(uint32_t now)
{
  for (uint8_t i = 0; i < FLOW_PEND_SLOTS; i++) {
    if (g_pend[i].kind != PEND_FREE && (int32_t)(now - g_pend[i].due_ms) >= 0) {
      pendFire(g_pend[i]);
    }
  }

  if (g_dbl_armed && (int32_t)(now - g_dbl_at) >= 0) {
    g_dbl_armed = false;
    tapMouseButton(g_dbl_btn, now);
  }
}

bool actionIsHold(const Action &a)
{
  return a.type == ACT_KEY_HOLD || a.type == ACT_MOUSE_HOLD;
}

void actionFire(const Action &a, uint32_t now)
{
  switch (a.type) {
    case ACT_KEY_TAP:      tapKey(a.code, MOD_NONE, now); break;
    case ACT_CHORD_TAP:    tapKey(a.code, a.mods, now); break;
    case ACT_CONSUMER_TAP: tapConsumer(a.code, now); break;
    case ACT_MOUSE_BTN:    tapMouseButton(a.code, now); break;
    case ACT_MOUSE_DOUBLE:
      tapMouseButton(a.code, now);
      g_dbl_btn   = a.code;
      g_dbl_at    = now + FLOW_DBLCLICK_GAP_MS;
      g_dbl_armed = true;
      break;
    default: /* ACT_NONE, and the hold types, which are edge-driven */
      break;
  }
}

void actionHoldPress(const Action &a)
{
  switch (a.type) {
    case ACT_KEY_HOLD:
      hidPressMods(a.mods);
      Keyboard.press((uint8_t)a.code);
      break;
    case ACT_MOUSE_HOLD:
      pendFlushKind(PEND_MOUSE); /* no deferred release may land mid-drag */
      Mouse.press((uint8_t)a.code);
      break;
    default:
      break;
  }
}

void actionHoldRelease(const Action &a)
{
  switch (a.type) {
    case ACT_KEY_HOLD:
      Keyboard.release((uint8_t)a.code);
      hidReleaseMods(a.mods);
      break;
    case ACT_MOUSE_HOLD:
      Mouse.release((uint8_t)a.code);
      break;
    default:
      break;
  }
}

void actionsReleaseAll(void)
{
  for (uint8_t i = 0; i < FLOW_PEND_SLOTS; i++) {
    if (g_pend[i].kind != PEND_FREE) pendFire(g_pend[i]);
  }
  g_dbl_armed = false;

  Keyboard.releaseAll();
  Consumer.release();
  Mouse.release(MOUSE_ALL);
}
