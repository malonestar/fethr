/*
 * keynames.cpp - the proto-2 key-name vocabulary.
 *
 * See keynames.h for what this is and for the VERIFIED constant list. The one
 * design note worth repeating here: every lookup is scoped by ActionType,
 * because "left" means KEY_LEFT_ARROW to a keyboard action and MOUSE_LEFT to
 * a mouse one, and a global name table could not tell them apart.
 *
 * Nothing in this file allocates, blocks or touches a bus. It is pure lookup,
 * called only while a host command is in hand.
 */

#include "keynames.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

/* Case-insensitive equality. Written out rather than using strcasecmp so the
 * file has no dependency on which libc the core happens to ship. */
static bool ieq(const char *a, const char *b)
{
  if (a == NULL || b == NULL) return false;
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  }
  return *a == *b;
}

static void copyName(char *out, size_t out_len, const char *name)
{
  if (out == NULL || out_len == 0) return;
  snprintf(out, out_len, "%s", (name != NULL) ? name : "");
}

/* ------------------------------------------------------------------ */
/* Action type <-> protocol name                                        */
/* ------------------------------------------------------------------ */

const char *actionTypeName(uint8_t type)
{
  switch (type) {
    case ACT_KEY_HOLD:     return "key_hold";
    /* ACT_CHORD_TAP is "key_tap with mods" on the wire - see keynames.h. */
    case ACT_KEY_TAP:
    case ACT_CHORD_TAP:    return "key_tap";
    case ACT_CONSUMER_TAP: return "consumer_tap";
    case ACT_MOUSE_BTN:    return "mouse_btn";
    case ACT_MOUSE_HOLD:   return "mouse_hold";
    case ACT_MOUSE_DOUBLE: return "mouse_double";
    default:               return "none";
  }
}

bool actionTypeByName(const char *name, uint8_t *out)
{
  if (name == NULL) return false;

  uint8_t t;
  if (ieq(name, "none")) t = ACT_NONE;
  else if (ieq(name, "key_hold")) t = ACT_KEY_HOLD;
  else if (ieq(name, "key_tap")) t = ACT_KEY_TAP;
  else if (ieq(name, "consumer_tap")) t = ACT_CONSUMER_TAP;
  else if (ieq(name, "mouse_btn")) t = ACT_MOUSE_BTN;
  else if (ieq(name, "mouse_hold")) t = ACT_MOUSE_HOLD;
  else if (ieq(name, "mouse_double")) t = ACT_MOUSE_DOUBLE;
  else return false;

  if (out != NULL) *out = t;
  return true;
}

bool actionTypeIsKeyboard(uint8_t type)
{
  return type == ACT_KEY_HOLD || type == ACT_KEY_TAP || type == ACT_CHORD_TAP;
}

static bool actionTypeIsMouse(uint8_t type)
{
  return type == ACT_MOUSE_BTN || type == ACT_MOUSE_HOLD || type == ACT_MOUSE_DOUBLE;
}

/* ------------------------------------------------------------------ */
/* Modifiers                                                            */
/* ------------------------------------------------------------------ */

struct ModEntry {
  const char *name;
  uint8_t     bit;
};

/* Index order is the order `get_layers` emits them in. */
static const ModEntry MODS[FLOW_MOD_COUNT] = {
    {"ctrl", MOD_CTRL},
    {"shift", MOD_SHIFT},
    {"alt", MOD_ALT},
    {"gui", MOD_GUI},
};

bool modByName(const char *name, uint8_t *bit_out)
{
  if (name == NULL) return false;
  for (uint8_t i = 0; i < FLOW_MOD_COUNT; i++) {
    if (ieq(MODS[i].name, name)) {
      if (bit_out != NULL) *bit_out = MODS[i].bit;
      return true;
    }
  }
  return false;
}

const char *modNameAt(uint8_t i)
{
  if (i >= FLOW_MOD_COUNT) return NULL;
  return MODS[i].name;
}

/* ------------------------------------------------------------------ */
/* Keyboard names                                                       */
/* ------------------------------------------------------------------ */

struct KeyEntry {
  const char *name;
  uint16_t    code;
};

/*
 * The NAMED keyboard keys. Letters, digits and punctuation are not in here:
 * they are their own ASCII byte and are handled arithmetically below, which
 * is both smaller and impossible to get out of step with itself.
 *
 * `space` IS listed even though KEY_SPACE is the ASCII 0x20, because " " as a
 * JSON key name would be unreadable and unguessable.
 */
static const KeyEntry KEYBOARD_NAMES[] = {
    {"enter", KEY_RETURN},
    {"esc", KEY_ESC},
    {"tab", KEY_TAB},
    {"space", KEY_SPACE},
    {"backspace", KEY_BACKSPACE},
    {"delete", KEY_DELETE},
    {"insert", KEY_INSERT},
    {"home", KEY_HOME},
    {"end", KEY_END},
    {"pageup", KEY_PAGE_UP},
    {"pagedown", KEY_PAGE_DOWN},
    {"up", KEY_UP_ARROW},
    {"down", KEY_DOWN_ARROW},
    {"left", KEY_LEFT_ARROW},
    {"right", KEY_RIGHT_ARROW},
    {"capslock", KEY_CAPS_LOCK},
    {"printscreen", KEY_PRINT_SCREEN},
};
static const uint8_t KEYBOARD_NAME_COUNT =
    (uint8_t)(sizeof(KEYBOARD_NAMES) / sizeof(KEYBOARD_NAMES[0]));

/* The punctuation the vocabulary accepts as a literal character. Anything
 * outside this set (and a-z / 0-9) is rejected rather than passed through,
 * because the core's ASCII map would silently turn an unlisted character into
 * a shifted keystroke on a US layout only. */
static const char PUNCTUATION[] = "-=[];',./\\`";

/* f1..f24. The header's F-key block is in TWO runs - F1..F12 at 0xC2 and
 * F13..F24 at 0xF0 - so neither direction can be a single subtraction. */
static bool fKeyToCode(const char *name, uint16_t *out)
{
  if (name == NULL || (name[0] != 'f' && name[0] != 'F')) return false;
  if (!isdigit((unsigned char)name[1])) return false;

  unsigned n = 0;
  for (const char *p = name + 1; *p != '\0'; p++) {
    if (!isdigit((unsigned char)*p)) return false;
    n = n * 10u + (unsigned)(*p - '0');
    if (n > 24u) return false;
  }
  if (n < 1u) return false;

  *out = (n <= 12u) ? (uint16_t)(KEY_F1 + (n - 1u)) : (uint16_t)(KEY_F13 + (n - 13u));
  return true;
}

static bool fKeyToName(uint16_t code, char *out, size_t out_len)
{
  unsigned n = 0;
  if (code >= KEY_F1 && code <= KEY_F12) {
    n = (unsigned)(code - KEY_F1) + 1u;
  } else if (code >= KEY_F13 && code <= KEY_F24) {
    n = (unsigned)(code - KEY_F13) + 13u;
  } else {
    return false;
  }
  snprintf(out, out_len, "f%u", n);
  return true;
}

static bool asciiKeyToCode(const char *name, uint16_t *out)
{
  if (name == NULL || name[0] == '\0' || name[1] != '\0') return false;

  char c = name[0];
  if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); /* fold to lower case */
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
      strchr(PUNCTUATION, c) != NULL) {
    *out = (uint16_t)(unsigned char)c;
    return true;
  }
  return false;
}

static bool asciiCodeToName(uint16_t code, char *out, size_t out_len)
{
  char c = (char)code;
  if (code > 0x7F) return false;
  if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        strchr(PUNCTUATION, c) != NULL)) {
    return false;
  }
  if (out_len < 2) return false;
  out[0] = c;
  out[1] = '\0';
  return true;
}

static bool keyboardNameToCode(const char *name, uint16_t *out)
{
  for (uint8_t i = 0; i < KEYBOARD_NAME_COUNT; i++) {
    if (ieq(KEYBOARD_NAMES[i].name, name)) {
      *out = KEYBOARD_NAMES[i].code;
      return true;
    }
  }
  if (fKeyToCode(name, out)) return true;
  return asciiKeyToCode(name, out);
}

static bool keyboardCodeToName(uint16_t code, char *out, size_t out_len)
{
  for (uint8_t i = 0; i < KEYBOARD_NAME_COUNT; i++) {
    if (KEYBOARD_NAMES[i].code == code) {
      copyName(out, out_len, KEYBOARD_NAMES[i].name);
      return true;
    }
  }
  if (fKeyToName(code, out, out_len)) return true;
  return asciiCodeToName(code, out, out_len);
}

/* ------------------------------------------------------------------ */
/* Consumer + mouse names                                               */
/* ------------------------------------------------------------------ */

static const KeyEntry CONSUMER_NAMES[] = {
    {"play_pause", CONSUMER_CONTROL_PLAY_PAUSE},
    {"next", CONSUMER_CONTROL_SCAN_NEXT},
    {"prev", CONSUMER_CONTROL_SCAN_PREVIOUS},
    {"stop", CONSUMER_CONTROL_STOP},
    {"mute", CONSUMER_CONTROL_MUTE},
    {"vol_up", CONSUMER_CONTROL_VOLUME_INCREMENT},
    {"vol_down", CONSUMER_CONTROL_VOLUME_DECREMENT},
    {"brightness_up", CONSUMER_CONTROL_BRIGHTNESS_INCREMENT},
    {"brightness_down", CONSUMER_CONTROL_BRIGHTNESS_DECREMENT},
};
static const uint8_t CONSUMER_NAME_COUNT =
    (uint8_t)(sizeof(CONSUMER_NAMES) / sizeof(CONSUMER_NAMES[0]));

static const KeyEntry MOUSE_NAMES[] = {
    {"left", MOUSE_LEFT},
    {"right", MOUSE_RIGHT},
    {"middle", MOUSE_MIDDLE},
};
static const uint8_t MOUSE_NAME_COUNT = (uint8_t)(sizeof(MOUSE_NAMES) / sizeof(MOUSE_NAMES[0]));

static bool tableNameToCode(const KeyEntry *t, uint8_t n, const char *name, uint16_t *out)
{
  for (uint8_t i = 0; i < n; i++) {
    if (ieq(t[i].name, name)) {
      *out = t[i].code;
      return true;
    }
  }
  return false;
}

static bool tableCodeToName(const KeyEntry *t, uint8_t n, uint16_t code, char *out, size_t out_len)
{
  for (uint8_t i = 0; i < n; i++) {
    if (t[i].code == code) {
      copyName(out, out_len, t[i].name);
      return true;
    }
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool keyNameToAction(uint8_t type, const char *key, uint8_t mods, uint8_t fn, Action *out,
                     const char **err)
{
  const char *local_err = "bad action";
  if (out == NULL) {
    if (err != NULL) *err = local_err;
    return false;
  }

  if (fn >= (uint8_t)FN_COUNT) {
    if (err != NULL) *err = "unknown fn";
    return false;
  }

  Action a;
  a.type = type;
  a.code = 0;
  a.mods = MOD_NONE;
  a.fn   = fn;

  if (type == ACT_NONE) {
    /* An unbound slot keeps no key, no mods and no function: a stale colour on
     * a key that now does nothing would be worse than a blank one. */
    a.fn  = FN_NONE;
    *out  = a;
    return true;
  }

  bool ok = false;
  if (actionTypeIsKeyboard(type)) {
    ok = keyboardNameToCode(key, &a.code);
    if (ok) {
      a.mods = (uint8_t)(mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_GUI));
      /* "key_tap with modifiers" IS ACT_CHORD_TAP - the dispatcher only
       * presses mods for that type and for ACT_KEY_HOLD. */
      if (type == ACT_KEY_TAP && a.mods != MOD_NONE) a.type = ACT_CHORD_TAP;
    }
  } else if (type == ACT_CONSUMER_TAP) {
    ok = tableNameToCode(CONSUMER_NAMES, CONSUMER_NAME_COUNT, key, &a.code);
  } else if (actionTypeIsMouse(type)) {
    ok = tableNameToCode(MOUSE_NAMES, MOUSE_NAME_COUNT, key, &a.code);
  } else {
    if (err != NULL) *err = "bad type";
    return false;
  }

  if (!ok) {
    if (err != NULL) *err = "unknown key";
    return false;
  }

  *out = a;
  return true;
}

bool actionToKeyName(const Action &a, char *out, size_t out_len)
{
  if (out == NULL || out_len == 0) return false;
  out[0] = '\0';

  if (a.type == ACT_NONE) return false;

  if (actionTypeIsKeyboard(a.type)) return keyboardCodeToName(a.code, out, out_len);
  if (a.type == ACT_CONSUMER_TAP) {
    return tableCodeToName(CONSUMER_NAMES, CONSUMER_NAME_COUNT, a.code, out, out_len);
  }
  if (actionTypeIsMouse(a.type)) {
    return tableCodeToName(MOUSE_NAMES, MOUSE_NAME_COUNT, a.code, out, out_len);
  }
  return false;
}
