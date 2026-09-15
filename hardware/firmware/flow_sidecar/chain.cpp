/* ==========================================================================
 * GENERATED FILE - DO NOT EDIT.
 * Source of truth: firmware/pio/src/chain.cpp
 * Regenerate with: firmware/pio/tools/sync_ino.ps1
 * ========================================================================== */
/*
 * chain.cpp - the Chain bus: enumeration, the one-transaction-per-loop
 * scheduler, per-node polling, the input semantics those polls drive, and the
 * chained nodes' own RGB LEDs.
 *
 * Scheduler shape (chainService() issues AT MOST ONE bus transaction per call):
 *
 *   1. Mono panel        - only writes on a state change, so it is cheap and
 *                          goes first; a stale indicator is the thing a user
 *                          notices soonest.
 *   2. Input round-robin - chain key, nav stick, scroll stick, angle knob.
 *                          Each has its own next-due timestamp.
 *   3. Node LEDs         - LOWEST priority: only reached when every input task
 *                          is up to date, and additionally rate-limited per
 *                          node. Input polling therefore cannot be starved by
 *                          decorative traffic.
 */

#include "sidecar.h"

/* ================================================================== */
/* Bus + roles                                                         */
/* ================================================================== */

Chain M5Chain;

static device_info_t g_dev_storage[MAX_CHAIN_DEVICES];
static device_list_t g_dev_list = {0, g_dev_storage};

static bool     g_chain_ready      = false;
static uint8_t  g_chain_fail       = 0;
static uint32_t g_reenum_at        = 0;
static uint16_t g_enum_please_seen = 0;

/* Role -> bus id. 0 means "absent" (chain ids start at 1). */
static uint8_t g_nav_id    = 0;
static uint8_t g_scroll_id = 0;
static uint8_t g_key_id    = 0;
static uint8_t g_angle_id  = 0;
static uint8_t g_mono_id   = 0;

uint8_t chainMonoId(void) { return g_mono_id; }
void    chainMonoUnavailable(void) { g_mono_id = 0; }

bool    chainIsReady(void) { return g_chain_ready; }
uint8_t chainNodeCount(void) { return (uint8_t)g_dev_list.count; }

/* Stable protocol names - these are wire format for the `nodes` arrays in the
 * `hello`, `status` and `chain` messages. */
static const char *typeNameFor(uint16_t type)
{
  switch (type) {
    case CHAIN_KEY_TYPE_CODE:      return "key";
    case CHAIN_JOYSTICK_TYPE_CODE: return "joystick";
    case CHAIN_ANGLE_TYPE_CODE:    return "angle";
    case CHAIN_MONO_TYPE_CODE:     return "mono";
    default:                       return "unknown";
  }
}

bool chainNodeAt(uint8_t index, uint16_t *id, uint16_t *type, const char **type_name,
                 const char **role)
{
  if (index >= g_dev_list.count) return false;

  uint16_t dev_id   = g_dev_list.devices[index].id;
  uint16_t dev_type = (uint16_t)g_dev_list.devices[index].device_type;

  if (id) *id = dev_id;
  if (type) *type = dev_type;
  if (type_name) *type_name = typeNameFor(dev_type);

  if (role) {
    const char *r = NULL;
    /* Two joysticks share a device type, so the role is the only thing that
     * says which one drives the cursor - hence reporting it at all. */
    if (dev_id == g_nav_id) r = "nav";
    else if (dev_id == g_scroll_id) r = "scroll";
    else if (dev_id == g_key_id) r = "key";
    else if (dev_id == g_angle_id) r = "angle";
    else if (dev_id == g_mono_id) r = "mono";
    *role = r;
  }
  return true;
}

/* Input round-robin cursor + per-task next-due timestamps. */
#define CHAIN_TASK_COUNT 4
static uint8_t  g_task_rr        = 0;
static uint32_t g_key_poll_at    = 0;
static uint32_t g_nav_poll_at    = 0;
static uint32_t g_scroll_poll_at = 0;
static uint32_t g_angle_poll_at  = 0;

/* ================================================================== */
/* Chain Key state (single tap / double tap / layer-switch hold)       */
/* ================================================================== */

static bool     g_ck_down     = false;
static bool     g_ck_consumed = false; /* the hold already cycled the layer */
static uint32_t g_ck_press_ms = 0;
static uint32_t g_ck_flash_until = 0;

/* Deferred single tap - see the CHAIN_KEY_DOUBLE_MS comment in config.h for
 * the latency tradeoff this implements. */
static bool     g_ck_single_pending = false;
static uint32_t g_ck_single_at      = 0;
static Action   g_ck_single_action  = {ACT_NONE, 0, MOD_NONE, FN_NONE};
static bool     g_ck_double_armed   = false;

/* ================================================================== */
/* Stick / knob state                                                  */
/* ================================================================== */

static uint8_t  g_nav_dir      = DIR_NONE;
static uint32_t g_nav_next_ms  = 0;
static bool     g_nav_btn_down = false;
static uint32_t g_mouse_next   = 0;
static int16_t  g_nav_mag      = 0; /* 0..JOY_FULLSCALE, for the node LED */

static uint32_t g_scroll_next_ms  = 0;
static uint8_t  g_scroll_x_dir    = DIR_NONE;
static uint32_t g_scroll_x_next   = 0;
static bool     g_scroll_btn_down = false;
static int16_t  g_scroll_mag      = 0;

static bool     g_angle_have      = false;
static int16_t  g_angle_detent    = 0;
static int8_t   g_angle_queue     = 0;
static uint32_t g_angle_next_emit = 0;

/* ================================================================== */
/* Chained-node LEDs                                                   */
/* ================================================================== */

enum NodeLedSlot : uint8_t { NL_KEY = 0, NL_NAV, NL_SCROLL, NL_ANGLE, NL_COUNT };

struct NodeLed {
  uint8_t  want[3];
  uint8_t  sent[3];
  uint32_t next_ms;
  bool     primed;
};

static NodeLed g_nl[NL_COUNT]; /* still touched by the enumerate/layer resets */
#if FLOW_NODE_LEDS
static uint8_t g_nl_rr = 0;
#endif

/* ================================================================== */
/* Small helpers                                                       */
/* ================================================================== */

static inline int16_t iabs16(int16_t v)
{
  return (v < 0) ? (int16_t)(-v) : v;
}

static inline int16_t clamp16(int16_t v, int16_t lo, int16_t hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline uint8_t scalePct(uint8_t v, uint8_t pct)
{
  return (uint8_t)(((uint16_t)v * pct) / 100);
}

/* ================================================================== */
/* Failure accounting                                                  */
/* ================================================================== */

void chainResult(chain_status_t st)
{
  if (st == CHAIN_OK) {
    g_chain_fail = 0;
    return;
  }
  if (++g_chain_fail >= CHAIN_FAIL_LIMIT) {
    FLOG("[chain] %u consecutive failures - re-enumerating\r\n", (unsigned)g_chain_fail);
    g_chain_fail  = 0;
    g_chain_ready = false;
    g_reenum_at   = millis(); /* cooldown is applied inside chainEnumerate */
  }
}

/* ================================================================== */
/* Enumeration                                                         */
/* ================================================================== */

static void sortDevicesById(device_list_t *list)
{
  for (uint16_t i = 1; i < list->count; i++) {
    device_info_t tmp = list->devices[i];
    int32_t       j   = (int32_t)i - 1;
    while (j >= 0 && list->devices[j].id > tmp.id) {
      list->devices[j + 1] = list->devices[j];
      j--;
    }
    list->devices[j + 1] = tmp;
  }
}

static void chainEnumerate(uint32_t now)
{
  g_reenum_at   = now + CHAIN_REENUM_COOLDOWN_MS;
  g_chain_ready = false;
  g_chain_fail  = 0;

  g_nav_id = g_scroll_id = g_key_id = g_angle_id = g_mono_id = 0;
  g_dev_list.count   = 0;
  g_dev_list.devices = g_dev_storage;

  for (uint8_t i = 0; i < NL_COUNT; i++) g_nl[i].primed = false;

  uint16_t count = 0;
  if (M5Chain.getDeviceNum(&count, CHAIN_ENUM_TIMEOUT_MS) != CHAIN_OK || count == 0) {
    FLOG("[chain] no devices\r\n");
    return;
  }
  if (count > MAX_CHAIN_DEVICES) count = MAX_CHAIN_DEVICES;

  g_dev_list.count = count;
  if (!M5Chain.getDeviceList(&g_dev_list, CHAIN_ENUM_TIMEOUT_MS)) {
    FLOG("[chain] device list failed\r\n");
    g_dev_list.count = 0;
    return;
  }

  /* Ids are assigned by distance from the DualKey; sort so that "first
   * joystick found" means "the one nearest the head unit". */
  sortDevicesById(&g_dev_list);

  for (uint16_t i = 0; i < g_dev_list.count; i++) {
    uint8_t id = (uint8_t)g_dev_list.devices[i].id;
    switch (g_dev_list.devices[i].device_type) {
      case CHAIN_JOYSTICK_TYPE_CODE:
        if (g_nav_id == 0) {
          g_nav_id = id;
        } else if (g_scroll_id == 0) {
          g_scroll_id = id;
        }
        break;
      case CHAIN_KEY_TYPE_CODE:
        if (g_key_id == 0) g_key_id = id;
        break;
      case CHAIN_ANGLE_TYPE_CODE:
        if (g_angle_id == 0) g_angle_id = id;
        break;
      case CHAIN_MONO_TYPE_CODE:
        if (g_mono_id == 0) g_mono_id = id;
        break;
      default:
        break;
    }
    FLOG("[chain] id=%u type=0x%04X\r\n", (unsigned)id,
         (unsigned)g_dev_list.devices[i].device_type);
  }

  FLOG("[chain] roles nav=%u scroll=%u key=%u angle=%u mono=%u\r\n", (unsigned)g_nav_id,
       (unsigned)g_scroll_id, (unsigned)g_key_id, (unsigned)g_angle_id, (unsigned)g_mono_id);

  /* Any subset of the nodes may be missing; every consumer below checks its
   * role id for 0 first, so features simply drop out instead of crashing. */
  monoInit();

  g_enum_please_seen = M5Chain.getEnumPleaseNum();
  g_angle_have       = false;
  g_chain_ready      = true;

  protoEventChain();
  companionEventChain();
}

/* ================================================================== */
/* Inputs -> HID                                                       */
/* ================================================================== */

static uint8_t stickDir(int16_t x, int16_t y)
{
  int16_t ax = iabs16(x);
  int16_t ay = iabs16(y);
  if (ax < JOY_DEADZONE && ay < JOY_DEADZONE) return DIR_NONE;
  /* Pick one axis - diagonals would otherwise fire two arrows per repeat. */
  if (ay >= ax) return (y > 0) ? DIR_UP : DIR_DOWN;
  return (x > 0) ? DIR_RIGHT : DIR_LEFT;
}

static void fireArrow(uint8_t dir, uint32_t now)
{
  switch (dir) {
    case DIR_UP:    tapKey(KEY_UP_ARROW, MOD_NONE, now); break;
    case DIR_DOWN:  tapKey(KEY_DOWN_ARROW, MOD_NONE, now); break;
    case DIR_LEFT:  tapKey(KEY_LEFT_ARROW, MOD_NONE, now); break;
    case DIR_RIGHT: tapKey(KEY_RIGHT_ARROW, MOD_NONE, now); break;
    default:        break;
  }
}

/*
 * Cursor acceleration curve: dead below JOY_DEADZONE, then a quadratic ramp
 * from MOUSE_MIN_PX to MOUSE_MAX_PX pixels per report. Quadratic (rather than
 * linear) is what makes the first third of the stick's travel usable for
 * pixel-accurate aiming while full deflection still crosses a 4K screen in
 * about a second (20 px * 100 reports/s).
 */
static int16_t accelAxis(int16_t v)
{
  int16_t a = iabs16(v);
  if (a <= JOY_DEADZONE) return 0;
  if (a > JOY_FULLSCALE) a = JOY_FULLSCALE;

  uint32_t span = (uint32_t)(JOY_FULLSCALE - JOY_DEADZONE);
  if (span == 0) return 0;

  uint32_t t  = ((uint32_t)(a - JOY_DEADZONE) * 1000UL) / span; /* 0..1000 */
  uint32_t sq = (t * t) / 1000UL;                               /* 0..1000 */
  uint32_t px = (uint32_t)MOUSE_MIN_PX + ((sq * (MOUSE_MAX_PX - MOUSE_MIN_PX)) / 1000UL);
  if (px > (uint32_t)MOUSE_MAX_PX) px = (uint32_t)MOUSE_MAX_PX;

  return (int16_t)((v < 0) ? -(int16_t)px : (int16_t)px);
}

static void navMouse(int16_t x, int16_t y, uint32_t now)
{
  /* MOUSE_*_SIGN is the fixed stick-space -> HID-cursor-space conversion;
   * g_cfg.mouse_y_sign is the user's optional inversion on top of it. */
  int16_t dx = (int16_t)(MOUSE_X_SIGN * accelAxis(x));
  int16_t dy = (int16_t)(MOUSE_Y_SIGN * g_cfg.mouse_y_sign * accelAxis(y));

  if (dx == 0 && dy == 0) {
    g_mouse_next = now; /* re-centred: next deflection moves immediately */
    return;
  }
  if ((int32_t)(now - g_mouse_next) < 0) return;
  g_mouse_next = now + MOUSE_REPORT_MS;

  mouseMove((int8_t)clamp16(dx, -127, 127), (int8_t)clamp16(dy, -127, 127));
}

static void navAxes(int16_t x, int16_t y, uint32_t now)
{
  int16_t ax = iabs16(x);
  int16_t ay = iabs16(y);
  g_nav_mag  = (ax > ay) ? ax : ay;

  uint8_t mode = layerAt(g_layer).nav_mode;

  if (mode == NAV_MOUSE) {
    navMouse(x, y, now);
    g_nav_dir = stickDir(x, y);
    monoHintNav(g_nav_dir);
    return;
  }

  if (mode != NAV_ARROWS) {
    g_nav_dir = DIR_NONE;
    monoHintNav(DIR_NONE);
    return;
  }

  uint8_t dir = stickDir(x, y);
  if (dir != g_nav_dir) {
    g_nav_dir = dir;
    monoHintNav(dir);
    if (dir != DIR_NONE) {
      fireArrow(dir, now);
      g_nav_next_ms = now + NAV_REPEAT_DELAY_MS;
    }
    return;
  }
  if (dir != DIR_NONE && (int32_t)(now - g_nav_next_ms) >= 0) {
    fireArrow(dir, now);
    g_nav_next_ms = now + NAV_REPEAT_RATE_MS;
  }
}

/* Deflection -> emit interval. Just past the deadzone scrolls slowly, full
 * deflection scrolls fast; this is what makes the wheel "proportional to Y"
 * without ever sending a wheel delta the host has to interpret as a jump. */
static uint32_t scrollInterval(int16_t magnitude)
{
  int16_t  m    = clamp16(magnitude, JOY_DEADZONE, JOY_FULLSCALE);
  uint32_t span = (uint32_t)(JOY_FULLSCALE - JOY_DEADZONE);
  if (span == 0) return SCROLL_INTERVAL_MIN_MS;
  uint32_t t = ((uint32_t)(m - JOY_DEADZONE) * 100UL) / span; /* 0..100 */
  return SCROLL_INTERVAL_MAX_MS -
         (((SCROLL_INTERVAL_MAX_MS - SCROLL_INTERVAL_MIN_MS) * t) / 100UL);
}

static void scrollAxes(int16_t x, int16_t y, uint32_t now)
{
  int16_t ax    = iabs16(x);
  int16_t ay    = iabs16(y);
  g_scroll_mag  = (ax > ay) ? ax : ay;

  uint8_t mode = layerAt(g_layer).scroll_mode;
  if (mode == SCROLL_OFF) {
    g_scroll_x_dir = DIR_NONE;
    return;
  }

  /* ---- Y -> vertical wheel ---- */
  if (ay > JOY_DEADZONE) {
    if ((int32_t)(now - g_scroll_next_ms) >= 0) {
      mouseScroll((int8_t)((y > 0) ? SCROLL_STEP : -SCROLL_STEP), 0);
      monoHintScroll(y > 0, now);
      g_scroll_next_ms = now + scrollInterval(ay);
    }
  } else {
    /* Re-centred: next deflection should scroll immediately. */
    g_scroll_next_ms = now;
  }

  /* ---- X -> horizontal pan, or Left/Right arrows ---- */
  if (mode == SCROLL_WHEEL_PAN) {
    if (ax > JOY_DEADZONE) {
      if ((int32_t)(now - g_scroll_x_next) >= 0) {
        mouseScroll(0, (int8_t)((x > 0) ? PAN_STEP : -PAN_STEP));
        monoHintScroll(x > 0, now);
        g_scroll_x_next = now + scrollInterval(ax);
      }
    } else {
      g_scroll_x_next = now;
    }
    g_scroll_x_dir = DIR_NONE;
  } else { /* SCROLL_WHEEL_ARROWS */
    uint8_t dir = DIR_NONE;
    if (ax > JOY_DEADZONE) dir = (x > 0) ? DIR_RIGHT : DIR_LEFT;
    if (dir != g_scroll_x_dir) {
      g_scroll_x_dir = dir;
      if (dir != DIR_NONE) {
        fireArrow(dir, now);
        g_scroll_x_next = now + NAV_REPEAT_DELAY_MS;
      }
    } else if (dir != DIR_NONE && (int32_t)(now - g_scroll_x_next) >= 0) {
      fireArrow(dir, now);
      g_scroll_x_next = now + NAV_REPEAT_RATE_MS;
    }
  }
}

static int16_t detentRaw(uint16_t adc)
{
  int16_t d = (int16_t)(adc / ANGLE_STEP);
  return clamp16(d, 0, (int16_t)(ANGLE_DETENTS - 1));
}

/* Stay in the current detent until the reading clears its boundary by
 * ANGLE_HYSTERESIS counts - a bare adc/STEP would chatter on the edges. */
static int16_t detentHysteresis(uint16_t adc)
{
  int32_t lo = (int32_t)g_angle_detent * ANGLE_STEP;
  int32_t hi = lo + ANGLE_STEP;
  int32_t v  = (int32_t)adc;
  if (v >= lo - ANGLE_HYSTERESIS && v < hi + ANGLE_HYSTERESIS) return g_angle_detent;
  return detentRaw(adc);
}

static void angleUpdate(uint16_t adc, uint32_t now)
{
  if (!g_angle_have) {
    /* First reading after boot / re-enumeration: adopt it silently so the knob
     * does not dump a burst of volume steps at startup. */
    g_angle_detent = detentRaw(adc);
    g_angle_have   = true;
    return;
  }

  if (layerAt(g_layer).angle_mode == ANGLE_OFF) {
    /* The knob still MOVED, and the companion draws its position bar whatever
     * the layer does with it - a knob that turns and shows nothing reads as a
     * broken screen rather than as an unbound control. */
    int16_t idle_d = detentHysteresis(adc);
    if (idle_d != g_angle_detent) {
      g_angle_detent = idle_d;
      companionEventKnob((uint8_t)g_angle_detent);
    }
    return;
  }

  int16_t d = detentHysteresis(adc);
  if (d == g_angle_detent) return;

  int16_t delta  = (int16_t)(d - g_angle_detent);
  g_angle_detent = d;
  companionEventKnob((uint8_t)g_angle_detent);

  if (layerAt(g_layer).angle_mode == ANGLE_VOLUME) {
    /* 0..8 filled columns, so a knob at the very bottom still shows an (empty)
     * bar rather than looking like the panel went blank. */
    uint8_t level = (uint8_t)(((uint32_t)(g_angle_detent + 1) * 8UL) / ANGLE_DETENTS);
    monoHintVolume(level, now);
  }

  int16_t queued = (int16_t)(g_angle_queue + delta);
  g_angle_queue  = (int8_t)clamp16(queued, -(int16_t)ANGLE_MAX_QUEUE, (int16_t)ANGLE_MAX_QUEUE);
}

/* One detent crossing per ANGLE_EMIT_GAP_MS. Consumer-control reports sent
 * back to back in the same millisecond can be coalesced by the host, which
 * would silently swallow fast knob sweeps. */
static void angleTick(uint32_t now)
{
  if (g_angle_queue == 0) return;
  if ((int32_t)(now - g_angle_next_emit) < 0) return;
  g_angle_next_emit = now + ANGLE_EMIT_GAP_MS;

  int8_t dir    = (g_angle_queue > 0) ? 1 : -1;
  g_angle_queue = (int8_t)(g_angle_queue - dir);

  switch (layerAt(g_layer).angle_mode) {
    case ANGLE_VOLUME:
      tapConsumer((dir > 0) ? CONSUMER_CONTROL_VOLUME_INCREMENT
                            : CONSUMER_CONTROL_VOLUME_DECREMENT,
                  now);
      break;
    case ANGLE_WHEEL:
      mouseScroll((int8_t)(ANGLE_WHEEL_SIGN * dir * SCROLL_STEP), 0);
      monoHintScroll(dir > 0, now);
      break;
    default:
      break;
  }
}

/* ================================================================== */
/* Chain Key: single tap / double tap / layer-switch hold              */
/* ================================================================== */

static void chainKeyFire(const Action &a, uint32_t now, const char *tap_name)
{
  actionFire(a, now);
  ledFlashKey(0, now);
  ledFlashKey(1, now);
  g_ck_flash_until = now + NODE_LED_TAP_MS;
  protoEventTap(tap_name);
  companionEventTap(tap_name, actionLabel(a));
  FLOG("[chainkey] fire type=%u code=0x%02X\r\n", (unsigned)a.type, (unsigned)a.code);
}

static void chainKeyUpdate(bool down, uint32_t now)
{
  if (down && !g_ck_down) {
    /* Press edge. A press inside the double-tap window turns the pending
     * single into a double. */
    g_ck_down     = true;
    g_ck_consumed = false;
    g_ck_press_ms = now;

    if (g_ck_single_pending) {
      g_ck_single_pending = false;
      g_ck_double_armed   = true;
    } else {
      g_ck_double_armed = false;
    }
    return;
  }

  if (down && g_ck_down) {
    /* One layer = nothing to cycle to, so the long hold is INERT: no LED
     * flash, no panel scroll, and g_ck_consumed stays false so the tap and
     * double-tap still fire on release. (Since 0.2.0 layerCount() is a runtime
     * value, so this is a live compare rather than a build variant.) */
    if (layerCount() > 1 && !g_ck_consumed && (now - g_ck_press_ms) >= g_cfg.layer_hold_ms) {
      g_ck_consumed       = true; /* suppress the tap action on release */
      g_ck_single_pending = false;
      g_ck_double_armed   = false;
      cycleLayer(now);
    }
    return;
  }

  if (!down && g_ck_down) {
    g_ck_down = false;
    if (g_ck_consumed) return;

    if (g_ck_double_armed) {
      g_ck_double_armed = false;
      chainKeyFire(layerAt(g_layer).chain_key_double, now, "chain2");
      return;
    }

    /* With double-tap disabled there is nothing to wait for, so the single
     * fires instantly - that is the whole latency saving g_cfg.double_tap
     * buys, and why it is worth exposing to the host. */
    const Action &dbl = layerAt(g_layer).chain_key_double;
    if (!g_cfg.double_tap || dbl.type == ACT_NONE) {
      chainKeyFire(layerAt(g_layer).chain_key, now, "chain");
      return;
    }

    /* Capture the action now, so a layer change - or a set_action - during the
     * window cannot retarget a tap the user already made. */
    g_ck_single_action  = layerAt(g_layer).chain_key;
    g_ck_single_at      = now + g_cfg.double_tap_ms;
    g_ck_single_pending = true;
  }
}

/* ================================================================== */
/* Per-node polling                                                    */
/* ================================================================== */

static bool pollChainKey(uint32_t now)
{
  if (g_key_id == 0) return false;
  if ((int32_t)(now - g_key_poll_at) < 0) return false;
  g_key_poll_at = now + CHAIN_POLL_KEY_MS;

  uint8_t        status = 0;
  chain_status_t st = M5Chain.getKeyButtonStatus(g_key_id, &status, CHAIN_CALL_TIMEOUT_MS);
  chainResult(st);
  if (st == CHAIN_OK) chainKeyUpdate(status != 0, now);
  return true;
}

static bool pollNavStick(uint32_t now)
{
  if (g_nav_id == 0) return false;
  if ((int32_t)(now - g_nav_poll_at) < 0) return false;
  g_nav_poll_at = now + CHAIN_POLL_JOY_MS;

  int16_t        x = 0, y = 0;
  chain_status_t st =
      M5Chain.getJoystickMappedInt16Value(g_nav_id, &x, &y, CHAIN_CALL_TIMEOUT_MS);
  chainResult(st);
  if (st == CHAIN_OK) {
    /* The swap comes BEFORE the signs: swap then negate covers all four
     * 90-degree mountings, whereas negating first and swapping after would
     * reach only two of them. */
    if (g_cfg.nav_swap_xy) {
      int16_t t = x;
      x         = y;
      y         = t;
    }
    navAxes((int16_t)(g_cfg.nav_x_sign * x), (int16_t)(g_cfg.nav_y_sign * y), now);
  }

  uint8_t btn = 0;
  st = M5Chain.getJoystickButtonStatus(g_nav_id, &btn, CHAIN_CALL_TIMEOUT_MS);
  chainResult(st);
  if (st == CHAIN_OK) {
    bool down = (btn != 0);
    if (down && !g_nav_btn_down) {
      actionFire(layerAt(g_layer).nav_click, now);
      protoEventTap("nav");
      companionEventTap("nav", actionLabel(layerAt(g_layer).nav_click));
    }
    g_nav_btn_down = down;
  }
  return true;
}

static bool pollScrollStick(uint32_t now)
{
  if (g_scroll_id == 0) return false;
  if ((int32_t)(now - g_scroll_poll_at) < 0) return false;
  g_scroll_poll_at = now + CHAIN_POLL_JOY_MS;

  int16_t        x = 0, y = 0;
  chain_status_t st =
      M5Chain.getJoystickMappedInt16Value(g_scroll_id, &x, &y, CHAIN_CALL_TIMEOUT_MS);
  chainResult(st);
  if (st == CHAIN_OK) {
    if (g_cfg.scroll_swap_xy) {
      int16_t t = x;
      x         = y;
      y         = t;
    }
    scrollAxes((int16_t)(g_cfg.scroll_x_sign * x), (int16_t)(g_cfg.scroll_y_sign * y), now);
  }

  uint8_t btn = 0;
  st = M5Chain.getJoystickButtonStatus(g_scroll_id, &btn, CHAIN_CALL_TIMEOUT_MS);
  chainResult(st);
  if (st == CHAIN_OK) {
    bool down = (btn != 0);
    if (down && !g_scroll_btn_down) {
      actionFire(layerAt(g_layer).scroll_click, now);
      protoEventTap("scroll");
      companionEventTap("scroll", actionLabel(layerAt(g_layer).scroll_click));
    }
    g_scroll_btn_down = down;
  }
  return true;
}

static bool pollAngle(uint32_t now)
{
  if (g_angle_id == 0) return false;
  if ((int32_t)(now - g_angle_poll_at) < 0) return false;
  g_angle_poll_at = now + CHAIN_POLL_ANGLE_MS;

  uint16_t       adc = 0;
  chain_status_t st  = M5Chain.getAngle12BitAdc(g_angle_id, &adc, CHAIN_CALL_TIMEOUT_MS);
  chainResult(st);
  if (st == CHAIN_OK) angleUpdate(adc, now);
  return true;
}

/* ================================================================== */
/* Chained-node LEDs                                                   */
/* ================================================================== */

#if FLOW_NODE_LEDS

static uint8_t nodeLedId(uint8_t slot)
{
  switch (slot) {
    case NL_KEY:    return g_key_id;
    case NL_NAV:    return g_nav_id;
    case NL_SCROLL: return g_scroll_id;
    case NL_ANGLE:  return g_angle_id;
    default:        return 0;
  }
}

/* Joystick brightness is QUANTISED to NODE_LED_JOY_THRESHOLD steps rather than
 * compared with a tolerance: identical quantised values compare equal, so the
 * generic "want != sent" test below is already the rate filter, and a slowly
 * drifting stick cannot trickle out a write per poll. */
static void nodeLedJoystick(uint8_t slot, int16_t magnitude, uint32_t now)
{
  (void)now;

  uint8_t base[3];
  layerRgb(g_layer, base);

  int16_t m = clamp16(magnitude, 0, JOY_FULLSCALE);
  uint8_t pct =
      (uint8_t)(NODE_LED_JOY_IDLE_PCT +
                (((uint32_t)m * (100UL - NODE_LED_JOY_IDLE_PCT)) / (uint32_t)JOY_FULLSCALE));

  for (uint8_t i = 0; i < 3; i++) {
    uint8_t v = scalePct(base[i], pct);
    g_nl[slot].want[i] = (uint8_t)((v / NODE_LED_JOY_THRESHOLD) * NODE_LED_JOY_THRESHOLD);
  }
}

static void nodeLedsCompute(uint32_t now)
{
  /* ---- Chain Key: function colour, layer colour while a switch is counting,
   *      full-brightness flash on tap ---- */
  if (g_key_id != 0) {
    uint8_t rgb[3];
    bool    any_hold = g_key[0].hold_active || g_key[1].hold_active;
    uint8_t hs       = hostState();
    if (any_hold) {
      /* Mirror the DualKey keys: solid red while a dictation hold is active,
       * so the third key reads as "recording" too. */
      rgb[0] = g_cfg.hold_rgb[0];
      rgb[1] = g_cfg.hold_rgb[1];
      rgb[2] = g_cfg.hold_rgb[2];
    } else if (hs == HOST_TRANSCRIBING || hs == HOST_CLEANING) {
      rgb[0] = 255; rgb[1] = 150; rgb[2] = 0; /* amber: server working */
    } else if (hs == HOST_PASTED && (now - hostStateSince()) < 600) {
      rgb[0] = 0; rgb[1] = 255; rgb[2] = 60;  /* green: text landed */
    } else if (hs == HOST_ERROR && (now - hostStateSince()) < 900) {
      rgb[0] = ((now - hostStateSince()) / 150) & 1 ? 255 : 40; /* red blink */
      rgb[1] = 0; rgb[2] = 0;
    } else if (layerCount() > 1 && g_ck_down && !g_ck_consumed &&
        (now - g_ck_press_ms) >= NODE_LED_COUNT_AFTER_MS) {
      /* "keep holding" - show where the hold is going. Suppressed on a
       * single-layer device, where the hold goes nowhere. */
      uint8_t next = (uint8_t)((g_layer + 1) % layerCount());
      layerRgb(next, rgb);
    } else if ((int32_t)(now - g_ck_flash_until) < 0) {
      fnColor(layerAt(g_layer).chain_key.fn, g_layer, rgb);
    } else {
      fnColor(layerAt(g_layer).chain_key.fn, g_layer, rgb);
      for (uint8_t i = 0; i < 3; i++) rgb[i] = scalePct(rgb[i], 80);
      tintForBattery(rgb);
    }
    for (uint8_t i = 0; i < 3; i++) g_nl[NL_KEY].want[i] = rgb[i];
  }

  /* ---- Joysticks: brighten with deflection ---- */
  if (g_nav_id != 0) nodeLedJoystick(NL_NAV, g_nav_mag, now);
  if (g_scroll_id != 0) nodeLedJoystick(NL_SCROLL, g_scroll_mag, now);

  /* ---- Angle: hue tracks knob position, green -> red ----
   * Only ever changes when the detent changes, so no extra gating is needed. */
  if (g_angle_id != 0) {
    uint16_t t = (uint16_t)(((uint32_t)g_angle_detent * 255UL) / (ANGLE_DETENTS - 1));
    g_nl[NL_ANGLE].want[0] = (uint8_t)t;
    g_nl[NL_ANGLE].want[1] = (uint8_t)(255 - t);
    g_nl[NL_ANGLE].want[2] = 0;
  }
}

/* Writes AT MOST one node LED, round-robin, and only when that node's own rate
 * limit has expired. Returns true if a bus transaction was consumed. */
static bool nodeLedsService(uint32_t now)
{
  /* Runtime off switch (PROTOCOL.md `node_leds`). Nodes keep whatever colour
   * they were last given rather than going dark - turning our writes off is
   * not the same as turning the nodes off, and blanking them would cost a
   * write per node for a setting whose point is to stop writing. Re-enabling
   * repaints on the next colour change (or immediately, since a layer change
   * or re-enumeration clears `primed`). */
  if (!g_cfg.node_leds) return false;

  nodeLedsCompute(now);

  for (uint8_t n = 0; n < NL_COUNT; n++) {
    uint8_t slot = (uint8_t)((g_nl_rr + n) % NL_COUNT);
    uint8_t id   = nodeLedId(slot);
    if (id == 0) continue;

    NodeLed &L = g_nl[slot];
    if (L.primed && L.want[0] == L.sent[0] && L.want[1] == L.sent[1] &&
        L.want[2] == L.sent[2]) {
      continue;
    }
    if (L.primed && (int32_t)(now - L.next_ms) < 0) continue;

    uint8_t rgb[3] = {L.want[0], L.want[1], L.want[2]};
    uint8_t op     = 0;
    /* Node RGB brightness is left at the device default (40): writing it costs
     * an extra transaction and the setting is the same for every node. */
    chainResult(M5Chain.setRGBValue(id, 0, 1, rgb, 3, &op, CHAIN_CALL_TIMEOUT_MS));

    L.sent[0] = rgb[0];
    L.sent[1] = rgb[1];
    L.sent[2] = rgb[2];
    L.primed  = true;
    L.next_ms = now + NODE_LED_MIN_INTERVAL_MS;
    g_nl_rr   = (uint8_t)((slot + 1) % NL_COUNT);
    return true;
  }
  return false;
}

#else /* !FLOW_NODE_LEDS */

static bool nodeLedsService(uint32_t now)
{
  (void)now;
  return false;
}

#endif

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Bus auto-probe                                                       */
/* ------------------------------------------------------------------ */

/* The DualKey has two Chain ports and the vendor docs disagree about which
 * GPIO pair is which (and about TX/RX order on one of them). Rather than
 * make the user care, try every candidate pin pair until a node answers.
 * The winner is remembered; if the chain later goes quiet, the periodic
 * re-enumeration rotates through the candidates again, so plugging into
 * the other port at runtime also works. */
struct BusPins {
  int8_t rx, tx;
  const char *name;
};

/* Order = most likely first. Verified on hardware 2026-09-14: a chain on the
 * port next to the lanyard hole answered on G47 rx / G48 tx. */
static const BusPins BUS_CANDIDATES[] = {
    {CHAIN_LEFT_RX_PIN, CHAIN_LEFT_TX_PIN, "port1 (G47 rx / G48 tx)"},
    {CHAIN_RX_PIN, CHAIN_TX_PIN, "port2 (G5 rx / G6 tx)"},
    {CHAIN_LEFT_TX_PIN, CHAIN_LEFT_RX_PIN, "port1 swapped (G48 rx / G47 tx)"},
    {CHAIN_TX_PIN, CHAIN_RX_PIN, "port2 swapped (G6 rx / G5 tx)"},
};
static const uint8_t BUS_CANDIDATE_COUNT = sizeof(BUS_CANDIDATES) / sizeof(BUS_CANDIDATES[0]);
static uint8_t       g_bus_index         = 0;
static uint32_t      g_hotplug_check_at  = 0;

static void busSelect(uint8_t index)
{
  g_bus_index = (uint8_t)(index % BUS_CANDIDATE_COUNT);
  const BusPins &b = BUS_CANDIDATES[g_bus_index];
  CHAIN_UART.end();
  M5Chain.begin(&CHAIN_UART, CHAIN_BAUD, b.rx, b.tx);
  FLOG("[chain] bus -> %s\r\n", b.name);
}

const char *chainBusName(void)
{
  return BUS_CANDIDATES[g_bus_index].name;
}

void chainBusPins(int8_t *rx, int8_t *tx)
{
  const BusPins &b = BUS_CANDIDATES[g_bus_index];
  if (rx) *rx = b.rx;
  if (tx) *tx = b.tx;
}

void chainBegin(uint32_t now)
{
  for (uint8_t i = 0; i < BUS_CANDIDATE_COUNT; i++) {
    busSelect(i);
    delay(30); /* let the UART settle; setup() only, never in loop() */
    chainEnumerate(now);
    if (g_chain_ready) return;
  }
  /* Nothing answered on any pair: settle on the first and keep retrying
   * from chainService(), rotating pairs on every failed attempt. */
  busSelect(0);
}

void chainService(uint32_t now)
{
  if (!g_chain_ready) {
    if ((int32_t)(now - g_reenum_at) >= 0) {
      chainEnumerate(now);
      if (!g_chain_ready) busSelect((uint8_t)(g_bus_index + 1));
    }
    return;
  }

  /* A node at the end of the chain pushes an "enumeration request" when the
   * topology changes; the library counts those for us. */
  /* On real hardware the last node repeats its "enumerate please" packet
   * roughly every 700 ms even when nothing changed, so the counter alone
   * cannot be trusted. Rate-limit the check and only re-enumerate when the
   * node COUNT actually differs from what we have. */
  uint16_t ep = M5Chain.getEnumPleaseNum();
  if (ep != g_enum_please_seen) {
    g_enum_please_seen = ep;
    if ((int32_t)(now - g_hotplug_check_at) >= 0) {
      g_hotplug_check_at = now + CHAIN_HOTPLUG_CHECK_MS;
      uint16_t count = 0;
      chain_status_t st = M5Chain.getDeviceNum(&count, CHAIN_CALL_TIMEOUT_MS);
      if (st == CHAIN_OK && count != g_dev_list.count) {
        FLOG("[chain] hot-plug: %u -> %u device(s)\r\n", (unsigned)g_dev_list.count,
             (unsigned)count);
        g_chain_ready = false;
        g_reenum_at   = now;
      } else {
        /* Same count, but SOMETHING sent "enumerate please" - typically a
         * node that was unplugged and re-plugged fast enough to keep its id.
         * It came back with its LED dark, and our "already sent" cache would
         * never repaint it. Forget what we sent; the next idle loops repaint
         * every node (one transaction each, rate-limited as usual). Seen on
         * hardware 2026-09-15: Chain Key dark at idle after a re-plug. */
        for (uint8_t i = 0; i < NL_COUNT; i++) g_nl[i].primed = false;
        monoConfigChanged(); /* re-push rotation/brightness + redraw the panel */
      }
      return; /* this loop's bus transaction was the count check */
    }
  }

  /* The panel matters more than one extra input sample, and it only writes on
   * a state change, so give it first refusal. */
  if (monoService(now)) return;

  for (uint8_t n = 0; n < CHAIN_TASK_COUNT; n++) {
    uint8_t task = (uint8_t)((g_task_rr + n) % CHAIN_TASK_COUNT);
    bool    did  = false;
    switch (task) {
      case 0: did = pollChainKey(now); break;
      case 1: did = pollNavStick(now); break;
      case 2: did = pollScrollStick(now); break;
      case 3: did = pollAngle(now); break;
      default: break;
    }
    if (did) {
      g_task_rr = (uint8_t)((task + 1) % CHAIN_TASK_COUNT);
      return;
    }
  }

  /* Lowest priority: nothing above wanted the bus this iteration. */
  nodeLedsService(now);
}

void chainInputTick(uint32_t now)
{
  angleTick(now);

  if (g_ck_single_pending && (int32_t)(now - g_ck_single_at) >= 0) {
    g_ck_single_pending = false;
    chainKeyFire(g_ck_single_action, now, "chain");
  }
}

void chainOnLayerChanged(uint32_t now)
{
  /* Drop any in-flight stick/knob repeat so the new layer starts clean. */
  g_nav_dir           = DIR_NONE;
  g_scroll_x_dir      = DIR_NONE;
  g_scroll_next_ms    = now;
  g_scroll_x_next     = now;
  g_mouse_next        = now;
  g_angle_queue       = 0;
  g_ck_single_pending = false;
  g_ck_double_armed   = false;

  /* Repaint the chained nodes' own LEDs against the new layer. */
  for (uint8_t i = 0; i < NL_COUNT; i++) g_nl[i].primed = false;

  monoOnLayerChanged(now);
}

void chainPrintStatus(void)
{
  Serial.printf("\r\n--- flow-sidecar " FLOW_SIDECAR_VERSION " ---\r\n");
  Serial.printf("layer   : %u/%u %s\r\n", (unsigned)g_layer, (unsigned)layerCount(),
                layerAt(g_layer).name);
  Serial.printf("chain   : %s, %u device(s), bus %s\r\n", g_chain_ready ? "ready" : "DOWN",
                (unsigned)g_dev_list.count, chainBusName());
  for (uint16_t i = 0; i < g_dev_list.count; i++) {
    Serial.printf("  id=%u type=0x%04X\r\n", (unsigned)g_dev_list.devices[i].id,
                  (unsigned)g_dev_list.devices[i].device_type);
  }
  Serial.printf("roles   : nav=%u scroll=%u key=%u angle=%u mono=%u\r\n", (unsigned)g_nav_id,
                (unsigned)g_scroll_id, (unsigned)g_key_id, (unsigned)g_angle_id,
                (unsigned)g_mono_id);
  Serial.printf("angle   : detent %d/%d\r\n", (int)g_angle_detent, (int)ANGLE_DETENTS);
  Serial.printf("battery : %umV%s\r\n", (unsigned)batteryMilliVolts(),
                batteryLow() ? " (LOW)" : "");
  Serial.printf("usb     : %umV\r\n", (unsigned)usbMilliVolts());
  Serial.printf("holds   : key1=%u key2=%u\r\n", (unsigned)g_key[0].hold_active,
                (unsigned)g_key[1].hold_active);
  {
    uint32_t    now  = millis();
    const char *name = hostStateName(hostState());
    Serial.printf("host    : %s, state %s\r\n", hostPresent(now) ? "present" : "absent",
                  (name != NULL) ? name : "?");
  }
  Serial.printf("config  : %s, proto %u, idle %s\r\n",
                settingsLoadedFromNvs() ? "from NVS" : "defaults",
                (unsigned)FLOW_SIDECAR_PROTO,
                (g_cfg.mono_idle == MONO_IDLE_LETTER) ? "letter" : "blank");
  Serial.printf("keys    : key1=G%u key2=G%u (swap %u), nav swapxy %u, scroll swapxy %u\r\n",
                (unsigned)g_key[0].pin, (unsigned)g_key[1].pin, (unsigned)g_cfg.swap_keys,
                (unsigned)g_cfg.nav_swap_xy, (unsigned)g_cfg.scroll_swap_xy);
#if FLOW_COMPANION
  companionPrintStatus();
#endif
}
