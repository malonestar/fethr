/*
 * serial_proto.h - the host settings protocol (PROTOCOL.md), v2.1.
 *
 * The public surface is declared in sidecar.h alongside every other module's,
 * so nothing outside serial_proto.cpp needs to include this file. It exists
 * for the internals that are worth naming, and to keep the contract with
 * PROTOCOL.md written down next to the code that implements it.
 *
 * SHAPE OF THE THING
 *   One CDC port carries three conversations at once:
 *     1. FLOG debug lines            - human, never start with '{'
 *     2. the '?' one-character console - human, unchanged from v2.0
 *     3. newline-delimited JSON      - machine, every line starts with '{'
 *   The reader disambiguates on the FIRST byte after a line boundary, which
 *   is exactly the rule PROTOCOL.md states, and means a terminal user can
 *   still just press '?' with no newline.
 *
 * ALLOCATION POLICY
 *   Replies and events are built with snprintf into ONE static buffer and
 *   written straight to Serial - no heap, no String, nothing per-loop.
 *   Inbound commands are parsed with ArduinoJson 7, which does allocate, but
 *   only on the arrival of a host command; the idle loop never touches it.
 *   The document is deserialised, read, and destroyed inside one call.
 */

#ifndef FLOW_SERIAL_PROTO_H
#define FLOW_SERIAL_PROTO_H

#include <Arduino.h>

#include "config.h"

/* Wire-format strings for `mono_idle`. */
#define PROTO_MONO_IDLE_BLANK  "blank"
#define PROTO_MONO_IDLE_LETTER "letter"

/* Every reply/event carries one of these as its "ev". */
#define PROTO_EV_HELLO   "hello"
#define PROTO_EV_STATUS  "status"
#define PROTO_EV_CONFIG  "config"
#define PROTO_EV_OK      "ok"
#define PROTO_EV_ERR     "err"
#define PROTO_EV_LAYER   "layer"
#define PROTO_EV_HOLD    "hold"
#define PROTO_EV_TAP     "tap"
#define PROTO_EV_CHAIN   "chain"
#define PROTO_EV_BATTERY "battery"
#define PROTO_EV_BOOT    "boot"

#endif /* FLOW_SERIAL_PROTO_H */
