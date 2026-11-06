// SPDX-License-Identifier: GPL-3.0-only
// The browser build (web/build.sh -> build/web/cultbox.wasm) under Node: it boots, draws the mix page, is silent until a
// key is held, then sounds; a button press reaches the firmware (SEQ lights its LED) and a dial turn changes the screen.
//   node tests/web_test.mjs [build/web/cultbox.wasm]
import fs from 'fs';
const x = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(process.argv[2] || 'build/web/cultbox.wasm')), {}).exports;
let bad = 0;
const check = (name, ok, info = '') => { console.log((ok ? 'ok   ' : 'FAIL ') + name + (info ? '  ' + info : '')); if (!ok) bad++; };
const run = (frames) => {                       // peak and RMS of what the firmware makes over `frames`
  let pk = 0, sum = 0, n = 0;
  for (let f = 0; f < frames; f += 128) {
    const p = x.web_run(128), st = x.web_out_stride();
    const a = new Float32Array(x.memory.buffer, p, 128), b = new Float32Array(x.memory.buffer, p + st * 4, 128);
    for (let i = 0; i < 128; i++) { pk = Math.max(pk, Math.abs(a[i]), Math.abs(b[i])); sum += a[i] * a[i] + b[i] * b[i]; n += 2; }
  }
  return { pk, rms: Math.sqrt(sum / n) };
};
const fbHash = () => { let h = 0; for (const v of new Uint16Array(x.memory.buffer, x.web_fb(), 240 * 240)) h = (h * 31 + v) | 0; return h; };
const dark = () => { let n = 0; for (const v of new Uint16Array(x.memory.buffer, x.web_fb(), 240 * 240)) if (v) n++; return n; };

x.web_init();
run(44100 / 2);
check('the screen is drawn', dark() > 5000, dark() + ' lit pixels');
check('silent with no key down', run(8192).pk < 0.001);
x.web_key(7, 1);
const on = run(44100 / 2);
check('a key held makes sound', on.pk > 0.05 && on.pk < 1.0, 'peak ' + on.pk.toFixed(3));
x.web_key(7, 0);
run(44100 * 2);
check('and it dies away', run(4096).pk < 0.01);
const h0 = fbHash();
x.web_btn(11, 1); run(2048); x.web_btn(11, 0); run(44100 / 4);   // SEQ
check('SEQ: the screen changes and its LED lights', fbHash() !== h0 && ((x.web_leds() >>> 11) & 1) === 1);
const h1 = fbHash();
x.web_enc(2, 3); run(44100 / 4);                // knob 1
check('a dial turn changes the screen', fbHash() !== h1);
x.web_midi(0x90, 69, 100); const m = run(44100 / 4);
check('MIDI note on sounds', m.pk > 0.05, 'peak ' + m.pk.toFixed(3));
x.web_btn(12, 0); run(44100 / 4);               // a release with no press (the page lost focus): PLAY must not toggle
const led = (x.web_leds() >>> 12) & 1;
x.web_btn(12, 0); run(44100 / 4);
check('a release with no press does nothing', ((x.web_leds() >>> 12) & 1) === led);
process.exit(bad ? 1 : 0);
