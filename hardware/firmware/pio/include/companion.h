/*
 * companion.h - the optional companion display link (COMPANION.md).
 *
 * WHAT THIS IS
 *   A second controller - an M5Stack AtomS3R on an Atomic ToChain Base - cabled
 *   to whichever of the DualKey's two HY2.0-4P ports the Chain bus did not
 *   claim. It is NOT a Chain node: it does not enumerate, it holds no bus id,
 *   and it speaks the same newline-delimited JSON as the host protocol
 *   (PROTOCOL.md) over a plain 115200 UART. It shows the active layer and the
 *   dictation state on its 128x128 LCD and its button cycles layers.
 *
 * WHY IT PROBES
 *   Two unknowns stack up:
 *     1. which port pair the CHAIN settled on ({G47,G48} or {G5,G6}) - the bus
 *        auto-probe in chain.cpp decides that at boot, so the companion has to
 *        read it back with chainBusPins() and take the complement;
 *     2. which way round TX and RX are - both ends are controllers on a
 *        straight-through cable, so there is no "host side" convention to
 *        follow. The Atom's side is FIXED (RX=G5, TX=G6 through the ToChain
 *        Base); the DualKey alternates its two candidate orders on every
 *        unanswered beacon until a line comes back.
 *   The upshot: either port, either cable orientation, no configuration.
 *
 * SCHEDULER RULES (identical to every other module here)
 *   Never blocks, never calls delay(), parses AT MOST ONE inbound line per
 *   loop(), and builds every outbound line with snprintf into one static
 *   buffer - no heap in the steady state. Only deserializeJson() allocates, and
 *   only while a companion command is in hand.
 *
 * The public API is declared in sidecar.h alongside every other module's.
 * With FLOW_COMPANION 0 every entry point below is an empty stub and no UART
 * is opened.
 */

#ifndef FLOW_COMPANION_H
#define FLOW_COMPANION_H

#include <Arduino.h>

#include "config.h"

/* Wire-format strings. The `ev` names deliberately match PROTOCOL.md's, so a
 * reader of one document already knows the other. */
#define COMPANION_EV_HELLO   "hello"
#define COMPANION_EV_LAYER   "layer"
#define COMPANION_EV_STATE   "state"
#define COMPANION_EV_HOLD    "hold"
#define COMPANION_EV_CHAIN   "chain"
#define COMPANION_EV_BATTERY "battery"
/* 0.3.0: the two events that exist purely so the screen can animate. `tap`
 * carries the human label from actionLabel(), NOT the FnId - the companion
 * picks its icon from it, and every MEDIA binding shares one FnId. */
#define COMPANION_EV_TAP     "tap"
#define COMPANION_EV_KNOB    "knob"

/* Commands accepted from the companion. */
#define COMPANION_CMD_HELLO      "hello"
#define COMPANION_CMD_PING       "ping"
#define COMPANION_CMD_LAYER_NEXT "layer_next"
#define COMPANION_CMD_LAYER_PREV "layer_prev"
#define COMPANION_CMD_LAYER      "layer"
#define COMPANION_CMD_IDENTIFY   "identify"

#endif /* FLOW_COMPANION_H */
