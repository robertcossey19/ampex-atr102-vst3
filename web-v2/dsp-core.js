/* ===================================================================================
   ANALOGEXACT — Ampex ATR-102 physics core (browser port of Source/TapeDSP.h, v2)
   Runs identically on the main thread (filter design) and inside the AudioWorklet
   (sample processing).  Levels inside the model: 1.0 = 0 VU = <flux> nWb/m on tape.
   Sources: Ampex ATR-100 manual 4890407-01 (Tables 1-4, 5-5, 5-11; schematic 4840421),
   Quantegy 406/456/499/GP9 and EMTEC SM900/SM911 sheets, Wallace 1951, Bertram/Mallinson.
   =================================================================================== */
const AmpexCore = (() => {
  'use strict';
  const PI = Math.PI, MU0 = 1.25663706212e-6;
  const dB2lin = db => Math.pow(10, db / 20), lin2dB = g => 20 * Math.log10(Math.max(g, 1e-30));
  // erf — Abramowitz & Stegun 7.1.26 refined with one Newton step on erfc series (|err| < 1e-9 in-band)
  function erf(x) {
    const s = x < 0 ? -1 : 1; x = Math.abs(x);
    if (x > 6) return s;
    const t = 1 / (1 + 0.5 * x);
    // Numerical Recipes erfc Chebyshev fit (fractional error < 1.2e-7)
    const y = t * Math.exp(-x * x - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
    return s * (1 - y);
  }

  // ---------------- tape stocks  {name, Hc A/m, Br T, coating µm, ref nWb/m, THD% @ref, MOL nWb/m, ΔS10 dB, noise re 456}
  const TAPES = [
    { name: '406',   Hc: 26260, Br: 0.130, coat: 10.1, ref: 260, thd: 0.20, mol: 810,  dS10: 1.75, noise: +1.0, cal: { kappa: 2.90, drive: 63.833380, sens: 0.00831621 } },
    { name: '456',   Hc: 25465, Br: 0.150, coat: 12.7, ref: 260, thd: 0.10, mol: 1072, dS10: 2.75, noise:  0.0, cal: { kappa: 3.75, drive: 58.194695, sens: 0.00922435 } },
    { name: '499',   Hc: 31035, Br: 0.160, coat: 15.2, ref: 260, thd: 0.06, mol: 1370, dS10: 3.0,  noise: -2.0, cal: { kappa: 3.85, drive: 56.784262, sens: 0.0110158 } },
    { name: 'GP9',   Hc: 27850, Br: 0.165, coat: 16.0, ref: 260, thd: 0.06, mol: 1387, dS10: 3.0,  noise: -2.5, cal: { kappa: 3.65, drive: 48.050360, sens: 0.0108866 } },
    { name: 'SM900', Hc: 29443, Br: 0.154, coat: 14.0, ref: 320, thd: 0.12, mol: 1604, dS10: 4.0,  noise: -1.5, cal: { kappa: 0.20, drive: 9.694703,  sens: 0.0183715 } },
    { name: 'SM911', Hc: 25465, Br: 0.145, coat: 13.0, ref: 320, thd: 0.10, mol: 1350, dS10: 3.0,  noise:  0.0, cal: { kappa: 3.40, drive: 42.465356, sens: 0.0121675 } },
  ];
  const SPEEDS = [7.5, 15, 30];
  const FLUX = [185, 250, 370, 500];
  const eqFor = (sp, eq) => sp === 2 ? { hf: 17.5, lf: 0, name: 'AES' } : eq === 0 ? { hf: 50, lf: 3180, name: 'NAB' } : { hf: sp === 1 ? 35 : 70, lf: 0, name: 'IEC' };
  const REPRO_GAP = 2.5, SPACING = 0.30, SPACING_WORN = 1.0;
  const sfdFor = t => (t === 0 || t === 1 || t === 5) ? 0.35 : 0.25;

  // ---------------- wavelength losses [Wallace]
  class LossModel {
    constructor() { this.v = 0.381; this.spacing = SPACING; this.coat = 12.7; this.gap = REPRO_GAP; this.zone = 2.09; }
    clone() { const l = new LossModel(); Object.assign(l, this); return l; }
    lambda(f) { return this.v / Math.max(f, 1e-3) * 1e6; }
    playback(f) {
      const lam = this.lambda(f), k = 2 * PI / lam;
      const sp = Math.exp(-k * this.spacing), x = k * this.coat, th = (1 - Math.exp(-x)) / x;
      const a = PI * this.gap / lam, gp = Math.abs(a < 1e-9 ? 1 : Math.sin(a) / a);
      return sp * th * gp;
    }
    recordZone(f) { return Math.exp(-2 * PI * this.zone / this.lambda(f)); }
    total(f) { return this.playback(f) * this.recordZone(f); }
  }

  // ---------------- FFT (radix-2, in place, split re/im)
  function fft(re, im, inverse) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
      if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = 2 * PI / len * (inverse ? 1 : -1), wr = Math.cos(ang), wi = Math.sin(ang);
      for (let i = 0; i < n; i += len) {
        let cr = 1, ci = 0;
        for (let j = 0; j < len / 2; j++) {
          const a = i + j, b = a + len / 2;
          const tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
          re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
          const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
        }
      }
    }
    if (inverse) for (let i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
  }
  // minimum-phase FIR from a magnitude function (real-cepstrum method), 8192-point
  function designMinPhase(magFn, fs, taps, order = 13) {
    const N = 1 << order, re = new Float64Array(N), im = new Float64Array(N);
    for (let k = 0; k < N; k++) { const kk = k <= N / 2 ? k : N - k; re[k] = Math.log(Math.max(magFn(kk * fs / N), 1e-5)); }
    fft(re, im, true);
    for (let n = 0; n < N; n++) { const w = (n === 0 || n === N / 2) ? 1 : (n < N / 2 ? 2 : 0); re[n] *= w; im[n] = 0; }
    fft(re, im, false);
    for (let k = 0; k < N; k++) { const m = Math.exp(re[k]); re[k] = m * Math.cos(im[k]); im[k] = m * Math.sin(im[k]); }
    fft(re, im, true);
    const h = new Float32Array(taps), fade = taps >> 2;
    for (let n = 0; n < taps; n++) { let w = 1; if (n >= taps - fade) { const t = (n - (taps - fade)) / fade; w = 0.5 + 0.5 * Math.cos(PI * t); } h[n] = re[n] * w; }
    return h;
  }
  class FIR {
    constructor() { this.h = new Float32Array(0); this.z = new Float32Array(0); this.pos = 0; }
    set(h) { if (h.length !== this.h.length) { this.z = new Float32Array(h.length * 2); this.pos = 0; } this.h = h; }
    reset() { this.z.fill(0); this.pos = 0; }
    process(x) {
      const n = this.h.length; if (!n) return x;
      const z = this.z, h = this.h; z[this.pos] = x; z[this.pos + n] = x;
      let acc = 0; const base = this.pos + 1;
      for (let i = 0; i < n; i++) acc += h[n - 1 - i] * z[base + i];
      if (++this.pos >= n) this.pos = 0;
      return acc;
    }
    magnitudeAt(f, fs) { let r = 0, i = 0; for (let n = 0; n < this.h.length; n++) { const a = -2 * PI * f * n / fs; r += this.h[n] * Math.cos(a); i += this.h[n] * Math.sin(a); } return Math.hypot(r, i); }
  }

  // ---------------- biquad (TDF2, double)
  class Biquad {
    constructor() { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.s1 = 0; this.s2 = 0; }
    reset() { this.s1 = this.s2 = 0; }
    process(x) { const y = this.b0 * x + this.s1; this.s1 = this.b1 * x - this.a1 * y + this.s2; this.s2 = this.b2 * x - this.a2 * y; return y; }
    identity() { this.b0 = 1; this.b1 = this.b2 = this.a1 = this.a2 = 0; return this; }
    coefs() { return [this.b0, this.b1, this.b2, this.a1, this.a2]; }
    setCoefs(c) { [this.b0, this.b1, this.b2, this.a1, this.a2] = c; return this; }
    fromAnalog(B0, B1, B2, A0, A1, A2, fs, fcWarp) {
      const K = fcWarp > 0 ? (2 * PI * fcWarp) / Math.tan(PI * fcWarp / fs) : 2 * fs, k2 = K * K;
      const a0 = A0 + A1 * K + A2 * k2;
      this.b0 = (B0 + B1 * K + B2 * k2) / a0; this.b1 = (2 * B0 - 2 * B2 * k2) / a0; this.b2 = (B0 - B1 * K + B2 * k2) / a0;
      this.a1 = (2 * A0 - 2 * A2 * k2) / a0; this.a2 = (A0 - A1 * K + A2 * k2) / a0; return this;
    }
    lowpass(fc, Q, fs) { const w = 2 * PI * fc; return this.fromAnalog(w * w, 0, 0, w * w, w / Q, 1, fs, fc); }
    highpass(fc, Q, fs) { const w = 2 * PI * fc; return this.fromAnalog(0, 0, 1, w * w, w / Q, 1, fs, fc); }
    peak(fc, Q, gDb, fs) { const A = Math.pow(10, gDb / 40), w = 2 * PI * fc; return this.fromAnalog(w * w, w * A / Q, 1, w * w, w / (A * Q), 1, fs, fc); }
    shelf1(tauZ, tauP, fs, gain = 1, fwarp = 0) {
      const fw = fwarp > 0 ? fwarp : 1 / (2 * PI * Math.max(Math.max(tauZ > 0 ? tauZ : 1e9, tauP > 0 ? tauP : 1e9), 1e-7));
      return this.fromAnalog(gain, gain * tauZ, 0, 1, tauP, 0, fs, Math.min(fw, fs * 0.45));
    }
    magnitudeAt(f, fs) {
      const w = -2 * PI * f / fs, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
      const nr = this.b0 + this.b1 * c1 + this.b2 * c2, ni = this.b1 * s1 + this.b2 * s2;
      const dr = 1 + this.a1 * c1 + this.a2 * c2, di = this.a1 * s1 + this.a2 * s2;
      return Math.hypot(nr, ni) / Math.hypot(dr, di);
    }
  }

  // ---------------- AC-bias anhysteretic remanence  m = erf((h − κm)/√2), log-normal SFD
  function anhSolve(h, kappa) {
    let lo = -1, hi = 1;
    for (let i = 0; i < 48; i++) { const mid = 0.5 * (lo + hi); const f = mid - erf((h - kappa * mid) / Math.SQRT2); if (f > 0) hi = mid; else lo = mid; }
    return 0.5 * (lo + hi);
  }
  function buildAnhTable(ts, tapeIdx, kappa, biasDb, HmaxSigma = 24, N = 769) {
    const Mr = ts.Br / MU0, Hc = ts.Hc, sigma = 0.5 * Hc, sfd = sfdFor(tapeIdx);
    const Hb = Hc * Math.exp(2 * sfd) * dB2lin(biasDb);
    const z = Math.log(Math.max(Hb, 1e-9) / Hc) / sfd, part = 0.5 * (1 + erf(z / Math.SQRT2));
    const MrEff = Mr * part, kEff = kappa * part, Hmax = HmaxSigma * sigma;
    const m = new Float32Array(N);
    for (let i = 0; i < N; i++) m[i] = MrEff * anhSolve((Hmax * i / (N - 1)) / sigma, kEff);
    m[0] = 0;
    return { Hmax, m };
  }
  function anhLookup(t, H) {
    const N = t.m.length, s = H < 0 ? -1 : 1; const x = Math.abs(H) / t.Hmax * (N - 1);
    if (x >= N - 1) return s * t.m[N - 1];
    const i = x | 0, f = x - i; return s * ((1 - f) * t.m[i] + f * t.m[i + 1]);
  }

  // ---------------- transformer: flux-domain core law, implicit trapezoid + Newton
  class Transformer {
    constructor() { this.fs = 48000; this.fL = 8; this.phiSat = 1; this.kappa = 1; this.fb = 1; this.phi = 0; this.prev = 0; this.hf = new Biquad(); }
    prepare(fs, fL, level1pct20Hz, nfbDb, hfRes, hfQ) {
      this.fs = fs; this.fL = fL; this.fb = 1 / dB2lin(nfbDb);
      const phiA = level1pct20Hz / (2 * PI * 20), lfDiv = fL / 20; this.kappa = 1;
      this.phiSat = phiA / Math.sqrt(0.01 * 4 / Math.max(lfDiv * this.kappa * this.fb, 1e-9));
      if (hfRes < 0.45 * fs) this.hf.lowpass(hfRes, hfQ, fs); else this.hf.identity();
      this.reset();
    }
    setFrom(o) { this.fs = o.fs; this.fL = o.fL; this.phiSat = o.phiSat; this.kappa = o.kappa; this.fb = o.fb; this.hf.setCoefs(o.hf); }
    export() { return { fs: this.fs, fL: this.fL, phiSat: this.phiSat, kappa: this.kappa, fb: this.fb, hf: this.hf.coefs() }; }
    reset() { this.phi = 0; this.prev = 0; this.hf.reset(); }
    process(vin) {
      const a = 2 * PI * this.fL * this.fb, h = 1 / this.fs, rhs = this.phi + 0.5 * h * (vin + this.prev), ps = this.phiSat, k = this.kappa;
      let pn = this.phi;
      for (let it = 0; it < 4; it++) {
        const r = pn / ps, r2 = r * r, g = pn * (1 + k * r2), dg = 1 + 3 * k * r2;
        pn -= (pn - rhs + 0.5 * h * a * g) / (1 + 0.5 * h * a * dg);
      }
      const r = pn / ps, vout = vin - a * pn * (1 + k * r * r);
      this.phi = pn; this.prev = vout;
      return this.hf.process(vout);
    }
  }

  // ---------------- 2× half-band polyphase IIR (Valenzuela–Constantinides), cascaded for 4×
  const HB_STAGE1 = [0.032701390248, 0.122921927274, 0.250924529315, 0.393848246461, 0.533438203773, 0.659440556758, 0.769175179754, 0.865498072578, 0.954972748445]; // 100 dB, tbw 0.04
  const HB_STAGE2 = [0.039151597734, 0.147377113601, 0.302646848328, 0.48246854277, 0.674615918547, 0.883005025769];                                              // 90 dB, tbw 0.10
  class HalfBand {
    constructor(coefs) {
      this.a0 = coefs.filter((_, i) => i % 2 === 0); this.a1 = coefs.filter((_, i) => i % 2 === 1);
      this.x0 = new Float64Array(this.a0.length); this.y0 = new Float64Array(this.a0.length);
      this.x1 = new Float64Array(this.a1.length); this.y1 = new Float64Array(this.a1.length);
      this.ux0 = new Float64Array(this.a0.length); this.uy0 = new Float64Array(this.a0.length);
      this.ux1 = new Float64Array(this.a1.length); this.uy1 = new Float64Array(this.a1.length);
    }
    reset() { for (const b of [this.x0, this.y0, this.x1, this.y1, this.ux0, this.uy0, this.ux1, this.uy1]) b.fill(0); }
    static ap(a, x, xs, ys, i) { const y = a[i] * (x - ys[i]) + xs[i]; xs[i] = x; ys[i] = y; return y; }  // allpass (a + z^-2)/(1 + a z^-2) at the polyphase rate
    // upsample one input sample → two outputs [even, odd]
    up(x, out, o) {
      let p0 = x, p1 = x;
      for (let i = 0; i < this.a0.length; i++) p0 = HalfBand.ap(this.a0, p0, this.ux0, this.uy0, i);
      for (let i = 0; i < this.a1.length; i++) p1 = HalfBand.ap(this.a1, p1, this.ux1, this.uy1, i);
      out[o] = p0; out[o + 1] = p1;   // even output from the direct path, odd from the z⁻¹ path
    }
    // downsample two inputs → one output
    down(x0, x1) {
      let p0 = x1, p1 = x0;
      for (let i = 0; i < this.a0.length; i++) p0 = HalfBand.ap(this.a0, p0, this.x0, this.y0, i);
      for (let i = 0; i < this.a1.length; i++) p1 = HalfBand.ap(this.a1, p1, this.x1, this.y1, i);
      return 0.5 * (p0 + p1);
    }
  }

  // ---------------- one channel of the machine
  const WF_DEPTH = [0.06, 0.04, 0.03];  // [ATR Table 1-4] NAB rms %
  class TapeChannel {
    constructor(fs, maxBlock) {
      this.fs = fs; this.osFactor = fs <= 50000 ? 4 : 2;
      this.hb1 = new HalfBand(HB_STAGE1); this.hb2 = new HalfBand(HB_STAGE2);
      this.pre = new FIR(); this.post = new FIR();
      this.recLF = new Biquad(); this.recHFpole = new Biquad(); this.recHF = new Biquad();
      this.repLF = new Biquad(); this.repHF = new Biquad(); this.bump1 = new Biquad(); this.bump2 = new Biquad(); this.bumpHP = new Biquad(); this.noiseLP = new Biquad();
      this.inX = new Transformer(); this.outX = new Transformer();
      this.table = { Hmax: 1, m: new Float32Array([0, 0]) };
      this.drive = 1; this.outScale = 1; this.recTrim = 1; this.repTrim = 1; this.noiseGain = 0; this.transformer = true;
      this.tmp = new Float64Array(maxBlock * 4 + 8); this.tmp2 = new Float64Array(maxBlock * 2 + 4);
      this.loss = new LossModel(); this.cfg = null;
      if (fs >= 88000) this.recHF.lowpass(28000, 0.707, fs); else this.recHF.identity();
    }
    latencySamples() { return this.osFactor === 4 ? 6 : 4; }   // measured group delay of the half-band cascade (≈)
    reset() { this.hb1.reset(); this.hb2.reset(); this.pre.reset(); this.post.reset(); for (const b of [this.recLF, this.recHFpole, this.recHF, this.repLF, this.repHF, this.bump1, this.bump2, this.bumpHP, this.noiseLP]) b.reset(); this.inX.reset(); this.outX.reset(); }
    smallSignalPreGain(f) { return this.pre.magnitudeAt(f, this.fs) * this.recHFpole.magnitudeAt(f, this.fs) * this.recLF.magnitudeAt(f, this.fs); }

    // ----- design (main thread) : cfg = {tape, speed, eq, flux, biasDb, transformer, vibe, mono}
    static design(fs, c) {
      const ch = new TapeChannel(fs, 128); ch.configure(c); return ch.exportConfig();
    }
    configure(c) {
      const fs = this.fs, ts = TAPES[c.tape], tc = ts.cal, ips = SPEEDS[c.speed], v = ips * 0.0254, eq = eqFor(c.speed, c.eq), flux = FLUX[c.flux];
      this.cfg = Object.assign({}, c);
      this.table = buildAnhTable(ts, c.tape, tc.kappa, c.biasDb - (c.vibe ? 1 : 0));
      this.drive = tc.drive * flux; this.outScale = tc.sens / flux;
      const L = this.loss; L.v = v; L.coat = ts.coat; L.gap = REPRO_GAP; L.spacing = c.vibe ? SPACING_WORN : SPACING;
      const zoneNominal = ts.dS10 * 38.1 / 54.6;                           // [ATR Table 5-11] over-bias at 1.5 mil at every speed
      L.zone = zoneNominal * Math.pow(dB2lin(c.biasDb), 3.0);              // [EST] zone depth ∝ I_bias³
      const tauFloor = 1 / (2 * PI * 12);
      if (eq.lf > 0) this.recLF.shelf1(eq.lf * 1e-6, tauFloor, fs, tauFloor / (eq.lf * 1e-6)); else this.recLF.identity();
      const fHF = 1 / (2 * PI * eq.hf * 1e-6);
      this.recHFpole.shelf1(0, eq.hf * 1e-6, fs, 1, fHF);
      const aligned = L.clone(); aligned.zone = zoneNominal; aligned.spacing = SPACING;   // record EQ aligned at operating bias on a clean machine
      const cap = dB2lin(30);
      this.pre.set(designMinPhase(f => { const l = aligned.total(Math.max(f, 1)); return 1 / Math.sqrt(l * l + 1 / (cap * cap)); }, fs, 192));
      this.post.set(designMinPhase(f => L.total(Math.max(f, 1)), fs, 192));
      this.repHF.shelf1(eq.hf * 1e-6, 1 / (2 * PI * 150000), fs, 1, fHF);
      if (eq.lf > 0) this.repLF.shelf1(tauFloor, eq.lf * 1e-6, fs, (eq.lf * 1e-6) / tauFloor); else this.repLF.identity();
      this.recTrim = 1 / Math.max(this.smallSignalPreGain(1000), 1e-6); this.repTrim = 1 / this.recTrim;   // REC GAIN alignment at 1 kHz
      const fBump = 60 * v / 0.762;                                         // head contour [ATR Fig 5-9][EST shape]
      this.bump1.peak(fBump, 1.0, 0.6, fs); this.bump2.peak(fBump * 2.1, 1.4, -0.4, fs); this.bumpHP.highpass(fBump * 0.3, 0.6, fs);
      this.inX.prepare(fs, 3.0, dB2lin(16), 0, 60000, 0.8);                // T2 bridging input [JEN proxy]
      this.outX.prepare(fs, 2.0, dB2lin(20), 8.0, 80000, 0.7);             // T1 output, 8 dB tertiary NFB [ATR]
      this.transformer = !!c.transformer;
      let f3 = 20000; for (let f = 2000; f < 60000; f *= 1.02) { if (L.playback(f) < 0.7071) { f3 = f; break; } }
      this.noiseLP.lowpass(Math.min(f3, fs * 0.45), 0.6, fs);
      let snr = 69; if (c.speed === 2) snr = 72; else if (c.speed === 1 && c.eq === 1) snr = 70; else if (c.speed === 0) snr = c.eq === 0 ? 71 : 68;   // [ATR Table 1-4]
      if (c.mono) snr += 4; snr -= ts.noise; if (c.vibe) snr -= 2;
      const targetRms = 0.70710678 * dB2lin(lin2dB(1040) - snr) / flux;
      let p = 0; const df = 10;
      for (let f = 30; f <= Math.min(18000, 0.49 * fs); f += df) { const m = this.noiseLP.magnitudeAt(f, fs) * this.repHF.magnitudeAt(f, fs) * this.repLF.magnitudeAt(f, fs) * this.bump1.magnitudeAt(f, fs) * this.bumpHP.magnitudeAt(f, fs); p += m * m * df; }
      this.noiseGain = targetRms / Math.max(Math.sqrt(p / (fs / 2)), 1e-9);
    }
    exportConfig() {
      return {
        cfg: this.cfg, table: this.table, drive: this.drive, outScale: this.outScale, recTrim: this.recTrim, repTrim: this.repTrim, noiseGain: this.noiseGain, transformer: this.transformer,
        pre: this.pre.h, post: this.post.h,
        bq: { recLF: this.recLF.coefs(), recHFpole: this.recHFpole.coefs(), recHF: this.recHF.coefs(), repLF: this.repLF.coefs(), repHF: this.repHF.coefs(), bump1: this.bump1.coefs(), bump2: this.bump2.coefs(), bumpHP: this.bumpHP.coefs(), noiseLP: this.noiseLP.coefs() },
        inX: this.inX.export(), outX: this.outX.export(),
      };
    }
    applyConfig(o) {
      this.cfg = o.cfg; this.table = o.table; this.drive = o.drive; this.outScale = o.outScale; this.recTrim = o.recTrim; this.repTrim = o.repTrim; this.noiseGain = o.noiseGain; this.transformer = o.transformer;
      this.pre.set(o.pre); this.post.set(o.post);
      for (const k in o.bq) this[k].setCoefs(o.bq[k]);
      this.inX.setFrom(o.inX); this.outX.setFrom(o.outX);
    }

    // ----- process one block in place (x: Float32Array, n samples, units 0 VU = 1.0)
    process(x, n, hissOn, hiss) {
      const t = this.table, drive = this.drive, outScale = this.outScale, xf = this.transformer;
      for (let i = 0; i < n; i++) {
        let v = x[i];
        if (xf) v = this.inX.process(v);
        v = this.recLF.process(v); v = this.recHFpole.process(v); v = this.recHF.process(v);
        x[i] = this.pre.process(v * this.recTrim);
      }
      if (this.osFactor === 4) {
        const u2 = this.tmp2, u4 = this.tmp;
        for (let i = 0; i < n; i++) this.hb1.up(x[i], u2, 2 * i);
        for (let i = 0; i < 2 * n; i++) this.hb2.up(u2[i], u4, 2 * i);
        for (let i = 0; i < 4 * n; i++) u4[i] = anhLookup(t, drive * u4[i]) * outScale;
        for (let i = 0; i < 2 * n; i++) u2[i] = this.hb2.down(u4[2 * i], u4[2 * i + 1]);
        for (let i = 0; i < n; i++) x[i] = this.hb1.down(u2[2 * i], u2[2 * i + 1]);
      } else {
        const u2 = this.tmp2;
        for (let i = 0; i < n; i++) this.hb1.up(x[i], u2, 2 * i);
        for (let i = 0; i < 2 * n; i++) u2[i] = anhLookup(t, drive * u2[i]) * outScale;
        for (let i = 0; i < n; i++) x[i] = this.hb1.down(u2[2 * i], u2[2 * i + 1]);
      }
      for (let i = 0; i < n; i++) {
        let y = this.post.process(x[i]) * this.repTrim;
        if (hissOn) y += this.noiseGain * this.noiseLP.process(hiss[i]);
        y = this.repLF.process(y); y = this.repHF.process(y);
        y = this.bumpHP.process(this.bump2.process(this.bump1.process(y)));
        if (xf) y = this.outX.process(y);
        x[i] = y;
      }
    }
  }

  // ---------------- wow & flutter [ATR Table 1-4]
  class WowFlutter {
    constructor(fs) { this.fs = fs; this.len = (fs * 0.02 | 0) + 8; this.buf = new Float32Array(this.len); this.w = 0; this.phW = 0; this.ph1 = 0; this.ph2 = 0; this.scrape = 0; this.rng = 0x9E3779B9; }
    frand() { let r = this.rng; r ^= r << 13; r ^= r >>> 17; r ^= r << 5; this.rng = r >>> 0; return ((this.rng & 0xFFFFFF) / 8388608) - 1; }
    process(x, ips, depthPct) {
      this.buf[this.w] = x;
      const fs = this.fs, fWow = 0.6, fCap = 13 * ips / 15, fFl2 = 0.44 * fCap;
      this.phW += 2 * PI * fWow / fs; this.ph1 += 2 * PI * fCap / fs; this.ph2 += 2 * PI * fFl2 / fs;
      this.scrape += 0.002 * (this.frand() - this.scrape);
      const k = depthPct * 0.01 * 1.4142 * 1.6;
      const dev = k * (0.45 * Math.sin(this.phW) + 0.35 * Math.sin(this.ph1) + 0.2 * Math.sin(this.ph2 + 1.3) + 0.6 * this.scrape);
      const tOff = k * (0.45 * -Math.cos(this.phW) / (2 * PI * fWow) + 0.35 * -Math.cos(this.ph1) / (2 * PI * fCap) + 0.2 * -Math.cos(this.ph2 + 1.3) / (2 * PI * fFl2)) + 0.6 * dev * 1e-4;
      let d = 0.008 * fs + tOff * fs; d = Math.min(Math.max(d, 1), this.len - 3);
      const di = d | 0, fr = d - di; let r0 = this.w - di; if (r0 < 0) r0 += this.len; let r1 = r0 - 1; if (r1 < 0) r1 += this.len;
      const y = (1 - fr) * this.buf[r0] + fr * this.buf[r1];
      if (++this.w >= this.len) this.w = 0;
      return y;
    }
  }

  return { TAPES, SPEEDS, FLUX, eqFor, dB2lin, lin2dB, LossModel, Biquad, FIR, designMinPhase, buildAnhTable, anhLookup, Transformer, HalfBand, TapeChannel, WowFlutter, WF_DEPTH, erf };
})();
if (typeof module !== 'undefined') module.exports = AmpexCore;
