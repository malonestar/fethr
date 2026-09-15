/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/include/keynames.h
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * keynames.h - the proto-2 key-name vocabulary (PROTOCOL.md, "v2 additions").
 *
 * WHY NAMES AND NOT CODES
 *   `set_action` carries `"key":"f8"`, not `"key":201`. The host app's chain
 *   builder should never have to know that KEY_F8 is 0xC9, that a letter is
 *   its own ASCII byte while an arrow is not, or that a consumer usage lives
 *   in a different 16-bit number space from a keyboard keycode. One table
 *   here owns all of that, in both directions.
 *
 * THREE NUMBER SPACES, PICKED BY THE ACTION TYPE
 *   The same string can mean different things: "left" is KEY_LEFT_ARROW for a
 *   keyboard action and MOUSE_LEFT for a mouse one. So every lookup takes the
 *   ActionType first and only searches that space - there is no global name
 *   table, deliberately.
 *
 * VERIFIED against the installed arduino-esp32 3.3.11 headers,
 * C:/pio/packages/framework-arduinoespressif32/libraries/USB/src/ :
 *   USBHIDKeyboard.h        KEY_RETURN 0xB0, KEY_ESC 0xB1, KEY_BACKSPACE 0xB2,
 *                           KEY_TAB 0xB3, KEY_SPACE 0x20, KEY_CAPS_LOCK 0xC1,
 *                           KEY_F1..KEY_F12 0xC2..0xCD, KEY_F13..KEY_F24
 *                           0xF0..0xFB, KEY_PRINT_SCREEN 0xCE, KEY_INSERT
 *                           0xD1, KEY_HOME 0xD2, KEY_PAGE_UP 0xD3, KEY_DELETE
 *                           0xD4, KEY_END 0xD5, KEY_PAGE_DOWN 0xD6,
 *                           KEY_RIGHT_ARROW 0xD7, KEY_LEFT_ARROW 0xD8,
 *                           KEY_DOWN_ARROW 0xD9, KEY_UP_ARROW 0xDA
 *   USBHIDConsumerControl.h CONSUMER_CONTROL_PLAY_PAUSE 0x00CD, _SCAN_NEXT
 *                           0x00B5, _SCAN_PREVIOUS 0x00B6, _STOP 0x00B7,
 *                           _MUTE 0x00E2, _VOLUME_INCREMENT 0x00E9,
 *                           _VOLUME_DECREMENT 0x00EA,
 *                           _BRIGHTNESS_INCREMENT 0x006F, _DECREMENT 0x0070
 *   USBHIDMouse.h           MOUSE_LEFT 0x01, MOUSE_RIGHT 0x02, MOUSE_MIDDLE
 *                           0x04
 * Note the header spells the page keys KEY_PAGE_UP / KEY_PAGE_DOWN while the
 * protocol vocabulary is `pageup` / `pagedown`; the mapping is here, and
 * nowhere else.
 */

#ifndef FLOW_KEYNAMES_H
#define FLOW_KEYNAMES_H

#include <Arduino.h>

#include "config.h"

/* Longest name the vocabulary contains ("brightness_down", 15) plus room for
 * the NUL. Any buffer handed to actionToKeyName() should be at least this. */
#define FLOW_KEYNAME_MAX 20

/* ------------------------------------------------------------------ */
/* Action type <-> protocol name                                        */
/* ------------------------------------------------------------------ */

/*
 * "none" | "key_hold" | "key_tap" | "consumer_tap" | "mouse_btn" |
 * "mouse_hold" | "mouse_double".
 *
 * ACT_CHORD_TAP has no name of its own: it IS "key_tap" with a non-empty
 * `mods`, which is how PROTOCOL.md describes the type set. actionTypeName()
 * therefore reports ACT_CHORD_TAP as "key_tap", and actionTypeByName()
 * resolves "key_tap" to ACT_KEY_TAP - keyNameToAction() upgrades it to
 * ACT_CHORD_TAP when mods are present.
 */
const char *actionTypeName(uint8_t type);
bool        actionTypeByName(const char *name, uint8_t *out);

/* True for the types whose `key` is a keyboard key, so a caller knows whether
 * `mods` is meaningful. */
bool actionTypeIsKeyboard(uint8_t type);

/* ------------------------------------------------------------------ */
/* Modifiers                                                            */
/* ------------------------------------------------------------------ */

/* "ctrl" | "shift" | "alt" | "gui" -> one MOD_* bit. False when unknown. */
bool modByName(const char *name, uint8_t *bit_out);

/* Name of bit `i` (0..3) of a MOD_* mask, or NULL past the end. Lets a caller
 * emit the `mods` array without repeating the vocabulary. */
const char *modNameAt(uint8_t i);
#define FLOW_MOD_COUNT 4

/* ------------------------------------------------------------------ */
/* Keys                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Build a complete Action from the protocol's four fields. Returns false and
 * leaves `out` untouched if anything does not resolve; `*err` then points at
 * a literal suitable for {"ev":"err","msg":...}. `err` may be NULL.
 *
 * `key` is ignored (and may be NULL) for ACT_NONE. `mods` is ignored for the
 * non-keyboard types rather than rejected, because "no modifiers on a mouse
 * button" is not an error the host can usefully act on.
 */
bool keyNameToAction(uint8_t type, const char *key, uint8_t mods, uint8_t fn, Action *out,
                     const char **err);

/*
 * The reverse, for `get_layers`. Writes the name of `a.code` in `a`'s number
 * space into `out` (always NUL-terminated, truncating rather than
 * overflowing). Returns false - with `out` set to "" - for ACT_NONE and for a
 * code the vocabulary has no name for, which is what lets the caller emit an
 * empty `key` instead of an invented one.
 */
bool actionToKeyName(const Action &a, char *out, size_t out_len);

#endif /* FLOW_KEYNAMES_H */
