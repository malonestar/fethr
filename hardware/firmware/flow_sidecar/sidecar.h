/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/include/sidecar.h
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * sidecar.h - the small amount of state and API the firmware's modules share.
 *
 * Module map:
 *   main.cpp        setup/loop, DualKey GPIO scanning, layer switching
 *   chain.cpp       Chain bus: enumeration, scheduler, node polling, node LEDs
 *   mono.cpp        Chain Mono panel: priority state machine + rendering
 *   leds.cpp        the DualKey's own two WS2812s + battery/USB sampling
 *   layers.cpp      the layer table + the colour/glyph/name maps
 *   actions.cpp     USB HID dispatch + the USB string descriptors
 *   glyphs.cpp      8x8 bitmaps
 *   settings.cpp    the runtime config + NVS persistence      (v2.1)
 *   serial_proto.cpp the host settings protocol + '?' console (v2.1)
 *   companion.cpp   the optional companion-display UART link  (0.2.0)
 *
 * Everything else stays file-static inside its module.
 */

#ifndef FLOW_SIDECAR_H
#define FLOW_SIDECAR_H

#include <Arduino.h>

#include "M5Chain.h"
#include "companion.h"
#include "config.h"
#include "settings.h"

/* ================================================================== */
/* Shared state (defined in main.cpp)                                  */
/* ================================================================== */

extern uint8_t g_layer;

struct LocalKey {
  uint8_t  pin;
  bool     raw;         /* last sampled level, active-high after inversion   */
  bool     stable;      /* debounced level                                   */
  uint32_t changed_ms;  /* when raw last differed from stable                */
  bool     hold_active; /* a hold action is currently pressed on the host    */
  Action   hold_action; /* captured at press time, used at release time      */
};

extern LocalKey g_key[2];

enum StickDir : uint8_t {
  DIR_NONE = 0,
  DIR_UP,
  DIR_DOWN,
  DIR_LEFT,
  DIR_RIGHT
};

/* ================================================================== */
/* Host engine state (the `state` command, PROTOCOL.md)                */
/* ================================================================== */

/*
 * What the host app says it is doing. The device cannot work any of this out
 * for itself: it knows a key is held, but not whether the transcript came
 * back. Order matches the protocol's value list.
 *
 * TRANSCRIBING/CLEANING are continuous - they drive the Mono spinner and the
 * amber LED breathe for as long as they are current. PASTED/ERROR are
 * momentary: hostStateSet() fires a timed panel overlay and an LED flash and
 * then nothing further depends on them. IDLE/RECORDING clear the continuous
 * effects and leave any running overlay to expire on its own, so a `pasted`
 * immediately followed by `idle` still shows its checkmark.
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

/* ================================================================== */
/* main.cpp                                                            */
/* ================================================================== */

void cycleLayer(uint32_t now);
void releaseAllHolds(void);

/*
 * Bind g_key[0]/g_key[1] to the two DualKey GPIOs according to
 * g_cfg.swap_keys (0.2.0 - it replaced the compile-time KEYS_SWAPPED).
 *
 * IDEMPOTENT: returns immediately when the assignment already matches, so
 * settingsApplyAll() can call it after every `set` without disturbing a key
 * that is down. When it does swap, it ends any hold first and re-seeds the
 * debounce state from the pins' current levels, so the change cannot leave a
 * key stuck on the host or synthesise a phantom edge.
 */
void keysApplySwap(void);

/* Jump straight to a layer (the `layer` protocol command and boot_layer).
 * Same housekeeping as cycleLayer(); a no-op if `layer` is out of range or is
 * already current. Returns true if the layer actually changed. */
bool setLayer(uint8_t layer, uint32_t now);

/* Seconds since boot, for the `status` reply. */
uint32_t uptimeSeconds(uint32_t now);

/* ---- host engine state ---- */

/* Apply a new host state: updates what mono.cpp/leds.cpp read, and fires the
 * one-shot overlay/flash for PASTED and ERROR. Issues no bus traffic. */
void hostStateSet(uint8_t state, uint32_t now);

uint8_t  hostState(void);       /* current HostState                        */
uint32_t hostStateSince(void);  /* millis() when it was last set            */

/* Stable protocol name for a HostState ("idle", ...); NULL out of range. */
const char *hostStateName(uint8_t state);

/* Reverse lookup for the `state` command. Returns false when unknown. */
bool hostStateByName(const char *name, uint8_t *out);

/* Note that a host has just spoken (`hello` or `state`). */
void hostSeen(uint32_t now);

/* True within HOST_PRESENT_MS of the last hostSeen(). While it is true the
 * device suppresses its own "hold released" checkmark and waits for the
 * host's `pasted` instead. */
bool hostPresent(uint32_t now);

/* ================================================================== */
/* chain.cpp                                                           */
/* ================================================================== */

void chainBegin(uint32_t now);
const char *chainBusName(void); /* which pin pair the bus auto-probe settled on */

/* The same answer as numbers, for companion.cpp: it has to take the port pair
 * the chain did NOT claim. Either pointer may be NULL. */
void chainBusPins(int8_t *rx, int8_t *tx);

void chainService(uint32_t now);   /* at most ONE bus transaction per call   */
void chainInputTick(uint32_t now); /* pure-software timers; every loop()     */
void chainOnLayerChanged(uint32_t now);
void chainPrintStatus(void);

/* Low-level handles, for mono.cpp only. The Chain object and the per-node ids
 * live in chain.cpp because that is what owns enumeration. */
extern Chain M5Chain;
uint8_t chainMonoId(void);       /* 0 = no Mono node on the bus */
void    chainMonoUnavailable(void);
void    chainResult(chain_status_t st);

/* ---- enumeration read-out, for the host protocol (v2.1) ---- */

bool    chainIsReady(void);
uint8_t chainNodeCount(void);

/* Reads node `index` (< chainNodeCount()). `type_name` is a stable protocol
 * string ("key"/"joystick"/"angle"/"mono"/"unknown"); `role` is "nav",
 * "scroll", "key", "angle", "mono" or NULL when the node holds no role.
 * Any out pointer may be NULL. Returns false for an out-of-range index. */
bool chainNodeAt(uint8_t index, uint16_t *id, uint16_t *type, const char **type_name,
                 const char **role);

/* ================================================================== */
/* mono.cpp                                                            */
/* ================================================================== */

void monoInit(void);            /* called from chain enumeration            */
bool monoService(uint32_t now); /* true = a bus transaction was consumed    */
void monoOverlay(uint8_t glyph_id, uint32_t ms, uint32_t now);
void monoOnLayerChanged(uint32_t now);

/* Scroll arbitrary text once (the `mono` and `identify` commands, and the
 * layer-name announcement). The text is COPIED into a fixed buffer, so the
 * caller's storage need not outlive the call; anything past
 * FLOW_PROTO_MONO_TEXT characters is dropped. `announce_layer` makes the
 * panel raise the layer letter when the scroll finishes, which is what a
 * layer change wants and an app notification does not. */
void monoScrollText(const char *text, bool announce_layer);

/* g_cfg changed: re-send rotation/brightness and force a repaint. Sets flags
 * only - the actual transactions are issued by monoService(), one per loop. */
void monoConfigChanged(void);

/* Hints pushed by the input handlers; all cheap, none touch the bus. */
void monoHintNav(uint8_t dir);
void monoHintScroll(bool up, uint32_t now);
void monoHintVolume(uint8_t level_0_8, uint32_t now);

/* ================================================================== */
/* leds.cpp                                                            */
/* ================================================================== */

void ledsBegin(void);
void ledsUpdate(uint32_t now);
void ledFlashAll(uint8_t r, uint8_t g, uint8_t b, uint8_t count, uint32_t now);
void ledFlashKey(uint8_t key_index, uint32_t now);
bool batteryLow(void);
uint16_t batteryMilliVolts(void);
uint16_t usbMilliVolts(void); /* VBUS on G2, same divider as VBAT (v2.1) */

/* g_cfg changed: drop the repaint cache so the next ledsUpdate() re-derives
 * every colour. Cheap; issues no bit-banging of its own. */
void ledsConfigChanged(void);

/* Shared colour helper (leds.cpp); layerRgb() lives in layers.cpp. */
void tintForBattery(uint8_t rgb[3]);

/* ================================================================== */
/* serial_proto.cpp (v2.1)                                             */
/* ================================================================== */

void protoBegin(void);        /* boot event; call once Serial.begin() is done */
void protoTick(uint32_t now); /* non-blocking reader + the battery event      */

/* Unsolicited events. All of these build their line in a static buffer and
 * write it straight to Serial - no heap, no allocation, safe to call from
 * anywhere in the loop. */
void protoEventLayer(uint8_t layer);
void protoEventHold(uint8_t key_1based, bool active, uint8_t fn);
void protoEventTap(const char *key_name);
void protoEventChain(void);

/* 0.2.0. The keymap changed (`set_action` / `set_layer_meta`), so a second
 * client that is showing it should re-read `get_layers`. Carries no payload
 * on purpose: the editing client already knows what it sent, and anyone else
 * needs the whole table rather than a diff. */
void protoEventLayoutChanged(void);

/* ================================================================== */
/* companion.cpp (0.2.0)                                               */
/* ================================================================== */

/* Call AFTER chainBegin(): the port the companion opens is chosen from the one
 * the Chain bus did not take. */
void companionBegin(uint32_t now);

/* Non-blocking: reads at most one line, runs the probe/keepalive timers and
 * emits the periodic battery event. One more step in loop(). */
void companionTick(uint32_t now);

bool companionLinked(void);

/* The `companion` section of the '?' console dump. */
void companionPrintStatus(void);

/* Events out. Each builds its line in a static buffer and writes it straight to
 * the UART; all are no-ops while the link is down, so a call site never has to
 * check. */
void companionEventLayer(uint8_t layer);
void companionEventState(uint8_t host_state);
void companionEventHold(uint8_t key_1based, bool active);
void companionEventChain(void);

/* 0.3.0. `key_name` is the same vocabulary protoEventTap() uses ("1", "2",
 * "chain", "chain2", "nav", "scroll"); `label` is actionLabel() for the action
 * that fired, which is what the companion picks its icon from. Either may be
 * NULL, in which case the field is omitted. */
void companionEventTap(const char *key_name, const char *label);

/* 0.3.0. A knob detent changed. Rate-limited inside companion.cpp to
 * COMPANION_KNOB_MIN_GAP_MS; a movement inside the gap is held pending and
 * emitted by companionTick(), so the LAST position is never lost. */
void companionEventKnob(uint8_t detent);

#endif /* FLOW_SIDECAR_H */
