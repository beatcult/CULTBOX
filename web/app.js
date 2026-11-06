// SPDX-License-Identifier: GPL-3.0-only
// CULTBOX in a browser: the page. The firmware runs in worklet.js; this draws its screen and sends it the keys,
// buttons and dials (ids as in firmware/src/cultbox.c: buttons BTN_*, encoders ENC_*, keys 0..26 with 0 = F).
(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const STORE = 'cultbox-web-store-1';
  const ENC = { SELECT: 0, ALGORITHM: 1, KNOB1: 2, KNOB2: 3, KNOB3: 4, KNOB4: 5, PRESETS: 6 };
  const BTN = [['SEL', 3], ['EDIT', 6], ['ENV', 4], ['LFO', 5], ['GLO', 7], ['FX', 2],
               ['HOME', 8], ['SAVE', 9], ['ARP', 10], ['SEQ', 11], ['PLAY', 12], ['REC', 13],
               ['OCT−', 0], ['OCT+', 1]];
  let node = null, ctx = null, gain = null, analyser = null, ready = false;
  const send = (m) => { if (node) node.port.postMessage(m); };

  // ---- the screen
  const cv = $('lcd'), g = cv.getContext('2d'), img = g.createImageData(240, 240);
  function draw(px) {
    const d = img.data;
    for (let i = 0; i < 240 * 240; i++) {
      const c = px[i];
      d[4 * i] = ((c >> 11) * 255 / 31) | 0;
      d[4 * i + 1] = (((c >> 5) & 63) * 255 / 63) | 0;
      d[4 * i + 2] = ((c & 31) * 255 / 31) | 0;
      d[4 * i + 3] = 255;
    }
    g.putImageData(img, 0, 0);
  }

  // ---- dials: drag up/down (clockwise = up), or scroll; arrow keys when focused
  const encEls = {};
  function dial(host, label, id, big) {
    const el = document.createElement('div');
    el.className = 'enc' + (big ? ' big' : '');
    el.tabIndex = 0; el.setAttribute('role', 'slider'); el.setAttribute('aria-label', label);
    el.innerHTML = '<div class="dial"><b><i></i></b></div><span></span>';
    el.lastChild.textContent = label;
    host.appendChild(el);
    let angle = 0, acc = 0, y0 = 0;
    const turn = (n) => { if (!n) return; send({ t: 'enc', id, n }); angle += n * 14; el.querySelector('b').style.transform = 'rotate(' + angle + 'deg)'; };
    el.addEventListener('pointerdown', (e) => { el.setPointerCapture(e.pointerId); y0 = e.clientY; acc = 0; e.preventDefault(); });
    el.addEventListener('pointermove', (e) => {
      if (!el.hasPointerCapture(e.pointerId)) return;
      acc += y0 - e.clientY; y0 = e.clientY;
      const n = Math.trunc(acc / 7); if (n) { acc -= n * 7; turn(n); }
    });
    el.addEventListener('wheel', (e) => {
      e.preventDefault();
      acc += -e.deltaY * (e.deltaMode === 1 ? 20 : 1);
      const n = Math.trunc(acc / 40); if (n) { acc -= n * 40; turn(n); }
    }, { passive: false });
    el.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowUp' || e.key === 'ArrowRight') { turn(1); e.preventDefault(); }
      if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') { turn(-1); e.preventDefault(); }
    });
    encEls[id] = el;
  }
  ['KNOB 1', 'KNOB 2', 'KNOB 3', 'KNOB 4'].forEach((l, i) => dial($('knobs'), l, ENC.KNOB1 + i, false));
  dial($('encs'), 'ALGORITHM', ENC.ALGORITHM, true);
  dial($('encs'), 'SELECT', ENC.SELECT, true);
  dial($('encs'), 'PRESETS', ENC.PRESETS, true);

  // ---- buttons
  const btnEls = {};
  function btnPress(id, d) { send({ t: 'btn', id, d }); if (btnEls[id]) btnEls[id].classList.toggle('down', !!d); }
  BTN.forEach(([name, id], n) => {
    if (n === 12) { const s = document.createElement('span'); s.className = 'gap'; $('btns').appendChild(s); }
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'btn'; b.textContent = name; b.setAttribute('aria-label', name);
    b.addEventListener('pointerdown', (e) => { b.setPointerCapture(e.pointerId); btnPress(id, 1); e.preventDefault(); });
    const up = (e) => { if (b.hasPointerCapture(e.pointerId)) { b.releasePointerCapture(e.pointerId); btnPress(id, 0); } };
    b.addEventListener('pointerup', up); b.addEventListener('pointercancel', up);
    b.addEventListener('keydown', (e) => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { btnPress(id, 1); e.preventDefault(); } });
    b.addEventListener('keyup', (e) => { if (e.key === ' ' || e.key === 'Enter') btnPress(id, 0); });
    $('btns').appendChild(b); btnEls[id] = b;
  });

  // ---- the 27 keys, F to G; key k is the pitch class (k + 5) % 12
  const keyEls = [], WHITE = [0, 2, 4, 5, 7, 9, 11];
  const QW = { // computer key -> key number
    '1': 0, '2': 1, '3': 2, '4': 3, '5': 4, '6': 5, '7': 6,
    z: 7, s: 8, x: 9, d: 10, c: 11, v: 12, g: 13, b: 14, h: 15, n: 16, j: 17, m: 18, ',': 19, l: 20, '.': 21, ';': 22, '/': 23, "'": 24,
  };
  const QLABEL = {}; Object.keys(QW).forEach((c) => { QLABEL[QW[c]] = c.toUpperCase(); });
  (function build() {
    const host = $('keys'), whites = [];
    for (let k = 0; k < 27; k++) if (WHITE.includes((k + 5) % 12)) whites.push(k);
    const w = 100 / whites.length, bw = w * 0.62;
    for (let k = 0; k < 27; k++) {
      const white = WHITE.includes((k + 5) % 12), el = document.createElement('div');
      el.className = 'k ' + (white ? 'w' : 'b');
      el.innerHTML = '<span class="d"></span><span></span>';
      el.lastChild.textContent = QLABEL[k] || '';
      if (white) el.style.cssText = 'left:' + whites.indexOf(k) * w + '%;width:' + w + '%';
      else { const prev = whites.indexOf(k - 1) + 1; el.style.cssText = 'left:' + (prev * w - bw / 2) + '%;width:' + bw + '%'; }
      el.addEventListener('pointerdown', (e) => { el.setPointerCapture(e.pointerId); keyDown(k); e.preventDefault(); });
      const up = (e) => { if (el.hasPointerCapture(e.pointerId)) { el.releasePointerCapture(e.pointerId); keyUp(k); } };
      el.addEventListener('pointerup', up); el.addEventListener('pointercancel', up);
      host.appendChild(el); keyEls[k] = el;
    }
  })();
  const down = new Set();
  function keyDown(k) { if (down.has(k)) return; down.add(k); send({ t: 'key', k, d: 1 }); keyEls[k].classList.add('on'); }
  function keyUp(k) { if (!down.has(k)) return; down.delete(k); send({ t: 'key', k, d: 0 }); keyEls[k].classList.remove('on'); }
  const held = {};                              // computer key -> the key it started (so a shift does not strand it)
  addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey || !ready || e.target.tagName === 'INPUT') return;
    const c = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (c === ' ' && e.target.tagName !== 'BUTTON') { if (!e.repeat) btnPress(12, 1); e.preventDefault(); return; }
    if (c === '[' || c === ']') { if (!e.repeat) { held[c] = 1; btnPress(c === '[' ? 0 : 1, 1); } e.preventDefault(); return; }
    if (c in QW && !e.repeat) { held[c] = QW[c]; keyDown(QW[c]); e.preventDefault(); }
  });
  addEventListener('keyup', (e) => {
    const c = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (c === ' ' && e.target.tagName !== 'BUTTON') { btnPress(12, 0); return; }
    if (c === '[' || c === ']') { btnPress(c === '[' ? 0 : 1, 0); return; }
    for (const h of [c, e.key]) if (h in held && typeof held[h] === 'number') { keyUp(held[h]); delete held[h]; }
  });
  addEventListener('blur', () => { [...down].forEach(keyUp); [0, 1, 12].forEach((i) => btnPress(i, 0)); });

  // ---- LEDs from the firmware: buttons 0..13 and the keys
  function leds(l) {
    const bit = (n) => (n < 32 ? (l[0] >>> n) & 1 : (l[1] >>> (n - 32)) & 1), gl = (n) => (n < 32 ? (l[2] >>> n) & 1 : (l[3] >>> (n - 32)) & 1);
    for (const id in btnEls) btnEls[id].classList.toggle('lit', !!bit(+id));
    keyEls.forEach((el, k) => { el.classList.toggle('lit', !!bit(14 + k)); el.classList.toggle('glow', !bit(14 + k) && !!gl(14 + k)); });
  }

  // ---- what the firmware saves is kept in this browser
  const b64 = (buf) => { let s = ''; const u = new Uint8Array(buf); for (let i = 0; i < u.length; i += 0x8000) s += String.fromCharCode.apply(null, u.subarray(i, i + 0x8000)); return btoa(s); };
  const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0)).buffer;
  function loadStore() { try { const j = JSON.parse(localStorage.getItem(STORE) || 'null'); return j && j.length === 3 ? j.map(unb64) : null; } catch (e) { return null; } }
  function saveStore(b) { try { localStorage.setItem(STORE, JSON.stringify(b.map(b64))); } catch (e) { /* private window or full: carry on */ } }
  $('reset').addEventListener('click', () => {
    if (confirm('Erase the settings and patterns this page has kept, and start again?')) { try { localStorage.removeItem(STORE); } catch (e) {} location.reload(); }
  });

  // ---- MIDI in (where the browser offers it)
  if (navigator.requestMIDIAccess) {
    const mb = $('midi'); mb.hidden = false;
    mb.addEventListener('click', async () => {
      try {
        const acc = await navigator.requestMIDIAccess();
        const hook = () => acc.inputs.forEach((i) => { i.onmidimessage = (m) => { const d = m.data; if (d.length) send({ t: 'midi', st: d[0], d1: d[1] || 0, d2: d[2] || 0 }); }; });
        acc.onstatechange = hook; hook();
        mb.textContent = 'MIDI in: ' + acc.inputs.size; mb.disabled = true;
      } catch (e) { mb.textContent = 'MIDI blocked'; }
    });
  }

  // ---- power on
  let wasm = null;
  const go = $('go'), err = $('err');
  function fail(m) { err.textContent = m; err.hidden = false; go.textContent = 'Not available'; go.disabled = true; }
  if (!window.AudioContext || !window.AudioWorklet || !window.WebAssembly) fail('This browser can not run it: it needs WebAssembly and AudioWorklet (a recent Chrome, Edge, Firefox or Safari).');
  else fetch('cultbox.wasm').then((r) => { if (!r.ok) throw new Error(r.status); return r.arrayBuffer(); }).then((b) => { wasm = b; go.textContent = 'Power on'; go.disabled = false; go.focus(); }).catch(() => fail('Could not load cultbox.wasm.'));
  go.addEventListener('click', async () => {
    go.disabled = true; go.textContent = 'Starting…';
    try {
      ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
      await ctx.audioWorklet.addModule('worklet.js');
      node = new AudioWorkletNode(ctx, 'cultbox', { numberOfInputs: 0, outputChannelCount: [2], processorOptions: { wasm, store: loadStore() } });
      gain = ctx.createGain(); analyser = ctx.createAnalyser(); analyser.fftSize = 1024;
      const vol = () => { gain.gain.value = Math.pow($('vol').value / 100, 2); };
      $('vol').addEventListener('input', vol); vol();
      node.connect(gain); gain.connect(analyser); analyser.connect(ctx.destination);
      node.port.onmessage = (e) => {
        const m = e.data;
        if (m.t === 'ready') { ready = true; $('over').hidden = true; }
        else if (m.t === 'fb') draw(m.px);
        else if (m.t === 'leds') leds(m.l);
        else if (m.t === 'save') saveStore(m.b);
      };
      await ctx.resume();
      window.__cultbox = { ctx, node, analyser };   // (for the tests)
    } catch (e) { fail('Could not start the sound: ' + e.message); }
  });
})();
