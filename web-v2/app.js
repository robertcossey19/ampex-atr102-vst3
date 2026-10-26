// =====================================================================
//  ANALOGEXACT · Ampex ATR-102  —  front panel + audio engine
// =====================================================================
(() => {
'use strict';
const $ = (s, r = document) => r.querySelector(s);
const C = AmpexCore;
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const HOSTED = (typeof window.__JUCE_HOST__ !== 'undefined') || location.protocol === 'data:' || location.protocol === 'juce:';

// ---------------- parameter model (same IDs/ranges as the VST3's APVTS)
const P = { inDb: 0, outDb: 0, biasDb: 0, headroomDb: 0, monitor: 1, headblock: 0, speed: 1, flux: 1, eqStd: 0, tapeType: 1,
            transformer: 1, tapeHiss: 0, wowFlutter: 0, vibe: 0, preserveAmount: 0, postDb: 0, mix: 1 };
const RANGE = { inDb: [-12, 12], outDb: [-24, 6], biasDb: [-5, 5], headroomDb: [-12, 12], preserveAmount: [0, 1], postDb: [-12, 12], mix: [0, 1] };
const CONFIG_KEYS = new Set(['tapeType', 'speed', 'eqStd', 'flux', 'biasDb', 'transformer', 'vibe', 'headblock']);
const TAPE_NAMES = ['406', '456', '499', 'GP9', 'SM900', 'SM911'];
const TAPE_LONG = ['Ampex 406', 'Ampex 456 Grand Master', 'Ampex 499 Grand Master Gold', 'Quantegy GP9', 'EMTEC/RMGI SM900', 'EMTEC/RMGI SM911'];

const stage = $('#stage');
let fromHost = false;

// ---------------- rotary knobs (SVG)
function knobSVG(size, big) {
  const r = size / 2, ns = 'http://www.w3.org/2000/svg';
  let ticks = '';
  if (big) for (let i = 0; i <= 10; i++) {
    const a = (-135 + 27 * i) * Math.PI / 180, x1 = r + (r - 1) * Math.sin(a), y1 = r - (r - 1) * Math.cos(a), x2 = r + (r - 5) * Math.sin(a), y2 = r - (r - 5) * Math.cos(a);
    ticks += `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}" stroke="#d8d8d2" stroke-width="${i % 5 === 0 ? 1.6 : 1}"/>`;
    if (i % 10 === 0) { const tx = r + (r + 7) * Math.sin(a), ty = r - (r + 7) * Math.cos(a); ticks += `<text x="${tx.toFixed(1)}" y="${(ty + 3).toFixed(1)}" font-size="7.5" fill="#bdbdb6" text-anchor="middle" font-family="var(--font)" font-weight="600">${i}</text>`; }
  }
  const cr = big ? r - 8 : r - 4;
  return `<svg viewBox="0 0 ${size} ${size}" xmlns="${ns}">
    <defs><radialGradient id="kc${size}" cx="40%" cy="30%" r="75%"><stop offset="0" stop-color="#4a4a4e"/><stop offset=".6" stop-color="#1b1b1d"/><stop offset="1" stop-color="#050506"/></radialGradient>
    <linearGradient id="ks${size}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a3a3e"/><stop offset="1" stop-color="#121214"/></linearGradient></defs>
    ${big ? `<circle cx="${r}" cy="${r}" r="${r - 1}" fill="url(#ks${size})" stroke="#000" stroke-width=".8"/>` : ''}
    ${ticks}
    <g class="cap"><circle cx="${r}" cy="${r}" r="${cr}" fill="url(#kc${size})" stroke="#000" stroke-width="1"/>
      <circle cx="${r}" cy="${r}" r="${cr - 3}" fill="none" stroke="#ffffff10" stroke-width="1"/>
      <line x1="${r}" y1="${r - cr + 3}" x2="${r}" y2="${r - cr * 0.35}" stroke="#f4f4ee" stroke-width="${big ? 2.4 : 2}" stroke-linecap="round"/></g>
  </svg>`;
}
class Knob {
  constructor(el, { min, max, value, fmt, onChange, step = 0, detents = null }) {
    this.el = el; this.min = min; this.max = max; this.v = value; this.fmt = fmt; this.onChange = onChange; this.step = step; this.detents = detents;
    const big = !el.classList.contains('sm'); el.innerHTML = knobSVG(big ? 62 : 46, big); this.cap = $('.cap', el);
    el.setAttribute('aria-valuemin', min); el.setAttribute('aria-valuemax', max);
    let startY = 0, startV = 0, moved = false;
    el.addEventListener('pointerdown', (e) => { el.setPointerCapture(e.pointerId); startY = e.clientY; startV = this.v; moved = false; e.preventDefault(); el.focus(); });
    el.addEventListener('pointermove', (e) => { if (!el.hasPointerCapture(e.pointerId)) return; const dy = startY - e.clientY; if (Math.abs(dy) > 2) moved = true; const span = max - min; this.set(startV + dy / (e.shiftKey ? 1200 : 200) * span, true); });
    el.addEventListener('pointerup', (e) => { if (!moved && detents) this.set(this.nextDetent(), true); });
    el.addEventListener('dblclick', () => this.set(this.def ?? value, true));
    el.addEventListener('wheel', (e) => { e.preventDefault(); const span = max - min; this.set(this.v - Math.sign(e.deltaY) * span * (detents ? 1 / (detents.length - 1) : 0.01), true); }, { passive: false });
    el.addEventListener('keydown', (e) => { const span = max - min, st = detents ? span / (detents.length - 1) : Math.max(this.step || 0, span / 100); if (e.key === 'ArrowUp' || e.key === 'ArrowRight') this.set(this.v + st, true); else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') this.set(this.v - st, true); else return; e.preventDefault(); });
    this.def = value; this.render();
  }
  nextDetent() { const i = this.detents.indexOf(this.v); return this.detents[(i + 1) % this.detents.length]; }
  set(v, user) {
    v = clamp(v, this.min, this.max);
    if (this.detents) { let best = this.detents[0]; for (const d of this.detents) if (Math.abs(d - v) < Math.abs(best - v)) best = d; v = best; }
    else if (this.step) v = Math.round(v / this.step) * this.step;
    if (v === this.v && user) return;
    this.v = v; this.render(); if (user) this.onChange(v);
  }
  render() { const t = (this.v - this.min) / (this.max - this.min); const ang = this.detents ? -60 + 120 * t : -135 + 270 * t; this.cap.style.transform = `rotate(${ang}deg)`; this.el.setAttribute('aria-valuenow', this.v.toFixed(2)); if (this.fmt) this.fmt(this.v); }
}

// ---------------- VU meters (SVG), true VU deflection: 0 VU = 71 % of arc, +3 = full scale
function vuSVG() {
  const cx = 104, cy = 150, R = 118, marks = [-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3];
  const ang = (db) => -45 + 90 * Math.pow(10, db / 20) * 0.7071;
  const pt = (a, r) => [cx + r * Math.sin(a * Math.PI / 180), cy - r * Math.cos(a * Math.PI / 180)];
  const arc = (a0, a1, r) => { const [x0, y0] = pt(a0, r), [x1, y1] = pt(a1, r); return `M${x0.toFixed(1)} ${y0.toFixed(1)} A${r} ${r} 0 0 1 ${x1.toFixed(1)} ${y1.toFixed(1)}`; };
  let s = '';
  for (const m of marks) { const a = ang(m); const [x0, y0] = pt(a, R - (m >= 0 ? 10 : 8)), [x1, y1] = pt(a, R); const [tx, ty] = pt(a, R - 19);
    s += `<line x1="${x0.toFixed(1)}" y1="${y0.toFixed(1)}" x2="${x1.toFixed(1)}" y2="${y1.toFixed(1)}" stroke="${m > 0 ? '#9a1d14' : '#141210'}" stroke-width="${m === 0 ? 2 : 1.2}"/>`;
    s += `<text x="${tx.toFixed(1)}" y="${(ty + 3.5).toFixed(1)}" text-anchor="middle" font-size="${m === 0 ? 11 : 9}" font-weight="600" fill="${m > 0 ? '#9a1d14' : '#141210'}" font-family="var(--font)">${m > 0 ? '+' + m : (m === 0 ? '0' : m)}</text>`; }
  // minor ticks between −10..+3 at 1 dB where not labelled
  for (let m = -9; m <= 2; m++) if (!marks.includes(m)) { const a = ang(m); const [x0, y0] = pt(a, R - 5), [x1, y1] = pt(a, R); s += `<line x1="${x0.toFixed(1)}" y1="${y0.toFixed(1)}" x2="${x1.toFixed(1)}" y2="${y1.toFixed(1)}" stroke="#141210" stroke-width=".8"/>`; }
  return `<svg viewBox="0 0 208 126" xmlns="http://www.w3.org/2000/svg">
    <defs><radialGradient id="vg" cx="50%" cy="95%" r="90%"><stop offset="0" stop-color="#fff2cf" stop-opacity=".55"/><stop offset=".6" stop-color="#ffcf7a" stop-opacity=".15"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient></defs>
    <rect class="vu-face" x="0" y="0" width="208" height="126"/>
    <rect class="vu-glow" x="0" y="0" width="208" height="126" fill="url(#vg)"/>
    <path d="${arc(ang(-20), ang(0), R)}" fill="none" stroke="#141210" stroke-width="2"/>
    <path d="${arc(ang(0), ang(3), R)}" fill="none" stroke="#9a1d14" stroke-width="4"/>
    ${s}
    <text x="104" y="84" text-anchor="middle" font-size="18" font-weight="700" fill="#141210" font-family="var(--font)" letter-spacing="2">VU</text>
    <text x="104" y="98" text-anchor="middle" font-size="6.5" fill="#3a3630" font-family="var(--font)" letter-spacing="1.5">AMPEX · ATR-102</text>
    <line class="needle" x1="104" y1="150" x2="104" y2="26" stroke="#141210" stroke-width="1.6" transform="rotate(-45 104 150)"/>
    <path d="M86 126 L122 126 L118 112 A14 14 0 0 0 90 112 Z" fill="#111"/>
    <rect x="0" y="0" width="208" height="126" fill="none" stroke="#000" stroke-width="4"/>
  </svg>`;
}
class VU {
  constructor(win) { win.innerHTML = vuSVG(); this.needle = $('.needle', win); this.db = -60; this.disp = -60; }
  setDb(db) { this.db = db; }
  tick(dt) { const tau = this.db > this.disp ? 0.12 : 0.18; const a = 1 - Math.exp(-dt / tau); this.disp += (this.db - this.disp) * a; const d = clamp(Math.pow(10, (this.disp + 18) / 20) * 0.7071, 0, 1.06); this.needle.setAttribute('transform', `rotate(${(-45 + 90 * d).toFixed(2)} 104 150)`); }
}
const vuL = new VU($('#vuLwin')), vuR = new VU($('#vuRwin'));

// ---------------- reels (10.5" NAB metal reels)
function reelSVG(id) {
  let win = '';
  for (let k = 0; k < 3; k++) { const a = k * 120; win += `<path transform="rotate(${a} 150 150)" d="M150 60 A90 90 0 0 1 228 105 L196 124 A53 53 0 0 0 150 97 Z" fill="#000"/>`; }
  return `<svg viewBox="0 0 300 300" xmlns="http://www.w3.org/2000/svg">
    <defs><radialGradient id="rm${id}" cx="35%" cy="30%" r="80%"><stop offset="0" stop-color="#f1f0ea"/><stop offset=".55" stop-color="#b9b8b1"/><stop offset="1" stop-color="#7d7c76"/></radialGradient>
    <radialGradient id="tp${id}" cx="50%" cy="50%" r="50%"><stop offset=".3" stop-color="#2f2219"/><stop offset=".9" stop-color="#4a3626"/><stop offset="1" stop-color="#2b1f16"/></radialGradient>
    <mask id="mk${id}"><rect width="300" height="300" fill="#fff"/>${win}</mask></defs>
    <circle cx="150" cy="150" r="149" fill="#101113"/>
    <circle class="pack" cx="150" cy="150" r="128" fill="url(#tp${id})"/>
    <g class="spin">
      <g mask="url(#mk${id})"><circle cx="150" cy="150" r="146" fill="url(#rm${id})" stroke="#5a5955" stroke-width="1.5"/>
      <circle cx="150" cy="150" r="138" fill="none" stroke="#ffffff55" stroke-width="1"/></g>
      <circle cx="150" cy="150" r="48" fill="#1b1c1f" stroke="#55565a" stroke-width="2"/>
      <circle cx="150" cy="150" r="30" fill="#2a2b2f" stroke="#000"/>
      <g fill="#cfcec8"><circle cx="150" cy="117" r="4"/><circle cx="178.6" cy="166.5" r="4"/><circle cx="121.4" cy="166.5" r="4"/></g>
      <circle cx="150" cy="150" r="9" fill="#0b0b0c"/>
    </g></svg>`;
}
$('#reelL').innerHTML = reelSVG('L'); $('#reelR').innerHTML = reelSVG('R');
const spinL = $('#reelL .spin'), spinR = $('#reelR .spin'), packL = $('#reelL .pack'), packR = $('#reelR .pack');
let angL = 0, angR = 0, posFrac = 0.15;   // fraction of the tape wound onto the take-up reel
const REEL_PX = 264, RSC = REEL_PX / 300, CXL = 118 + REEL_PX / 2, CXR = 770 + REEL_PX / 2, CY = 34 + REEL_PX / 2;
function reelRadii() { const rmin = 52, rmax = 128; const a = (1 - posFrac) * (rmax * rmax - rmin * rmin) + rmin * rmin, b = posFrac * (rmax * rmax - rmin * rmin) + rmin * rmin; return [Math.sqrt(a), Math.sqrt(b)]; }   // in viewBox units (300 = reel)
function drawTapePath() {
  const [rl, rr] = reelRadii(); const RL = rl * RSC, RR = rr * RSC;
  const gx1 = 396, gy1 = 354, gx2 = 756, gy2 = 354, gr = 10;
  // tangent from the pack circle to the tension roller (outer tangent, tape leaves the right side of the supply reel)
  const tan = (cx, cy, R, px, py, sign) => { const dx = px - cx, dy = py - cy, D = Math.hypot(dx, dy), b = Math.atan2(dy, dx), g = Math.acos(Math.min(1, R / D)); const t = b + sign * g; return [cx + R * Math.cos(t), cy + R * Math.sin(t)]; };
  const p1 = tan(CXL, CY, RL, gx1, gy1 - gr, -1), p2 = tan(CXR, CY, RR, gx2, gy2 - gr, 1);
  const mid = `L${gx1 + 6} ${gy1 - gr + 1} L452 344 L700 344 L${gx2 - 6} ${gy2 - gr + 1}`;
  const d = `M${p1[0].toFixed(1)} ${p1[1].toFixed(1)} L${gx1} ${gy1 - gr} ${mid} L${gx2} ${gy2 - gr} L${p2[0].toFixed(1)} ${p2[1].toFixed(1)}`;
  $('#tapePath').setAttribute('d', d); $('#tapePathHi').setAttribute('d', d);
  const dt = `M${gx1} ${gy1 - gr} ${mid} L${gx2} ${gy2 - gr}`; $('#tapePathTop').setAttribute('d', dt); $('#tapePathTopHi').setAttribute('d', dt);
  packL.setAttribute('r', rl.toFixed(1)); packR.setAttribute('r', rr.toFixed(1));
}
drawTapePath();

// ---------------- keys
function mkKey(label, id, cls = '') { const b = document.createElement('button'); b.className = 'key ' + cls; b.id = id; b.innerHTML = `<span class="led"></span><span>${label}</span>`; return b; }
const tapeKeys = $('#tapeKeys'), fluxKeys = $('#fluxKeys');
TAPE_NAMES.forEach((n, i) => { const k = mkKey(n, 'tape-' + i); k.title = TAPE_LONG[i]; k.onclick = () => setParam('tapeType', i, true); tapeKeys.appendChild(k); });
C.FLUX.forEach((f, i) => { const k = mkKey(f, 'flux-' + i); k.title = f + ' nWb/m operating level'; k.onclick = () => setParam('flux', i, true); fluxKeys.appendChild(k); });
const matrix = $('#matrix');
function rowLab(t) { const s = document.createElement('span'); s.className = 'row-lab'; s.textContent = t; return s; }
matrix.append(rowLab('Monitor'), mkKey('Input', 'k-mon-in'), mkKey('Repro', 'k-mon-rep'), rowLab('EQ'), mkKey('NAB', 'k-nab'));
matrix.append(rowLab('Heads'), mkKey('Stereo', 'k-st'), mkKey('Mono', 'k-mono'), rowLab(''), mkKey('IEC', 'k-iec'));
matrix.append(rowLab('Model'), mkKey('XFMR', 'k-xf'), mkKey('Hiss', 'k-hiss'), mkKey('W&F', 'k-wf'), mkKey('Vibe', 'k-vibe'));
$('#k-mon-in').onclick = () => setParam('monitor', 0, true); $('#k-mon-rep').onclick = () => setParam('monitor', 1, true);
$('#k-nab').onclick = () => setParam('eqStd', 0, true); $('#k-iec').onclick = () => setParam('eqStd', 1, true);
$('#k-st').onclick = () => setParam('headblock', 0, true); $('#k-mono').onclick = () => setParam('headblock', 1, true);
$('#k-xf').onclick = () => setParam('transformer', P.transformer ? 0 : 1, true); $('#k-hiss').onclick = () => setParam('tapeHiss', P.tapeHiss ? 0 : 1, true);
$('#k-wf').onclick = () => setParam('wowFlutter', P.wowFlutter ? 0 : 1, true); $('#k-vibe').onclick = () => setParam('vibe', P.vibe ? 0 : 1, true);
$('#k-nab').title = 'NAB: 3180 + 50 µs (7.5/15 ips)'; $('#k-iec').title = 'IEC/CCIR: 70 µs @7.5, 35 µs @15'; $('#k-xf').title = 'Input/output transformer model (T2 bridging in, T1 out)';
$('#k-vibe').title = 'Worn/under-biased machine: −1 dB bias, 1.0 µm spacing, +50 % flutter, −2 dB S/N'; $('#k-mono').title = '¼-in full-track headblock: L+R summed, +4 dB S/N';
const transport = $('#transport');
transport.append(rowLab('Source'), mkKey('File', 'k-file'), mkKey('Mic', 'k-mic'), mkKey('Tone', 'k-tone'), mkKey('Rec', 'k-rec', 'red'));
transport.append(rowLab('Deck'), mkKey('◀◀', 'k-rew', 'icon'), mkKey('Stop', 'k-stop', 'red'), mkKey('Play', 'k-play', 'red'), mkKey('▶▶', 'k-ff', 'icon'));
$('#k-rec').title = 'Arm the microphone and roll'; $('#k-tone').title = '1 kHz sine at 0 VU (−18 dBFS)'; $('#k-file').title = 'Load an audio file (or drop one on the deck)'; $('#k-rew').title = 'Back 15 s'; $('#k-ff').title = 'Forward 15 s';

// ---------------- knobs wiring
const fmtDb = (id) => (v) => { $(id).textContent = (v >= 0 ? '+' : '−') + Math.abs(v).toFixed(1) + ' dB'; };
const knobs = {
  inDb: new Knob($('#k-in'), { min: -12, max: 12, value: P.inDb, step: 0.1, fmt: fmtDb('#v-in'), onChange: v => setParam('inDb', v, true) }),
  outDb: new Knob($('#k-out'), { min: -24, max: 6, value: P.outDb, step: 0.1, fmt: fmtDb('#v-out'), onChange: v => setParam('outDb', v, true) }),
  biasDb: new Knob($('#k-bias'), { min: -5, max: 5, value: P.biasDb, step: 0.1, fmt: fmtDb('#v-bias'), onChange: v => setParam('biasDb', v, true) }),
  headroomDb: new Knob($('#k-cal'), { min: -12, max: 12, value: P.headroomDb, step: 0.5, fmt: (v) => { fmtDb('#v-cal')(v); const d = v - 18; $('#calTxt').textContent = (d > 0 ? '+' : '\u2212') + Math.abs(d).toFixed(1).replace(/\.0$/, '') + ' dBFS'; }, onChange: v => setParam('headroomDb', v, true) }),
  mix: new Knob($('#k-mix'), { min: 0, max: 1, value: P.mix, step: 0.01, fmt: v => { $('#v-mix').textContent = Math.round(v * 100) + ' %'; }, onChange: v => setParam('mix', v, true) }),
  postDb: new Knob($('#k-post'), { min: -12, max: 12, value: P.postDb, step: 0.1, fmt: fmtDb('#v-post'), onChange: v => setParam('postDb', v, true) }),
  preserveAmount: new Knob($('#k-pres'), { min: 0, max: 1, value: P.preserveAmount, step: 0.01, fmt: v => { $('#v-pres').textContent = Math.round(v * 100) + ' %'; }, onChange: v => setParam('preserveAmount', v, true) }),
  speed: new Knob($('#k-speed'), { min: 0, max: 2, value: P.speed, detents: [0, 1, 2], onChange: v => setParam('speed', v, true) }),
};
knobs.inDb.def = 0; knobs.outDb.def = 0; knobs.biasDb.def = 0; knobs.headroomDb.def = 0; knobs.mix.def = 1; knobs.postDb.def = 0; knobs.preserveAmount.def = 0; knobs.speed.def = 1;

// ---------------- state → UI
function tapeSpec() {
  const ts = C.TAPES[P.tapeType], flux = C.FLUX[P.flux];
  // headroom to the 3 % THD point above the operating level, from the tape sheet MOL (3 % at 1 kHz, nWb/m)
  const molVu = 20 * Math.log10(ts.mol / flux);
  const spd = C.SPEEDS[P.speed]; let snr = 69; if (P.speed === 2) snr = 72; else if (P.speed === 1 && P.eqStd === 1) snr = 70; else if (P.speed === 0) snr = P.eqStd === 0 ? 71 : 68;
  if (P.headblock) snr += 4; snr -= ts.noise; if (P.vibe) snr -= 2; snr += 20 * Math.log10(flux / 250);    // S/N re the operating level
  return { molVu, snr, spd, thd: ts.thd, ref: ts.ref };
}
function refresh() {
  for (const k in knobs) knobs[k].set(P[k], false);
  TAPE_NAMES.forEach((_, i) => $('#tape-' + i).classList.toggle('on', P.tapeType === i));
  C.FLUX.forEach((_, i) => $('#flux-' + i).classList.toggle('on', P.flux === i));
  $('#k-mon-in').classList.toggle('on', !P.monitor); $('#k-mon-rep').classList.toggle('on', !!P.monitor);
  $('#k-nab').classList.toggle('on', P.eqStd === 0); $('#k-iec').classList.toggle('on', P.eqStd === 1);
  $('#k-st').classList.toggle('on', !P.headblock); $('#k-mono').classList.toggle('on', !!P.headblock);
  $('#k-xf').classList.toggle('on', !!P.transformer); $('#k-hiss').classList.toggle('on', !!P.tapeHiss); $('#k-wf').classList.toggle('on', !!P.wowFlutter); $('#k-vibe').classList.toggle('on', !!P.vibe);
  const sp = tapeSpec(), eq = C.eqFor(P.speed, P.eqStd);
  $('#tdName').textContent = TAPE_NAMES[P.tapeType]; $('#tdFlux').textContent = C.FLUX[P.flux] + ' nWb/m';
  $('#tdSpec').textContent = `MOL +${sp.molVu.toFixed(1)} VU · S/N ${Math.round(sp.snr)} dB · ${sp.spd} ips ${eq.name}`;
  $('#eqTxt').textContent = `${sp.spd} IPS ${eq.name}`; $('#fluxTxt').textContent = C.FLUX[P.flux] + ' nWb/m';
  $('#headTxt').innerHTML = (P.headblock ? '<b>¼ IN · FULL TRACK</b>' : '<b>½ IN · 2 TRACK</b>') + 'ATR-102 HEADS'; $('#speedTxt').textContent = sp.spd + ' ips';
  $('#tapeDisp').title = TAPE_LONG[P.tapeType] + ` — Hc ${C.TAPES[P.tapeType].Hc} A/m, coating ${C.TAPES[P.tapeType].coat} µm, ref ${sp.ref} nWb/m`;
}

// ---------------- param changes → engine (+ host)
let designTimer = 0;
function setParam(id, v, user) {
  if (id in RANGE) v = clamp(v, RANGE[id][0], RANGE[id][1]);
  P[id] = v;
  refresh();
  if (CONFIG_KEYS.has(id)) scheduleDesign(); else engine.pushParams();
  if (user) toHost(id, v);
}
function scheduleDesign() { clearTimeout(designTimer); designTimer = setTimeout(() => engine.pushConfig(), 40); }

// ---------------- JUCE bridge (same contract as the shipped editor)
function toHost(id, v) {
  if (!HOSTED || fromHost) return;
  let out = v;
  if (id === 'speed') out = C.SPEEDS[v];            // editor maps ips → index
  if (id === 'flux') out = C.FLUX[v];               // editor maps nWb/m → index
  hostSend(`juce://param/${id}/${(+out).toFixed(6)}`);
}
function hostSend(url) { if (window.__JUCE_HOST__ && typeof window.__JUCE_HOST__.send === 'function') window.__JUCE_HOST__.send(url); else location.href = url; }
window.juceSetParam = (id, raw) => {
  if (!(id in P)) return;
  fromHost = true;
  const v = (id in RANGE) ? +raw : Math.round(+raw);
  if (Math.abs(P[id] - v) > 1e-6) setParam(id, v, false);
  fromHost = false;
};
window.juceUpdateVU = (l, r) => { vuL.setDb(l); vuR.setDb(r); hostLevel = Math.max(l, r); };
let hostLevel = -120;

// ---------------- audio engine
const engine = {
  ctx: null, node: null, src: null, gain: null, running: false, source: 'file', buffer: null, fileName: '', startedAt: 0, offset: 0, mic: null, osc: null, ready: false,
  async start() {
    if (this.ctx) { if (this.ctx.state !== 'running') await this.ctx.resume(); return; }
    const ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    this.ctx = ctx;
    const core = document.getElementById('ampex-core').textContent;
    const url = URL.createObjectURL(new Blob([core + '\n' + WORKLET_SRC], { type: 'application/javascript' }));
    await ctx.audioWorklet.addModule(url);
    this.node = new AudioWorkletNode(ctx, 'analogexact-atr102', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit', channelInterpretation: 'speakers' });
    this.node.port.onmessage = (e) => { if (e.data.type === 'meter') { vuL.setDb(e.data.l); vuR.setDb(e.data.r); } };
    this.gain = ctx.createGain(); this.gain.gain.value = 1;
    this.gain.connect(this.node).connect(ctx.destination);
    this.ready = true; this.pushConfig(); this.pushParams();
  },
  pushConfig() { if (!this.ready) return; const cfg = { tape: P.tapeType, speed: P.speed, eq: P.eqStd, flux: P.flux, biasDb: P.biasDb, transformer: !!P.transformer, vibe: !!P.vibe, mono: !!P.headblock }; const t0 = performance.now(); const data = C.TapeChannel.design(this.ctx.sampleRate, cfg); this.node.port.postMessage({ type: 'config', data }); this.lastDesignMs = performance.now() - t0; },
  pushParams() { if (!this.ready) return; this.node.port.postMessage({ type: 'params', data: { inDb: P.inDb, outDb: P.outDb, headroomDb: P.headroomDb, monitor: P.monitor, tapeHiss: P.tapeHiss, wowFlutter: P.wowFlutter, preserveAmount: P.preserveAmount, postDb: P.postDb, mix: P.mix } }); },
  stopSource() { if (this.src) { try { this.src.stop(); } catch (e) {} try { this.src.disconnect(); } catch (e) {} this.src = null; } if (this.osc) { try { this.osc.stop(); } catch (e) {} this.osc = null; } if (this.micNode) { try { this.micNode.disconnect(); } catch (e) {} this.micNode = null; } if (this.running && this.source === 'file') this.offset = this.position(); this.running = false; },
  position() { if (!this.buffer) return 0; if (!this.running || this.source !== 'file') return this.offset; return (this.offset + (this.ctx.currentTime - this.startedAt)) % this.buffer.duration; },
  async play() {
    await this.start(); this.stopSource();
    const ctx = this.ctx;
    if (this.source === 'file') {
      if (!this.buffer) { toast('No tape loaded — drop an audio file on the deck or press REC for the mic'); return; }
      const s = ctx.createBufferSource(); s.buffer = this.buffer; s.loop = true; s.connect(this.gain); s.start(0, this.offset % this.buffer.duration); this.src = s; this.startedAt = ctx.currentTime;
    } else if (this.source === 'tone') {
      const o = ctx.createOscillator(); o.frequency.value = 1000; const g = ctx.createGain(); g.gain.value = Math.pow(10, -18 / 20); o.connect(g).connect(this.gain); o.start(); this.osc = o; this.src = g; this.startedAt = ctx.currentTime;
    } else {
      if (!this.mic) { try { this.mic = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } }); } catch (e) { toast('Microphone unavailable: ' + (e.message || e.name) + (location.protocol === 'http:' ? ' (needs HTTPS)' : '')); return; } }
      this.micNode = ctx.createMediaStreamSource(this.mic); this.micNode.connect(this.gain); this.startedAt = ctx.currentTime;
    }
    this.running = true; this.node.port.postMessage({ type: 'reset' }); updateTransport();
  },
  stop() { this.stopSource(); updateTransport(); },
  async loadFile(file) {
    await this.start(); const was = this.running; this.stopSource();
    try { const ab = await file.arrayBuffer(); this.buffer = await this.ctx.decodeAudioData(ab); this.fileName = file.name; this.offset = 0; posFrac = 0.02; toast(`Loaded ${file.name} — ${this.buffer.duration.toFixed(1)} s, ${this.buffer.numberOfChannels} ch, ${this.buffer.sampleRate} Hz`); this.source = 'file'; if (was || true) this.play(); }
    catch (e) { toast('Could not decode that file: ' + (e.message || e)); }
    updateTransport();
  },
  seek(dt) { if (!this.buffer) return; const pos = this.position(); this.offset = ((pos + dt) % this.buffer.duration + this.buffer.duration) % this.buffer.duration; if (this.running) { this.stopSource(); this.play(); } else updateTransport(); },
};
const WORKLET_SRC = document.getElementById('ampex-worklet').textContent;

// ---------------- transport / source UI
function setSource(s) { engine.source = s; if (engine.running) engine.play(); updateTransport(); }
$('#k-file').onclick = () => { if (engine.buffer && engine.source !== 'file') setSource('file'); else $('#file').click(); };
$('#k-mic').onclick = () => setSource('mic'); $('#k-rec').onclick = () => { setSource('mic'); engine.play(); };
$('#k-tone').onclick = () => setSource('tone');
$('#k-play').onclick = () => engine.play(); $('#k-stop').onclick = () => engine.stop();
$('#k-rew').onclick = () => engine.seek(-15); $('#k-ff').onclick = () => engine.seek(15);
$('#file').onchange = (e) => { const f = e.target.files[0]; if (f) engine.loadFile(f); e.target.value = ''; };
const deck = $('#deck');
['dragenter', 'dragover'].forEach(ev => stage.addEventListener(ev, e => { e.preventDefault(); stage.classList.add('dragging'); }));
['dragleave', 'drop'].forEach(ev => stage.addEventListener(ev, e => { e.preventDefault(); if (ev === 'drop' || e.target === stage) stage.classList.remove('dragging'); }));
stage.addEventListener('drop', e => { const f = e.dataTransfer.files && e.dataTransfer.files[0]; if (f) engine.loadFile(f); });
function updateTransport() {
  if (HOSTED) { $('#srcDisp').textContent = 'HOST'; setPower(true); return; }
  $('#k-file').classList.toggle('on', engine.source === 'file'); $('#k-mic').classList.toggle('on', engine.source === 'mic'); $('#k-tone').classList.toggle('on', engine.source === 'tone');
  $('#k-play').classList.toggle('on', engine.running); $('#k-stop').classList.toggle('on', !engine.running); $('#k-rec').classList.toggle('on', engine.running && engine.source === 'mic');
  $('#srcDisp').textContent = engine.source === 'file' ? (engine.fileName ? engine.fileName.replace(/\.[^.]+$/, '').slice(0, 10) : 'FILE') : engine.source.toUpperCase();
  setPower(!!engine.ctx && engine.ctx.state === 'running');
}
function setPower(on) { stage.classList.toggle('off', !on); stage.classList.toggle('on', on); $('#pwr').classList.toggle('on', on); $('#pwrSw').setAttribute('aria-checked', on); $('#pwrTxt').textContent = on ? 'ON' : 'OFF'; $('#pwrTxt').style.color = on ? 'var(--amber)' : '#8a8a84'; }
$('#pwrSw').onclick = async () => { if (engine.ctx && engine.ctx.state === 'running') { engine.stop(); await engine.ctx.suspend(); } else { await engine.start(); } updateTransport(); };
$('#pwrSw').onkeydown = (e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); $('#pwrSw').click(); } };
let toastT = 0; function toast(m) { const t = $('#toast'); t.textContent = m; t.classList.add('show'); clearTimeout(toastT); toastT = setTimeout(() => t.classList.remove('show'), 3200); }

// ---------------- animation: meters, reels, timer
let last = performance.now();
function frame(now) {
  const dt = Math.min(0.1, (now - last) / 1000); last = now;
  vuL.tick(dt); vuR.tick(dt);
  const moving = HOSTED ? hostLevel > -50 : engine.running;
  if (moving) {
    const ips = C.SPEEDS[P.speed], pxPerSec = ips * 25.4 * (300 / 267);        // 10.5-in reel (267 mm) = 300 viewBox units
    const [rl, rr] = reelRadii();
    angL += (pxPerSec / rl) * dt * 180 / Math.PI; angR += (pxPerSec / rr) * dt * 180 / Math.PI;
    spinL.setAttribute('transform', `rotate(${angL % 360} 150 150)`); spinR.setAttribute('transform', `rotate(${angR % 360} 150 150)`);
    if (!HOSTED && engine.source === 'file' && engine.buffer) { posFrac = 0.02 + 0.96 * (engine.position() / engine.buffer.duration); }
    else posFrac = (posFrac + dt / 1800) % 1;
    drawTapePath();
    const el = engine.running && engine.ctx ? (engine.source === 'file' ? engine.position() : engine.ctx.currentTime - engine.startedAt) : 0;
    const h = Math.floor(el / 3600), m = Math.floor(el / 60) % 60, s = Math.floor(el) % 60;
    $('#segTime').textContent = `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// ---------------- fit the 1152×700 panel into the window
function fit() { const vw = window.innerWidth, vh = window.innerHeight; const s = Math.min(vw / 1152, vh / 700); stage.style.transform = `scale(${s})`; stage.style.position = 'absolute'; stage.style.left = ((vw - 1152 * s) / 2) + 'px'; stage.style.top = ((vh - 700 * s) / 2) + 'px'; }
window.addEventListener('resize', fit); fit();

// ---------------- host mode: DAW supplies the audio
if (HOSTED) { $('#transport').innerHTML = ''; $('#transport').append(rowLab('Audio'), Object.assign(document.createElement('div'), { className: 'disp', style: 'grid-column:span 4;padding:5px 8px', innerHTML: '<div class="l1" style="font-size:11px"><span>FROM HOST</span><span>VST3 · AU</span></div><div class="l2">DAW audio in → tape → out · timer idle</div>' })); $('#pwr').classList.add('hidden'); }
refresh(); updateTransport();
if (!HOSTED) setTimeout(() => toast('Drop an audio file on the deck, or pick FILE / MIC / TONE, then PLAY.'), 600);
if (HOSTED) { setTimeout(() => hostSend('juce://param/__ready__/1'), 50); }
window.ANALOGEXACT = { P, engine, setParam, HOSTED };
})();
