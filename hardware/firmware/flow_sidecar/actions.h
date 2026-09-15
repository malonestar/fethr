/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/include/actions.h
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * actions.h - HID action descriptors + dispatch for the Flow sidecar.
 *
 * An "action" is data, not code: the layer table in layers.cpp is a plain array
 * of Action structs, and actions.cpp knows how to turn one into USB HID traffic.
 *
 * All release() work is DEFERRED through a tiny pending queue so that a "tap"
 * never calls delay(). Call actionsTick() once per loop().
 *
 * v2 change: this used to be a header-only module with `static` HID objects.
 * The firmware is now split across several translation units, and a static
 * object in a header would give every .cpp its own private USBHIDKeyboard.
 * Declarations live here, definitions in actions.cpp.
 *
 * Verified against arduino-esp32 3.3.11, libraries/USB/src/:
 *   USBHIDKeyboard.h        KEY_F7/F8/F9, KEY_RETURN, KEY_*_ARROW,
 *                           KEY_LEFT_CTRL/SHIFT/ALT/GUI, press/release/releaseAll
 *   USBHIDMouse.h           MOUSE_LEFT/RIGHT/MIDDLE/ALL,
 *                           USBHIDRelativeMouse::move(int8_t x, int8_t y,
 *                                                     int8_t wheel = 0, int8_t pan = 0),
 *                           press/release(uint8_t)
 *   USBHIDConsumerControl.h CONSUMER_CONTROL_*, press(uint16_t), release()
 */

#ifndef FLOW_ACTIONS_H
#define FLOW_ACTIONS_H

#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
#include "USBHIDConsumerControl.h"

/* ------------------------------------------------------------------ */
/* Action descriptor                                                    */
/* ------------------------------------------------------------------ */

enum ActionType : uint8_t {
  ACT_NONE = 0,     /* do nothing                                          */
  ACT_KEY_HOLD,     /* keyboard key down on press, up on release           */
  ACT_KEY_TAP,      /* keyboard key press + deferred release               */
  ACT_CHORD_TAP,    /* modifiers + key, press + deferred release           */
  ACT_CONSUMER_TAP, /* consumer-control usage, press + deferred release    */
  ACT_MOUSE_BTN,    /* mouse button press + deferred release               */
  ACT_MOUSE_HOLD,   /* v2: mouse button down on press, up on release       */
  ACT_MOUSE_DOUBLE  /* v2: two clicks, second one scheduled, no delay()    */
};

/* Modifier bitmask (independent of the HID wire format). */
#define MOD_NONE  0x00
#define MOD_CTRL  0x01
#define MOD_SHIFT 0x02
#define MOD_ALT   0x04
#define MOD_GUI   0x08

/*
 * v2: every action also carries what it MEANS, so the LEDs and the Mono panel
 * can show a per-key function colour / glyph without any of them having to
 * pattern-match on keycodes. Colour and glyph lookups live in layers.cpp.
 */
enum FnId : uint8_t {
  FN_NONE = 0,   /* fall back to the layer colour */
  FN_DICT_RAW,   /* blue    */
  FN_DICT_CLEAN, /* violet  */
  FN_REPASTE,    /* cyan    */
  FN_MEDIA,      /* amber   */
  FN_UNDO,       /* green   */
  FN_REDO,       /* teal    */
  FN_MOUSE_L,    /* white   */
  FN_MOUSE_R,    /* white   */
  FN_MOUSE_M,    /* white   */
  FN_ENTER,      /* pale yellow */
  FN_COUNT
};

struct Action {
  uint8_t  type; /* ActionType                                                  */
  uint16_t code; /* KEY_* / ASCII, or CONSUMER_CONTROL_*, or MOUSE_* button mask */
  uint8_t  mods; /* MOD_* bitmask, only meaningful for ACT_CHORD_TAP             */
  uint8_t  fn;   /* FnId - drives the LED colour and the hold glyph              */
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void actionsBegin(void);
void actionsTick(uint32_t now); /* service deferred releases; once per loop() */

/* True for the action types that must be driven off raw press/release edges
 * rather than fired as a one-shot. */
bool actionIsHold(const Action &a);

void actionFire(const Action &a, uint32_t now);
void actionHoldPress(const Action &a);
void actionHoldRelease(const Action &a);
void actionsReleaseAll(void); /* panic path: drop everything the host holds */

/* Primitives, used directly by the stick/knob handlers. */
void tapKey(uint16_t code, uint8_t mods, uint32_t now);
void tapConsumer(uint16_t usage, uint32_t now);
void tapMouseButton(uint16_t button, uint32_t now);
void mouseScroll(int8_t wheel, int8_t pan);
void mouseMove(int8_t dx, int8_t dy);

#endif /* FLOW_ACTIONS_H */
