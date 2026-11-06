// SPDX-License-Identifier: GPL-3.0-only
// CULTBOX in a browser: the firmware (cultbox.wasm) runs here, in the audio thread, and the sound clocks it. The page
// (index.html) sends keys, buttons and encoder turns; this sends back the screen, the LEDs and what the firmware saves.
const FS = 44100;                               // the firmware's sample rate; other rates are resampled
class Cultbox extends AudioWorkletProcessor {
  constructor(opts) {
    super();
    const { wasm, store } = opts.processorOptions;
    this.q = [];
    this.x = null;
    this.ratio = FS / sampleRate;
    this.inL = new Float32Array(4096); this.inR = new Float32Array(4096); this.have = 0; this.pos = 0;
    this.fbVer = -1; this.fbAt = 0; this.leds = [-1, -1, -1, -1]; this.saves = 0;
    this.port.onmessage = (e) => this.q.push(e.data);
    WebAssembly.instantiate(wasm, {}).then(({ instance }) => {
      const x = (this.x = instance.exports);
      for (let i = 0; i < 3; i++) {             // what an earlier visit saved, if it still fits this version
        const n = x.web_store_size(i), s = store && store[i];
        if (s && s.byteLength === n) new Uint8Array(x.memory.buffer, x.web_store(i), n).set(new Uint8Array(s));
      }
      x.web_init();
      this.saves = x.web_saves();
      this.port.postMessage({ t: 'ready' });
    });
  }
  event(m) {
    const x = this.x;
    switch (m.t) {
      case 'key': x.web_key(m.k, m.d); break;
      case 'btn': x.web_btn(m.id, m.d); break;
      case 'enc': x.web_enc(m.id, m.n); break;
      case 'midi': x.web_midi(m.st, m.d1, m.d2); break;
    }
  }
  pull(n) {                                     // n frames of the firmware's sound, appended to the input buffers
    const x = this.x;
    while (n > 0) {
      const k = Math.min(n, 1024), p = x.web_run(k), st = x.web_out_stride();
      if (this.have + k > this.inL.length) {
        const a = new Float32Array(this.inL.length * 2), b = new Float32Array(this.inR.length * 2);
        a.set(this.inL); b.set(this.inR); this.inL = a; this.inR = b;
      }
      this.inL.set(new Float32Array(x.memory.buffer, p, k), this.have);
      this.inR.set(new Float32Array(x.memory.buffer, p + st * 4, k), this.have);
      this.have += k; n -= k;
    }
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length, x = this.x;
    if (!x) return true;
    while (this.q.length) this.event(this.q.shift());
    if (this.ratio === 1) {
      this.have = 0; this.pull(n);
      L.set(this.inL.subarray(0, n)); R.set(this.inR.subarray(0, n));
    } else {                                    // the device's rate is not 44.1 kHz: linear interpolation
      const need = Math.ceil(this.pos + n * this.ratio) + 2;
      if (need > this.have) this.pull(need - this.have);
      for (let i = 0; i < n; i++) {
        const p = this.pos + i * this.ratio, j = Math.floor(p), f = p - j;
        L[i] = this.inL[j] * (1 - f) + this.inL[j + 1] * f;
        R[i] = this.inR[j] * (1 - f) + this.inR[j + 1] * f;
      }
      const end = this.pos + n * this.ratio, used = Math.floor(end);
      this.inL.copyWithin(0, used, this.have); this.inR.copyWithin(0, used, this.have);
      this.have -= used; this.pos = end - used;
    }
    // back to the page: the screen (at most ~30 a second), the LEDs, and anything newly saved
    const v = x.web_fb_ver();
    if (v !== this.fbVer && currentFrame - this.fbAt >= sampleRate / 30) {
      this.fbVer = v; this.fbAt = currentFrame;
      const px = new Uint16Array(240 * 240);
      px.set(new Uint16Array(x.memory.buffer, x.web_fb(), 240 * 240));
      this.port.postMessage({ t: 'fb', px }, [px.buffer]);
    }
    const l = [x.web_leds(), x.web_leds_hi(), x.web_glow(), x.web_glow_hi()];
    if (l.some((a, i) => a !== this.leds[i])) { this.leds = l; this.port.postMessage({ t: 'leds', l }); }
    const s = x.web_saves();
    if (s !== this.saves) {
      this.saves = s;
      const b = [0, 1, 2].map((i) => new Uint8Array(x.memory.buffer, x.web_store(i), x.web_store_size(i)).slice().buffer);
      this.port.postMessage({ t: 'save', b }, b);
    }
    return true;
  }
}
registerProcessor('cultbox', Cultbox);
