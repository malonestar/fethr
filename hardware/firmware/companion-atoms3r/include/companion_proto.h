/*
 * companion_proto.h - the companion end of the sidecar link.
 *
 * The wire format is the one in ../../PROTOCOL.md: newline-delimited JSON, one
 * object per line, 115200 8N1. Lines that do not begin with '{' are the other
 * end's human output and are ignored on both sides.
 *
 * DIRECTION OF THE CONVERSATION
 *   companion -> sidecar : {"cmd": ...}     commands
 *   sidecar -> companion : {"ev": ...}      events (and the hello reply)
 *
 * WIRING AND WHO PROBES
 *   The Atomic ToChain Base brings the AtomS3R's bottom IO out to a HY2.0-4P
 *   socket as GND / 5V / IO1=G5 / IO2=G6. This end is therefore FIXED at
 *   RX=G5, TX=G6. The other end is not: the DualKey's Chain bus auto-probe
 *   decides at boot which of its two ports carries the chain, so the companion
 *   could be on either, and because both ends are controllers on a
 *   straight-through cable the TX/RX orientation is unknown as well. The
 *   DUALKEY side resolves both - it takes the port the chain did not claim and
 *   alternates its two candidate pin orders every COMPANION_HELLO_MS until an
 *   answer arrives. Nothing here has to be configured.
 *
 * LINK STATE
 *   Unlinked: send {"cmd":"hello"} every COMPANION_HELLO_MS.
 *   Linked:   send {"cmd":"ping"}  every COMPANION_HELLO_MS as a keepalive;
 *             the sidecar drops back to probing if it hears nothing for 10 s.
 *   Either way, any inbound event refreshes the link; silence for
 *   COMPANION_LINK_TIMEOUT_MS drops it and restarts the hellos.
 */

#ifndef FETHR_COMPANION_PROTO_H
#define FETHR_COMPANION_PROTO_H

#include <Arduino.h>

/* Still 0.1.0, and deliberately: the project's rule is that a version moves
 * when an IMAGE IS CUT, not when a feature lands, and `bin/` has only ever
 * carried one companion release. The legend/animation work is labelled "0.2.0"
 * in the build notes, exactly as the sidecar labels its own unreleased work
 * while FLOW_SIDECAR_VERSION stays at 0.1.0. */
#define COMPANION_FW_VERSION "0.1.0"
#define COMPANION_WHO        "atoms3r"

/* ---- link UART (fixed on this end; see the header comment) ---- */
#define COMPANION_UART   Serial1
#define COMPANION_RX_PIN 5
#define COMPANION_TX_PIN 6
#define COMPANION_BAUD   115200

/* ---- timings ---- */
#define COMPANION_HELLO_MS         3000  /* hello while unlinked, ping while linked */
#define COMPANION_LINK_TIMEOUT_MS  10000 /* no inbound line for this long = gone     */

/* ---- reader limits ---- */
/* The `hello` beacon is the longest line on the wire: four layer names plus the
 * four legend strings lands near 230 bytes, so 384 matches the sidecar's own
 * outbound buffer and leaves the same headroom. Longer lines are dropped, not
 * truncated - half a message must never parse. */
#define COMPANION_MAX_LINE  384
#define COMPANION_OUT_BYTES 128 /* every outbound command fits easily              */

/* ---- inbound event names ---- */
#define COMPANION_EV_HELLO   "hello"
#define COMPANION_EV_LAYER   "layer"
#define COMPANION_EV_STATE   "state"
#define COMPANION_EV_HOLD    "hold"
#define COMPANION_EV_CHAIN   "chain"
#define COMPANION_EV_BATTERY "battery"
#define COMPANION_EV_TAP     "tap"  /* 0.2.0 */
#define COMPANION_EV_KNOB    "knob" /* 0.2.0 */

/* ---- the key legend (0.2.0) ----
 *
 * The sidecar builds these from its own layer table and ships them with every
 * `hello` and `layer`. This firmware holds NO table of bindings: it renders
 * four strings it was handed, so the screen cannot disagree with the keys. Slot
 * 0/1/2 are Key1/Key2/Chain Key and become the three legend rows; slot 3
 * describes the stick and the knob and becomes the footer.
 *
 * 20 characters is what the 6-pixel console font fits across a 128 px panel
 * with a margin; the sidecar builds the three key rows to fit 16 so they can
 * also be drawn in the larger font when they happen to be short. */
#define LEGEND_SLOTS 4
#define LEGEND_MAX   20

/* ---- outbound command names ---- */
#define COMPANION_CMD_HELLO      "hello"
#define COMPANION_CMD_PING       "ping"
#define COMPANION_CMD_LAYER_NEXT "layer_next"
#define COMPANION_CMD_LAYER_PREV "layer_prev"
#define COMPANION_CMD_IDENTIFY   "identify"

/*
 * Engine states, in the order PROTOCOL.md lists them. The names are WIRE
 * FORMAT - they are the values of the `state` event.
 */
enum HostState : uint8_t {
  HOST_IDLE = 0,
  HOST_RECORDING,
  HOST_TRANSCRIBING,
  HOST_CLEANING,
  HOST_PASTED,
  HOST_ERROR,
  HOST_STATE_COUNT
};

/* ---- display timings (mirrored from the Chain Mono panel's state machine,
 *      so both screens agree about how long a result stays up) ---- */
#define UI_SPIN_FRAME_MS 120 /* 4-frame spinner                     */
#define UI_OK_MS         700 /* green check on `pasted`             */
#define UI_ERR_MS        900 /* red cross on `error`                */
#define UI_BRIGHTNESS    153 /* 60% of 255                          */
#define UI_ROTATION      3   /* 0..3 clockwise quarter turns; 3 = 90 CCW */

/* ---- animation (0.2.0) ----
 *
 * Everything below is millis()-driven and quantised into UI_FRAME_MS buckets
 * before it reaches the frame struct, which is what caps the redraw rate: the
 * frame only compares unequal when its bucket changes, so an animation costs
 * one sprite blit per bucket and an idle screen still costs none.
 */
#define UI_FRAME_MS      33  /* ~30 fps ceiling for every animation  */
#define UI_REC_PERIOD_MS 700 /* mic pulse, one full sine per period   */
#define UI_WIPE_MS       250 /* layer-change colour wipe, in and out  */
#define UI_TAP_MS        150 /* legend row flashes this long on a tap */
#define UI_KNOB_MS       800 /* knob bar lingers after the last move  */

#endif /* FETHR_COMPANION_PROTO_H */
