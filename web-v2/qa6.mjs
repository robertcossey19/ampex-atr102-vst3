import { chromium } from 'playwright';
const browser = await chromium.launch({ headless: true, channel: 'chromium', args: ['--autoplay-policy=no-user-gesture-required'] });
const page = await (await browser.newContext()).newPage();
page.on('pageerror', e => console.log('PAGEERROR', e.message));
await page.goto('http://127.0.0.1:8765/index.html', { waitUntil: 'networkidle' });
const res = await page.evaluate(async () => {
  const fs = 48000, N = fs * 2;
  async function render({ f = 1000, vu = 0, params = {}, cfg = {}, silence = false }) {
    const ctx = new OfflineAudioContext(2, N, fs);
    const core = document.getElementById('ampex-core').textContent, wk = document.getElementById('ampex-worklet').textContent;
    await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([core + '\n' + wk], { type: 'application/javascript' })));
    const node = new AudioWorkletNode(ctx, 'analogexact-atr102', { outputChannelCount: [2] });
    const meters = []; node.port.onmessage = e => { if (e.data.type === 'meter') meters.push(e.data.l); };
    const c = Object.assign({ tape: 1, speed: 1, eq: 0, flux: 2, biasDb: 0, transformer: true, vibe: false, mono: false }, cfg);
    node.port.postMessage({ type: 'config', data: AmpexCore.TapeChannel.design(fs, c) });
    node.port.postMessage({ type: 'params', data: Object.assign({ inDb: 0, outDb: 0, headroomDb: 0, monitor: 1, tapeHiss: 0, wowFlutter: 0, preserveAmount: 0, postDb: 0, mix: 1 }, params) });
    const buf = ctx.createBuffer(2, N, fs); const a = silence ? 0 : Math.pow(10, (vu - 18) / 20);   // 0 VU = sine peak −18 dBFS
    for (let ch = 0; ch < 2; ch++) { const d = buf.getChannelData(ch); for (let i = 0; i < N; i++) d[i] = a * Math.sin(2 * Math.PI * f * i / fs); }
    const s = ctx.createBufferSource(); s.buffer = buf; s.connect(node).connect(ctx.destination); s.start();
    const out = await ctx.startRendering(); await new Promise(r => setTimeout(r, 50));
    return { y: out.getChannelData(0), x: buf.getChannelData(0), meters };
  }
  const n0 = fs, L = fs;
  const mag = (x, f) => { let re = 0, im = 0; for (let i = 0; i < L; i++) { const w = 2 * Math.PI * f * i / fs; re += x[n0 + i] * Math.cos(w); im -= x[n0 + i] * Math.sin(w); } return Math.hypot(re, im) * 2 / L; };
  const thd = (x, f) => { const f1 = mag(x, f); let h = 0; for (let k = 2; k <= 9; k++) { const m = mag(x, f * k); h += m * m; } return 100 * Math.sqrt(h) / f1; };
  const rms = (x) => { let s = 0; for (let i = n0; i < n0 + L; i++) s += x[i] * x[i]; return Math.sqrt(s / L); };
  const dB = v => 20 * Math.log10(v);
  const out = {};
  // --- VU meter: meter dB for 0 VU and −6 VU tone; and headroom +6 shifts it
  let r = await render({ vu: 0 }); out.vu_0VU_meter_dBFS = r.meters.slice(-10).reduce((a, b) => a + b) / 10;
  r = await render({ vu: -6 }); out.vu_minus6_meter = r.meters.slice(-10).reduce((a, b) => a + b) / 10;
  r = await render({ vu: 0, params: { headroomDb: 6 } }); out.vu_0VU_headroom6_meter = r.meters.slice(-10).reduce((a, b) => a + b) / 10; out.headroom6_thd_at_plus9 = null;
  // headroom in the chain: +9 VU input with headroom 0 vs headroom +6 (more headroom → less saturation)
  out.thd_plus9_headroom0 = thd((await render({ vu: 9 })).y, 1000); out.thd_plus9_headroom6 = thd((await render({ vu: 9, params: { headroomDb: 6 } })).y, 1000); out.thd_plus9_headroomMinus6 = thd((await render({ vu: 9, params: { headroomDb: -6 } })).y, 1000);
  // --- preserve: 0 / 0.5 / 1 at +9 VU; preserve 1 must equal the delayed dry signal
  const p0 = await render({ vu: 9 }), p5 = await render({ vu: 9, params: { preserveAmount: 0.5 } }), p1 = await render({ vu: 9, params: { preserveAmount: 1 } });
  out.preserve = { thd0: thd(p0.y, 1000), thd50: thd(p5.y, 1000), thd100: thd(p1.y, 1000), gain100_dB: dB(mag(p1.y, 1000) / mag(p1.x, 1000)) };
  let best = { lag: 0, err: 1e9 }; for (let lag = 0; lag < 16; lag++) { let e = 0; for (let i = n0; i < n0 + 4800; i++) e += Math.abs(p1.y[i] - p1.x[i - lag]); if (e < best.err) best = { lag, err: e / 4800 }; } out.preserve.dry_latency_samples = best.lag; out.preserve.residual_vs_dry = best.err;
  // --- vibe: HF loss, THD at 0 VU, hiss
  const v0 = await render({ f: 10000 }), v1 = await render({ f: 10000, cfg: { vibe: true } });
  out.vibe = { hf10k_off_dB: dB(mag(v0.y, 10000) / mag(v0.x, 10000)), hf10k_on_dB: dB(mag(v1.y, 10000) / mag(v1.x, 10000)) };
  out.vibe.thd0_off = thd((await render({})).y, 1000); out.vibe.thd0_on = thd((await render({ cfg: { vibe: true } })).y, 1000);
  const h0 = await render({ silence: true, params: { tapeHiss: 1 } }), h1 = await render({ silence: true, params: { tapeHiss: 1 }, cfg: { vibe: true } });
  out.vibe.hiss_off_dBFS = dB(rms(h0.y)), out.vibe.hiss_on_dBFS = dB(rms(h1.y));
  const w0 = await render({ f: 3150, params: { wowFlutter: 1 } }), w1 = await render({ f: 3150, params: { wowFlutter: 1 }, cfg: { vibe: true } });
  // flutter depth: zero-crossing period deviation rms over 1 s
  const flut = (y) => { const zc = []; for (let i = n0; i < n0 + L - 1; i++) if (y[i] <= 0 && y[i + 1] > 0) zc.push(i + y[i] / (y[i] - y[i + 1])); const per = []; for (let i = 1; i < zc.length; i++) per.push(zc[i] - zc[i - 1]); const m = per.reduce((a, b) => a + b) / per.length; let s = 0; for (const p of per) s += (p - m) ** 2; return 100 * Math.sqrt(s / per.length) / m; };
  out.vibe.wf_pct_off = flut(w0.y); out.vibe.wf_pct_on = flut(w1.y);
  return out;
});
console.log(JSON.stringify(res, null, 1));
await browser.close();
