// ---- AudioWorklet processor (the dsp-core source is prepended to this text at load time)
class AmpexProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    const fs = sampleRate;
    this.fs = fs;
    this.ch = [new AmpexCore.TapeChannel(fs, 128), new AmpexCore.TapeChannel(fs, 128)];
    this.wf = [new AmpexCore.WowFlutter(fs), new AmpexCore.WowFlutter(fs)];
    this.wf[1].rng = 0x12345678;
    this.rng = [0x1234567, 0x7654321];
    this.p = { inDb: 0, outDb: 0, headroomDb: 0, monitor: 1, tapeHiss: 0, wowFlutter: 0, preserveAmount: 0, postDb: 0, mix: 1, mono: 0, speed: 1, vibe: 0 };
    this.cfgReady = false;
    this.work = [new Float32Array(128), new Float32Array(128)];
    this.noise = [new Float32Array(128), new Float32Array(128)];
    this.dryLen = 64; this.dry = [new Float32Array(64), new Float32Array(64)]; this.dryPos = 0;
    this.meter = [0, 0]; this.meterAcc = 0;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'config') { this.ch[0].applyConfig(m.data); this.ch[1].applyConfig(m.data); this.p.mono = m.data.cfg.mono ? 1 : 0; this.p.speed = m.data.cfg.speed; this.p.vibe = m.data.cfg.vibe ? 1 : 0; this.cfgReady = true; }
      else if (m.type === 'params') Object.assign(this.p, m.data);
      else if (m.type === 'reset') { this.ch[0].reset(); this.ch[1].reset(); }
    };
  }
  gauss(c) { let s = 0; for (let k = 0; k < 4; k++) { let r = this.rng[c]; r ^= r << 13; r ^= r >>> 17; r ^= r << 5; this.rng[c] = r >>> 0; s += ((this.rng[c] & 0xFFFFFF) / 8388608) - 1; } return s * 0.8660254; }
  process(inputs, outputs) {
    const inp = inputs[0], out = outputs[0];
    if (!out || out.length === 0) return true;
    const n = out[0].length;
    const L = (inp && inp[0]) || new Float32Array(n), R = (inp && inp[1]) || L;
    const oL = out[0], oR = out[1] || oL;
    const p = this.p;
    if (!this.cfgReady || p.monitor < 0.5) {               // INPUT monitor → hard bypass (unity), meters off
      oL.set(L); if (oR !== oL) oR.set(R);
      this.meter[0] = this.meter[1] = 0; this.postMeter(n); return true;
    }
    const db2 = AmpexCore.dB2lin;
    const inGain = db2(p.inDb), calGain = db2(18 - p.headroomDb), decal = 1 / calGain, outGain = db2(p.outDb) * db2(p.postDb);
    const wetMix = p.mix, dryMix = 1 - p.mix, lat = this.ch[0].latencySamples();
    const hissOn = p.tapeHiss > 0.5, wfOn = p.wowFlutter > 0.5, mono = p.mono > 0.5, pres = p.preserveAmount;
    const w0 = this.work[0], w1 = this.work[1];
    const nch = mono ? 1 : 2;
    for (let c = 0; c < nch; c++) {
      const w = this.work[c], src = c === 0 ? L : R;
      if (mono) for (let i = 0; i < n; i++) w[i] = 0.5 * (L[i] + R[i]) * inGain * calGain;
      else for (let i = 0; i < n; i++) w[i] = src[i] * inGain * calGain;
      if (hissOn) { const nz = this.noise[c]; for (let i = 0; i < n; i++) nz[i] = this.gauss(c); }
      this.ch[c].process(w, n, hissOn, this.noise[c]);
    }
    if (mono) w1.set(w0.subarray(0, n));
    if (wfOn) {
      const d = AmpexCore.WF_DEPTH[p.speed] * (p.vibe ? 1.5 : 1), ips = AmpexCore.SPEEDS[p.speed];
      for (let c = 0; c < 2; c++) { const w = this.work[c], f = this.wf[c]; for (let i = 0; i < n; i++) w[i] = f.process(w[i], ips, d); }
    }
    let sL = 0, sR = 0;
    const dL0 = this.dry[0], dR0 = this.dry[1], dlen = this.dryLen;
    for (let i = 0; i < n; i++) {
      dL0[this.dryPos] = L[i]; dR0[this.dryPos] = R[i];
      let rd = this.dryPos - lat; if (rd < 0) rd += dlen;
      const dL = dL0[rd], dR = dR0[rd];
      if (++this.dryPos >= dlen) this.dryPos = 0;
      let yL = w0[i], yR = w1[i];
      sL += yL * yL; sR += yR * yR;
      yL *= decal; yR *= decal;
      if (pres > 0) { yL += pres * (dL * inGain - yL); yR += pres * (dR * inGain - yR); }
      yL = (dL * dryMix + yL * wetMix) * outGain; yR = (dR * dryMix + yR * wetMix) * outGain;
      if (!Number.isFinite(yL)) yL = 0; if (!Number.isFinite(yR)) yR = 0;
      oL[i] = yL; oR[i] = yR;
    }
    const k = 1.41421356 * 0.12589254, sm = 0.8;             // 0 VU sine (rms .707 in tape units) → −18 dBFS
    this.meter[0] = sm * this.meter[0] + (1 - sm) * Math.sqrt(sL / n) * k;
    this.meter[1] = sm * this.meter[1] + (1 - sm) * Math.sqrt(sR / n) * k;
    this.postMeter(n);
    return true;
  }
  postMeter(n) {
    this.meterAcc += n;
    if (this.meterAcc >= 1024) {        // ≈ every 21 ms at 48 k
      this.meterAcc = 0;
      const dbL = this.meter[0] > 1e-6 ? 20 * Math.log10(this.meter[0]) : -120, dbR = this.meter[1] > 1e-6 ? 20 * Math.log10(this.meter[1]) : -120;
      this.port.postMessage({ type: 'meter', l: dbL, r: dbR });
    }
  }
}
registerProcessor('analogexact-atr102', AmpexProcessor);
