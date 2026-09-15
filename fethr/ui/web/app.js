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
// names, and the firmware ships FETHR only. The table below is just the
// human-readable description of the one layer we can describe; a layer the
// firmware was built with but this page has never heard of still gets a card,
// named, with a pointer at the firmware's own table.
// `fn` names index the device's fn_rgb palette so every row can show the
// colour the key actually lights up.

const LAYER_DETAIL = {
  FETHR: {
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
  return Array.isArray(names) && names.length ? names : ['FETHR'];
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

/** The Sidecar page's Layout / Settings tabs. */
function initTabs() {
  $$('#sidecar-tabs .tab').forEach((tab) => {
    tab.addEventListener('click', () => {
      $$('#sidecar-tabs .tab').forEach((other) => {
        const on = other === tab;
        other.classList.toggle('is-active', on);
        other.setAttribute('aria-selected', String(on));
      });
      $$('.tabpane').forEach((pane) =>
        pane.classList.toggle('is-active', pane.id === `tab-${tab.dataset.tab}`));
      // The canvas is laid out in absolute pixels, so it has to be (re)drawn
      // once it actually has a size.
      if (tab.dataset.tab === 'layout') renderCanvas();
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
  paintBuilderLayer();
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
    await syncBuilder(status);
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

// ========================================================= chain builder ==
//
// The Layout tab. The canvas is a picture of the user's own sidecar: where
// each M5Stack Chain module sits on the desk and which way round it is
// mounted. Placement is cosmetic and saved in settings; orientation is not —
// it is pushed to the device as axis settings the moment it changes.
//
// The order of the modules on the bus is NOT editable here. It is physical,
// the device reports it in `hello`/`status`, and the connector is drawn from
// that report. A module the user places that the device has not reported is
// drawn dimmed, because it is a plan rather than a fact.
//
// Two rules the code leans on:
//   * anything derived from the protocol (module footprints, allowed
//     rotations, the key vocabulary, the rotation -> settings truth table)
//     comes from Python, so the page cannot drift from PROTOCOL.md;
//   * nothing here assumes protocol 2. On a 0.1.x device the canvas still
//     works and the action editor says why it is off.

const builder = {
  model: null,        // api.builder_model(): modules, grid, vocabulary
  layout: null,       // {version, nodes: {key: {module, bus_id, x, y, rotation}}}
  chain: [],          // detected bus nodes, in order: {key, module, bus_id, role}
  companion: false,
  selected: null,     // layout key
  layers: [],         // proto 2 get_layers() detail
  editable: false,    // action editing available
  hint: '',           // why it is not
  layerIndex: 0,      // which layer the inspector is editing
  dragging: null,
  saveTimer: null,
};

/** Outward normal of the IN edge, per rotation, in screen coordinates (y down).
 *  Unrotated a module takes the bus in on its left and passes it out right;
 *  CSS rotate() turns that vector with the node. */
const IN_NORMAL = { 0: [-1, 0], 90: [0, -1], 180: [1, 0], 270: [0, 1] };

const gridPx = () => (builder.model ? builder.model.grid_px : 24);
const moduleSpec = (module) => (builder.model && builder.model.modules[module]) || null;
const negate = ([x, y]) => [-x, -y];

/** Pixel footprint of a placed node. */
function nodeSize(node) {
  const spec = moduleSpec(node.module);
  const g = gridPx();
  const cells = spec ? spec.cells : [3, 3];
  return [cells[0] * g, cells[1] * g];
}

/** Centre of a node's IN or OUT edge, in canvas pixels. */
function portPoint(node, which) {
  const [w, h] = nodeSize(node);
  const g = gridPx();
  const inward = IN_NORMAL[node.rotation] || IN_NORMAL[0];
  const n = which === 'in' ? inward : negate(inward);
  return [node.x * g + w / 2 + n[0] * (w / 2), node.y * g + h / 2 + n[1] * (h / 2)];
}

const chainEntry = (key) => builder.chain.find((c) => c.key === key) || null;
const roleFor = (key) => (chainEntry(key) || {}).role || null;

/** Is this node really there? Placed-but-absent modules are drawn dimmed.
 *  The DualKey and the companion are not bus nodes, so they have their own
 *  answers: the connection itself, and the `companion` field. */
function isDetected(key) {
  const node = builder.layout && builder.layout.nodes[key];
  if (!node) return false;
  if (node.module === 'dualkey') return Boolean(state.device && state.device.connected);
  if (node.module === 'atom') return builder.companion;
  return Boolean(chainEntry(key));
}

/** The DualKey entry, which is always present: it is the canvas's origin. */
function dualKeyEntry() {
  const found = Object.entries(builder.layout.nodes)
    .find(([, node]) => node.module === 'dualkey');
  return found ? { key: found[0], node: found[1] } : null;
}

/** Slots this module offers. Mirrors fethr.core.layout.slots_for(). */
function slotsFor(module, role) {
  const spec = moduleSpec(module);
  if (!spec) return [];
  if (module === 'joystick') {
    return builder.model.role_slots[role === 'scroll' ? 'scroll' : 'nav'] || spec.slots;
  }
  return spec.slots;
}

// ------------------------------------------------------------- lifecycle --

async function builderBoot() {
  if (builder.model) return;
  builder.model = await api.builder_model();
  builder.layout = await api.get_sidecar_layout();
  buildNodePicker();
  initBuilderControls();
  renderCanvas();
  renderInspector();
}

/** Fold a device status into the builder: bus order, roles, new modules. */
async function syncBuilder(status) {
  if (!builder.model || !builder.layout) return;

  let joysticks = 0;
  builder.chain = (status.nodes || [])
    .filter((node) => moduleSpec(node.type) && moduleSpec(node.type).chained)
    .map((node) => {
      let role = null;
      if (node.type === 'joystick') {
        // The device names the roles; two unnamed sticks are nav then scroll,
        // which is the order the firmware assigns them in.
        role = node.role || (joysticks === 0 ? 'nav' : 'scroll');
        joysticks += 1;
      }
      return { key: `${node.type}:${node.id}`, module: node.type, bus_id: node.id, role };
    });
  builder.companion = Boolean(status.companion && status.companion.linked);

  const placed = builder.layout.nodes;
  const missing = builder.chain.filter((c) => !placed[c.key]);
  if (missing.length && Object.keys(placed).length <= 1) {
    // Nothing has ever been arranged — lay the whole chain out at once.
    builder.layout = await api.layout_auto_arrange(status.nodes || [], builder.companion);
    persistLayout();
  } else if (missing.length) {
    // Keep the user's arrangement and drop the newcomers into free space.
    for (const item of missing) placed[item.key] = freshNode(item.module, item.bus_id);
    persistLayout();
  }
  if (builder.companion && !Object.values(placed).some((n) => n.module === 'atom')) {
    placed['atom:companion'] = freshNode('atom', 'companion');
    persistLayout();
  }

  await refreshBuilderLayers();
  renderCanvas();
  renderInspector();
}

/** Ask the device for its editable key map, or find out why we cannot. */
async function refreshBuilderLayers() {
  const reply = await api.device_get_layers().catch(() => null);
  if (!reply) { builder.editable = false; builder.layers = []; return; }
  builder.editable = Boolean(reply.editable);
  builder.layers = reply.layers || [];
  builder.hint = reply.hint || reply.error || '';
  if (builder.layerIndex >= Math.max(1, builder.layers.length)) builder.layerIndex = 0;
}

// ------------------------------------------------------------- placement --

/** A new node at the first free grid slot, scanning in reading order. */
function freshNode(module, busId) {
  const spec = moduleSpec(module);
  const [cols, rows] = builder.model.canvas_cells;
  const [w, h] = spec.cells;
  for (let y = 0; y + h <= rows; y += 1) {
    for (let x = 0; x + w <= cols; x += 1) {
      if (!overlapsAnything(x, y, w, h)) {
        return { module, bus_id: busId, x, y, rotation: spec.rotations[0] };
      }
    }
  }
  return { module, bus_id: busId, x: 0, y: 0, rotation: spec.rotations[0] };
}

function overlapsAnything(x, y, w, h) {
  return Object.values(builder.layout.nodes).some((node) => {
    const spec = moduleSpec(node.module);
    if (!spec) return false;
    const [nw, nh] = spec.cells;
    return x < node.x + nw && node.x < x + w && y < node.y + nh && node.y < y + h;
  });
}

function clampCell(x, y, module) {
  const [cols, rows] = builder.model.canvas_cells;
  const spec = moduleSpec(module);
  const [w, h] = spec ? spec.cells : [3, 3];
  return [
    Math.max(0, Math.min(Math.round(x), cols - w)),
    Math.max(0, Math.min(Math.round(y), rows - h)),
  ];
}

/** Save the canvas. Debounced: dragging a node fires this on every frame.
 *  The normalised document that comes back is deliberately NOT adopted — the
 *  user may have moved something else in the meantime, and the layout on screen
 *  is the one they are looking at. */
function persistLayout() {
  clearTimeout(builder.saveTimer);
  builder.saveTimer = setTimeout(async () => {
    const r = await api.set_sidecar_layout(builder.layout)
      .catch((err) => ({ ok: false, error: String(err) }));
    if (!r.ok) toast(r.error || 'could not save the layout', 'bad');
  }, 350);
}

// --------------------------------------------------------------- drawing --

function renderCanvas() {
  if (!builder.model || !builder.layout) return;
  const canvas = $('#canvas');
  const g = gridPx();
  const [cols, rows] = builder.model.canvas_cells;
  canvas.style.width = `${cols * g}px`;
  canvas.style.height = `${rows * g}px`;
  canvas.style.backgroundSize = `${g}px ${g}px`;

  $$('.bnode', canvas).forEach((node) => node.remove());
  for (const [key, node] of Object.entries(builder.layout.nodes)) {
    canvas.append(nodeElement(key, node));
  }
  drawWires();
  paintBuilderLayer();
  updateBuilderFoot();
}

function nodeElement(key, node) {
  const spec = moduleSpec(node.module);
  const g = gridPx();
  const [w, h] = nodeSize(node);
  const detected = isDetected(key);

  const wrap = el('div', {
    class: `bnode${builder.selected === key ? ' is-selected' : ''}`
         + `${detected ? '' : ' is-ghost'}`,
    'data-key': key,
    'data-module': node.module,
    style: `left:${node.x * g}px; top:${node.y * g}px; width:${w}px; height:${h}px`,
    title: detected ? spec.label : `${spec.label} — not detected`,
  });

  const rotated = el('div', {
    class: 'bnode-rot', style: `transform: rotate(${node.rotation}deg)`,
  }, el('img', { class: 'bnode-photo', src: `img/${spec.sprite}`, alt: '', draggable: 'false' }));
  rotated.append(...liveOverlays(key, node));
  wrap.append(rotated);

  wrap.append(...portBadges(node, detected));
  wrap.append(nameBadge(key, node, detected));
  return wrap;
}

/** The little blue arrow the modules wear on their side faces.
 *  `direction` is the screen vector the bus signal travels in. */
function arrowGlyph(direction) {
  const angle = Math.round(Math.atan2(direction[1], direction[0]) * 180 / Math.PI);
  return svg('svg', { viewBox: '0 0 10 10' },
    svg('path', { d: 'M2 1.2 L8.4 5 L2 8.8 Z', transform: `rotate(${angle} 5 5)` }));
}

/** An RGB indicator, as printed between IN and OUT on every Chain module. */
function bulbGlyph() {
  return svg('svg', { viewBox: '0 0 10 10' },
    svg('path', { d: 'M5 1a2.6 2.6 0 0 1 1.7 4.6V7H3.3V5.6A2.6 2.6 0 0 1 5 1Z' }),
    svg('rect', { x: 3.5, y: 7.8, width: 3, height: 1.4, rx: .6 }));
}

/** IN / OUT (and the DualKey's two ports) placed on the right edges.
 *  These are positioned from the rotation rather than rotated with the node,
 *  so "IN" never ends up upside down on a module mounted at 180. */
function portBadges(node, detected) {
  const [w, h] = nodeSize(node);
  const inward = IN_NORMAL[node.rotation] || IN_NORMAL[0];
  const flow = negate(inward);           // the way the signal travels
  const at = (normal) => `left:${w / 2 + normal[0] * (w / 2)}px;`
                       + `top:${h / 2 + normal[1] * (h / 2)}px;`
                       + 'transform: translate(-50%, -50%)';

  if (node.module === 'dualkey') {
    // Both Chain ports are identical sockets; the firmware probes which one
    // the bus came up on, so the labels describe roles, not connectors.
    return [
      el('span', { class: 'port', style: at(flow) }, arrowGlyph(flow), 'BUS'),
      el('span', { class: 'port is-aux', style: at(inward) }, 'AUX'),
      el('span', { class: 'mode-chip' }, 'BLE · OFF · USB'),
    ];
  }
  if (node.module === 'atom') {
    return [el('span', { class: 'port is-aux', style: at(inward) }, 'LINK')];
  }

  const badges = [
    el('span', { class: 'port', style: at(inward) }, arrowGlyph(flow), 'IN'),
    el('span', { class: 'port', style: at(flow) }, 'OUT', arrowGlyph(flow)),
  ];
  if (detected) {
    // The bulb sits between IN and OUT on the real sticker; on the canvas it
    // goes on the edge at right angles to the bus so it never collides.
    const side = [inward[1], -inward[0]];
    badges.push(el('span', { class: 'port is-rgb', style: at(side) }, bulbGlyph()));
  }
  return badges;
}

function nameBadge(key, node, detected) {
  const spec = moduleSpec(node.module);
  const kids = [];
  if (node.module === 'dualkey') {
    // The DualKey is the device, not a node on its bus: it has no id, and
    // "not detected" would be a confusing way to say "nothing is plugged in".
    const layer = deviceLayers()[state.activeLayer] || '';
    kids.push(el('i', { class: 'layer-dot' }), spec.short);
    if (layer) kids.push(el('span', { class: 'bus' }, layer));
  } else if (node.module === 'atom') {
    kids.push(spec.short);
    if (!detected) kids.push(el('span', { class: 'bus' }, 'not linked'));
  } else {
    kids.push(spec.short,
      el('span', { class: 'bus' }, detected ? `#${node.bus_id}` : 'not detected'));
  }
  return el('span', { class: 'bnode-name' }, kids);
}

/** Per-module overlay elements that the live device events light up. */
function liveOverlays(key, node) {
  switch (node.module) {
    case 'dualkey':
      return [
        el('i', { class: 'hotspot', 'data-hot': 'key1',
                  style: 'left:4%; top:8%; width:42%; height:80%' }),
        el('i', { class: 'hotspot', 'data-hot': 'key2',
                  style: 'left:54%; top:8%; width:42%; height:80%' }),
      ];
    case 'key':
      return [el('i', { class: 'hotspot', 'data-hot': 'chain',
                        style: 'left:10%; top:10%; width:80%; height:78%' })];
    case 'joystick':
      return [el('i', { class: 'hotspot', 'data-hot': `stick:${key}`,
                        style: 'left:26%; top:24%; width:48%; height:48%; border-radius:50%' })];
    case 'angle':
      return [svg('svg', { class: 'knob-arc', viewBox: '0 0 100 100', 'data-arc': key },
        svg('circle', { cx: 50, cy: 50, r: 34, 'stroke-dasharray': '0 214',
                        transform: 'rotate(-90 50 50)' }))];
    case 'mono':
      return [el('i', { class: 'mono-state', 'data-mono': '1' })];
    default:
      return [];
  }
}

function drawWires() {
  const wires = $('#wires');
  wires.replaceChildren();
  const g = gridPx();
  const [cols, rows] = builder.model.canvas_cells;
  wires.setAttribute('viewBox', `0 0 ${cols * g} ${rows * g}`);
  wires.setAttribute('width', cols * g);
  wires.setAttribute('height', rows * g);

  const dual = dualKeyEntry();
  if (!dual) return;

  let from = portPoint(dual.node, 'out');
  for (const item of builder.chain) {
    const node = builder.layout.nodes[item.key];
    if (!node) continue;
    const entry = portPoint(node, 'in');
    const exit = portPoint(node, 'out');
    wires.append(svg('path', { class: 'wire is-live', d: elbow(from, entry) }));
    // The in→out leg runs under the photograph; it is drawn so a rotated
    // module still reads as part of one continuous run.
    wires.append(svg('path', { class: 'wire is-live', d: elbow(entry, exit) }));
    from = exit;
  }

  const atom = Object.values(builder.layout.nodes).find((n) => n.module === 'atom');
  if (atom) {
    wires.append(svg('path', {
      class: 'wire', d: elbow(portPoint(dual.node, 'in'), portPoint(atom, 'in')),
    }));
  }
}

/** Orthogonal two-bend connector between two points. */
function elbow(a, b) {
  const [x1, y1] = a;
  const [x2, y2] = b;
  if (Math.abs(y1 - y2) < 1.5) return `M${x1},${y1} L${x2},${y2}`;
  const mid = (x1 + x2) / 2;
  return `M${x1},${y1} L${mid},${y1} L${mid},${y2} L${x2},${y2}`;
}

function paintBuilderLayer() {
  const colour = layerColour(state.activeLayer);
  $$('#canvas .bnode-name .layer-dot').forEach((dot) => {
    dot.style.background = colour;
  });
}

function updateBuilderFoot() {
  const total = Object.keys(builder.layout.nodes).length;
  const detected = Object.keys(builder.layout.nodes).filter(isDetected).length;
  const ghosts = total - detected;
  const parts = [`${detected} module${detected === 1 ? '' : 's'} detected`];
  if (ghosts) parts.push(`${ghosts} placed by hand`);
  const note = builderCapability();
  if (note) parts.push(note);
  $('#builder-foot').textContent = parts.join(' · ');
}

/** One phrase for what the builder can and cannot do with this device. */
function builderCapability() {
  if (!state.device || !state.device.connected) return 'no device — arrangement only';
  if (!builder.editable) return 'firmware 0.2 needed to edit actions';
  return '';
}

// ----------------------------------------------------------- interaction --

function initBuilderControls() {
  if (builder.wired) return;
  builder.wired = true;
  const wrap = $('#canvas-wrap');
  const canvas = $('#canvas');

  canvas.addEventListener('pointerdown', onNodePointerDown);
  wrap.addEventListener('pointerdown', onPanPointerDown);
  wrap.addEventListener('keydown', onBuilderKey);

  $('#btn-add-node').addEventListener('click', (event) => {
    event.stopPropagation();  // or the document handler below closes it again
    const picker = $('#node-picker');
    const open = picker.classList.contains('hidden');
    picker.classList.toggle('hidden', !open);
    $('#btn-add-node').setAttribute('aria-expanded', String(open));
  });
  document.addEventListener('click', (event) => {
    if (!event.target.closest('.picker-wrap')) {
      $('#node-picker').classList.add('hidden');
      $('#btn-add-node').setAttribute('aria-expanded', 'false');
    }
  });

  $('#btn-auto-arrange').addEventListener('click', async () => {
    builder.layout = await api.layout_auto_arrange(
      (state.device && state.device.nodes) || [], builder.companion);
    persistLayout();
    renderCanvas();
    renderInspector();
    toast('Arranged in bus order', 'good');
  });

  $('#btn-clear-layout').addEventListener('click', async () => {
    builder.layout = await api.layout_auto_arrange([], false);
    builder.selected = null;
    persistLayout();
    renderCanvas();
    renderInspector();
  });
}

function buildNodePicker() {
  const picker = $('#node-picker');
  picker.replaceChildren();
  for (const [module, spec] of Object.entries(builder.model.modules)) {
    if (module === 'dualkey') continue;  // the origin, and there is only one
    picker.append(el('button', {
      class: 'picker-item', type: 'button', role: 'menuitem',
      onclick: () => addNode(module),
    }, el('img', { src: `img/${spec.sprite}`, alt: '' }), spec.label));
  }
}

/** Place a module the device has not reported (yet).
 *  It takes the lowest free bus id for its type, so when the real one turns
 *  up with that id the placeholder simply becomes detected. */
function addNode(module) {
  const used = new Set(Object.values(builder.layout.nodes)
    .filter((n) => n.module === module)
    .map((n) => String(n.bus_id)));
  let busId = module === 'atom' ? 'companion' : 1;
  while (used.has(String(busId))) busId = Number(busId) + 1;
  const key = `${module}:${busId}`;
  if (builder.layout.nodes[key]) return;
  builder.layout.nodes[key] = freshNode(module, busId);
  builder.selected = key;
  persistLayout();
  renderCanvas();
  renderInspector();
  $('#node-picker').classList.add('hidden');
}

function removeNode(key) {
  const node = builder.layout.nodes[key];
  if (!node || node.module === 'dualkey') return;
  if (isDetected(key)) {
    toast('That module is plugged in — unplug it to take it off the canvas', '');
    return;
  }
  delete builder.layout.nodes[key];
  if (builder.selected === key) builder.selected = null;
  persistLayout();
  renderCanvas();
  renderInspector();
}

function selectNode(key) {
  if (builder.selected === key) return;
  builder.selected = key;
  $$('#canvas .bnode').forEach((node) =>
    node.classList.toggle('is-selected', node.dataset.key === key));
  renderInspector();
}

function onNodePointerDown(event) {
  const target = event.target.closest('.bnode');
  if (!target) return;
  event.stopPropagation();
  const key = target.dataset.key;
  selectNode(key);
  $('#canvas-wrap').focus({ preventScroll: true });

  const node = builder.layout.nodes[key];
  builder.dragging = {
    key, el: target, pointerId: event.pointerId,
    startX: event.clientX, startY: event.clientY,
    originX: node.x, originY: node.y, moved: false,
  };
  // Capture keeps the drag alive if the pointer outruns the node. It can
  // legitimately fail (a pointer that is already gone), and a drag that cannot
  // be captured is still a drag.
  try { target.setPointerCapture(event.pointerId); } catch { /* not fatal */ }
  target.classList.add('is-dragging');
  target.addEventListener('pointermove', onNodePointerMove);
  target.addEventListener('pointerup', onNodePointerUp);
  target.addEventListener('pointercancel', onNodePointerUp);
}

function onNodePointerMove(event) {
  const drag = builder.dragging;
  if (!drag || event.pointerId !== drag.pointerId) return;
  const g = gridPx();
  const node = builder.layout.nodes[drag.key];
  const [x, y] = clampCell(
    drag.originX + (event.clientX - drag.startX) / g,
    drag.originY + (event.clientY - drag.startY) / g,
    node.module,
  );
  if (x === node.x && y === node.y) return;
  drag.moved = true;
  node.x = x;
  node.y = y;
  drag.el.style.left = `${x * g}px`;
  drag.el.style.top = `${y * g}px`;
  drawWires();
}

function onNodePointerUp(event) {
  const drag = builder.dragging;
  if (!drag) return;
  drag.el.classList.remove('is-dragging');
  drag.el.removeEventListener('pointermove', onNodePointerMove);
  drag.el.removeEventListener('pointerup', onNodePointerUp);
  drag.el.removeEventListener('pointercancel', onNodePointerUp);
  try { drag.el.releasePointerCapture(event.pointerId); } catch { /* already gone */ }
  builder.dragging = null;
  if (drag.moved) { persistLayout(); updateBuilderFoot(); }
}

/** Dragging the empty canvas pans it; the wrapper's own scrollbars do the work. */
function onPanPointerDown(event) {
  if (event.target.closest('.bnode')) return;
  const wrap = $('#canvas-wrap');
  wrap.focus({ preventScroll: true });
  const start = {
    x: event.clientX, y: event.clientY,
    left: wrap.scrollLeft, top: wrap.scrollTop,
  };
  const move = (moveEvent) => {
    wrap.scrollLeft = start.left - (moveEvent.clientX - start.x);
    wrap.scrollTop = start.top - (moveEvent.clientY - start.y);
  };
  const up = () => {
    wrap.classList.remove('is-panning');
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', up);
  };
  wrap.classList.add('is-panning');
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up);
}

const NUDGE = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };

function onBuilderKey(event) {
  const key = builder.selected;
  if (!key || !builder.layout.nodes[key]) return;
  const node = builder.layout.nodes[key];

  if (NUDGE[event.key]) {
    event.preventDefault();
    const [dx, dy] = NUDGE[event.key];
    const [x, y] = clampCell(node.x + dx, node.y + dy, node.module);
    if (x === node.x && y === node.y) return;
    node.x = x;
    node.y = y;
    renderCanvas();
    persistLayout();
    return;
  }
  if (event.key === 'r' || event.key === 'R') {
    event.preventDefault();
    rotateNode(key, event.shiftKey ? -1 : 1);
    return;
  }
  if (event.key === 'Delete' || event.key === 'Backspace') {
    event.preventDefault();
    removeNode(key);
  }
}

// --------------------------------------------------------- orientation ----

function rotateNode(key, direction) {
  const node = builder.layout.nodes[key];
  const spec = moduleSpec(node.module);
  const list = spec.rotations;
  const at = Math.max(0, list.indexOf(node.rotation));
  const step = direction < 0 ? list.length - 1 : 1;
  return setNodeRotation(key, list[(at + step) % list.length]);
}

/** Turn a module on the canvas and tell the firmware which way it now faces.
 *  The truth table lives in fethr.core.layout.orientation_settings(). */
async function setNodeRotation(key, rotation) {
  const node = builder.layout.nodes[key];
  if (!node || node.rotation === rotation) return;
  node.rotation = rotation;
  renderCanvas();
  renderInspector();
  persistLayout();

  const result = await api
    .device_apply_orientation(node.module, rotation, roleFor(key))
    .catch((err) => ({ ok: false, error: String(err) }));

  if (!result.ok) {
    if (state.device && state.device.connected) {
      toast(result.error || 'the device refused the orientation', 'bad');
    }
    return;
  }
  const applied = result.applied || {};
  if (Object.keys(applied).length) {
    for (const [path, value] of Object.entries(applied)) writeLocalConfig(path, value);
    renderDeviceConfig();
    markDirty(true);
  }
  if ((result.dropped || []).length) {
    toast(`This firmware cannot store ${result.dropped.join(', ')} — needs 0.2`, '');
  }
}

// ------------------------------------------------------------- inspector --

function renderInspector() {
  const box = $('#inspector');
  if (!box) return;
  box.replaceChildren();
  if (!builder.model || !builder.layout) return;

  const key = builder.selected;
  const node = key ? builder.layout.nodes[key] : null;
  if (!node) {
    box.append(el('p', { class: 'empty' },
      'Select a module on the canvas to place it and remap its keys.'));
    return;
  }

  const spec = moduleSpec(node.module);
  const role = roleFor(key);
  const detected = isDetected(key);
  box.append(el('h3', {}, spec.label));
  box.append(el('p', { class: 'sub' }, detected
    ? `Bus id ${node.bus_id}${role ? ` · ${role} stick` : ''}`
    : 'Not detected — placed by hand'));

  box.append(orientationSection(key, node, spec));

  const slots = slotsFor(node.module, role);
  const modeField = modeFieldFor(node.module, role);
  if (!slots.length && !modeField) {
    box.append(el('div', { class: 'insp-section' },
      el('h4', {}, 'Bindings'),
      el('p', { class: 'muted small' }, informational(node.module))));
    return;
  }

  if (!builder.editable) {
    box.append(el('div', { class: 'insp-hint' },
      builder.hint || 'Editing what the keys do needs firmware 0.2.'));
  }
  box.append(layerTabs());
  box.append(layerSection(modeField));
  for (const slot of slots) box.append(slotSection(slot));
}

function informational(module) {
  if (module === 'atom') {
    return 'The companion display renders whatever the sidecar sends it; '
         + 'it holds no key map of its own.';
  }
  if (module === 'mono') {
    return 'The 8×8 panel shows the layer letter and the engine state. '
         + 'Its brightness and idle behaviour are on the Settings tab.';
  }
  return 'This module has no bindable controls.';
}

function orientationSection(key, node, spec) {
  const section = el('div', { class: 'insp-section' }, el('h4', {}, 'Orientation'));
  const row = el('div', { class: 'rot-row' });
  for (const angle of spec.rotations) {
    row.append(el('button', {
      class: `rot-btn${node.rotation === angle ? ' is-active' : ''}`,
      type: 'button',
      onclick: () => setNodeRotation(key, angle),
    }, `${angle}°`));
  }
  section.append(row);

  const note = {
    dualkey: '180° is USB away from you, which swaps Key 1 and Key 2.',
    joystick: 'The stick’s axes follow the module, so "up" stays up.',
    mono: 'The panel rotates its glyphs to match.',
  }[node.module];
  if (note) section.append(el('p', { class: 'muted small', style: 'margin:8px 0 0' }, note));
  if (node.module !== 'dualkey') {
    section.append(el('div', { class: 'actions', style: 'margin-top:8px' },
      el('button', {
        class: 'btn btn-ghost', type: 'button', onclick: () => removeNode(key),
        disabled: isDetected(key) ? '' : null,
        title: isDetected(key) ? 'Unplug it to take it off the canvas' : null,
      }, 'Remove from canvas')));
  }
  return section;
}

function layerTabs() {
  const names = builder.layers.length
    ? builder.layers.map((layer) => layer.name)
    : deviceLayers();
  const row = el('div', { class: 'layer-tabs' });
  names.forEach((name, index) => {
    const layer = builder.layers[index];
    const colour = layer && layer.rgb ? rgbToHex(layer.rgb) : layerColour(index);
    row.append(el('button', {
      class: `layer-tab${index === builder.layerIndex ? ' is-active' : ''}`,
      type: 'button', style: `--layer:${colour}`,
      onclick: () => { builder.layerIndex = index; renderInspector(); },
    }, el('i', { class: 'layer-dot' }), name));
  });
  return row;
}

/** Which layer-level mode dropdown, if any, belongs to this module. */
function modeFieldFor(module, role) {
  if (module === 'joystick') return role === 'scroll' ? 'scroll_mode' : 'nav_mode';
  if (module === 'angle') return 'angle_mode';
  return null;
}

function currentLayer() {
  return builder.layers[builder.layerIndex] || null;
}

/** Layer name and colour, plus the one control mode this module owns.
 *  All three are `set_layer_meta` fields, so they share a section. */
function layerSection(modeField) {
  const layer = currentLayer();
  const section = el('div', { class: 'insp-section' }, el('h4', {}, 'This layer'));
  section.append(layerMetaRow(layer));

  if (!modeField) return section;
  const vocab = builder.model.vocabulary;
  const choices = vocab[`${modeField.replace('_mode', '')}_modes`] || [];
  const select = el('select', { disabled: builder.editable ? null : '' },
    ...choices.map((choice) => el('option', { value: choice.id }, choice.label)));
  if (layer && layer[modeField]) select.value = layer[modeField];
  select.addEventListener('change', async () => {
    const r = await api
      .device_set_layer_meta(builder.layerIndex, { [modeField]: select.value })
      .catch((err) => ({ ok: false, error: String(err) }));
    if (!r.ok) { toast(r.error || 'the device refused that mode', 'bad'); return; }
    if (layer) layer[modeField] = select.value;
    markDirty(true);
  });
  section.append(el('label', { class: 'field' },
    el('span', {}, modeField === 'angle_mode' ? 'Knob mode' : 'Stick mode'), select));
  return section;
}

/** Layer name + colour, editable on protocol 2. */
function layerMetaRow(layer) {
  const row = el('div', { class: 'row-2' });
  const name = el('input', {
    type: 'text', maxlength: '8', disabled: builder.editable ? null : '',
    value: layer ? layer.name : (deviceLayers()[builder.layerIndex] || ''),
  });
  const colour = el('input', {
    type: 'color', disabled: builder.editable ? null : '',
    value: layer && layer.rgb ? rgbToHex(layer.rgb) : layerColour(builder.layerIndex),
  });
  const push = async (fields) => {
    const r = await api.device_set_layer_meta(builder.layerIndex, fields)
      .catch((err) => ({ ok: false, error: String(err) }));
    if (!r.ok) { toast(r.error || 'the device refused that change', 'bad'); return; }
    Object.assign(layer || {}, fields);
    markDirty(true);
    renderInspector();
  };
  // 1-8 characters, refused rather than truncated (PROTOCOL.md §Actions), so
  // put the limit on the field and do not send an empty one.
  name.addEventListener('change', () => {
    const value = name.value.trim();
    if (!value) { renderInspector(); return; }   // put the old name back
    push({ name: value });
  });
  colour.addEventListener('change', () => push({ rgb: hexToRgb(colour.value) }));
  row.append(
    el('label', { class: 'field' }, el('span', {}, 'Layer name'), name),
    el('label', { class: 'field' }, el('span', {}, 'Layer colour'), colour));
  return row;
}

const NO_ACTION = { type: 'none', key: '', mods: [], fn: 'custom' };

/** One slot's editor: type, key, modifiers, function class, Apply. */
function slotSection(slot) {
  const vocab = builder.model.vocabulary;
  const layer = currentLayer();
  const bound = (layer && layer.actions && layer.actions[slot]) || NO_ACTION;
  const draft = { ...NO_ACTION, ...bound, mods: [...(bound.mods || [])] };
  const disabled = builder.editable ? null : '';

  const section = el('div', { class: 'slot' });
  const dot = el('i', { class: 'fn-dot' });
  const summary = el('span', { class: 'summary' });
  section.append(el('div', { class: 'slot-head' }, dot,
    vocab.slots[slot] || slot, summary));

  const typeSelect = el('select', { disabled },
    ...vocab.types.map((type) => el('option', { value: type.id }, type.label)));
  typeSelect.value = draft.type;
  section.append(el('label', { class: 'field' }, el('span', {}, 'Does'), typeSelect));

  const search = el('input', { type: 'text', placeholder: 'search keys…', disabled });
  const list = el('div', { class: 'key-list' });
  const keyBox = el('div', { class: 'key-search' },
    el('label', { class: 'field' }, el('span', {}, 'Key'), search), list);
  section.append(keyBox);

  const mods = el('div', { class: 'mods' });
  for (const modifier of vocab.modifiers) {
    const input = el('input', { type: 'checkbox', disabled });
    input.checked = draft.mods.includes(modifier.id);
    input.addEventListener('change', () => {
      draft.mods = input.checked
        ? [...draft.mods, modifier.id]
        : draft.mods.filter((m) => m !== modifier.id);
      refresh();
    });
    mods.append(el('label', {}, input, modifier.label));
  }
  section.append(mods);

  const fnSelect = el('select', { disabled },
    ...vocab.fn_classes.map((fn) => el('option', { value: fn.id }, fn.label)));
  fnSelect.value = draft.fn;
  section.append(el('label', { class: 'field' }, el('span', {}, 'Looks like'), fnSelect));

  const apply = el('button', { class: 'btn btn-primary', type: 'button', disabled },
    'Apply');
  section.append(el('div', { class: 'actions' }, apply));

  const keysFor = () =>
    (vocab.types.find((type) => type.id === draft.type) || { keys: [] }).keys;

  function drawKeys() {
    const needle = search.value.trim().toLowerCase();
    const options = keysFor().filter((name) =>
      !needle || name.includes(needle) || (vocab.key_labels[name] || '').toLowerCase().includes(needle));
    list.replaceChildren();
    if (!keysFor().length) {
      list.append(el('div', { class: 'none' }, 'This action needs no key.'));
      return;
    }
    if (!options.length) {
      list.append(el('div', { class: 'none' }, 'Nothing matches.'));
      return;
    }
    for (const name of options) {
      list.append(el('button', {
        class: `key-option${name === draft.key ? ' is-chosen' : ''}`,
        type: 'button', disabled,
        onclick: () => { draft.key = name; refresh(); },
      }, vocab.key_labels[name] || name));
    }
  }

  function refresh() {
    const palette = (builder.model.vocabulary.fn_classes
      .find((fn) => fn.id === draft.fn) || {}).palette;
    dot.style.background = palette ? fnColour(palette) : 'var(--muted)';
    summary.textContent = describeAction(draft);
    keyBox.classList.toggle('hidden', !keysFor().length);
    mods.classList.toggle('hidden',
      draft.type !== 'key_hold' && draft.type !== 'key_tap');
    drawKeys();
  }

  typeSelect.addEventListener('change', () => {
    draft.type = typeSelect.value;
    if (!keysFor().includes(draft.key)) draft.key = '';
    refresh();
  });
  fnSelect.addEventListener('change', () => { draft.fn = fnSelect.value; refresh(); });
  search.addEventListener('input', drawKeys);

  apply.addEventListener('click', async () => {
    const r = await api.device_set_action(builder.layerIndex, slot, draft)
      .catch((err) => ({ ok: false, error: String(err) }));
    if (!r.ok) { toast(r.error || 'the device refused that binding', 'bad'); return; }
    if (layer) layer.actions = { ...(layer.actions || {}), [slot]: r.action || draft };
    markDirty(true);
    toast(`${vocab.slots[slot] || slot} → ${describeAction(draft)}`, 'good');
  });

  refresh();
  return section;
}

function describeAction(action) {
  if (!action || action.type === 'none' || !action.key) return 'unbound';
  const mods = (action.mods || []).map((m) => (m === 'gui' ? 'Win' : m));
  const label = (builder.model.vocabulary.key_labels || {})[action.key] || action.key;
  return [...mods, label].join('+');
}

// ------------------------------------------------------- live overlay ----
// The canvas mirrors what the device is doing, using the same events the
// panel does. None of it touches the stored layout.

function hotspotsFor(tapKey) {
  const dual = dualKeyEntry();
  if (tapKey === '1' || tapKey === '2') {
    return dual ? $$(`[data-key="${dual.key}"] [data-hot="key${tapKey}"]`) : [];
  }
  if (tapKey === 'chain' || tapKey === 'chain2') return $$('[data-hot="chain"]');
  if (tapKey === 'nav' || tapKey === 'scroll') {
    const entry = builder.chain.find((c) => c.role === tapKey);
    return entry ? $$(`[data-hot="stick:${entry.key}"]`) : [];
  }
  return [];
}

function builderHold(key, active) {
  for (const spot of hotspotsFor(String(key))) {
    spot.classList.toggle('is-held', Boolean(active));
  }
}

function builderTap(key) {
  for (const spot of hotspotsFor(String(key))) {
    spot.classList.remove('is-tapped');
    void spot.offsetWidth;  // restart the animation
    spot.classList.add('is-tapped');
  }
}

/** Draw the knob's position as an arc on the Angle module. */
function builderKnob(detent, of) {
  const total = Number(of) || 24;
  const fraction = Math.max(0, Math.min(1, (Number(detent) || 0) / Math.max(1, total - 1)));
  const circumference = 2 * Math.PI * 34;
  for (const arc of $$('.knob-arc')) {
    const circle = $('circle', arc);
    if (!circle) continue;
    circle.setAttribute('stroke-dasharray',
      `${(fraction * circumference).toFixed(1)} ${circumference.toFixed(1)}`);
    arc.classList.add('is-live');
    clearTimeout(arc._fadeTimer);
    arc._fadeTimer = setTimeout(() => arc.classList.remove('is-live'), 1400);
  }
}

const MONO_GLYPH = {
  recording: ['●', 'is-recording is-busy'],
  transcribing: ['◐', 'is-busy'],
  cleaning: ['◑', 'is-busy'],
  pasted: ['✓', 'is-pasted'],
  error: ['✗', 'is-error'],
};

/** Mirror the engine state onto the Mono panel, as the device itself does. */
function builderState(name) {
  const [glyph, kind] = MONO_GLYPH[name] || ['', ''];
  for (const panel of $$('[data-mono]')) {
    panel.className = `mono-state ${kind}${glyph ? ' is-live' : ''}`;
    panel.textContent = glyph;
  }
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
      builderState(event.state);
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
      builderHold(ev.key, ev.active);
      break;
    case 'tap':
      builderTap(ev.key);
      break;
    case 'knob':
      builderKnob(ev.detent, ev.of);
      break;
    case 'battery':
      state.device.vbat_mv = ev.vbat_mv;
      renderDeviceStats(state.device);
      break;
    case 'chain':
      refreshDevice();
      break;
    case 'layout_changed':
      // Another client edited the key map; re-read it rather than guess.
      refreshBuilderLayers().then(renderInspector);
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
  initTabs();
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
    await builderBoot();
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
