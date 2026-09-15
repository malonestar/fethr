/* fethr UI controller.
 *
 * Talks to Python through window.pywebview.api (see fethr/ui/window.py::Api)
 * and receives pushes through window.fethrEvent(). Written as plain ES2020 —
 * no framework, no bundler, no network access at runtime.
 */
'use strict';

// ---------------------------------------------------------------- helpers --

const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const el = (tag, attrs = {}, ...kids) => {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') node.className = v;
    else if (k === 'style') node.style.cssText = v;
    else if (k.startsWith('on')) node.addEventListener(k.slice(2), v);
    else if (v !== null && v !== undefined) node.setAttribute(k, v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined) continue;
    node.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
  }
  return node;
};

const svg = (tag, attrs = {}, ...kids) => {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  for (const kid of kids.flat()) if (kid) node.append(kid);
  return node;
};

const rgbToHex = (rgb) => {
  if (!Array.isArray(rgb) || rgb.length < 3) return '#000000';
  return '#' + rgb.slice(0, 3)
    .map((c) => Math.max(0, Math.min(255, c | 0)).toString(16).padStart(2, '0'))
    .join('');
};

const hexToRgb = (hex) => [1, 3, 5].map((i) => parseInt(hex.substr(i, 2), 16));

const humanUptime = (seconds) => {
  if (seconds === null || seconds === undefined) return '—';
  const s = Math.floor(seconds);
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
};

const setNested = (path, value) => {
  const out = {};
  const parts = path.split('.');
  let cursor = out;
  parts.forEach((part, i) => {
    if (i === parts.length - 1) cursor[part] = value;
    else cursor = (cursor[part] = {});
  });
  return out;
};

function toast(message, kind = '') {
  const node = el('div', { class: `toast ${kind}` }, message);
  $('#toasts').append(node);
  setTimeout(() => {
    node.style.opacity = '0';
    node.style.transition = 'opacity .25s ease';
    setTimeout(() => node.remove(), 260);
  }, 3200);
}

// ------------------------------------------------------------- app state --

const state = {
  settings: null,
  device: { connected: false },
  config: null,
  activeLayer: 0,
  dirty: false,
  bridge: false,
};

const api = new Proxy({}, {
  get: (_t, name) => async (...args) => {
    if (!window.pywebview || !window.pywebview.api || !window.pywebview.api[name]) {
      throw new Error(`bridge unavailable: ${String(name)}`);
    }
    return window.pywebview.api[name](...args);
  },
});

// ------------------------------------------------------- the layer map ----
// Which layers exist is the DEVICE's answer: the `hello` reply carries the
// names, and the firmware ships FLOW only. The table below is just the
// human-readable description of the one layer we can describe; a layer the
// firmware was built with but this page has never heard of still gets a card,
// named, with a pointer at the firmware's own table.
// `fn` names index the device's fn_rgb palette so every row can show the
// colour the key actually lights up.

const LAYER_DETAIL = {
  FLOW: {
    letter: 'F',
    rows: [
      ['Key 1',       'Hold → F8 · raw dictation',                    'DICT_RAW'],
      ['Key 2',       'Hold → F9 · cleaned dictation',                'DICT_CLEAN'],
      ['Chain tap',   'F7 · re-paste the last transcript',            'REPASTE'],
      ['Chain 2×',    'Ctrl+Z · undo the paste you just made',        'UNDO'],
      ['Nav stick',   'Arrow keys with auto-repeat · click = Enter',  'ENTER'],
      ['Scroll',      'Wheel ∝ deflection, X pans · click = middle',  'MOUSE_M'],
      ['Knob',        'Volume up/down, 24 detents',                   null],
    ],
  },
};

/** Layer names the device reports, or the single layer the firmware ships. */
function deviceLayers() {
  const names = state.device && state.device.layers;
  return Array.isArray(names) && names.length ? names : ['FLOW'];
}

/** Name + letter + rows for one layer, described or not. */
function layerInfo(index) {
  const names = deviceLayers();
  const name = names[index] !== undefined ? names[index] : names[0];
  const detail = LAYER_DETAIL[name];
  return {
    name,
    letter: detail ? detail.letter : (name[0] || '?').toUpperCase(),
    rows: detail ? detail.rows : [],
  };
}

/** Function colour for a layer's Key 1 / Key 2 / Chain key, to tint the strip. */
function keyFn(index, row) {
  const rows = layerInfo(index).rows;
  return rows[row] ? rows[row][2] : null;
}

// ------------------------------------------------------------ navigation --

function initNav() {
  $$('.nav-item').forEach((button) => {
    button.addEventListener('click', () => {
      $$('.nav-item').forEach((b) => b.classList.toggle('is-active', b === button));
      const page = button.dataset.page;
      $$('.page').forEach((p) => p.classList.toggle('is-active', p.id === `page-${page}`));
      if (page === 'audio') refreshAudio();
      if (page === 'sidecar') refreshDevice();
    });
  });
}

function initTheme() {
  const toggle = $('#theme-toggle');
  toggle.addEventListener('click', () => {
    const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    setTheme(next);
    saveSetting('ui.theme', next);
  });
}

function setTheme(theme) {
  document.documentElement.dataset.theme = theme;
  $('#theme-label').textContent = theme === 'dark' ? 'Dark' : 'Light';
}

// -------------------------------------------------------------- settings --

/** Push one dotted setting to Python and keep the local copy in step. */
async function saveSetting(path, value) {
  const parts = path.split('.');
  let cursor = state.settings;
  for (const part of parts.slice(0, -1)) cursor = cursor[part];
  cursor[parts.at(-1)] = value;
  try {
    await api.set_settings(setNested(path, value));
  } catch (err) {
    toast(`Could not save ${path}: ${err}`, 'bad');
  }
}

function renderSettings(settings) {
  state.settings = settings;
  $$('[data-path]').forEach((input) => {
    const value = input.dataset.path.split('.').reduce((o, k) => (o ? o[k] : undefined), settings);
    if (value === undefined) return;
    if (input.type === 'checkbox') input.checked = Boolean(value);
    else input.value = value;
  });
  setTheme(settings.ui.theme === 'light' ? 'light' : 'dark');
  $('#first-run').classList.toggle('hidden', !settings.first_run);
  $$('.kbd[data-slot]').forEach((k) => {
    k.textContent = (settings.dictation[`hotkey_${k.dataset.slot}`] || '').toUpperCase();
  });
  renderSidecarEnabled(Boolean(settings.sidecar_enabled));
}

// ------------------------------------------------------- sidecar master ---

/** Show the clean "turn it on" card (disabled) or the full device UI (enabled). */
function renderSidecarEnabled(enabled) {
  $('#sidecar-off').classList.toggle('hidden', enabled);
  $('#sidecar-controls').classList.toggle('hidden', !enabled);
  const toggle = $('#sidecar-toggle');
  if (toggle) toggle.checked = enabled;
}

function initSidecarToggle() {
  const toggle = $('#sidecar-toggle');
  toggle.addEventListener('change', async () => {
    const enabled = toggle.checked;
    toggle.disabled = true;
    try {
      const r = await api.set_sidecar_enabled(enabled);
      if (!r.ok) {
        toggle.checked = !enabled;
        toast(r.error || 'could not change the sidecar state', 'bad');
        return;
      }
      if (r.settings) renderSettings(r.settings);
      if (r.status) renderStatus(r.status);
      if (enabled) refreshDevice();
      toast(enabled ? 'Sidecar enabled' : 'Sidecar disabled', 'good');
    } catch (err) {
      toggle.checked = !enabled;
      toast(String(err), 'bad');
    } finally {
      toggle.disabled = false;
    }
  });
}

function initSettingsInputs() {
  $$('[data-path]').forEach((input) => {
    if (input.classList.contains('hotkey')) return;  // handled by the capturer
    input.addEventListener('change', () => {
      let value = input.value;
      if (input.type === 'checkbox') value = input.checked;
      else if (input.type === 'number') value = Number(value);
      saveSetting(input.dataset.path, value);
    });
  });

  $('#btn-dismiss-first').addEventListener('click', async () => {
    $('#first-run').classList.add('hidden');
    try { await api.dismiss_first_run(); } catch { /* non-fatal */ }
  });
}

// -------------------------------------------------------- hotkey capture --

/** Map a DOM KeyboardEvent onto a name the `keyboard` package understands. */
function keyName(event) {
  const key = event.key;
  if (/^F\d{1,2}$/.test(key)) return key.toLowerCase();
  if (key === ' ') return 'space';
  if (key.length === 1) return key.toLowerCase();
  const named = {
    Escape: 'esc', Control: 'ctrl', Enter: 'enter', Tab: 'tab',
    Backspace: 'backspace', Delete: 'delete', Insert: 'insert',
    Home: 'home', End: 'end', PageUp: 'page up', PageDown: 'page down',
    ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
    Shift: 'shift', Alt: 'alt', Pause: 'pause', ScrollLock: 'scroll lock',
  };
  return named[key] || key.toLowerCase();
}

function initHotkeyCapture() {
  $$('.hotkey').forEach((input) => {
    input.addEventListener('focus', () => {
      input.classList.add('capturing');
      input.dataset.previous = input.value;
      input.value = 'press a key…';
    });
    input.addEventListener('blur', () => {
      if (input.classList.contains('capturing')) {
        input.classList.remove('capturing');
        input.value = input.dataset.previous || '';
      }
    });
    input.addEventListener('keydown', (event) => {
      event.preventDefault();
      if (event.key === 'Escape') { input.blur(); return; }
      if (['Control', 'Shift', 'Alt', 'Meta'].includes(event.key)) return;
      const name = keyName(event);
      input.classList.remove('capturing');
      input.value = name;
      input.dataset.previous = name;
      saveSetting(input.dataset.path, name);
      input.blur();
      toast(`Hotkey set to ${name.toUpperCase()}`, 'good');
    });
  });
}

// ---------------------------------------------------------- status & bar --

const STATE_LABELS = {
  idle: 'Idle', recording: '● Recording', transcribing: 'Transcribing…',
  cleaning: 'Cleaning up…', pasted: 'Pasted ✓', error: 'Error',
};

function setStatePill(stateName, detail) {
  const pill = $('#state-pill');
  pill.dataset.state = stateName;
  $('span', pill).textContent = STATE_LABELS[stateName] || stateName;
  pill.title = detail || '';
}

function setDot(node, up, label, warn = false) {
  node.classList.toggle('is-up', up === true && !warn);
  node.classList.toggle('is-down', up === false);
  node.classList.toggle('is-warn', Boolean(warn));
  $('em', node).textContent = label;
}

function renderStatus(status) {
  if (!status) return;
  const asr = status.asr || {};
  setDot($('#st-asr'), asr.ok, asr.ok ? `${asr.latency_ms ?? '?'} ms` : (asr.detail || 'down'));

  const cleanup = status.cleanup || {};
  const off = cleanup.detail === 'disabled';
  setDot($('#st-cleanup'), off ? null : cleanup.ok,
         off ? 'off' : (cleanup.ok ? `${cleanup.latency_ms ?? '?'} ms` : (cleanup.detail || 'down')),
         off);

  const dev = status.sidecar || {};
  state.device = dev;
  if (dev.enabled === false) {
    setDot($('#st-sidecar'), null, 'off');
  } else {
    const battery = dev.vbat_mv ? ` · ${dev.vbat_mv} mV` : '';
    setDot($('#st-sidecar'), dev.connected,
           dev.connected ? `${dev.fw || 'fw ?'}${battery}` : 'not found');
  }

  if (status.engine) {
    setStatePill(status.engine.state, '');
    renderLastTranscript(status.engine);
  }
  renderDeviceStats(dev);
}

function renderLastTranscript(engine) {
  if (!engine) return;
  if (engine.last_final) $('#last-transcript').value = engine.last_final;
  $('#last-meta').textContent = engine.last_ms
    ? `${engine.last_mode || 'raw'} · ${engine.last_ms} ms`
    : '';
}

// ------------------------------------------------------------- the pages --
// Sidecar

function renderDeviceStats(dev) {
  const stats = $('#dev-stats');
  const set = (k, v) => { $(`[data-k="${k}"]`, stats).textContent = v; };
  set('port', dev.port || '—');
  set('fw', dev.fw || '—');
  set('proto', dev.proto ?? '—');
  set('uptime', humanUptime(dev.uptime_s));
  set('vbat', dev.vbat_mv ? `${dev.vbat_mv} mV${dev.battery_low ? ' ⚠' : ''}` : '—');
  set('usb', dev.usb_mv ? `${dev.usb_mv} mV` : '—');

  $('#no-device').classList.toggle('hidden', Boolean(dev.connected));
  $$('.dev-only').forEach((node) => { node.disabled = !dev.connected; });
  $$('[data-dev]').forEach((node) => { node.disabled = !dev.connected; });
  if (typeof dev.layer === 'number') setActiveLayer(dev.layer);
}

function setActiveLayer(index) {
  state.activeLayer = index;
  $$('.layer-card').forEach((card, i) => card.classList.toggle('is-active', i === index));
  $$('#layer-switch .btn').forEach((b, i) =>
    b.classList.toggle('btn-primary', i === index));
  paintDeviceStrip();
}

function fnColour(name) {
  const palette = (state.config && state.config.fn_rgb) || {};
  return rgbToHex(palette[name] || [120, 130, 145]);
}

function layerColour(index) {
  const layers = (state.config && state.config.layer_rgb) || [];
  return rgbToHex(layers[index] || [77, 163, 255]);
}

function buildLayerUI() {
  const grid = $('#layer-grid');
  const strip = $('#layer-switch');
  grid.replaceChildren();
  strip.replaceChildren();

  const names = deviceLayers();
  const multi = names.length > 1;

  // Everything layer-related is only meaningful with more than one of them.
  $('#layers-hint').textContent = multi
    ? 'The Chain Key held for ~1 s cycles layers; the active one is highlighted.'
    : 'This firmware builds one layer, so the Chain Key’s long hold does nothing.';
  $('#layer-hold-field').classList.toggle('hidden', !multi);
  $('#boot-layer-field').classList.toggle('hidden', !multi);

  $('#boot-layer').replaceChildren(...names.map((_n, i) =>
    el('option', { value: i }, `${i} · ${layerInfo(i).name}`)));

  names.forEach((_name, index) => {
    const layer = layerInfo(index);

    // One layer means nothing to switch to, so the strip stays out of the way.
    if (names.length > 1) {
      strip.append(el('button', {
        class: 'btn', 'data-layer': index,
        onclick: () => switchLayer(index),
      }, `${index} · ${layer.name}`));
    }

    const map = el('dl', { class: 'map' });
    for (const [what, does, fn] of layer.rows) {
      map.append(el('dt', {},
        fn ? el('i', { class: 'fn-dot', 'data-fn': fn }) : el('i', { class: 'fn-dot', style: 'opacity:0' }),
        what));
      map.append(el('dd', {}, does));
    }
    if (names.length > 1) {
      map.append(el('dt', {}, el('i', { class: 'fn-dot', style: 'opacity:0' }), 'Chain hold'));
      map.append(el('dd', {}, 'Cycle to the next layer'));
    }
    if (!layer.rows.length) {
      map.append(el('dt', {}, el('i', { class: 'fn-dot', style: 'opacity:0' }), 'Bindings'));
      map.append(el('dd', {}, 'Defined in the firmware — see hardware/firmware/pio/src/layers.cpp'));
    }

    grid.append(el('div', { class: 'layer-card', 'data-layer': index },
      el('h3', {},
        el('span', { class: 'idx' }, String(index)),
        layer.name,
        el('span', { class: 'live' }, 'ACTIVE')),
      map));
  });
  paintLayerColours();
}

function paintLayerColours() {
  $$('.layer-card').forEach((card, i) => card.style.setProperty('--layer', layerColour(i)));
  $$('.fn-dot[data-fn]').forEach((dot) => {
    dot.style.background = fnColour(dot.dataset.fn);
  });
}

/** Simple inline illustration of the chain: DualKey, Key, 2 sticks, Angle, Mono. */
function buildDeviceStrip() {
  const strip = $('#device-strip');
  strip.replaceChildren();

  const box = (w, label, ...kids) => el('div', { class: 'node', 'data-node': label },
    svg('svg', { width: w, height: 64, viewBox: `0 0 ${w} 64` },
      svg('rect', {
        x: 1, y: 1, width: w - 2, height: 62, rx: 8,
        fill: 'var(--panel-2)', stroke: 'var(--border)',
      }),
      ...kids),
    el('span', {}, label));

  const led = (cx, cy, r, id) => svg('circle', {
    cx, cy, r, class: 'led', 'data-led': id, fill: 'var(--border)',
  });

  const link = () => el('div', { class: 'chain-link' });

  const dualkey = box(86, 'DualKey',
    svg('rect', { x: 10, y: 14, width: 30, height: 30, rx: 6, fill: 'var(--bg)', stroke: 'var(--border)' }),
    svg('rect', { x: 46, y: 14, width: 30, height: 30, rx: 6, fill: 'var(--bg)', stroke: 'var(--border)' }),
    led(25, 52, 4, 'key1'), led(61, 52, 4, 'key2'));

  const chainkey = box(52, 'Key',
    svg('rect', { x: 11, y: 14, width: 30, height: 30, rx: 6, fill: 'var(--bg)', stroke: 'var(--border)' }),
    led(20, 52, 4, 'chain'), led(32, 52, 4, 'chain'));

  const stick = (label) => box(52, label,
    svg('circle', { cx: 26, cy: 28, r: 15, fill: 'var(--bg)', stroke: 'var(--border)' }),
    svg('circle', { cx: 26, cy: 28, r: 6, fill: 'var(--border)' }),
    led(26, 52, 4, 'node'));

  const angle = box(52, 'Angle',
    svg('circle', { cx: 26, cy: 28, r: 15, fill: 'var(--bg)', stroke: 'var(--border)' }),
    svg('line', { x1: 26, y1: 28, x2: 26, y2: 15, stroke: 'var(--accent)', 'stroke-width': 2.5, 'stroke-linecap': 'round' }),
    led(26, 52, 4, 'node'));

  const dots = [];
  for (let row = 0; row < 8; row += 1) {
    for (let col = 0; col < 8; col += 1) {
      dots.push(svg('rect', {
        x: 9 + col * 5.2, y: 11 + row * 5.2, width: 3.4, height: 3.4, rx: 1,
        class: 'mono-px', fill: 'var(--border)',
      }));
    }
  }
  const mono = box(62, 'Mono', ...dots);

  strip.append(dualkey, link(), chainkey, link(), stick('Joystick'), link(),
               stick('Joystick'), link(), angle, link(), mono);
  paintDeviceStrip();
}

function paintDeviceStrip() {
  const idle = (state.config && state.config.led_idle_pct) || 18;
  const dim = (hex) => {
    const [r, g, b] = hexToRgb(hex);
    const k = 0.35 + (idle / 100) * 0.65;
    return `rgb(${Math.round(r * k)}, ${Math.round(g * k)}, ${Math.round(b * k)})`;
  };
  const colours = {
    key1: fnColour(keyFn(state.activeLayer, 0)),
    key2: fnColour(keyFn(state.activeLayer, 1)),
    chain: fnColour(keyFn(state.activeLayer, 2)),
    node: layerColour(state.activeLayer),
  };
  const on = state.config && state.config.node_leds !== false;
  $$('[data-led]').forEach((node) => {
    const id = node.dataset.led;
    const lit = id === 'key1' || id === 'key2' || on;
    node.setAttribute('fill', lit ? dim(colours[id] || '#4DA3FF') : 'var(--border)');
  });
  $$('.mono-px').forEach((px, i) => {
    // Draw the active layer's letter dimly so the panel is not a dead grid.
    const glyph = GLYPHS[layerInfo(state.activeLayer).letter];
    px.setAttribute('fill', glyph && glyph[i]
      ? layerColour(state.activeLayer) : 'var(--border)');
  });
}

/* 8x8 layer glyphs, mirroring the firmware's idle "letter" mode. A layer with
 * no glyph here simply leaves the illustrated panel dark. */
const GLYPHS = {
  F: bits([
    '11111111', '11000000', '11000000', '11111100',
    '11000000', '11000000', '11000000', '11000000']),
};

function bits(rows) {
  return rows.join('').split('').map((c) => c === '1');
}

function buildColourEditors() {
  const layerBox = $('#layer-colours');
  layerBox.replaceChildren();
  deviceLayers().forEach((_name, index) => {
    layerBox.append(el('div', { class: 'swatch' },
      el('input', { type: 'color', 'data-dev': `layer_rgb.${index}` }),
      el('span', {}, layerInfo(index).name)));
  });

  const fnBox = $('#fn-colours');
  fnBox.replaceChildren();
  const palette = (state.config && state.config.fn_rgb) || {};
  Object.keys(palette).forEach((name) => {
    fnBox.append(el('div', { class: 'swatch' },
      el('input', { type: 'color', 'data-dev': `fn_rgb.${name}` }),
      el('span', {}, name)));
  });

  const axes = $('#axis-grid');
  axes.replaceChildren();
  const signs = [
    ['nav_x_sign', 'Nav stick X'], ['nav_y_sign', 'Nav stick Y'],
    ['scroll_x_sign', 'Scroll X'], ['scroll_y_sign', 'Scroll Y'],
    ['mouse_y_sign', 'Mouse cursor Y'],
  ];
  for (const [path, label] of signs) {
    axes.append(el('label', { class: 'switch' },
      el('input', { type: 'checkbox', 'data-dev': path, 'data-sign': '1' }),
      el('i', {}), el('span', {}, `Invert ${label}`)));
  }
  bindDeviceInputs();
}

/** Read a dotted path out of the cached device config. */
function configAt(path) {
  return path.split('.').reduce((o, k) => {
    if (o === undefined || o === null) return undefined;
    return Array.isArray(o) ? o[Number(k)] : o[k];
  }, state.config);
}

function renderDeviceConfig() {
  if (!state.config) return;
  $$('[data-dev]').forEach((input) => {
    const value = configAt(input.dataset.dev);
    if (value === undefined) return;
    if (input.type === 'color') input.value = rgbToHex(value);
    else if (input.dataset.sign) input.checked = Number(value) < 0;
    else if (input.type === 'checkbox') input.checked = Boolean(value);
    else input.value = value;
    if (input.dataset.out) $(`#${input.dataset.out}`).value = value;
  });
  paintLayerColours();
  paintDeviceStrip();
}

/** Attach change handlers to every device control that lacks one.
 *  The colour swatches are rebuilt on each config load, so this runs often. */
function bindDeviceInputs() {
  $$('[data-dev]').forEach((input) => {
    if (!input.dataset.bound) attachDeviceInput(input);
  });
}

function attachDeviceInput(input) {
  input.dataset.bound = '1';
  const handler = async () => {
    let value;
    if (input.type === 'color') value = hexToRgb(input.value);
    else if (input.dataset.sign) value = input.checked ? -1 : 1;
    else if (input.type === 'checkbox') value = input.checked;
    else if (input.type === 'number' || input.type === 'range') value = Number(input.value);
    else if (input.tagName === 'SELECT' && /^-?\d+$/.test(input.value)) value = Number(input.value);
    else value = input.value;

    if (input.dataset.out) $(`#${input.dataset.out}`).value = input.value;

    const result = await api.device_set(input.dataset.dev, value).catch((e) => ({ ok: false, error: String(e) }));
    if (!result.ok) { toast(result.error || 'device rejected the change', 'bad'); return; }
    writeLocalConfig(input.dataset.dev, value);
    markDirty(true);
    paintLayerColours();
    paintDeviceStrip();
  };
  input.addEventListener('change', handler);
  if (input.type === 'range') {
    // Live-update the readout while dragging; only commit on release.
    input.addEventListener('input', () => {
      if (input.dataset.out) $(`#${input.dataset.out}`).value = input.value;
    });
  }
}

function writeLocalConfig(path, value) {
  if (!state.config) return;
  const parts = path.split('.');
  let cursor = state.config;
  for (const part of parts.slice(0, -1)) {
    cursor = Array.isArray(cursor) ? cursor[Number(part)] : cursor[part];
    if (cursor === undefined) return;
  }
  const leaf = parts.at(-1);
  if (Array.isArray(cursor)) cursor[Number(leaf)] = value;
  else cursor[leaf] = value;
}

function markDirty(dirty) {
  state.dirty = dirty;
  $('#unsaved').classList.toggle('hidden', !dirty);
}

async function switchLayer(index) {
  const result = await api.device_layer(index).catch((e) => ({ ok: false, error: String(e) }));
  if (result.ok) setActiveLayer(index);
  else toast(result.error || 'no sidecar connected', 'bad');
}

async function refreshDevice() {
  try {
    const status = await api.device_status();
    state.device = status;
    // The card list follows the device's own layer list, so rebuild it before
    // anything paints against it.
    buildLayerUI();
    renderDeviceStats(status);
    const cfg = await api.device_get_config();
    state.config = cfg.config;
    buildColourEditors();
    renderDeviceConfig();
    renderDeviceStats(status);
  } catch (err) {
    // Bridge missing (page opened outside pywebview) — leave defaults on screen.
    console.warn('device refresh failed', err);
  }
}

function initDeviceButtons() {
  $('#btn-identify').addEventListener('click', async () => {
    const r = await api.device_identify().catch((e) => ({ ok: false, error: String(e) }));
    toast(r.ok ? 'Sidecar flashing white' : (r.error || 'no device'), r.ok ? 'good' : 'bad');
  });
  $('#btn-reconnect').addEventListener('click', async () => {
    const r = await api.device_reconnect().catch((e) => ({ ok: false, error: String(e) }));
    toast(r.ok ? 'Sidecar connected' : (r.error || 'no sidecar detected'), r.ok ? 'good' : 'bad');
    refreshDevice();
  });
  $('#btn-dev-save').addEventListener('click', async () => {
    const r = await api.device_save().catch((e) => ({ ok: false, error: String(e) }));
    if (r.ok) { markDirty(false); toast('Saved to device NVS', 'good'); }
    else toast(r.error || 'save failed', 'bad');
  });
  $('#btn-dev-reset').addEventListener('click', async () => {
    const r = await api.device_reset().catch((e) => ({ ok: false, error: String(e) }));
    if (!r.ok) { toast(r.error || 'reset failed', 'bad'); return; }
    markDirty(false);
    toast('Device restored to factory defaults', 'good');
    refreshDevice();
  });
}

// Audio

async function refreshAudio() {
  let info;
  try { info = await api.audio_get(); } catch { return; }
  $('#audio-unavailable').classList.toggle('hidden', info.available);
  const slider = $('#volume');
  slider.disabled = !info.available;
  $('#btn-mute').disabled = !info.available;
  if (info.volume !== null && info.volume !== undefined) {
    slider.value = info.volume;
    $('#volume-out').value = `${info.volume}%`;
  }
  $('#btn-mute').textContent = info.muted ? 'Unmute' : 'Mute';

  const select = $('#mic-select');
  select.replaceChildren(el('option', { value: '' }, 'System default'));
  for (const device of info.devices) {
    select.append(el('option', { value: device.name },
      device.default ? `${device.name}  (default)` : device.name));
  }
  select.value = info.selected || '';
}

function initAudioControls() {
  $('#volume').addEventListener('input', (e) => { $('#volume-out').value = `${e.target.value}%`; });
  $('#volume').addEventListener('change', async (e) => {
    await api.audio_set_volume(Number(e.target.value)).catch(() => {});
  });
  $('#btn-mute').addEventListener('click', async () => {
    const r = await api.audio_toggle_mute().catch(() => ({ ok: false }));
    if (r.ok) $('#btn-mute').textContent = r.muted ? 'Unmute' : 'Mute';
  });
  $('#mic-select').addEventListener('change', (e) => {
    saveSetting('audio.input_device', e.target.value || null);
    toast('Microphone updated', 'good');
  });
  $('#btn-mic-refresh').addEventListener('click', refreshAudio);
}

// Dictation actions

function initDictationActions() {
  $('#btn-beep').addEventListener('click', () => api.test_beep().catch(() => {}));

  $('#btn-test').addEventListener('click', async () => {
    const button = $('#btn-test');
    const out = $('#test-result');
    button.disabled = true;
    out.className = 'result';
    out.textContent = 'Contacting the ASR server…';
    try {
      const r = await api.test_transcribe();
      out.className = `result ${r.ok ? 'ok' : 'bad'}`;
      out.textContent = r.ok
        ? `OK in ${r.ms} ms\n\n${r.text || '(the sample produced no text — the server answered, which is what this checks)'}`
        : `Failed after ${r.ms} ms\n\n${r.error}`;
    } catch (err) {
      out.className = 'result bad';
      out.textContent = String(err);
    } finally {
      button.disabled = false;
    }
  });

  $('#btn-repaste').addEventListener('click', async () => {
    const r = await api.repaste().catch(() => ({ ok: false }));
    toast(r.ok ? 'Re-pasted' : 'Nothing to paste yet', r.ok ? 'good' : '');
  });

  $('#btn-probe').addEventListener('click', async () => {
    const button = $('#btn-probe');
    button.classList.add('spin');
    try { renderStatus(await api.probe_servers()); } catch { /* offline */ }
    button.classList.remove('spin');
  });
}

// About

function initAbout() {
  $('#btn-github').addEventListener('click', () =>
    api.open_url('https://github.com/malonestar/fethr').catch(() => {}));
  $('#btn-open-folder').addEventListener('click', async () => {
    const r = await api.open_settings_folder().catch((e) => ({ ok: false, error: String(e) }));
    if (!r.ok) toast(r.error || 'could not open the folder', 'bad');
  });
  $('#btn-check-asr').addEventListener('click', async () => {
    $('#about-check').textContent = 'Checking…';
    try {
      const status = await api.probe_servers();
      renderStatus(status);
      const asr = status.asr;
      $('#about-check').textContent = asr.ok
        ? `ASR server responded in ${asr.latency_ms} ms (${asr.detail}).`
        : `ASR server unreachable: ${asr.detail}.`;
    } catch (err) {
      $('#about-check').textContent = String(err);
    }
  });
}

// ------------------------------------------------------------- push feed --

/** Called from Python via evaluate_js; see WindowManager.push(). */
window.fethrEvent = function fethrEvent(event) {
  if (!event || !event.type) return;
  switch (event.type) {
    case 'state':
      setStatePill(event.state, event.detail);
      break;
    case 'transcript':
      $('#last-transcript').value = event.final;
      $('#last-meta').textContent = `${event.mode} · ${event.ms} ms`;
      break;
    case 'status':
      renderStatus(event.status);
      break;
    case 'settings':
      if (event.settings) renderSettings(event.settings);
      break;
    case 'device':
      handleDeviceEvent(event.event || {});
      break;
    default:
      break;
  }
};

function handleDeviceEvent(ev) {
  switch (ev.ev) {
    case 'layer':
      setActiveLayer(ev.index);
      break;
    case 'hold':
      pulseKey(ev.key, ev.active);
      break;
    case 'battery':
      state.device.vbat_mv = ev.vbat_mv;
      renderDeviceStats(state.device);
      break;
    case 'chain':
      refreshDevice();
      break;
    case 'connected':
    case 'disconnected':
      refreshDevice();
      toast(ev.ev === 'disconnected' ? 'Sidecar unplugged' : 'Sidecar connected',
            ev.ev === 'disconnected' ? 'bad' : 'good');
      break;
    default:
      break;
  }
}

/** Flash the held key's LED red, matching the firmware's hold breathe. */
function pulseKey(key, active) {
  const node = $('[data-node="DualKey"]');
  if (!node) return;
  node.classList.toggle('pulse', Boolean(active));
  const led = $(`[data-led="key${key}"]`, node);
  if (active && led) led.setAttribute('fill', 'var(--err)');
  if (!active) paintDeviceStrip();
}

// ------------------------------------------------------------------ boot --

let booted = false;

async function boot() {
  if (booted) return;
  booted = true;
  initNav();
  initTheme();
  initSettingsInputs();
  initHotkeyCapture();
  initDictationActions();
  initDeviceButtons();
  initSidecarToggle();
  initAudioControls();
  initAbout();
  buildLayerUI();
  buildDeviceStrip();
  buildColourEditors();

  try {
    const info = await api.get_settings();
    state.bridge = true;
    renderSettings(info.settings);
    $('#brand-version').textContent = `v${info.version}`;
    $('#about-version').textContent = info.version;
    $('#about-path').textContent = info.settings_path;
    renderStatus(await api.get_status());
    renderLastTranscript(await api.get_engine_state());
    await refreshDevice();
    await refreshAudio();
  } catch (err) {
    console.warn('running without the Python bridge:', err);
    $$('input, select, textarea, .btn').forEach((n) => { n.disabled = true; });
  }
}

if (window.pywebview && window.pywebview.api) boot();
else window.addEventListener('pywebviewready', boot, { once: true });
// Fall back to a bridge-less render so the page is still inspectable in a browser.
setTimeout(boot, 3000);
