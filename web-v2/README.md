# ANALOGEXACT · ATR-102 — browser front panel (v2 DSP)

`index.html` is the self-contained page (CodePen/Browser). `analogexact-atr102-codepen.html` is the same thing as a body fragment for CodePen's HTML panel.

- `dsp-core.js` — JavaScript port of `Source/TapeDSP.h` (anhysteretic SFD tape model, Wallace losses, ATR-100 record/repro EQ + bias alignment, T1/T2 transformers, hiss, W&F). Validated against the C++ build with `node test-core.js` (THD, response, bias, hiss, alias all within 0.1 dB of the VST3).
- `worklet.js` — AudioWorklet processor; same block structure as `PluginProcessor::processBlock` (0 VU = −18 dBFS + headroom, latency-aligned dry, linear mix, preserve blend, meter = tape-side rms).
- `app.js` / `ui.html` — the ATR-102 front panel. `python3 build.py` concatenates them into `index.html`.
- JUCE bridge: exposes `juceSetParam(id, raw)` / `juceUpdateVU(dbL, dbR)` and reports `juce://param/<id>/<value>` (speed in ips, flux in nWb/m, as the editor expects). Host mode is detected via `location.protocol === 'data:'` or `window.__JUCE_HOST__`.
